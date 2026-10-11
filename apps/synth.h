/* Sounds an app makes for itself: notes into a WAV file on the card, made
 * once, played with api->audio(). The API plays files and has nothing that
 * makes a tone, so this is how Timer's beep, Iron's and Game Night's sounds
 * and the jar's are made. 8 kHz mono, 16-bit.
 *
 * A note is a tone that dies away over its length -- a triangle, a square,
 * a buzzer (two squares a fifth apart), noise, or a rest (hz 0, not noise)
 * -- with a short fade at each end so it does not click.
 *
 * synth_ensure(api, path, notes, n) makes the file if it is missing or the
 * wrong size, and says whether it is there. Header-only, static. */
#ifndef CARDOS_SYNTH_H
#define CARDOS_SYNTH_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define SY_OPT __attribute__((unused))
#else
#define SY_OPT
#endif

#define SYNTH_RATE 8000u
enum { SY_TRI = 0, SY_SQR, SY_NOISE, SY_BUZZ };
typedef struct { uint16_t hz, ms; uint8_t vol, wave; } SyNote;

static SY_OPT int synth_samples(const SyNote *n, int count) {
  int i, s = 0;
  for (i = 0; i < count; i++) s += (int)(n[i].ms * SYNTH_RATE / 1000u);
  return s;
}

static SY_OPT int synth_ms(const SyNote *n, int count) {
  int i, ms = 0;
  for (i = 0; i < count; i++) ms += n[i].ms;
  return ms;
}

static SY_OPT void synth_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static SY_OPT void synth_write(const CardApi *api, const char *path, const SyNote *s, int count) {
  uint8_t hdr[44];
  int16_t buf[128];
  int fd, i, nb = 0;
  uint32_t seed = 0x13579BDu, bytes = (uint32_t)synth_samples(s, count) * 2u;
  fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  api->mem_set(hdr, 0, sizeof hdr);
  api->mem_cpy(hdr, "RIFF", 4);
  synth_put32(hdr + 4, 36u + bytes);
  api->mem_cpy(hdr + 8, "WAVEfmt ", 8);
  synth_put32(hdr + 16, 16);
  hdr[20] = 1; hdr[22] = 1;
  synth_put32(hdr + 24, SYNTH_RATE);
  synth_put32(hdr + 28, SYNTH_RATE * 2u);
  hdr[32] = 2; hdr[34] = 16;
  api->mem_cpy(hdr + 36, "data", 4);
  synth_put32(hdr + 40, bytes);
  api->write(fd, hdr, sizeof hdr);
  for (i = 0; i < count; i++) {
    const SyNote *nt = &s[i];
    int n = (int)(nt->ms * SYNTH_RATE / 1000u), j;
    uint32_t ph = 0, ph2 = 0, step = (uint32_t)nt->hz * 65536u / SYNTH_RATE;
    for (j = 0; j < n; j++) {
      int amp = nt->vol * 300, v = 0;
      if (nt->wave != SY_BUZZ) amp = amp * (n - j) / n;
      if (j < 24) amp = amp * j / 24;
      if (n - j < 24) amp = amp * (n - j) / 24;
      if (nt->wave == SY_NOISE) {
        seed = seed * 1664525u + 1013904223u;
        v = ((int)((seed >> 16) & 0x7FFFu) - 16384) * amp / 16384;
      } else if (nt->hz) {
        uint32_t p = ph & 0xFFFFu, q = ph2 & 0xFFFFu;
        if (nt->wave == SY_BUZZ) {
          v = (p < 32768u ? amp : -amp) + (q < 32768u ? amp : -amp) / 2;
          if (v > 30000) v = 30000;
          if (v < -30000) v = -30000;
          ph2 += step * 3u / 2u;
        } else if (nt->wave == SY_SQR) {
          v = p < 32768u ? amp : -amp;
        } else {
          v = ((p < 32768u ? (int)p - 16384 : 49152 - (int)p) * amp) / 16384;
        }
        ph += step;
      }
      buf[nb++] = (int16_t)v;
      if (nb == 128) { api->write(fd, buf, sizeof buf); nb = 0; }
    }
  }
  if (nb) api->write(fd, buf, (size_t)nb * 2);
  api->close(fd);
}

/* The file there and whole, made if it is not: 1 if it can be played. */
static SY_OPT int synth_ensure(const CardApi *api, const char *path, const SyNote *n, int count) {
  CappStat st;
  uint32_t want = 44u + (uint32_t)synth_samples(n, count) * 2u;
  if (api->stat(path, &st) != 0 || st.is_dir || st.size != want) synth_write(api, path, n, count);
  return api->stat(path, &st) == 0 && !st.is_dir && st.size == want;
}

#endif
