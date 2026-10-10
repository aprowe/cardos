/* ovltest -- data overlays, checked on the device.
 *
 * Fills overlay 0, switches to 1 and finds it zeroed, fills it, switches
 * back and checks every byte of 0, switches again and checks 1, then
 * switches with DISCARD and finds 1 zeroed the next time. Says pass, with
 * the two switch times, or the step that failed.
 *
 * Also a fixture: tools/test_build_apps.py checks the build's overlay rules
 * on it, and test/test_capprel.c relocates its constants (STEP is a table
 * of pointers among the constants, so it has .rela.rodata).
 * docs/superpowers/specs/2026-10-10-app-data-overlays-design.md */

#include "kernel/app/capp.h"

#define BIG   20480     /* overlay 0: the size the spec times */
#define SMALL 6000      /* overlay 1: smaller, so a save at the wrong size shows */

static const CardApi *api;

CAPP_OVERLAY(0) static uint8_t big[BIG];
CAPP_OVERLAY(1) static uint8_t small[SMALL];

static const char *const STEP[] = {
  "start", "fill 0", "0 -> 1", "1 zeroed", "fill 1", "1 -> 0", "0 back",
  "0 -> 1 again", "1 back", "1 -> 0 discard", "0 still", "0 -> 1 after discard",
  "1 zeroed again", "out of range", "same overlay", "live",
};

static uint8_t pat(uint32_t i, int k) { return (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8)); }

static void fill(uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) p[i] = pat(i, k);
}

static int same(const uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i] != pat(i, k)) return 0;
  return 1;
}

static int zero(const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

static int fail(int step) {
  char line[64];
  api->fmt(line, sizeof line, "ovltest: FAIL at %s (live %d)", STEP[step], api->overlay_live());
  api->out_line(line);
  return 1;
}

/* One switch, timed; 0 when it worked. */
static int sw(int n, int flags, uint32_t *ms) {
  uint32_t t = api->ticks_ms();
  int r = api->overlay(n, flags);
  if (ms) *ms = api->ticks_ms() - t;
  return r;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_CLI,
  "ovltest",
  { 0 },
  NULL,
};

int capp_main(const CardApi *a, int argc, char **argv) {
  uint32_t t01 = 0, t10 = 0;
  char line[96];
  (void)argc; (void)argv;
  api = a;
  if (api->overlay_live() != 0) return fail(0);
  fill(big, BIG, 0);
  if (sw(1, CAPP_OVL_KEEP, &t01) != 0) return fail(2);
  if (!zero(small, SMALL)) return fail(3);
  fill(small, SMALL, 1);
  if (sw(0, CAPP_OVL_KEEP, &t10) != 0) return fail(5);
  if (!same(big, BIG, 0)) return fail(6);
  if (sw(1, CAPP_OVL_KEEP, NULL) != 0) return fail(7);
  if (!same(small, SMALL, 1)) return fail(8);
  if (sw(0, CAPP_OVL_DISCARD, NULL) != 0) return fail(9);
  if (!same(big, BIG, 0)) return fail(10);
  if (sw(1, CAPP_OVL_KEEP, NULL) != 0) return fail(11);
  if (!zero(small, SMALL)) return fail(12);
  if (api->overlay(4, CAPP_OVL_KEEP) != -1 || api->overlay(-1, CAPP_OVL_KEEP) != -1) return fail(13);
  if (api->overlay(1, CAPP_OVL_KEEP) != 0) return fail(14);
  if (api->overlay_live() != 1) return fail(15);
  api->fmt(line, sizeof line, "ovltest: pass; 0->1 %u ms, 1->0 %u ms (%u and %u bytes)",
           (unsigned)t01, (unsigned)t10, (unsigned)BIG, (unsigned)SMALL);
  api->out_line(line);
  return 0;
}
