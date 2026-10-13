/* The title and artist of an MP3, from its ID3v2 tag: for Music's tracks
 * put on the card by hand. Header-only and libc-free, so an app includes it
 * (test/test_id3.c runs it on the PC).
 *
 * `b` is the start of the file (the first KB holds the frames that matter).
 * v2.3 and v2.4 frames; text in Latin-1, UTF-8, or UTF-16 with a byte
 * order mark (kept to its ASCII; anything else is '?'). 1 found, 0 not. */
#ifndef CARDOS_APP_ID3_H
#define CARDOS_APP_ID3_H

#include <stdint.h>

/* Big-endian, 7 or 8 bits a byte. One loop with the width as a variable:
 * written out, gcc sees a byte swap and calls libgcc's __bswapsi2, which
 * an app does not link. */
static uint32_t id3__size(const uint8_t *p, int syncsafe) {
  int bits = syncsafe ? 7 : 8, i;
  uint32_t v = 0, mask = syncsafe ? 0x7F : 0xFF;
  for (i = 0; i < 4; i++) v = v << bits | (p[i] & mask);
  return v;
}

static int id3_text(const uint8_t *b, int n, const char *frame, char *out, int outn) {
  int ver, at = 10, end, o = 0;
  if (outn < 1) return 0;
  out[0] = 0;
  if (n < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
  ver = b[3];
  if (ver != 3 && ver != 4) return 0;
  end = 10 + (int)id3__size(b + 6, 1);
  if (end > n) end = n;
  if (b[5] & 0x40) at += (int)id3__size(b + 10, ver == 4) + (ver == 3 ? 4 : 0);  /* extended header */
  while (at + 10 <= end && b[at]) {
    int size = (int)id3__size(b + at + 4, ver == 4), i, enc;
    const uint8_t *t = b + at + 10;
    if (size <= 0 || at + 10 + size > end) return 0;
    if (b[at] == frame[0] && b[at + 1] == frame[1] && b[at + 2] == frame[2] && b[at + 3] == frame[3]) {
      enc = t[0];
      if (enc == 1 || enc == 2) {                 /* UTF-16: a byte order mark, then pairs */
        int le = enc == 1 && size >= 3 && t[1] == 0xFF;
        i = enc == 1 ? 3 : 1;
        for (; i + 1 < size && o < outn - 1; i += 2) {
          unsigned c = le ? (unsigned)(t[i] | t[i + 1] << 8) : (unsigned)(t[i] << 8 | t[i + 1]);
          if (!c) break;
          out[o++] = (char)(c < 0x80 && c >= 0x20 ? c : '?');
        }
      } else {
        for (i = 1; i < size && o < outn - 1 && t[i]; i++) {
          unsigned c = t[i];
          if (enc == 3 && c >= 0x80) {            /* UTF-8: one '?' a character */
            if ((c & 0xC0) == 0x80) continue;
            c = '?';
          } else if (c >= 0x80 || c < 0x20) c = '?';
          out[o++] = (char)c;
        }
      }
      out[o] = 0;
      return o > 0;
    }
    at += 10 + size;
  }
  return 0;
}

#endif /* CARDOS_APP_ID3_H */
