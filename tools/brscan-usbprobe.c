/* brscan-usbprobe - listen for unsolicited device->host traffic on a Brother MFP.
 *
 * Purpose: determine whether the panel SCAN key delivers an event to the host,
 * and on which endpoint. Brother's proprietary brscan-skey daemon is x86-only,
 * so the notification path has never been observed on ARM. Two facts motivate
 * this probe:
 *
 *   - The DCP-7060D exposes an interrupt-IN endpoint 0x89 (8-byte) on the
 *     vendor-specific interface 1, which libsane-brother never touches.
 *   - tests/eop_hang/usb_stub.c stubs usb_interrupt_read/write because the
 *     vendor blob dlsym's them, so something in Brother's stack uses them.
 *
 * Deliberately does NOT touch interface 0: that is the printer-class interface
 * bound to usblp, and detaching it would disturb the CUPS print queue. For the
 * same reason usb_set_configuration() is not called - it would require
 * releasing every interface, printing included.
 *
 * Usage:
 *   brscan-usbprobe [--pid 0x0249] [--ep 0x89] [--bulk] [--open] [--secs N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <usb.h>

#define BREQ_TYPE        0xC0
#define BREQ_GET_OPEN    0x01
#define BREQ_GET_CLOSE   0x02
#define BREQ_GET_LENGTH  5
#define BCOMMAND_SCANNER 0x02

/* Recovered from Brother's brscan-skey-exe 0.3.5-0 (i386, unstripped):
 * usb_scanner_check_status() issues a vendor control transfer with
 * bRequest 0x03, wValue 0, wIndex 0, wLength 0xff and a 200 s timeout,
 * then usleep(100 ms). The long timeout is the point: the device holds the
 * request open until the operator presses a key, so this is a blocking
 * long-poll on EP0 rather than an endpoint the device pushes to. That is why
 * bulk 0x85 and interrupt 0x89 both stay silent while the panel waits.
 * decode_key_data() then parses the reply from offset +4 as the same ASCII
 * key/value string the network path uses:
 *   TYPE=BR;BUTTON=SCAN;USER=...;FUNC=IMAGE|OCR|EMAIL|FILE;HOST=...
 */
#define BREQ_KEY_STATUS  0x03
#define BREQ_KEY_LENGTH  0xff
#define KEY_POLL_TIMEOUT 200000
#define KEY_DATA_OFFSET  4

#define SCANNER_VENDOR   0x04F9
#define SCAN_INTERFACE   1

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void hexdump(const unsigned char *buf, int len, double t)
{
	int i;

	printf("[%8.3f] %3d bytes:", t, len);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0 && i)
			printf("\n                     ");
		printf(" %02x", buf[i]);
	}
	printf("   |");
	for (i = 0; i < len; i++)
		printf("%c", (buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '.');
	printf("|\n");
	fflush(stdout);
}

static struct usb_device *find_device(int vid, int pid)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	for (bus = usb_get_busses(); bus; bus = bus->next)
		for (dev = bus->devices; dev; dev = dev->next)
			if (dev->descriptor.idVendor == vid &&
			    dev->descriptor.idProduct == pid)
				return dev;
	return NULL;
}

int main(int argc, char **argv)
{
	int pid = 0x0249, vid = SCANNER_VENDOR;
	int endpoint = 0x89, use_bulk = 0, do_open = 0, run_secs = 0, skey_mode = 0;
	int i, rc, claimed = 0;
	unsigned char buf[4096];
	struct usb_device *dev;
	usb_dev_handle *h;
	double t0, last_report;
	long reads = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pid") && i + 1 < argc)
			pid = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--vid") && i + 1 < argc)
			vid = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--ep") && i + 1 < argc)
			endpoint = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--bulk"))
			use_bulk = 1;
		else if (!strcmp(argv[i], "--open"))
			do_open = 1;
		else if (!strcmp(argv[i], "--skey"))
			skey_mode = 1;
		else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
			run_secs = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: %s [--vid 0x04f9] [--pid 0x0249] [--ep 0x89]\n"
				"          [--bulk] [--open] [--secs N]\n"
				"  --bulk  poll a bulk endpoint instead of interrupt\n"
				"  --skey  long-poll EP0 vendor request 0x03 for key events\n"
				"  --open  send the BREQ_GET_OPEN handshake first\n"
				"  --secs  exit after N seconds (default: run until Ctrl-C)\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	usb_init();
	usb_find_busses();
	usb_find_devices();

	dev = find_device(vid, pid);
	if (!dev) {
		fprintf(stderr, "device %04x:%04x not found\n", vid, pid);
		return 1;
	}

	h = usb_open(dev);
	if (!h) {
		fprintf(stderr, "usb_open failed: %s\n", usb_strerror());
		return 1;
	}

	/* Endpoint polling needs interface 1 claimed; the key-status poll does
	 * not, because it rides EP0. Brother's check_skey() only does
	 * usb_open -> semaphore -> control transfer -> usb_close per cycle,
	 * never claiming an interface, which is precisely what lets a scan run
	 * in the gaps between polls. Match that here or the probe would lock
	 * scanimage out for as long as it runs.
	 *
	 * Interface 1 only in either case - interface 0 belongs to usblp/CUPS. */
	if (!skey_mode) {
#ifdef LIBUSB_HAS_DETACH_KERNEL_DRIVER_NP
		usb_detach_kernel_driver_np(h, SCAN_INTERFACE);
#endif
		rc = usb_claim_interface(h, SCAN_INTERFACE);
		if (rc < 0) {
			fprintf(stderr, "usb_claim_interface(%d) failed: %s\n",
				SCAN_INTERFACE, usb_strerror());
			usb_close(h);
			return 1;
		}
		claimed = 1;
	}

	printf("# device %04x:%04x%s\n", vid, pid,
	       claimed ? ", interface 1 claimed" : " (EP0 only, no claim)");

	if (do_open) {
		unsigned char data[BREQ_GET_LENGTH];

		memset(data, 0, sizeof(data));
		rc = usb_control_msg(h, BREQ_TYPE, BREQ_GET_OPEN,
				     BCOMMAND_SCANNER, 0,
				     (char *)data, BREQ_GET_LENGTH, 2000);
		printf("# BREQ_GET_OPEN rc=%d", rc);
		if (rc > 0) {
			printf(" data:");
			for (i = 0; i < rc; i++)
				printf(" %02x", data[i]);
		}
		printf("\n");
	}

	if (skey_mode) {
		printf("# long-polling EP0 vendor request 0x%02x (%d ms timeout)\n",
		       BREQ_KEY_STATUS, KEY_POLL_TIMEOUT);
		printf("# press SCAN on the panel and choose a destination\n");
		printf("# (Ctrl-C to stop)\n");
		fflush(stdout);

		t0 = now_seconds();
		while (!stop_requested) {
			unsigned char kbuf[BREQ_KEY_LENGTH];
			double t;

			memset(kbuf, 0, sizeof(kbuf));
			rc = usb_control_msg(h, BREQ_TYPE, BREQ_KEY_STATUS, 0, 0,
					     (char *)kbuf, BREQ_KEY_LENGTH,
					     KEY_POLL_TIMEOUT);
			t = now_seconds() - t0;
			reads++;

			if (rc > 0) {
				/* Idle replies are "04 10 03 00" every 100 ms.
				 * Filter them here rather than through a pipe:
				 * grep block-buffers, which hides events until
				 * the process exits. */
				int idle = (rc == 4 && kbuf[3] == 0x00);

				if (!idle) {
					hexdump(kbuf, rc, t);
					printf("           status=0x%02x", kbuf[3]);
					if (rc > KEY_DATA_OFFSET)
						printf(" code=0x%02x", kbuf[KEY_DATA_OFFSET]);
					printf("\n");
					fflush(stdout);
				}
			} else {
				printf("[%8.3f] control rc=%d (%s)\n", t, rc,
				       usb_strerror());
				fflush(stdout);
				if (rc == -ENODEV)
					break;
			}
			usleep(100 * 1000);
			if (run_secs && t >= run_secs)
				break;
		}
		goto done;
	}

	printf("# polling %s endpoint 0x%02x - press SCAN on the panel now\n",
	       use_bulk ? "bulk" : "interrupt", endpoint);
	printf("# (Ctrl-C to stop)\n");
	fflush(stdout);

	t0 = now_seconds();
	last_report = t0;

	while (!stop_requested) {
		double t;

		if (use_bulk)
			rc = usb_bulk_read(h, endpoint, (char *)buf, sizeof(buf), 1000);
		else
			rc = usb_interrupt_read(h, endpoint, (char *)buf,
						sizeof(buf), 1000);
		t = now_seconds() - t0;
		reads++;

		if (rc > 0) {
			hexdump(buf, rc, t);
		} else if (rc != -ETIMEDOUT && rc < 0) {
			/* -ETIMEDOUT is the normal idle case; anything else is
			 * worth seeing once rather than spinning silently. */
			static int last_err = 0;

			if (rc != last_err) {
				printf("[%8.3f] read rc=%d (%s)\n", t, rc, usb_strerror());
				fflush(stdout);
				last_err = rc;
			}
			if (rc == -ENODEV || rc == -EPIPE)
				break;
		}

		if (now_seconds() - last_report >= 15.0) {
			printf("# ... still listening (%ld reads, %.0fs)\n",
			       reads, t);
			fflush(stdout);
			last_report = now_seconds();
		}

		if (run_secs && t >= run_secs)
			break;
	}

done:
	printf("# stopping after %ld reads\n", reads);

	if (do_open) {
		unsigned char data[BREQ_GET_LENGTH];

		memset(data, 0, sizeof(data));
		rc = usb_control_msg(h, BREQ_TYPE, BREQ_GET_CLOSE,
				     BCOMMAND_SCANNER, 0,
				     (char *)data, BREQ_GET_LENGTH, 2000);
		printf("# BREQ_GET_CLOSE rc=%d\n", rc);
	}

	if (claimed)
		usb_release_interface(h, SCAN_INTERFACE);
	usb_close(h);
	return 0;
}
