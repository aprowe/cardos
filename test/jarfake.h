/* For the Jar Factory app tests (test_jar.c, test_jarshop.c, test_jarpost.c):
 * a screen to draw on and dump, and a server to talk to.
 *
 * THE SCREEN. fill, frame, text and pixels into one 240x135 buffer, so a
 * companion app (which paints with the API's calls) can be looked at:
 * jf_dump("name") writes JAR_DUMP/name.ppm when JAR_DUMP is set.
 *
 * THE SERVER. http_start/http_poll answered by a handler the test sets,
 * from the path (the "http://srv" in front taken off); every request is
 * logged. Records are "signed" with a stand-in that the stand-in sig_verify
 * checks -- the real ECDSA is the kernel's (and the server's), not the
 * app's: what is tested here is that the app checks the right bytes and
 * acts on the answer.
 *
 * Not a test file (gen_test_main.py scans test_*.c only); static, so each
 * test file has its own.
 */
#ifndef CARDOS_TEST_JARFAKE_H
#define CARDOS_TEST_JARFAKE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/app/capp.h"
#include "kernel/console/font6x8.h"
#include "apps/jaritem.h"
#include "apps/b64.h"

/* ---- the screen ---------------------------------------------------------------- */

static uint16_t JF_FB[135][240];

static void jf_put(int x, int y, uint16_t c) {
  if (x >= 0 && x < 240 && y >= 0 && y < 135) JF_FB[y][x] = c;
}

static void jf_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++) jf_put(x, y, c);
}

static void jf_frame(CRect r, uint16_t c) {
  int i;
  for (i = 0; i < r.w; i++) { jf_put(r.x + i, r.y, c); jf_put(r.x + i, r.y + r.h - 1, c); }
  for (i = 0; i < r.h; i++) { jf_put(r.x, r.y + i, c); jf_put(r.x + r.w - 1, r.y + i, c); }
}

static void jf_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  int col, row;
  for (; *s; s++, x += 6) {
    const uint8_t *g = 0;
    if ((unsigned char)*s >= FONT_FIRST && (unsigned char)*s <= FONT_LAST) g = font6x8[(unsigned char)*s - FONT_FIRST];
    for (col = 0; col < 6; col++)
      for (row = 0; row < 8; row++)
        jf_put(x + col, y + row, (g && col < 5 && (g[col] >> row & 1)) ? fg : bg);
  }
}

static void jf_pixels(CRect r, const uint16_t *p) {
  int x, y;
  for (y = 0; y < r.h; y++)
    for (x = 0; x < r.w; x++) jf_put(r.x + x, r.y + y, p[y * r.w + x]);
}

static void jf_screen(CardApi *a) {
  a->fill = jf_fill;
  a->frame = jf_frame;
  a->text = jf_text;
  a->pixels = jf_pixels;
}

static void jf_dump(const char *name) {
  const char *dir = getenv("JAR_DUMP");
  char path[256];
  FILE *f;
  int x, y;
  if (!dir) return;
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n240 135\n255\n");
  for (y = 0; y < 135; y++)
    for (x = 0; x < 240; x++) {
      uint16_t c = (uint16_t)((JF_FB[y][x] >> 8) | (JF_FB[y][x] << 8));
      unsigned char rgb3[3];
      rgb3[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
      rgb3[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      rgb3[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb3, 1, 3, f);
    }
  fclose(f);
}

/* ---- the server --------------------------------------------------------------------- */

typedef struct { char method[8]; char path[200]; char body[2048]; } JfReq;

static JfReq JF_REQ[64];
static int JF_NREQ;
static int JF_PENDING, JF_STATUS;
static char JF_REPLY[4096];

/* The handler: the reply's bytes into `out` (returns how many), or a
 * negative status (-1 no network, -404 ...). */
static int (*jf_handler)(const char *method, const char *path, const char *body, char *out, int cap);

static int jf_start(const char *m, const char *u, const char *b, const char *ct, const char *br, int ms) {
  JfReq *r;
  (void)ct; (void)br; (void)ms;
  if (JF_PENDING) return -1;
  if (!strncmp(u, "http://srv", 10)) u += 10;
  r = &JF_REQ[JF_NREQ < 63 ? JF_NREQ++ : 63];
  snprintf(r->method, sizeof r->method, "%s", m);
  snprintf(r->path, sizeof r->path, "%s", u);
  snprintf(r->body, sizeof r->body, "%s", b ? b : "");
  JF_STATUS = jf_handler ? jf_handler(m, u, b ? b : "", JF_REPLY, sizeof JF_REPLY) : -1;
  JF_PENDING = 1;
  return 0;
}

static int jf_poll(char *out, size_t n) {
  int k;
  if (!JF_PENDING) return -1;
  JF_PENDING = 0;
  if (JF_STATUS < 0) return JF_STATUS;
  k = JF_STATUS < (int)n - 1 ? JF_STATUS : (int)n - 1;
  memcpy(out, JF_REPLY, (size_t)k);
  out[k] = 0;
  return k;
}

/* How many requests went to a path starting with `p`. */
static int jf_count(const char *p) {
  int i, n = 0;
  for (i = 0; i < JF_NREQ; i++) if (!strncmp(JF_REQ[i].path, p, strlen(p))) n++;
  return n;
}

static const JfReq *jf_last(const char *p) {
  int i;
  for (i = JF_NREQ - 1; i >= 0; i--) if (!strncmp(JF_REQ[i].path, p, strlen(p))) return &JF_REQ[i];
  return 0;
}

/* The stand-in signature: an FNV hash of the message, spread over 64 bytes. */
static void jf_sig_of(const uint8_t *m, size_t n, uint8_t *sig) {
  uint32_t h = 2166136261u;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ m[i]) * 16777619u;
  for (i = 0; i < 64; i++) sig[i] = (uint8_t)((h >> (8 * (i % 4))) ^ i);
}

static int JF_VERIFIES;
static int jf_verify(const uint8_t pub[65], const void *m, size_t n, const uint8_t sig[64]) {
  uint8_t want[64];
  JF_VERIFIES++;
  if (pub[0] != 4) return 0;
  jf_sig_of((const uint8_t *)m, n, want);
  return memcmp(want, sig, 64) == 0;
}

static const char JF_PUBHEX[] =
  "04"
  "11111111111111111111111111111111111111111111111111111111111111111"
  "111111111111111111111111111111111111111111111111111111111111111";

static void jf_server(CardApi *a) {
  a->http_start = jf_start;
  a->http_poll = jf_poll;
  a->sig_verify = jf_verify;
  JF_NREQ = 0;
  JF_PENDING = 0;
  JF_VERIFIES = 0;
  jf_handler = 0;
}

/* An item, made up. */
static void jf_item(JItem *it, uint32_t id, const char *name, const char *tags) {
  int i;
  memset(it, 0, sizeof *it);
  it->version = JI_VERSION;
  it->id = id;
  it->kind = (uint8_t)(id % 3);
  it->nframes = 2;
  it->move = (uint8_t)(id % JM_KINDS);
  it->speed = JSP_MEDIUM;
  it->zone = JZ_ANYWHERE;
  snprintf(it->name, sizeof it->name, "%s", name);
  snprintf(it->line, sizeof it->line, "A thing the server made.");
  snprintf(it->maker, sizeof it->maker, "Maya");
  snprintf(it->tags, sizeof it->tags, "%s", tags);
  it->made = 1791504000u;
  for (i = 1; i < 8; i++) it->pal[i] = (uint16_t)(0x2104 * i + id * 97);
  for (i = 0; i < 256; i++) {
    int x = i % 16, y = i / 16, d = (x - 8) * (x - 8) + (y - 8) * (y - 8);
    ji_set_px(it->frames[0], i, d < 40 ? 1 + (int)((id + (unsigned)y) % 7) : 0);
    ji_set_px(it->frames[1], i, d < 30 ? 1 + (int)((id + (unsigned)x) % 7) : 0);
  }
}

/* `it` as a record the server signed (good) or one it did not (!good):
 * into raw, its length. */
static int jf_signed(JItem *it, uint8_t *raw, int good) {
  uint8_t msg[JI_MAX];
  int n, m;
  it->sig_len = 64;
  memset(it->sig, 0, 64);
  n = jitem_encode(it, raw, JI_MAX);
  m = ji_signed_message(raw, n, msg, sizeof msg);
  jf_sig_of(msg, (size_t)m, raw + JI_HDR);
  if (!good) raw[JI_HDR + 5] ^= 0x55;
  return n;
}

/* The same, as one line of base64 into out. */
static int jf_signed_b64(JItem *it, char *out, int cap, int good) {
  uint8_t raw[JI_MAX];
  int n = jf_signed(it, raw, good);
  return b64_encode(raw, n, out, cap);
}

#endif /* CARDOS_TEST_JARFAKE_H */
