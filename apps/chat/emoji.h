/* Chat's dog emojis: Arty and Marcy, eight moods each.
 *
 * On the wire an emoji is only text -- a message that is exactly
 * ":arty-happy:" (spaces round it allowed) -- so the server and older
 * Chats need nothing: they show the words. The pictures are pixel art,
 * drawn in tools/make_emoji.py and generated into chat_emoji.h as palette
 * indices; a code is dog * EMO_MOODS + mood.
 *
 * Header-only and libc-free, so the app and test/test_chat_emoji.c share it.
 */
#ifndef CARDOS_CHAT_EMOJI_H
#define CARDOS_CHAT_EMOJI_H

#include <stdint.h>

#include "apps/chat/chat_emoji.h"

#if defined(__GNUC__)
#define EMO_OPT __attribute__((unused))
#else
#define EMO_OPT
#endif

/* The part of `s` up to `end` is exactly `word`. */
static EMO_OPT int emo__is(const char *s, const char *end, const char *word) {
  while (s < end && *word && *s == *word) { s++; word++; }
  return s == end && !*word;
}

/* The emoji `text` is, or -1 for text. */
static EMO_OPT int emo_parse(const char *text) {
  const char *a, *dash, *b;
  int d, m;
  while (*text == ' ') text++;
  if (*text != ':') return -1;
  a = ++text;
  while (*text && *text != '-' && *text != ':') text++;
  if (*text != '-') return -1;
  dash = text++;
  while (*text && *text != ':') text++;
  if (*text != ':') return -1;
  b = text++;
  while (*text == ' ') text++;
  if (*text) return -1;
  for (d = 0; d < EMO_DOGS; d++) {
    if (!emo__is(a, dash, EMO_DOG[d])) continue;
    for (m = 0; m < EMO_MOODS; m++)
      if (emo__is(dash + 1, b, EMO_MOOD[m])) return d * EMO_MOODS + m;
  }
  return -1;
}

/* ":dog-mood:" into out; its length, or 0 when the code is not one or it
 * does not fit. */
static EMO_OPT int emo_text(char *out, int n, int code) {
  const char *parts[2];
  int k = 0, i;
  if (code < 0 || code >= EMO_DOGS * EMO_MOODS) return 0;
  parts[0] = EMO_DOG[code / EMO_MOODS];
  parts[1] = EMO_MOOD[code % EMO_MOODS];
  for (i = 0; i < 2; i++) {
    const char *p = parts[i];
    if (k >= n) return 0;
    out[k++] = i ? '-' : ':';
    while (*p) { if (k >= n) return 0; out[k++] = *p++; }
  }
  if (k + 2 > n) return 0;
  out[k++] = ':';
  out[k] = 0;
  return k;
}

/* One row of the picture at `scale`: y is in drawn rows (0 .. EMO_H*scale-1),
 * out gets EMO_W*scale pixels, `bg` where there is nothing. */
static EMO_OPT void emo_row(int code, int y, int scale, uint16_t bg, uint16_t *out) {
  const uint8_t *row = EMO_PIX[code] + (y / scale) * EMO_W;
  int x, s;
  for (x = 0; x < EMO_W; x++) {
    uint16_t c = row[x] ? EMO_PAL[row[x]] : bg;
    for (s = 0; s < scale; s++) *out++ = c;
  }
}

#endif /* CARDOS_CHAT_EMOJI_H */
