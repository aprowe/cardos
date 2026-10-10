/* Relocating an app a window at a time must give exactly the bytes
 * relocating it whole does -- the flash cache writes code and constants
 * through a 28 KB window -- and every absolute reference must land in the
 * part it names: code, data or constants. Fixtures are real .capps from
 * tools/build_apps.py. */
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/capprel.h"

typedef struct {
  uint8_t  *file;
  long      size;
  uint32_t  code_off, code_size, data_off, data_size, ro_off, ro_size;
  uint32_t  block;          /* the data block: .data and anything after it, overlays too */
  CappRela *code_rel, *data_rel, *ro_rel;
  uint32_t  ncode_rel, ndata_rel, nro_rel;
} Capp;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void fixture_path(char *out, size_t cap, const char *name) {
  const char *f = __FILE__, *cut = strrchr(f, '/');
  const char *bs = strrchr(f, '\\');
  if (bs && (!cut || bs > cut)) cut = bs;
  if (cut) snprintf(out, cap, "%.*sfixtures/%s", (int)(cut - f + 1), f, name);
  else snprintf(out, cap, "test/fixtures/%s", name);
}

/* Just enough ELF: the three parts, by link address as the loader classifies
 * them, and the RELA sections aimed at each. */
static int load_capp(const char *name, Capp *c) {
  char path[512];
  FILE *fp;
  uint32_t shoff, i;
  uint16_t shentsize, shnum;
  int code_sec = -1, data_sec = -1, ro_sec = -1;
  memset(c, 0, sizeof *c);
  fixture_path(path, sizeof path, name);
  if ((fp = fopen(path, "rb")) == NULL) return -1;
  fseek(fp, 0, SEEK_END); c->size = ftell(fp); fseek(fp, 0, SEEK_SET);
  c->file = malloc((size_t)c->size);
  if (fread(c->file, 1, (size_t)c->size, fp) != (size_t)c->size) { fclose(fp); return -1; }
  fclose(fp);
  shoff = u32(c->file + 0x20); shentsize = u16(c->file + 0x2E); shnum = u16(c->file + 0x30);
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    uint32_t type = u32(s + 4), flags = u32(s + 8), addr = u32(s + 12), size = u32(s + 20);
    if (!(flags & 2) || (type != 1 && type != 8) || size == 0) continue;
    if (addr >= CAPP_RODATA_ORIGIN) {
      if (ro_sec < 0 && type == 1) { ro_sec = (int)i; c->ro_off = u32(s + 16); c->ro_size = size; }
    } else if (addr >= CAPP_DATA_ORIGIN) {
      if (addr - CAPP_DATA_ORIGIN + size > c->block) c->block = addr - CAPP_DATA_ORIGIN + size;
      if (data_sec < 0 && type == 1) { data_sec = (int)i; c->data_off = u32(s + 16); c->data_size = size; }
    } else if (code_sec < 0) {
      code_sec = (int)i; c->code_off = u32(s + 16); c->code_size = size;
    }
  }
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    int to = (int)u32(s + 28);
    if (u32(s + 4) != 4) continue;                       /* SHT_RELA */
    if (to == code_sec) { c->code_rel = (CappRela *)(c->file + u32(s + 16)); c->ncode_rel = u32(s + 20) / 12; }
    else if (to == data_sec) { c->data_rel = (CappRela *)(c->file + u32(s + 16)); c->ndata_rel = u32(s + 20) / 12; }
    else if (ro_sec >= 0 && to == ro_sec) { c->ro_rel = (CappRela *)(c->file + u32(s + 16)); c->nro_rel = u32(s + 20) / 12; }
  }
  return code_sec >= 0 && data_sec >= 0 ? 0 : -1;
}

#define CODE_BASE 0x42480080u
#define DATA_BASE 0x3FC9A000u
#define RO_BASE   0x3C4800A0u

static const CappBases BASES = { CODE_BASE, DATA_BASE, RO_BASE };

static const char *FIXTURES[] = { "capp_jar.capp", "capp_forklift.capp", "capp_cat.capp",
                                  "capp_ovltest.capp" };
#define NFIX (sizeof FIXTURES / sizeof *FIXTURES)

/* One section relocated whole, then in windows of every awkward size: the
 * same bytes. */
static void check_windows(const uint8_t *src, uint32_t size, uint32_t origin,
                          const CappRela *rel, uint32_t nrel) {
  static const uint32_t WINDOWS[] = { 4, 8, 12, 4096, 28672 };
  uint8_t *whole = malloc(size), *win = malloc(size);
  size_t w;
  uint32_t o;
  memcpy(whole, src, size);
  CHECK_EQ(capprel_apply(whole, 0, size, size, origin, rel, nrel, &BASES), 0);
  if (nrel > 0) CHECK(memcmp(whole, src, size) != 0);      /* it did something */
  for (w = 0; w < sizeof WINDOWS / sizeof *WINDOWS; w++) {
    memcpy(win, src, size);
    for (o = 0; o < size; o += WINDOWS[w]) {
      uint32_t n = size - o < WINDOWS[w] ? size - o : WINDOWS[w];
      CHECK_EQ(capprel_apply(win + o, o, n, size, origin, rel, nrel, &BASES), 0);
    }
    CHECK(memcmp(win, whole, size) == 0);
  }
  free(whole); free(win);
}

void test_capprel_windows_match_whole_image(void) {
  size_t f;
  for (f = 0; f < NFIX; f++) {
    Capp c;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    CHECK_EQ(c.code_size % 4, 0);
    check_windows(c.file + c.code_off, c.code_size, 0, c.code_rel, c.ncode_rel);
    if (c.ro_size) {
      CHECK_EQ(c.ro_size % 4, 0);                          /* capp.ld's ALIGN(4) */
      check_windows(c.file + c.ro_off, c.ro_size, CAPP_RODATA_ORIGIN, c.ro_rel, c.nro_rel);
    }
    free(c.file);
  }
}

static int in(uint32_t v, uint32_t base, uint32_t size) { return v >= base && v <= base + size; }

/* The premise of the arena and of the flash cache: code and constants hold
 * addresses of all three parts, and each moves by its own base. */
void test_capprel_refs_land_in_the_right_part(void) {
  size_t f;
  for (f = 0; f < NFIX; f++) {
    Capp c;
    uint8_t *code, *ro = NULL;
    uint32_t j, to_data = 0, to_ro = 0;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    code = malloc(c.code_size);
    memcpy(code, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(code, 0, c.code_size, c.code_size, 0, c.code_rel, c.ncode_rel, &BASES), 0);
    if (c.ro_size) {
      ro = malloc(c.ro_size);
      memcpy(ro, c.file + c.ro_off, c.ro_size);
      CHECK_EQ(capprel_apply(ro, 0, c.ro_size, c.ro_size, CAPP_RODATA_ORIGIN, c.ro_rel, c.nro_rel,
                             &BASES), 0);
    }
    for (j = 0; j < c.ncode_rel + c.nro_rel; j++) {
      const CappRela *r = j < c.ncode_rel ? &c.code_rel[j] : &c.ro_rel[j - c.ncode_rel];
      uint32_t v;
      if ((r->r_info & 0xFF) != R_XTENSA_32) continue;
      v = j < c.ncode_rel ? u32(code + r->r_offset) : u32(ro + (r->r_offset - CAPP_RODATA_ORIGIN));
      CHECK(in(v, CODE_BASE, c.code_size) || in(v, DATA_BASE, c.block) || in(v, RO_BASE, c.ro_size));
      if (in(v, DATA_BASE, c.block)) to_data++;
      if (in(v, RO_BASE, c.ro_size)) to_ro++;
    }
    if (strcmp(FIXTURES[f], "capp_cat.capp") != 0) { CHECK(to_data > 0); CHECK(to_ro > 0); }
    free(code); free(ro); free(c.file);
  }
}

/* Constants hold pointers -- action tables, string tables, const CappUi --
 * so a third relocation section exists and must be applied. */
void test_capprel_constants_carry_relocations(void) {
  Capp c;
  CHECK_EQ(load_capp("capp_jar.capp", &c), 0);
  CHECK(c.ro_size > 8u * 1024u);
  CHECK(c.nro_rel > 0);
  free(c.file);
}

void test_capprel_refuses_what_it_cannot_apply(void) {
  uint8_t buf[64];
  CappRela r;
  memset(buf, 0, sizeof buf);
  r.r_addend = 0;
  r.r_offset = 8;  r.r_info = 5;                 /* a type the loader does not know */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_info = R_XTENSA_32; r.r_offset = 64;       /* past the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 62;                               /* runs off the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 6;                                /* unaligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = CAPP_DATA_ORIGIN - 2;             /* below the data origin */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, CAPP_DATA_ORIGIN, &r, 1, &BASES), -1);
  r.r_offset = CAPP_DATA_ORIGIN + 4;             /* a data offset aimed at the constants */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, CAPP_RODATA_ORIGIN, &r, 1, &BASES), -1);
  r.r_offset = 60;                               /* outside the window, still checked */
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 0, &r, 1, &BASES), 0);
  r.r_offset = 64;
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 0;
  CHECK_EQ(capprel_apply(buf, 2, 32, 64, 0, &r, 1, &BASES), -1);   /* window not aligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, NULL), -1);     /* no bases */
}

void test_capprel_leaves_pc_relative_alone(void) {
  static const CappBases B = { 0x1000, 0x2000, 0x3000 };
  uint8_t buf[16], before[16];
  CappRela r[3];
  int i;
  for (i = 0; i < 16; i++) buf[i] = before[i] = (uint8_t)(i * 13);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_SLOT0_OP;   r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_NONE;       r[1].r_addend = 0;
  r[2].r_offset = 8; r[2].r_info = R_XTENSA_ASM_EXPAND; r[2].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 0, r, 3, &B), 0);
  CHECK(memcmp(buf, before, 16) == 0);
}

void test_capprel_adds_the_right_base(void) {
  uint8_t buf[16];
  uint32_t vals[4] = { 0x40, CAPP_DATA_ORIGIN + 0x10, CAPP_RODATA_ORIGIN + 0x20,
                       CAPP_RODATA_ORIGIN - 4 };              /* the last data word */
  uint32_t v;
  CappRela r[4];
  int i;
  memcpy(buf, vals, sizeof vals);
  for (i = 0; i < 4; i++) { r[i].r_offset = (uint32_t)(i * 4); r[i].r_info = R_XTENSA_32; r[i].r_addend = 0; }
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 0, r, 4, &BASES), 0);
  memcpy(&v, buf, 4);      CHECK_EQ(v, CODE_BASE + 0x40);
  memcpy(&v, buf + 4, 4);  CHECK_EQ(v, DATA_BASE + 0x10);
  memcpy(&v, buf + 8, 4);  CHECK_EQ(v, RO_BASE + 0x20);
  memcpy(&v, buf + 12, 4); CHECK_EQ(v, DATA_BASE + (CAPP_RODATA_ORIGIN - 4 - CAPP_DATA_ORIGIN));
}

/* Relocations into the constants themselves: offsets carry 0x20000000. */
void test_capprel_relocates_inside_the_constants(void) {
  uint8_t buf[8];
  uint32_t a = CAPP_DATA_ORIGIN + 8, v;
  CappRela r;
  memset(buf, 0, sizeof buf);
  memcpy(buf + 4, &a, 4);
  r.r_offset = CAPP_RODATA_ORIGIN + 4; r.r_info = R_XTENSA_32; r.r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 8, 8, CAPP_RODATA_ORIGIN, &r, 1, &BASES), 0);
  memcpy(&v, buf + 4, 4);
  CHECK_EQ(v, DATA_BASE + 8);
}
