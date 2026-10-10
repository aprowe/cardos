/* Switching data overlays, over files held in memory: keep and discard, a
 * first visit zeroed, a switch back exact, saves at the outgoing overlay's
 * own size, failures that change nothing or say so, and files that go with
 * the app and never collide between two loads. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/ovl.h"

#define NFILES 8
typedef struct { int used; char path[OVL_PATH_MAX]; uint32_t n; uint8_t data[8192]; } MemFile;
static MemFile s_files[NFILES];
static int s_save_fail, s_load_fail, s_saves, s_loads;

static MemFile *find(const char *path) {
  int i;
  for (i = 0; i < NFILES; i++) if (s_files[i].used && !strcmp(s_files[i].path, path)) return &s_files[i];
  return NULL;
}
static int m_save(void *ctx, const char *path, const void *buf, uint32_t n) {
  MemFile *f = find(path);
  int i;
  (void)ctx;
  if (s_save_fail || n > sizeof f->data) return -1;
  for (i = 0; !f && i < NFILES; i++) if (!s_files[i].used) f = &s_files[i];
  if (!f) return -1;
  f->used = 1;
  snprintf(f->path, sizeof f->path, "%s", path);
  memcpy(f->data, buf, n);
  f->n = n;
  s_saves++;
  return 0;
}
static int m_load(void *ctx, const char *path, void *buf, uint32_t n) {
  MemFile *f = find(path);
  (void)ctx;
  if (s_load_fail || !f || f->n != n) return -1;
  memcpy(buf, f->data, n);
  s_loads++;
  return 0;
}
static void m_remove(void *ctx, const char *path) {
  MemFile *f = find(path);
  (void)ctx;
  if (f) f->used = 0;
}
static const OvlIo IO = { m_save, m_load, m_remove, NULL };

static int nfiles(void) {
  int i, k = 0;
  for (i = 0; i < NFILES; i++) k += s_files[i].used;
  return k;
}
static uint32_t file_size(uint32_t load, int n) {
  char p[OVL_PATH_MAX];
  MemFile *f;
  ovl_path(p, sizeof p, load, n);
  f = find(p);
  return f ? f->n : 0;
}

static uint8_t s_region[4096], s_region2[4096];
static const uint32_t SIZES[CAPP_OVL_MAX] = { 4096, 1000, 0, 0 };

static void fresh(Ovl *o, uint8_t *region, uint32_t load) {
  memset(region, 0, 4096);
  ovl_init(o, region, SIZES, 2, load);
}
static void reset(void) {
  memset(s_files, 0, sizeof s_files);
  s_save_fail = s_load_fail = s_saves = s_loads = 0;
}
static void fill(uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) p[i] = (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8));
}
static int same(const uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i] != (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8))) return 0;
  return 1;
}
static int zero(const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

void test_ovl_starts_with_0_live_and_none_without_overlays(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_live(&o), 0);
  ovl_init(&o, s_region, SIZES, 0, 2);
  CHECK_EQ(ovl_live(&o), -1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), -1);
  CHECK_EQ(nfiles(), 0);
}

void test_ovl_a_first_visit_is_zeroed(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  memset(s_region, 0xAA, sizeof s_region);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_live(&o), 1);
  CHECK(zero(s_region, 1000));
  CHECK_EQ(s_loads, 0);                          /* nothing to read: no card asked */
}

void test_ovl_a_switch_back_restores_exactly(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 1000, 1));
}

void test_ovl_saves_at_its_own_size(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(file_size(1, 0), 4096);
  CHECK_EQ(file_size(1, 1), 1000);
}

void test_ovl_discard_drops_the_live_one(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);      /* 1 has a file now */
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 1000, 1));
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(file_size(1, 1), 0);                            /* its file is gone */
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(zero(s_region, 1000));
}

void test_ovl_discard_needs_no_card(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  s_save_fail = 1;
  s_load_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(ovl_live(&o), 1);
}

void test_ovl_a_write_failure_leaves_the_live_one_untouched(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  s_save_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(nfiles(), 0);
}

void test_ovl_a_read_failure_zeroes_and_reports(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  fill(s_region, 4096, 0);
  s_load_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 1);
  CHECK(zero(s_region, 1000));
}

void test_ovl_switching_to_the_live_one_is_a_noop(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(s_saves, 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 2, CAPP_OVL_KEEP), -1);     /* past the declared ones */
  CHECK_EQ(ovl_switch(&o, &IO, -1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_switch(&o, &IO, CAPP_OVL_MAX, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 0);
}

void test_ovl_drop_deletes_the_files(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(nfiles(), 2);
  ovl_drop(&o, &IO);
  CHECK_EQ(nfiles(), 0);
  CHECK_EQ(ovl_live(&o), -1);
}

/* The same app on screen and headless at once: two loads, two sets of files. */
void test_ovl_two_loads_never_share_files(void) {
  Ovl a, b;
  char p1[OVL_PATH_MAX], p2[OVL_PATH_MAX];
  reset();
  fresh(&a, s_region, 1);
  fresh(&b, s_region2, 12);
  fill(s_region, 4096, 3);
  fill(s_region2, 4096, 4);
  CHECK_EQ(ovl_switch(&a, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&b, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(nfiles(), 2);
  ovl_drop(&a, &IO);
  CHECK_EQ(nfiles(), 1);
  CHECK_EQ(ovl_switch(&b, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region2, 4096, 4));
  ovl_path(p1, sizeof p1, 1, 1);
  ovl_path(p2, sizeof p2, 11, 1);
  CHECK(strcmp(p1, p2) != 0);
  CHECK(strcmp(p1, "/cache/ovl/1-1.bin") == 0);
}
