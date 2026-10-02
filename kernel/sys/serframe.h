/* Frames on the serial line: how a PC talks to the device without typing.
 *
 * Bytes arriving on the USB serial port are keystrokes -- that is how the
 * console is driven from a PC, and it stays so. But typing is blind (a
 * carriage return in the launcher opens whatever is highlighted), slow (the
 * shell takes one byte a pass) and cannot write a file. A frame is the other
 * way in: it starts with STX, a byte no keyboard sends, and everything up to
 * ETX is a request for kernel/sys/serlink.c, never a key.
 *
 *   STX  id TAB verb TAB field ... TAB crc32-hex  ETX
 *
 * The reply has the same shape, with `@id`, a status (`ok` or `err`) and one
 * field of base64 data, so it can be picked out of the log lines the device
 * prints around it, and the CRC says whether a log line landed in the middle.
 * Binary goes as base64 both ways. tools/cardctl.py is the other end.
 *
 * Portable, so the host suite tests it; serlink.c is the device glue.
 */
#ifndef CARDOS_SERFRAME_H
#define CARDOS_SERFRAME_H

#include <stddef.h>
#include <stdint.h>

#define SF_STX 0x02
#define SF_ETX 0x03

/* A get answers with up to SF_CHUNK bytes. A put carries at most SF_PUT
 * bytes, so that the whole request -- base64, path and header -- fits the
 * 2 KB the serial driver holds while the shell gets round to reading it. */
#define SF_CHUNK  3072
#define SF_PUT    1024
#define SF_MAX    1900

typedef struct {
  char *buf;      /* the body so far, NUL-terminated when complete */
  int   cap;
  int   len;
  int   in;       /* between STX and ETX */
  int   over;     /* ran past cap: dropped at ETX */
} SfReader;

enum { SF_NONE = 0, SF_FRAME = 1, SF_KEY = 2, SF_BAD = -1 };

void sf_reader_init(SfReader *r, char *buf, int cap);

/* One byte. SF_KEY: not in a frame and not STX, the byte is a keystroke.
 * SF_FRAME: a complete frame whose CRC matched; r->buf holds its body
 * without the CRC field. SF_BAD: a frame ended that was too long or failed
 * its CRC, and is dropped. SF_NONE: taken, nothing yet. An STX inside a
 * frame starts over, so a sender that gave up halfway cannot wedge it. */
int sf_feed(SfReader *r, uint8_t c);

uint32_t sf_crc32(const void *data, size_t n);

/* Standard base64 with padding. Encode returns the length written (NUL
 * added), or -1 if `cap` is too small; decode returns the bytes written, or
 * -1 on a character outside the alphabet or too little room. */
int sf_b64_encode(const uint8_t *in, int n, char *out, int cap);
int sf_b64_decode(const char *in, int n, uint8_t *out, int cap);

/* Split a body on tabs in place. Returns how many fields, at most `max`;
 * the last field keeps any further tabs. */
int sf_split(char *body, char **field, int max);

/* A whole reply frame, STX to ETX and a newline: `@id status base64(data)`.
 * Returns its length, or -1 if it does not fit. */
int sf_reply(char *out, int cap, const char *id, const char *status,
             const uint8_t *data, int n);

#endif /* CARDOS_SERFRAME_H */
