/* brscan-skeyd - open-source replacement for Brother's brscan-skey daemon.
 *
 * Watches a USB-attached Brother MFP for panel SCAN key presses and runs a
 * handler command when one occurs. Brother's daemon is x86-only with no
 * source, so on ARM (Raspberry Pi) this is the only way to get press-to-scan.
 *
 * See include/brscan_skey.h for the protocol and why it is a poll rather than
 * an endpoint listener.
 *
 * Three invariants matter and are easy to get wrong:
 *
 *  1. No USB interface is ever claimed. Claiming interface 1 would lock out
 *     scanimage, which needs it to perform the actual scan. Merely holding an
 *     open device handle excludes nobody, so the handle is kept open across
 *     polls: reopening it per poll (as Brother's check_skey does) missed key
 *     presses entirely in testing.
 *
 *  2. The handler runs *outside* the USB critical section, with the device
 *     handle closed. The handler invokes a SANE frontend that reopens the
 *     same device; running it while holding the semaphore would deadlock
 *     against libsane-brother's own enter_usb_criticalsection().
 *
 *  3. Key status is a level, not a pulse. After a scan the device re-asserts
 *     the same event, so it must be drained afterwards or one press scans
 *     repeatedly. See drain_until_idle().
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <usb.h>

#include "brscan_skey.h"

#define BROTHER_VENDOR 0x04F9

union semun {
	int val;
	struct semid_ds *buf;
	unsigned short *array;
};

static volatile sig_atomic_t stop_requested;
static int sem_id = -1;
static int verbose;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	time_t now = time(NULL);
	struct tm tm;
	char stamp[32];

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "[%s] ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* Create the semaphore shared with libsane-brother. Missing semaphore is not
 * fatal: without it polls and scans can still interleave, just without the
 * mutual exclusion, which is how the backend already behaves when Brother's
 * daemon is absent. */
static void sem_setup(void)
{
	union semun arg;
	key_t key;
	int fd;

	fd = open(SKEY_SEM_PATH, O_CREAT | O_RDONLY, 0644);
	if (fd >= 0)
		close(fd);

	key = ftok(SKEY_SEM_PATH, SKEY_SEM_PROJ);
	if (key == (key_t)-1) {
		logmsg("warning: ftok(%s) failed: %s - running without USB interlock",
		       SKEY_SEM_PATH, strerror(errno));
		return;
	}

	sem_id = semget(key, 1, IPC_CREAT | IPC_EXCL | 0666);
	if (sem_id >= 0) {
		arg.val = 1;
		if (semctl(sem_id, 0, SETVAL, arg) == -1) {
			logmsg("warning: semctl(SETVAL) failed: %s", strerror(errno));
			sem_id = -1;
		}
	} else if (errno == EEXIST) {
		sem_id = semget(key, 1, 0666);
	}

	if (sem_id < 0)
		logmsg("warning: no USB interlock semaphore: %s", strerror(errno));
	else if (verbose)
		logmsg("USB interlock semaphore id %d (key 0x%x)", sem_id, (unsigned)key);
}

static void sem_op(int delta)
{
	struct sembuf op;

	if (sem_id < 0)
		return;
	op.sem_num = 0;
	op.sem_op = delta;
	op.sem_flg = SEM_UNDO;
	while (semop(sem_id, &op, 1) == -1 && errno == EINTR)
		;
}

static struct usb_device *find_device(int pid)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	usb_find_busses();
	usb_find_devices();

	for (bus = usb_get_busses(); bus; bus = bus->next)
		for (dev = bus->devices; dev; dev = dev->next)
			if (dev->descriptor.idVendor == BROTHER_VENDOR &&
			    (pid == 0 || dev->descriptor.idProduct == pid))
				return dev;
	return NULL;
}

/* One poll cycle on an already-open handle. Returns 1 and sets *code when a
 * selection is pending, 0 when idle, negative on I/O error.
 *
 * The handle is kept open across polls rather than reopened each time.
 * Brother's check_skey() does open/close per cycle, but doing the same here
 * missed key presses entirely, while a persistent handle catches them
 * reliably. Holding the handle open is harmless for scanning: only claiming
 * an interface locks other processes out, and this never claims one. */
static int poll_handle(usb_dev_handle *h, unsigned char *code)
{
	unsigned char buf[SKEY_REQ_LENGTH];
	int rc, found = 0;

	memset(buf, 0, sizeof(buf));

	sem_op(-1);
	rc = usb_control_msg(h, SKEY_REQ_TYPE, SKEY_REQ_STATUS, 0, 0,
			     (char *)buf, SKEY_REQ_LENGTH, SKEY_POLL_TIMEOUT);
	sem_op(1);

	if (rc < 0)
		return rc;

	if (rc >= SKEY_REPLY_MIN && buf[SKEY_OFF_STATUS] == SKEY_STATUS_EVENT &&
	    rc > SKEY_OFF_FUNCTION) {
		*code = buf[SKEY_OFF_FUNCTION];
		found = 1;
	}

	if (verbose && rc >= SKEY_REPLY_MIN && buf[SKEY_OFF_STATUS] != SKEY_STATUS_IDLE)
		logmsg("poll: %d bytes, status=0x%02x, code=0x%02x", rc,
		       buf[SKEY_OFF_STATUS],
		       rc > SKEY_OFF_FUNCTION ? buf[SKEY_OFF_FUNCTION] : 0);

	return found;
}

/* Consume and discard any key events still pending after a scan.
 *
 * A single press produced two scans without this. The status field is a level,
 * not a pulse: after the scan completes the device re-asserts status 0x20 with
 * the same function code (and a stale press queued from an earlier session
 * shows up the same way), so treating every 0x20 poll as a fresh press scans
 * repeatedly. Swallow whatever is asserted for a short window after each scan
 * before listening again.
 *
 * The window is deliberately short. A scan takes ~45s on this hardware, so it
 * cannot mask a genuine second press, but it must not be so long that the
 * daemon ignores someone deliberately scanning another page. */
static void drain_until_idle(int pid_filter, int drain_ms)
{
	struct usb_device *dev;
	usb_dev_handle *h;
	unsigned char code;
	int waited = 0, discarded = 0;

	dev = find_device(pid_filter);
	if (!dev)
		return;

	h = usb_open(dev);
	if (!h)
		return;

	while (waited < drain_ms) {
		int rc = poll_handle(h, &code);

		if (rc < 0)
			break;
		if (rc == 1)
			discarded++;

		usleep(SKEY_POLL_INTERVAL * 1000);
		waited += SKEY_POLL_INTERVAL;
	}

	usb_close(h);

	if (discarded)
		logmsg("discarded %d repeat event(s) after scan", discarded);
}

/* Run the handler synchronously. Synchronous on purpose: the scan it starts
 * needs exclusive USB access, and polling meanwhile would fight it for the
 * bus. The device reports nothing new until the scan completes anyway. */
static void run_handler(const char *handler, unsigned char code, const char *device)
{
	pid_t pid;
	int status = 0;

	logmsg("key event: code=0x%02x -> %s", code, handler);

	pid = fork();
	if (pid < 0) {
		logmsg("fork failed: %s", strerror(errno));
		return;
	}

	if (pid == 0) {
		char codebuf[8];

		snprintf(codebuf, sizeof(codebuf), "0x%02x", code);
		setenv("BRSCAN_SKEY_CODE", codebuf, 1);
		if (device && *device)
			setenv("BRSCAN_DEVICE", device, 1);
		execl("/bin/sh", "sh", "-c", handler, (char *)NULL);
		_exit(127);
	}

	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		logmsg("handler exited with status %d", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		logmsg("handler killed by signal %d", WTERMSIG(status));
	else
		logmsg("handler completed");
}

int main(int argc, char **argv)
{
	const char *handler = NULL, *device = "";
	int pid_filter = 0, poll_ms = SKEY_POLL_INTERVAL, drain_ms = 3000, i;
	struct usb_device *dev;
	usb_dev_handle *handle = NULL;
	int warned_missing = 0, polls_ok = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--handler") && i + 1 < argc)
			handler = argv[++i];
		else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--pid") && i + 1 < argc)
			pid_filter = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--poll-ms") && i + 1 < argc)
			poll_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--drain-ms") && i + 1 < argc)
			drain_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
			verbose = 1;
		else {
			fprintf(stderr,
				"usage: %s --handler CMD [--device SANEDEV] [--pid 0xNNNN]\n"
				"          [--poll-ms N] [--drain-ms N] [--verbose]\n\n"
				"Runs CMD via /bin/sh when the panel SCAN key is pressed.\n"
				"CMD receives BRSCAN_SKEY_CODE and, if given, BRSCAN_DEVICE.\n",
				argv[0]);
			return 2;
		}
	}

	if (!handler) {
		fprintf(stderr, "%s: --handler is required\n", argv[0]);
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGCHLD, SIG_DFL);

	usb_init();
	sem_setup();

	logmsg("brscan-skeyd started (poll %d ms, handler: %s)", poll_ms, handler);

	while (!stop_requested) {
		unsigned char code = 0;
		int rc;

		if (!handle) {
			dev = find_device(pid_filter);
			if (!dev) {
				if (!warned_missing) {
					logmsg("no Brother device found - waiting");
					warned_missing = 1;
				}
				sleep(2);
				continue;
			}
			warned_missing = 0;

			handle = usb_open(dev);
			if (!handle) {
				logmsg("usb_open failed: %s - retrying", usb_strerror());
				sleep(2);
				continue;
			}
			logmsg("watching %04x:%04x for SCAN key presses",
			       dev->descriptor.idVendor, dev->descriptor.idProduct);
			polls_ok = 0;
		}

		rc = poll_handle(handle, &code);

		if (rc < 0) {
			/* Never fail silently here: an unreadable device and an
			 * idle one are indistinguishable otherwise, which is
			 * exactly the confusion that hid this bug the first
			 * time round. */
			logmsg("poll failed: rc=%d (%s) - reopening device",
			       rc, usb_strerror());
			usb_close(handle);
			handle = NULL;
			usleep(500 * 1000);
			continue;
		}

		if (verbose && !polls_ok) {
			logmsg("device responding to key-status polls");
			polls_ok = 1;
		}

		if (rc == 1) {
			/* Drop the handle for the duration of the scan so the
			 * SANE backend gets an unambiguously free device. */
			usb_close(handle);
			handle = NULL;
			run_handler(handler, code, device);
			drain_until_idle(pid_filter, drain_ms);
			continue;
		}

		usleep(poll_ms * 1000);
	}

	if (handle)
		usb_close(handle);

	logmsg("brscan-skeyd stopping");
	return 0;
}
