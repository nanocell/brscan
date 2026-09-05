/* brscan_skey.h - Brother scan-key (press-to-scan) USB protocol.
 *
 * Recovered by static analysis of Brother's brscan-skey-exe 0.3.5-0 (i386,
 * unstripped) plus live capture against a DCP-7060D. Brother ships no ARM
 * build of that daemon, and no source, so this header is the open-source
 * record of the wire protocol.
 *
 * The key event does NOT arrive on an endpoint. The device never pushes: a
 * capture of the vendor interface's bulk IN (0x85) and interrupt IN (0x89)
 * endpoints stays silent across button presses. Instead the host polls EP0
 * with a vendor control transfer:
 *
 *   usb_control_msg(h, 0xC0, 0x03, 0, 0, buf, 0xFF, timeout)
 *
 * Reply layout (observed on DCP-7060D):
 *
 *   byte 0  total length of this reply (4 or 9)
 *   byte 1  0x10  descriptor type (BDESC_TYPE, as in brother_devaccs.h)
 *   byte 2  0x03  the request code, echoed
 *   byte 3  status: 0x00 idle, 0x10 menu active, 0x20 selection pending
 *   byte 4  function code identifying the chosen destination (status 0x20)
 *   5..8    observed zero
 *
 * Brother's check_skey() runs one cycle as:
 *
 *   usb_open -> enter_usb_criticalsection -> usb_scanner_check_status
 *            -> release_usb_criticalsection -> usb_close, then usleep(100ms)
 *
 * It never claims an interface (EP0 needs none) and never holds the device
 * between polls. That is deliberate and must be preserved: it is what allows
 * a scan to acquire the device in the gaps between polls.
 *
 * IMPORTANT - the transaction must be completed. After the device reports a
 * selection it waits for the host to actually open a scan session; until then
 * the panel displays "Connecting to PC" and no further key events are
 * reported. Consuming the event without scanning wedges the panel until the
 * operator presses Stop.
 *
 * The function codes are NOT the APPNUM values Brother uses in its network
 * protocol (1=IMAGE, 2=EMAIL, 3=OCR, 5=FILE); codes outside that set have
 * been observed on USB. Treat them as opaque and model-specific.
 */

#ifndef BRSCAN_SKEY_H
#define BRSCAN_SKEY_H

#include <sys/types.h>
#include <sys/ipc.h>

/* Vendor control transfer used to poll for key events. */
#define SKEY_REQ_TYPE       0xC0
#define SKEY_REQ_STATUS     0x03
#define SKEY_REQ_LENGTH     0xff
#define SKEY_POLL_TIMEOUT   200000  /* ms; an upper bound, not a blocking wait */
#define SKEY_POLL_INTERVAL  100     /* ms between polls, as Brother does */

/* Reply fields. */
#define SKEY_OFF_LENGTH     0
#define SKEY_OFF_DESCTYPE   1
#define SKEY_OFF_REQUEST    2
#define SKEY_OFF_STATUS     3
#define SKEY_OFF_FUNCTION   4

#define SKEY_STATUS_IDLE    0x00
#define SKEY_STATUS_MENU    0x10
#define SKEY_STATUS_EVENT   0x20

#define SKEY_REPLY_MIN      4

/* Path whose inode seeds the SysV semaphore key shared between this daemon
 * and libsane-brother. Brother derives its key with ftok() over its own
 * config file; that file only exists with the proprietary package installed,
 * so the open-source stack uses a path of its own. Both sides must agree, so
 * the derivation lives here rather than being duplicated. */
#define SKEY_SEM_PATH       "/var/lib/brscan/skey.lock"
#define SKEY_SEM_PROJ       'b'

#endif /* BRSCAN_SKEY_H */
