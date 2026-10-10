/* The flash cache of relocated app code, over a fake flash that behaves
 * like the real one: programming only clears bits, erases are whole
 * sectors, and a write can be made to fail as a pulled battery would. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/xipcache.h"

#define FSIZE (128u * XIP_SECTOR)
static uint8_t s_flash[FSIZE];
static int s_bad_writes;     /* a write that needed a 0 -> 1 */
static int s_fail_after;     /* writes left before the power goes; -1 never */

static int f_read(void *ctx, uint32_t off, void *buf, uint32_t n) {
  (void)ctx;
  if (off > FSIZE || n > FSIZE - off) return -1;
  memcpy(buf, s_flash + off, n);
  return 0;
}
static int f_write(void *ctx, uint32_t off, const void *buf, uint32_t n) {
  const uint8_t *b = buf;
  uint32_t i;
  (void)ctx;
  if (off > FSIZE || n > FSIZE - off) return -1;
  if (s_fail_after == 0) return -1;
  if (s_fail_after > 0) s_fail_after--;
  for (i = 0; i < n; i++) {
    if (b[i] & (uint8_t)~s_flash[off + i]) s_bad_writes++;
    s_flash[off + i] &= b[i];
  }
  return 0;
}
static int f_erase(void *ctx, uint32_t off, uint32_t n) {
  (void)ctx;
  if (off % XIP_SECTOR || n % XIP_SECTOR || off > FSIZE || n > FSIZE - off) return -1;
  memset(s_flash + off, 0xFF, n);
  return 0;
}

static void reset(XipCache *c) {
  XipFlash f = { f_read, f_write, f_erase, NULL, FSIZE };
  memset(s_flash, 0xFF, FSIZE);
  s_bad_writes = 0;
  s_fail_after = -1;
  CHECK_EQ(xip_open(c, &f), XIP_OK);
}
static void reboot(XipCache *c) {
  XipFlash f = { f_read, f_write, f_erase, NULL, FSIZE };
  s_fail_after = -1;
  CHECK_EQ(xip_open(c, &f), XIP_OK);
}

static XipKey key(const char *path, uint32_t size, uint32_t mtime) {
  XipKey k;
  memset(&k, 0, sizeof k);
  k.path_hash = xip_path_hash(path);
  k.file_size = size + 1000;
  k.file_mtime = mtime;
  k.api = 36;
  k.code_size = size;
  k.map_base = 0x42400000u;
  k.arena = 0x3FC9A000u;
  return k;
}

/* As the loader does it: begin, the code, commit. The entry offset, or a
 * negative XIP_ERR. */
static long install(XipCache *c, const char *path, uint32_t size, uint32_t mtime, uint8_t fill) {
  static uint8_t buf[96 * 1024];
  XipKey k = key(path, size, mtime);
  uint32_t off;
  int r;
  memset(buf, fill, size);
  if ((r = xip_begin(c, size, &off)) != XIP_OK) return r;
  if ((r = xip_write(c, 0, buf, size)) != XIP_OK) { xip_abandon(c); return r; }
  if ((r = xip_commit(c, &k, xip_crc32(0, buf, size), path)) != XIP_OK) return r;
  return (long)off;
}
static int found(XipCache *c, const char *path, uint32_t size, uint32_t mtime, uint32_t *off) {
  XipKey k = key(path, size, mtime);
  uint32_t o;
  int r = xip_find(c, &k, off ? off : &o);
  return r == XIP_OK;
}

#define JAR 24480u                  /* 7 sectors with the header */

void test_xip_crc32_is_the_usual_one(void) {
  CHECK_EQ(xip_crc32(0, "123456789", 9), 0xCBF43926u);
  CHECK_EQ(xip_crc32(xip_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

void test_xip_path_hash_folds_case(void) {
  CHECK_EQ(xip_path_hash("/apps/Games/Jar.capp"), xip_path_hash("/APPS/games/jar.CAPP"));
  CHECK(xip_path_hash("/apps/jar.capp") != xip_path_hash("/apps/jab.capp"));
}

void test_xip_empty_flash_finds_nothing(void) {
  XipCache c;
  reset(&c);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK_EQ(c.head, 0);
}

void test_xip_finds_what_it_wrote_after_a_reboot(void) {
  XipCache c;
  uint32_t off = 99;
  reset(&c);
  CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x5A), 0);
  CHECK(found(&c, "/apps/jar.capp", JAR, 1, &off));
  CHECK_EQ(off, 0);
  reboot(&c);
  CHECK(found(&c, "/apps/jar.capp", JAR, 1, &off));
  CHECK_EQ(off, 0);
  CHECK_EQ(s_flash[XIP_HDR + 100], 0x5A);
  CHECK_EQ(s_bad_writes, 0);
}

/* A new firmware moves the arena: the entry must not be used. */
void test_xip_another_key_misses(void) {
  XipCache c;
  XipKey k;
  uint32_t off;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 2, NULL));        /* changed file */
  CHECK(!found(&c, "/apps/jab.capp", JAR, 1, NULL));        /* another file */
  k = key("/apps/jar.capp", JAR, 1);
  k.arena += 16;
  CHECK_EQ(xip_find(&c, &k, &off), XIP_MISS);
  k = key("/apps/jar.capp", JAR, 1);
  k.map_base += XIP_SECTOR * 16;
  CHECK_EQ(xip_find(&c, &k, &off), XIP_MISS);
}

static int s_live, s_dead;
static void count(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  (void)off; (void)in_use; (void)ctx;
  if (h->live == 0xFFFFFFFFu) s_live++; else s_dead++;
}

void test_xip_rewrite_kills_the_old_entry(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  install(&c, "/apps/todo.capp", 15044, 1, 0x22);
  install(&c, "/apps/jar.capp", JAR, 2, 0x33);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(found(&c, "/apps/jar.capp", JAR, 2, NULL));
  CHECK(found(&c, "/apps/todo.capp", 15044, 1, NULL));
  s_live = s_dead = 0;
  xip_each(&c, count, NULL);
  CHECK_EQ(s_live, 2);
  CHECK_EQ(s_dead, 1);
  CHECK_EQ(s_bad_writes, 0);
}

void test_xip_a_bad_crc_is_a_miss(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  s_flash[XIP_HDR + 5000] = 0x00;
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
}

/* The battery pulled after each write in turn: code, header, commit. */
void test_xip_power_cut_leaves_nothing_half_valid(void) {
  int n;
  for (n = 0; n < 3; n++) {
    XipCache c;
    reset(&c);
    s_fail_after = n;
    CHECK(install(&c, "/apps/jar.capp", JAR, 1, 0x5A) < 0);
    reboot(&c);
    CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
    CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x5A) >= 0, 1);
    CHECK(found(&c, "/apps/jar.capp", JAR, 1, NULL));
    CHECK_EQ(s_bad_writes, 0);
  }
}

void test_xip_head_survives_a_reboot(void) {
  XipCache c;
  reset(&c);
  install(&c, "/a.capp", JAR, 1, 1);
  install(&c, "/b.capp", JAR, 1, 2);
  install(&c, "/c.capp", JAR, 1, 3);
  reboot(&c);
  CHECK_EQ(install(&c, "/d.capp", JAR, 1, 4), 21 * (long)XIP_SECTOR);
}

/* 18 entries of 7 sectors fill 126 of 128; the 19th wraps to 0 and takes
 * only the first entry with it. */
void test_xip_wraps_and_evicts_the_oldest(void) {
  XipCache c;
  char name[16];
  int i;
  reset(&c);
  for (i = 0; i < 18; i++) {
    snprintf(name, sizeof name, "/%02d.capp", i);
    CHECK_EQ(install(&c, name, JAR, 1, (uint8_t)i), (long)i * 7 * (long)XIP_SECTOR);
  }
  CHECK_EQ(install(&c, "/new.capp", JAR, 1, 0x77), 0);
  CHECK(!found(&c, "/00.capp", JAR, 1, NULL));
  CHECK(found(&c, "/01.capp", JAR, 1, NULL));
  CHECK(found(&c, "/17.capp", JAR, 1, NULL));
  CHECK(found(&c, "/new.capp", JAR, 1, NULL));
}

void test_xip_never_erases_an_entry_in_use(void) {
  XipCache c;
  char name[16];
  uint32_t off;
  int i;
  reset(&c);
  for (i = 0; i < 18; i++) {
    snprintf(name, sizeof name, "/%02d.capp", i);
    install(&c, name, JAR, 1, (uint8_t)i);
  }
  CHECK_EQ(xip_ref(&c, 0), XIP_OK);                 /* /00 is running */
  CHECK_EQ(xip_begin(&c, JAR, &off), XIP_ERR_BUSY);
  CHECK(found(&c, "/00.capp", JAR, 1, NULL));
  CHECK_EQ(xip_wipe(&c), XIP_ERR_BUSY);
  xip_unref(&c, 0);
  CHECK_EQ(xip_begin(&c, JAR, &off), XIP_OK);
  CHECK_EQ(off, 0);
  xip_abandon(&c);
}

/* Jar updated on the card while it runs from flash: the new version kills
 * the old entry, but the code under the running app is not touched. */
void test_xip_a_killed_entry_in_use_keeps_its_code(void) {
  XipCache c;
  uint8_t before[JAR];
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  memcpy(before, s_flash + XIP_HDR, JAR);
  CHECK_EQ(xip_ref(&c, 0), XIP_OK);
  CHECK(install(&c, "/apps/jar.capp", JAR, 2, 0x22) > 0);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(memcmp(before, s_flash + XIP_HDR, JAR) == 0);
  xip_unref(&c, 0);
}

/* The card changed under the cache (same size, same 1980 mtime): forget. */
void test_xip_forget_kills_by_path(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  install(&c, "/apps/todo.capp", 15044, 1, 0x22);
  CHECK_EQ(xip_forget(&c, xip_path_hash("/APPS/JAR.CAPP")), XIP_OK);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(found(&c, "/apps/todo.capp", 15044, 1, NULL));
}

void test_xip_too_big_is_full(void) {
  XipCache c;
  uint32_t off;
  reset(&c);
  CHECK_EQ(xip_begin(&c, FSIZE, &off), XIP_ERR_FULL);
  CHECK_EQ(xip_begin(&c, 0, &off), XIP_ERR_FULL);
}

void test_xip_one_write_at_a_time(void) {
  XipCache c;
  uint32_t off;
  XipKey k = key("/a.capp", 100, 1);
  reset(&c);
  CHECK_EQ(xip_write(&c, 0, "x", 1), XIP_ERR_STATE);              /* nothing begun */
  CHECK_EQ(xip_commit(&c, &k, 0, "/a.capp"), XIP_ERR_STATE);
  CHECK_EQ(xip_begin(&c, 100, &off), XIP_OK);
  CHECK_EQ(xip_begin(&c, 100, &off), XIP_ERR_STATE);
  CHECK_EQ(xip_write(&c, XIP_SECTOR, "x", 1), XIP_ERR_STATE);     /* past the entry */
  xip_abandon(&c);
}

void test_xip_wipe(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  CHECK_EQ(xip_wipe(&c), XIP_OK);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x11), 0);
}

/* Many laps of mixed sizes: every live entry the walk reports must still
 * check, i.e. no erase ever cut an entry and left its header standing. */
static XipCache *s_soak;
static int s_soak_bad;
static void soak_check(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  uint32_t got;
  (void)in_use; (void)ctx;
  if (h->live != 0xFFFFFFFFu) return;
  if (xip_find(s_soak, &h->key, &got) != XIP_OK || got != off) s_soak_bad++;
}
void test_xip_soak_every_live_entry_checks(void) {
  XipCache c;
  char name[16];
  uint32_t seed = 12345;
  int i;
  reset(&c);
  for (i = 0; i < 400; i++) {
    uint32_t size;
    seed = seed * 1103515245u + 12345u;
    size = 4 + ((seed >> 8) % (40u * 1024u)) / 4 * 4;
    snprintf(name, sizeof name, "/%02u.capp", (unsigned)((seed >> 4) % 30));
    CHECK(install(&c, name, size, (uint32_t)i, (uint8_t)i) >= 0);
  }
  s_soak = &c;
  s_soak_bad = 0;
  xip_each(&c, soak_check, NULL);
  CHECK_EQ(s_soak_bad, 0);
  CHECK_EQ(s_bad_writes, 0);
}

/* What the loader does after a commit: read the code back. A byte that
 * did not program the way it was asked is caught before anything runs it. */
void test_xip_verify_reads_the_code_back(void) {
  XipCache c;
  long off;
  reset(&c);
  off = install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  CHECK(off >= 0);
  CHECK_EQ(xip_verify(&c, (uint32_t)off), XIP_OK);
  s_flash[(uint32_t)off + XIP_HDR + 1234] ^= 0x01;
  CHECK(xip_verify(&c, (uint32_t)off) != XIP_OK);
  CHECK(xip_verify(&c, (uint32_t)off + XIP_SECTOR) != XIP_OK);   /* no entry there */
}

void test_xip_kill_makes_find_miss(void) {
  XipCache c;
  long off;
  reset(&c);
  off = install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  CHECK(install(&c, "/apps/todo.capp", 15044, 1, 0x11) > 0);
  CHECK(off >= 0);
  CHECK_EQ(xip_kill(&c, (uint32_t)off), XIP_OK);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(found(&c, "/apps/todo.capp", 15044, 1, NULL));
  CHECK_EQ(s_bad_writes, 0);
}

/* Once the commit word is down the new entry is valid; a write that fails
 * while killing the old one does not make the commit a failure. The old
 * entry stays live, and the next commit or forget tries again. */
void test_xip_commit_stands_when_the_kill_walk_fails(void) {
  XipCache c;
  reset(&c);
  CHECK(install(&c, "/apps/jar.capp", JAR, 1, 0x5A) >= 0);
  s_fail_after = 3;                 /* code, header, commit; then the kill */
  CHECK(install(&c, "/apps/jar.capp", JAR, 2, 0x22) >= 0);
  s_fail_after = -1;
  CHECK(found(&c, "/apps/jar.capp", JAR, 2, NULL));
  reboot(&c);
  CHECK(found(&c, "/apps/jar.capp", JAR, 2, NULL));
}
