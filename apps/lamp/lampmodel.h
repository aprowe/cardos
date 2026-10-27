/* Lamp's remotes: what each model's buttons send. Portable; host-tested
 * (test/test_lampmodel.c).
 *
 * A model is text, the same for the ones built in and the ones a person
 * adds in /config/lamps.txt, so there is one reader:
 *
 *   model 24-key RGB remote     a new model, the rest of the line its name
 *   nec 0xEF00                  NEC with this address (<= 0xFF standard)
 *   necmsb                      or: each key's code is the old 32-bit form
 *   cols 4                      buttons a row, as on the remote
 *   key 03 On on                code, label, then a colour and/or a role
 *   key 04 Red #FF0000
 *   # a comment
 *
 * Codes are hex, with or without 0x. A label is one word, at most 7
 * characters (_ shows as a space). A colour makes the button a swatch. The
 * roles are what Lamp's own keys reach for whatever the layout: on, off,
 * power (a toggle), up and down (brightness).
 */
#ifndef CARDOS_LAMPMODEL_H
#define CARDOS_LAMPMODEL_H

#include <stdint.h>

#if defined(__GNUC__)
#define LM_OPT __attribute__((unused))
#else
#define LM_OPT
#endif

#define LM_KEYS    48
#define LM_LABEL   8
#define LM_NAME    28

#define LM_NEC      1         /* CAPP_IR_NEC */
#define LM_NEC_MSB  2         /* CAPP_IR_NEC_MSB */

enum { LM_ROLE_NONE, LM_ROLE_ON, LM_ROLE_OFF, LM_ROLE_POWER, LM_ROLE_UP, LM_ROLE_DOWN };

typedef struct {
  uint32_t cmd;
  uint8_t  r, g, b;
  uint8_t  swatch;            /* it had a colour */
  uint8_t  role;
  char     label[LM_LABEL];
} LampKey;

typedef struct {
  char     name[LM_NAME];
  uint8_t  proto;
  uint8_t  cols;
  uint8_t  n;
  uint16_t addr;
  LampKey  key[LM_KEYS];
} LampModel;

/* ---- the remotes built in ----
 *
 * The two that come with nearly every cheap RGB strip, bulb and lamp.
 * Codes are the ones the remotes send (checked against the grids at
 * revk.uk, "IR LED controllers"); colours approximate the swatches printed
 * on them. */
static const char LM_BUILTIN[] =
  "model 24-key RGB remote\n"
  "nec 0xEF00\ncols 4\n"
  "key 00 Brt+ up\nkey 01 Brt- down\nkey 02 Off off\nkey 03 On on\n"
  "key 04 Red #FF0000\nkey 05 Green #00FF00\nkey 06 Blue #0000FF\nkey 07 White #FFFFFF\n"
  "key 08 . #FF5000\nkey 09 . #00FF60\nkey 0A . #4040FF\nkey 0B Flash\n"
  "key 0C . #FF8000\nkey 0D . #00FFC0\nkey 0E . #8000FF\nkey 0F Strobe\n"
  "key 10 . #FFB000\nkey 11 . #00C0FF\nkey 12 . #C000FF\nkey 13 Fade\n"
  "key 14 . #FFFF00\nkey 15 . #0080FF\nkey 16 . #FF00FF\nkey 17 Smooth\n"
  "\n"
  "model 44-key RGB remote\n"
  "nec 0x00\ncols 4\n"
  "key 5C Brt+ up\nkey 5D Brt- down\nkey 41 Play\nkey 40 Power power\n"
  "key 58 Red #FF0000\nkey 59 Green #00FF00\nkey 45 Blue #0000FF\nkey 44 White #FFFFFF\n"
  "key 54 . #FF5000\nkey 55 . #00FF60\nkey 49 . #4040FF\nkey 48 . #FFC8C8\n"
  "key 50 . #FF8000\nkey 51 . #00FFC0\nkey 4D . #8000FF\nkey 4C . #FFD8E8\n"
  "key 1C . #FFB000\nkey 1D . #00C0FF\nkey 1E . #C000FF\nkey 1F . #C8E0FF\n"
  "key 18 . #FFFF00\nkey 19 . #0080FF\nkey 1A . #FF00FF\nkey 1B . #C8FFFF\n"
  "key 14 Red+\nkey 15 Green+\nkey 16 Blue+\nkey 17 Quick\n"
  "key 10 Red-\nkey 11 Green-\nkey 12 Blue-\nkey 13 Slow\n"
  "key 0C DIY1\nkey 0D DIY2\nkey 0E DIY3\nkey 0F Auto\n"
  "key 08 DIY4\nkey 09 DIY5\nkey 0A DIY6\nkey 0B Flash\n"
  "key 04 Jump3\nkey 05 Jump7\nkey 06 Fade3\nkey 07 Fade7\n";

/* ---- reading ---- */

static LM_OPT int lm_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* The next word of [*p, end) into w (cut at n-1); its length, 0 at the end. */
static LM_OPT int lm_word(const char **p, const char *end, char *w, int n) {
  const char *s = *p;
  int k = 0;
  while (s < end && lm_space(*s)) s++;
  while (s < end && !lm_space(*s)) {
    if (k < n - 1) w[k] = *s;
    k++;
    s++;
  }
  w[k < n - 1 ? k : n - 1] = 0;
  *p = s;
  return k;
}

static LM_OPT int lm_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* Hex, with or without 0x, all of it; -1 if it is not. Up to 32 bits. */
static LM_OPT int lm_hex(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int k = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (; *s; s++, k++) {
    int d = *s >= '0' && *s <= '9' ? *s - '0'
          : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
          : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
    if (d < 0 || k >= 8) return -1;
    v = v << 4 | (uint32_t)d;
  }
  if (!k) return -1;
  *out = v;
  return 0;
}

static LM_OPT int lm_role(const char *w) {
  if (lm_eq(w, "on")) return LM_ROLE_ON;
  if (lm_eq(w, "off")) return LM_ROLE_OFF;
  if (lm_eq(w, "power")) return LM_ROLE_POWER;
  if (lm_eq(w, "up")) return LM_ROLE_UP;
  if (lm_eq(w, "down")) return LM_ROLE_DOWN;
  return -1;
}

/* One `key` line's words after "key"; 0, or -1 if it is not one. */
static LM_OPT int lm_key(LampModel *m, const char **p, const char *end) {
  char w[16];
  LampKey *k;
  uint32_t v;
  int i, len;
  if (m->n >= LM_KEYS || !lm_word(p, end, w, sizeof w) || lm_hex(w, &v)) return -1;
  if (m->proto == LM_NEC && v > 0xFF) return -1;
  k = &m->key[m->n];
  k->cmd = v;
  k->swatch = 0;
  k->role = LM_ROLE_NONE;
  k->r = k->g = k->b = 0;
  len = lm_word(p, end, w, sizeof w);
  if (!len || len > LM_LABEL - 1) return -1;
  for (i = 0; i < LM_LABEL; i++) k->label[i] = w[i] == '_' ? ' ' : w[i];
  if (k->label[0] == '.' && !k->label[1]) k->label[0] = 0;   /* a swatch alone */
  while ((len = lm_word(p, end, w, sizeof w)) != 0) {
    int role = lm_role(w);
    if (w[0] == '#') {
      if (len != 7 || lm_hex(w + 1, &v)) return -1;
      k->r = (uint8_t)(v >> 16); k->g = (uint8_t)(v >> 8); k->b = (uint8_t)v;
      k->swatch = 1;
    } else if (role > 0) {
      k->role = (uint8_t)role;
    } else {
      return -1;
    }
  }
  m->n++;
  return 0;
}

/* Read `text` (n bytes) into out[*count..max), adding to *count. Lines it
 * cannot read are skipped; the first one's number (from 1) is in *bad, 0
 * when there were none. A model with no keys is dropped. */
static LM_OPT void lm_parse(const char *text, int n, LampModel *out, int *count,
                            int max, int *bad) {
  const char *p = text, *end = text + n;
  LampModel *m = NULL;
  int line = 0, full = 0;
  *bad = 0;
  while (p < end) {
    const char *eol = p, *q;
    char w[16];
    int ok = 1;
    while (eol < end && *eol != '\n') eol++;
    line++;
    q = p;
    if (lm_word(&q, eol, w, sizeof w) && w[0] != '#') {
      if (lm_eq(w, "model")) {
        int k = 0;
        if (m && m->n) (*count)++;
        m = NULL;
        full = *count >= max;
        while (q < eol && lm_space(*q)) q++;
        if (*count < max && q < eol) {
          m = &out[*count];
          while (q < eol && k < LM_NAME - 1 && *q != '\r') m->name[k++] = *q++;
          while (k && lm_space(m->name[k - 1])) k--;
          m->name[k] = 0;
          m->proto = LM_NEC;
          m->addr = 0;
          m->cols = 4;
          m->n = 0;
        } else {
          ok = full;                   /* full: skipped, not wrong */
        }
      } else if (full) {
        ok = 1;
      } else if (!m) {
        ok = 0;
      } else if (lm_eq(w, "nec")) {
        uint32_t a = 0;
        ok = lm_word(&q, eol, w, sizeof w) && !lm_hex(w, &a) && a <= 0xFFFF && !m->n;
        if (ok) { m->proto = LM_NEC; m->addr = (uint16_t)a; }
      } else if (lm_eq(w, "necmsb")) {
        ok = !m->n;
        if (ok) { m->proto = LM_NEC_MSB; m->addr = 0; }
      } else if (lm_eq(w, "cols")) {
        ok = lm_word(&q, eol, w, sizeof w) == 1 && w[0] >= '1' && w[0] <= '6';
        if (ok) m->cols = (uint8_t)(w[0] - '0');
      } else if (lm_eq(w, "key")) {
        ok = lm_key(m, &q, eol) == 0;
      } else {
        ok = 0;
      }
      if (!ok && !*bad) *bad = line;
    }
    p = eol < end ? eol + 1 : end;
  }
  if (m && m->n) (*count)++;
}

/* The first key of `role` in m, or -1. */
static LM_OPT int lm_find_role(const LampModel *m, int role) {
  int i;
  for (i = 0; i < m->n; i++)
    if (m->key[i].role == role) return i;
  return -1;
}

/* ---- mapping a lamp by hand ----
 *
 * When a lamp answers a remote's address but its buttons are laid out
 * differently, Lamp sends each code in turn and the person picks what the
 * lamp did from these. What they picked becomes a model of their own. */

typedef struct {
  const char *name;            /* shown while choosing */
  const char *label;           /* on the button; "." a swatch alone */
  uint8_t r, g, b, swatch, role;
} LampOpt;

#define LM_OPT_MODE  5
#define LM_OPT_NONE  6
#define LM_NOPTS     21

static const LampOpt LM_OPTS[LM_NOPTS] = {
  { "On", "On", 0, 0, 0, 0, LM_ROLE_ON },
  { "Off", "Off", 0, 0, 0, 0, LM_ROLE_OFF },
  { "Power (on/off)", "Power", 0, 0, 0, 0, LM_ROLE_POWER },
  { "Brighter", "Brt+", 0, 0, 0, 0, LM_ROLE_UP },
  { "Dimmer", "Brt-", 0, 0, 0, 0, LM_ROLE_DOWN },
  { "A mode (flash, fade...)", "Mode", 0, 0, 0, 0, 0 },
  { "Nothing happened", "", 0, 0, 0, 0, 0 },
  { "Red", ".", 255, 0, 0, 1, 0 },
  { "Orange", ".", 255, 96, 0, 1, 0 },
  { "Amber", ".", 255, 160, 0, 1, 0 },
  { "Yellow", ".", 255, 240, 0, 1, 0 },
  { "Lime", ".", 160, 255, 0, 1, 0 },
  { "Green", ".", 0, 255, 0, 1, 0 },
  { "Mint", ".", 0, 255, 140, 1, 0 },
  { "Cyan", ".", 0, 255, 255, 1, 0 },
  { "Sky blue", ".", 0, 140, 255, 1, 0 },
  { "Blue", ".", 0, 0, 255, 1, 0 },
  { "Purple", ".", 140, 0, 255, 1, 0 },
  { "Magenta", ".", 255, 0, 255, 1, 0 },
  { "Pink", ".", 255, 110, 180, 1, 0 },
  { "White", "White", 255, 255, 255, 1, 0 },
};

typedef struct { char *p; int left; } LmOut;

static LM_OPT void lm_put(LmOut *o, const char *s) {
  while (*s) {
    if (o->left > 1) { *o->p++ = *s; o->left--; }
    else o->left = 0;
    s++;
  }
  if (o->left > 0) *o->p = 0;
}

static LM_OPT void lm_put_hex(LmOut *o, uint32_t v, int digits) {
  char h[9];
  int i;
  for (i = digits - 1; i >= 0; i--, v >>= 4) h[i] = "0123456789ABCDEF"[v & 15];
  h[digits] = 0;
  lm_put(o, h);
}

static LM_OPT void lm_put_dec(LmOut *o, int v) {
  char d[12];
  int i = 11;
  d[i] = 0;
  do { d[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
  lm_put(o, d + i);
}

/* A model named `name` on NEC address `addr`, from what each code was
 * picked as: pick[code] is an LM_OPTS index + 1, or 0 for not tried.
 * Buttons go in the order of LM_OPTS -- the switches first, then the
 * colours -- and by code within one; "nothing" is left out, and so is
 * anything past LM_KEYS. The length written, or -1 if it did not fit. */
static LM_OPT int lm_map_text(const char *name, uint16_t addr, const uint8_t pick[256],
                              char *out, int max) {
  LmOut o;
  int opt, c, keys = 0, modes = 0;
  o.p = out;
  o.left = max;
  if (max > 0) out[0] = 0;
  lm_put(&o, "model ");
  lm_put(&o, name);
  lm_put(&o, "\nnec 0x");
  lm_put_hex(&o, addr, addr > 0xFF ? 4 : 2);
  lm_put(&o, "\ncols 4\n");
  for (opt = 0; opt < LM_NOPTS; opt++) {
    const LampOpt *q = &LM_OPTS[opt];
    if (opt == LM_OPT_NONE) continue;
    for (c = 0; c < 256 && keys < LM_KEYS; c++) {
      if (pick[c] != opt + 1) continue;
      lm_put(&o, "key ");
      lm_put_hex(&o, (uint32_t)c, 2);
      lm_put(&o, " ");
      lm_put(&o, q->label);
      if (opt == LM_OPT_MODE) lm_put_dec(&o, ++modes);
      if (q->swatch) {
        lm_put(&o, " #");
        lm_put_hex(&o, (uint32_t)q->r << 16 | (uint32_t)q->g << 8 | q->b, 6);
      }
      if (q->role) {
        static const char *const R[] = { "", "on", "off", "power", "up", "down" };
        lm_put(&o, " ");
        lm_put(&o, R[q->role]);
      }
      lm_put(&o, "\n");
      keys++;
    }
  }
  return o.left > 0 ? max - o.left : -1;
}

/* Is the line [p, eol) "model NAME"? */
static LM_OPT int lm_is_model(const char *p, const char *eol, const char *name) {
  const char *q = p;
  char w[8];
  int k = 0;
  if (!lm_word(&q, eol, w, sizeof w) || !lm_eq(w, "model")) return 0;
  while (q < eol && lm_space(*q)) q++;
  while (eol > q && lm_space(eol[-1])) eol--;
  while (q < eol && name[k] && *q == name[k]) { q++; k++; }
  return q == eol && !name[k];
}

/* The file `old` (n bytes) with the model called `name` taken out and
 * `block` added at the end, into out. The length, or -1 if it did not fit. */
static LM_OPT int lm_replace(const char *old, int n, const char *name, const char *block,
                             char *out, int max) {
  const char *p = old, *end = old + n;
  int skip = 0, k = 0, i;
  while (p < end) {
    const char *eol = p, *nx;
    while (eol < end && *eol != '\n') eol++;
    nx = eol < end ? eol + 1 : end;
    {
      const char *q = p;
      char w[8];
      if (lm_word(&q, eol, w, sizeof w) && lm_eq(w, "model")) skip = lm_is_model(p, eol, name);
    }
    if (!skip) {
      for (; p < nx; p++) {
        if (k >= max - 1) return -1;
        out[k++] = *p;
      }
      if (nx == end && eol == end && k && out[k - 1] != '\n') {
        if (k >= max - 1) return -1;
        out[k++] = '\n';
      }
    }
    p = nx;
  }
  for (i = 0; block[i]; i++) {
    if (k >= max - 1) return -1;
    out[k++] = block[i];
  }
  out[k] = 0;
  return k;
}

#endif /* CARDOS_LAMPMODEL_H */
