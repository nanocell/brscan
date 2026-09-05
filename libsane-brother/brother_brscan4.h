#ifndef BROTHER_BRSCAN4_H
#define BROTHER_BRSCAN4_H

#ifdef __cplusplus
extern "C" {
#endif

#define BRSCAN4_READ_CACHE_SIZE 0x20400
#define BRSCAN4_MAX_REQUEST_SIZE 0x20200

/* Bulk read size. MUST be a multiple of the endpoint's max packet size.
 *
 * This was 0x7FFF (32767), presumably to stay inside a signed 16-bit value —
 * but the scanner's bulk IN endpoint has wMaxPacketSize 512, and 32767 is
 * 63*512 + 511. On the last packet of such a transfer the device sends a full
 * 512 bytes into 511 bytes of remaining buffer and the trailing byte is lost.
 *
 * Observed on a DCP-7060D: every ~32KB read dropped exactly one byte, which
 * desynchronised the record parser (records are [hdr][wrapper][len][payload],
 * so a one-byte shift makes the next length field garbage). The parser then
 * blocked waiting for a payload that never arrived, timed out after 20s, and
 * the EOF fallback in PageScan padded the rest of the page white — producing a
 * scan with ~15 real lines and the remainder blank.
 *
 * 0x7E00 (32256) is 63*512, and is also a whole multiple of 64, 128 and 256,
 * so it is safe for full-speed endpoints as well as high-speed ones. It stays
 * below 0x8000, so callers that carry the size in a signed 16-bit type (see
 * ReadNonFixedData's WORD parameter) are unaffected.
 */
#define BRSCAN4_MAX_USB_READ_SIZE 0x7E00

/* Largest wMaxPacketSize a bulk endpoint can have (USB 2.0 high speed). Any
 * read size that is a multiple of this is also a multiple of the 64/128/256
 * byte sizes used by slower endpoints. */
#define BRSCAN4_MAX_BULK_PACKET 512

/* Guard the invariant above at compile time: a read size that is not a whole
 * number of max-size packets silently loses the tail byte(s) of the final
 * packet, which desynchronises the record parser. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(BRSCAN4_MAX_USB_READ_SIZE % BRSCAN4_MAX_BULK_PACKET == 0,
               "BRSCAN4_MAX_USB_READ_SIZE must be a multiple of the bulk max packet size");
#else
typedef char brscan4_read_size_must_be_packet_multiple[
	(BRSCAN4_MAX_USB_READ_SIZE % BRSCAN4_MAX_BULK_PACKET == 0) ? 1 : -1];
#endif

typedef int (*brscan4_read_fn)(void *ctx, unsigned char *dst, int size);

typedef struct Brscan4ReadCache {
	unsigned char data[BRSCAN4_READ_CACHE_SIZE];
	unsigned int len;
} Brscan4ReadCache;

int brscan4_is_boundary_status(unsigned char header);
int brscan4_record_length(const unsigned char *buf,
                          unsigned int len,
                          unsigned int *record_len,
                          unsigned int *payload_offset,
                          unsigned int *payload_len);
int brscan4_status_at_frame_boundary(const unsigned char *buf, unsigned int len);
void brscan4_cache_reset(Brscan4ReadCache *cache);
int brscan4_cache_read(Brscan4ReadCache *cache,
                       brscan4_read_fn read_fn,
                       void *read_ctx,
                       unsigned char *dst,
                       int want);
int brscan4_read_next_record(Brscan4ReadCache *cache,
                             brscan4_read_fn read_fn,
                             void *read_ctx,
                             unsigned char *dst,
                             int maxlen);

#ifdef __cplusplus
}
#endif

#endif
