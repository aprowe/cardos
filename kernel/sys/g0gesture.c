#include "kernel/sys/g0gesture.h"

#include <stdio.h>

int g0_press(G0Gesture *g, uint32_t now_ms) {
  /* Unsigned subtraction: right across the counter wrapping. */
  int memo = g->armed && (uint32_t)(now_ms - g->tap_up_ms) <= G0_GAP_MS;
  g->armed = 0;
  return memo ? G0_MEMO : G0_VOICE;
}

int g0_release(G0Gesture *g, uint32_t held_ms, uint32_t now_ms) {
  if (held_ms >= G0_TAP_MS) return 0;
  g->armed = 1;
  g->tap_up_ms = now_ms;
  return 1;
}

void memo_filename(char *out, size_t n, const char *dir, int have_time,
                   int month, int day, int hour, int min, int sec, int counter) {
  if (have_time)
    snprintf(out, n, "%s/%02d%02d-%02d%02d%02d.wav", dir, month, day, hour, min, sec);
  else
    snprintf(out, n, "%s/m%04d.wav", dir, counter);
}
