/* Jar Factory's item record: one collectible, as it lives on the card and as
 * it will travel between devices (docs/superpowers/specs/
 * 2026-10-09-jar-factory-design.md, "Items").
 *
 * A record is a fixed header, then the signature, the script and the frames,
 * all little-endian, at most JI_MAX bytes -- 912 is the most v1 can be:
 *
 *   off  size  field
 *     0     1  version (1)
 *     1     1  header length (208 in v1; a newer header may be longer, and
 *              what follows starts after it, so v1 can still find it)
 *     2     2  total length of the record
 *     4     4  id, assigned by the server (built-ins are 1..12)
 *     8     1  kind: 0 floor decor, 1 hanging decor, 2 critter
 *     9     1  frames, 1..4
 *    10     1  flags: 1 arrived as a gift, 2 built in (trusted, unsigned)
 *    11     1  bubbles, 0..4
 *    12    12  name, NUL-padded (not necessarily NUL-terminated)
 *    24    32  line, NUL-padded
 *    56    16  palette: 8 x RGB565 (plain, not the panel's byte order);
 *              slot 0 is transparent and its value is ignored
 *    72     1  movement: sits hops wanders sways floats (JM_*)
 *    73     1  speed: slow medium fast (JSP_*)
 *    74     1  favourite zone: garden works dock water high anywhere (JZ_*)
 *    75     1  habits, 0..3
 *    76    12  habits: 3 x { event, event arg, action, action arg }
 *    88    48  bubbles: 4 x 12, NUL-padded
 *   136    16  memory: 8 x int16, persisted per item (scripts, phase 2)
 *   152    12  maker's name
 *   164     4  made: UTC seconds, 0 unknown
 *   168    24  the seeding tags, comma-separated ("cosy,soft")
 *   192    12  who gifted it, empty if nobody
 *   204     1  signature length, 0..64 (0 for built-ins)
 *   205     2  script length, 0..256 (0 in phase 1)
 *   207     1  price / 5, chosen by the server (0: worked out on the device)
 *   208        signature bytes, then script bytes, then frames x 96
 *
 * A frame is 16x16 at 3 bits a pixel, 96 bytes: pixel i (row-major) is bits
 * 3i..3i+2 of the frame read as one little-endian bit string. Frame 0 is the
 * shop picture.
 *
 * Reading is forgiving where the spec says to be: a newer version keeps its
 * recipe and drops its script; a habit naming an event or action this
 * device does not know is dropped; a movement, speed or zone it does not know
 * becomes sits / medium / anywhere. Anything that would make the record
 * unsafe to draw or store -- lengths that disagree, no frames, more than four
 * -- is refused.
 *
 * Header-only and libc-free, like apps/petsim.h, so apps/jar.c and the host
 * tests (test/test_jaritem.c) share one copy, and so the server can be
 * checked against the same layout.
 */
#ifndef CARDOS_JARITEM_H
#define CARDOS_JARITEM_H

#include <stdint.h>

#if defined(__GNUC__)
#define JI_OPT __attribute__((unused))
#else
#define JI_OPT
#endif

#define JI_VERSION      1
#define JI_HDR          208
#define JI_MAX          1024
#define JI_FRAME_BYTES  96
#define JI_MAX_FRAMES   4
#define JI_NAME         12
#define JI_LINE         32
#define JI_BUB          12
#define JI_MAX_BUB      4
#define JI_MAX_HAB      3
#define JI_MEM          8
#define JI_WHO          12
#define JI_TAGS         24
#define JI_SIG_MAX      64
#define JI_SCRIPT_MAX   256

enum { JK_FLOOR = 0, JK_HANGING, JK_CRITTER, JK_KINDS };
enum { JM_SITS = 0, JM_HOPS, JM_WANDERS, JM_SWAYS, JM_FLOATS, JM_KINDS };
enum { JSP_SLOW = 0, JSP_MEDIUM, JSP_FAST, JSP_KINDS };
enum { JZ_GARDEN = 0, JZ_WORKS, JZ_DOCK, JZ_WATER, JZ_HIGH, JZ_ANYWHERE, JZ_KINDS };

/* The events (spec, "Events"). The event argument narrows a habit:
 *   tick     every N item ticks (4 a second); 0 is every tick
 *   near     what came near: 0 anything, then JN_* below
 *   jam      0 either, 1 jammed, 2 fixed
 *   time     0 any change, then 1 dawn, 2 day, 3 dusk, 4 night
 * and is ignored by the rest. */
enum { JE_TICK = 0, JE_POKE, JE_NEAR, JE_SHIPPED, JE_JAM, JE_GIFT, JE_TIME,
       JE_WEATHER, JE_HOUR, JE_SIGNAL, JE_WORLD, JE_NEW, JE_BERRY, JE_BUMPED, JE_KINDS };
enum { JN_ANY = 0, JN_MOSS, JN_SNAIL, JN_CRITTER, JN_DECOR };

/* The actions (spec, "Actions"), with what the argument means:
 *   hop      height in pixels (0: 6)        walk     to zone JZ_*
 *   float    for N ticks                    face     the nearest thing
 *   stop     -                              frame    show frame N
 *   flip     -                              glow     0 off, 1 on, 2 toggle
 *   particle JP_* below                     say      bubble N for 2 s
 *   wait     N ticks */
enum { JA_NONE = 0, JA_HOP, JA_WALK, JA_FLOAT, JA_FACE, JA_STOP, JA_FRAME,
       JA_FLIP, JA_GLOW, JA_PARTICLE, JA_SAY, JA_WAIT,
       JA_SOUND, JA_BURST, JA_THROW, JA_EAT, JA_DROP, JA_SIGNAL, JA_SEEK, JA_FLY, JA_HOME,
       JA_BOOST, JA_NUDGE, JA_SHAKE, JA_KINDS };
enum { JP_SPARKLE = 0, JP_HEART, JP_NOTE, JP_ZZZ, JP_PUFF, JP_KINDS };

#define JIF_GIFT     0x01
#define JIF_BUILTIN  0x02
#define JIF_NEWER    0x80     /* decoded from a newer version: script dropped */

typedef struct { uint8_t event, earg, action, aarg; } JHabit;

typedef struct {
  uint8_t  version, kind, nframes, flags, nbub;
  uint8_t  move, speed, zone, nhab;
  uint32_t id;
  char     name[JI_NAME + 1];
  char     line[JI_LINE + 1];
  uint16_t pal[8];                       /* plain RGB565 */
  JHabit   hab[JI_MAX_HAB];
  char     bub[JI_MAX_BUB][JI_BUB + 1];
  int16_t  mem[JI_MEM];
  char     maker[JI_WHO + 1];
  uint32_t made;
  char     tags[JI_TAGS + 1];
  char     gifted[JI_WHO + 1];
  uint8_t  sig_len;
  uint8_t  sig[JI_SIG_MAX];
  uint16_t script_len;
  uint8_t  price5;                       /* the price in fives; 0 none given */
  uint8_t  script[JI_SCRIPT_MAX];
  uint8_t  frames[JI_MAX_FRAMES][JI_FRAME_BYTES];
} JItem;

/* Bytes by hand, through a volatile pointer: an app links no memset, and a
 * plain loop like this one is what the compiler likes to turn into a call
 * to it. */
static JI_OPT void ji_zero(void *p, int n) {
  volatile uint8_t *q = (volatile uint8_t *)p;
  while (n-- > 0) *q++ = 0;
}

static JI_OPT void ji_copy(void *d, const void *s, int n) {
  volatile uint8_t *q = (volatile uint8_t *)d;
  const uint8_t *r = (const uint8_t *)s;
  while (n-- > 0) *q++ = *r++;
}

/* The colour index of pixel i of a packed 3bpp picture. Reads two bytes, so a
 * picture's storage must run one byte past its last pixel's -- an item frame
 * never needs that (96 bytes end exactly on a pixel), because i < 256 keeps
 * the second byte inside unless i is in the last byte, and the last pixel's
 * bits sit wholly in byte 95. */
static JI_OPT int ji_px(const uint8_t *px, int i) {
  int bit = i * 3, b = bit >> 3, sh = bit & 7;
  unsigned v = px[b];
  if (sh > 5) v |= (unsigned)px[b + 1] << 8;
  return (int)((v >> sh) & 7);
}

static JI_OPT void ji_set_px(uint8_t *px, int i, int c) {
  int bit = i * 3, b = bit >> 3, sh = bit & 7;
  unsigned m = 7u << sh, v = (unsigned)(c & 7) << sh;
  px[b] = (uint8_t)((px[b] & ~m) | (v & 0xFF));
  if (sh > 5) px[b + 1] = (uint8_t)((px[b + 1] & ~(m >> 8)) | (v >> 8));
}

static JI_OPT void ji_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static JI_OPT void ji_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static JI_OPT unsigned ji_get16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static JI_OPT uint32_t ji_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A string into a fixed field, NUL-padded, cut to fit. */
static JI_OPT void ji_put_str(uint8_t *p, const char *s, int n) {
  int i = 0;
  for (; s && s[i] && i < n; i++) p[i] = (uint8_t)s[i];
  for (; i < n; i++) p[i] = 0;
}

/* A fixed field out into a NUL-terminated string: printable ASCII only, so
 * nothing from a stranger's record reaches the 6x8 font as a control code. */
static JI_OPT void ji_get_str(char *out, const uint8_t *p, int n) {
  int i;
  for (i = 0; i < n && p[i]; i++) out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
  out[i] = 0;
}

/* The size an item would encode to. */
static JI_OPT int jitem_size(const JItem *it) {
  return JI_HDR + it->sig_len + it->script_len + it->nframes * JI_FRAME_BYTES;
}

/* The record for `it` into `out`. Its length, or -1 if `it` breaks a limit
 * or does not fit in `cap`. Always writes the current version. */
static JI_OPT int jitem_encode(const JItem *it, uint8_t *out, int cap) {
  int n, i, o;
  if (it->kind >= JK_KINDS || it->nframes < 1 || it->nframes > JI_MAX_FRAMES) return -1;
  if (it->nbub > JI_MAX_BUB || it->nhab > JI_MAX_HAB) return -1;
  if (it->sig_len > JI_SIG_MAX || it->script_len > JI_SCRIPT_MAX) return -1;
  n = jitem_size(it);
  if (n > JI_MAX || n > cap) return -1;
  ji_zero(out, JI_HDR);
  out[0] = JI_VERSION;
  out[1] = JI_HDR;
  ji_put16(out + 2, (unsigned)n);
  ji_put32(out + 4, it->id);
  out[8] = it->kind;
  out[9] = it->nframes;
  out[10] = (uint8_t)(it->flags & (JIF_GIFT | JIF_BUILTIN));
  out[11] = it->nbub;
  ji_put_str(out + 12, it->name, JI_NAME);
  ji_put_str(out + 24, it->line, JI_LINE);
  for (i = 0; i < 8; i++) ji_put16(out + 56 + 2 * i, i ? it->pal[i] : 0);
  out[72] = it->move;
  out[73] = it->speed;
  out[74] = it->zone;
  out[75] = it->nhab;
  for (i = 0; i < it->nhab; i++) {
    out[76 + 4 * i] = it->hab[i].event;
    out[77 + 4 * i] = it->hab[i].earg;
    out[78 + 4 * i] = it->hab[i].action;
    out[79 + 4 * i] = it->hab[i].aarg;
  }
  for (i = 0; i < it->nbub; i++) ji_put_str(out + 88 + JI_BUB * i, it->bub[i], JI_BUB);
  for (i = 0; i < JI_MEM; i++) ji_put16(out + 136 + 2 * i, (unsigned)(uint16_t)it->mem[i]);
  ji_put_str(out + 152, it->maker, JI_WHO);
  ji_put32(out + 164, it->made);
  ji_put_str(out + 168, it->tags, JI_TAGS);
  ji_put_str(out + 192, it->gifted, JI_WHO);
  out[204] = it->sig_len;
  ji_put16(out + 205, it->script_len);
  out[207] = it->price5;
  o = JI_HDR;
  ji_copy(out + o, it->sig, it->sig_len);       o += it->sig_len;
  ji_copy(out + o, it->script, it->script_len); o += it->script_len;
  for (i = 0; i < it->nframes; i++, o += JI_FRAME_BYTES)
    ji_copy(out + o, it->frames[i], JI_FRAME_BYTES);
  return n;
}

/* `in` (n bytes) into `it`. 0, or a negative reason:
 *   -1 too short or too long   -2 the lengths disagree   -3 a bad count */
static JI_OPT int jitem_decode(JItem *it, const uint8_t *in, int n) {
  int hdr, total, i, k, o;
  ji_zero(it, (int)sizeof *it);
  if (n < JI_HDR || in[0] == 0) return -1;
  hdr = in[1];
  total = (int)ji_get16(in + 2);
  if (hdr < JI_HDR || total > JI_MAX || total > n || total < hdr) return -1;
  it->version = in[0];
  it->id = ji_get32(in + 4);
  it->kind = in[8];
  it->nframes = in[9];
  it->flags = (uint8_t)(in[10] & (JIF_GIFT | JIF_BUILTIN));
  it->nbub = in[11];
  if (it->kind >= JK_KINDS || it->nframes < 1 || it->nframes > JI_MAX_FRAMES ||
      it->nbub > JI_MAX_BUB || in[75] > JI_MAX_HAB || in[204] > JI_SIG_MAX ||
      ji_get16(in + 205) > JI_SCRIPT_MAX)
    return -3;
  it->sig_len = in[204];
  it->script_len = (uint16_t)ji_get16(in + 205);
  it->price5 = in[207];
  if (hdr + it->sig_len + it->script_len + it->nframes * JI_FRAME_BYTES != total) return -2;

  ji_get_str(it->name, in + 12, JI_NAME);
  ji_get_str(it->line, in + 24, JI_LINE);
  for (i = 0; i < 8; i++) it->pal[i] = (uint16_t)(i ? ji_get16(in + 56 + 2 * i) : 0);
  it->move = in[72] < JM_KINDS ? in[72] : JM_SITS;
  it->speed = in[73] < JSP_KINDS ? in[73] : JSP_MEDIUM;
  it->zone = in[74] < JZ_KINDS ? in[74] : JZ_ANYWHERE;
  for (i = 0, k = 0; i < in[75]; i++) {
    const uint8_t *h = in + 76 + 4 * i;
    if (h[0] >= JE_KINDS || h[2] >= JA_KINDS) continue;     /* not known here */
    it->hab[k].event = h[0];
    it->hab[k].earg = h[1];
    it->hab[k].action = h[2];
    it->hab[k].aarg = h[3];
    k++;
  }
  it->nhab = (uint8_t)k;
  for (i = 0; i < it->nbub; i++) ji_get_str(it->bub[i], in + 88 + JI_BUB * i, JI_BUB);
  for (i = 0; i < JI_MEM; i++) it->mem[i] = (int16_t)ji_get16(in + 136 + 2 * i);
  ji_get_str(it->maker, in + 152, JI_WHO);
  it->made = ji_get32(in + 164);
  ji_get_str(it->tags, in + 168, JI_TAGS);
  ji_get_str(it->gifted, in + 192, JI_WHO);

  o = hdr;
  ji_copy(it->sig, in + o, it->sig_len);
  o += it->sig_len;
  if (it->version > JI_VERSION) {
    it->script_len = 0;                 /* newer: the recipe takes over */
    it->flags |= JIF_NEWER;
  } else {
    ji_copy(it->script, in + o, it->script_len);
  }
  o += (int)ji_get16(in + 205);
  for (i = 0; i < it->nframes; i++, o += JI_FRAME_BYTES)
    ji_copy(it->frames[i], in + o, JI_FRAME_BYTES);
  return 0;
}

/* What the server signed: the record as it would be with no signature and
 * no memory -- the header with its signature length (offset 204) zeroed,
 * its eight memory slots (136..151, which scripts write, so a gift must
 * verify whatever they hold) zeroed, and its total length (offset 2) less
 * the signature; then everything after the signature. Fixed with
 * server/jar.py (2026-10-09). The signature itself is at
 * [hdr, hdr + in[204]). The message's length, or -1 if `in` is not a record
 * whose lengths agree or the message does not fit in `cap`. */
static JI_OPT int ji_signed_message(const uint8_t *in, int n, uint8_t *out, int cap) {
  int hdr, total, sig, rest;
  if (n < JI_HDR || in[0] == 0) return -1;
  hdr = in[1];
  total = (int)ji_get16(in + 2);
  sig = in[204];
  if (hdr < JI_HDR || total > n || total < hdr + sig || sig > JI_SIG_MAX) return -1;
  rest = total - hdr - sig;
  if (hdr + rest > cap) return -1;
  ji_copy(out, in, hdr);
  out[204] = 0;
  ji_zero(out + 136, 2 * JI_MEM);
  ji_put16(out + 2, (unsigned)(total - sig));
  ji_copy(out + hdr, in + hdr + sig, rest);
  return hdr + rest;
}

/* The display number, "No. 0042". */
static JI_OPT void jitem_number(uint32_t id, char *out) {
  int i;
  const char *p = "No. ";
  for (i = 0; p[i]; i++) out[i] = p[i];
  if (id > 99999999u) id = 99999999u;
  {
    char d[9];
    int n = 0;
    do { d[n++] = (char)('0' + id % 10); id /= 10; } while (id);
    while (n < 4) d[n++] = '0';
    while (n) out[i++] = d[--n];
  }
  out[i] = 0;
}

#endif /* CARDOS_JARITEM_H */
