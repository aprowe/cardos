/* Relocating an app's code a window at a time must give exactly the bytes
 * relocating it whole does -- the flash cache writes code through a 28 KB
 * window -- and every absolute reference must land in the half it names.
 * Fixtures are real .capps from tools/build_apps.py. */
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/capprel.h"

typedef struct {
  uint8_t  *file;
  long      size;
  uint32_t  code_off, code_size, data_off, data_size;
  CappRela *code_rel, *data_rel;
  uint32_t  ncode_rel, ndata_rel;
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

/* Just enough ELF: the two allocated sections (by link address, as the
 * loader does) and the RELA sections aimed at them. */
static int load_capp(const char *name, Capp *c) {
  char path[512];
  FILE *fp;
  uint32_t shoff, i;
  uint16_t shentsize, shnum;
  int code_sec = -1, data_sec = -1;
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
    if (addr >= CAPP_DATA_ORIGIN) { if (data_sec < 0) { data_sec = (int)i; c->data_off = u32(s + 16); c->data_size = size; } }
    else if (code_sec < 0) { code_sec = (int)i; c->code_off = u32(s + 16); c->code_size = size; }
  }
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    if (u32(s + 4) != 4) continue;                       /* SHT_RELA */
    if ((int)u32(s + 28) == code_sec) {
      c->code_rel = (CappRela *)(c->file + u32(s + 16)); c->ncode_rel = u32(s + 20) / 12;
    } else if ((int)u32(s + 28) == data_sec) {
      c->data_rel = (CappRela *)(c->file + u32(s + 16)); c->ndata_rel = u32(s + 20) / 12;
    }
  }
  return code_sec >= 0 && data_sec >= 0 ? 0 : -1;
}

#define CODE_BASE 0x42480080u
#define DATA_BASE 0x3FC9A000u

static const char *FIXTURES[] = { "capp_jar.capp", "capp_forklift.capp", "capp_cat.capp" };

void test_capprel_windows_match_whole_image(void) {
  static const uint32_t WINDOWS[] = { 4, 8, 12, 4096, 28672 };
  size_t f, w;
  for (f = 0; f < sizeof FIXTURES / sizeof *FIXTURES; f++) {
    Capp c;
    uint8_t *whole, *win;
    uint32_t o;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    CHECK_EQ(c.code_size % 4, 0);
    whole = malloc(c.code_size); win = malloc(c.code_size);
    memcpy(whole, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(whole, 0, c.code_size, c.code_size, 1, c.code_rel, c.ncode_rel,
                           CODE_BASE, DATA_BASE), 0);
    if (c.ncode_rel > 0) CHECK(memcmp(whole, c.file + c.code_off, c.code_size) != 0);   /* it did something */
    for (w = 0; w < sizeof WINDOWS / sizeof *WINDOWS; w++) {
      memcpy(win, c.file + c.code_off, c.code_size);
      for (o = 0; o < c.code_size; o += WINDOWS[w]) {
        uint32_t n = c.code_size - o < WINDOWS[w] ? c.code_size - o : WINDOWS[w];
        CHECK_EQ(capprel_apply(win + o, o, n, c.code_size, 1, c.code_rel, c.ncode_rel,
                               CODE_BASE, DATA_BASE), 0);
      }
      CHECK(memcmp(win, whole, c.code_size) == 0);
    }
    free(whole); free(win); free(c.file);
  }
}

/* The premise of the arena: code holds data addresses. */
void test_capprel_refs_land_in_the_right_half(void) {
  size_t f;
  for (f = 0; f < sizeof FIXTURES / sizeof *FIXTURES; f++) {
    Capp c;
    uint8_t *code;
    uint32_t j, to_data = 0;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    code = malloc(c.code_size);
    memcpy(code, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(code, 0, c.code_size, c.code_size, 1, c.code_rel, c.ncode_rel,
                           CODE_BASE, DATA_BASE), 0);
    for (j = 0; j < c.ncode_rel; j++) {
      uint32_t v;
      if ((c.code_rel[j].r_info & 0xFF) != R_XTENSA_32) continue;
      v = u32(code + c.code_rel[j].r_offset);
      CHECK((v >= CODE_BASE && v <= CODE_BASE + c.code_size) ||
            (v >= DATA_BASE && v <= DATA_BASE + c.data_size));
      if (v >= DATA_BASE && v <= DATA_BASE + c.data_size) to_data++;
    }
    if (strcmp(FIXTURES[f], "capp_cat.capp") != 0) CHECK(to_data > 0);
    free(code); free(c.file);
  }
}

void test_capprel_refuses_what_it_cannot_apply(void) {
  uint8_t buf[64];
  CappRela r;
  memset(buf, 0, sizeof buf);
  r.r_addend = 0;
  r.r_offset = 8;  r.r_info = 5;                 /* a type the loader does not know */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_info = R_XTENSA_32; r.r_offset = 64;       /* past the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 62;                               /* runs off the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 6;                                /* unaligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = CAPP_DATA_ORIGIN - 2;             /* wraps to near 2^32 in data */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, 0, 0), -1);
  r.r_offset = 60;                               /* outside the window, still checked */
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 1, &r, 1, 0, 0), 0);
  r.r_offset = 64;
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 0;
  CHECK_EQ(capprel_apply(buf, 2, 32, 64, 1, &r, 1, 0, 0), -1);   /* window not aligned */
}

void test_capprel_leaves_pc_relative_alone(void) {
  uint8_t buf[16], before[16];
  CappRela r[3];
  int i;
  for (i = 0; i < 16; i++) buf[i] = before[i] = (uint8_t)(i * 13);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_SLOT0_OP; r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_NONE;     r[1].r_addend = 0;
  r[2].r_offset = 8; r[2].r_info = R_XTENSA_ASM_EXPAND; r[2].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 1, r, 3, 0x1000, 0x2000), 0);
  CHECK(memcmp(buf, before, 16) == 0);
}

void test_capprel_adds_the_right_base(void) {
  uint8_t buf[8];
  uint32_t a = 0x40, b = CAPP_DATA_ORIGIN + 0x10, v;
  CappRela r[2];
  memcpy(buf, &a, 4); memcpy(buf + 4, &b, 4);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_32; r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_32; r[1].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 8, 8, 1, r, 2, CODE_BASE, DATA_BASE), 0);
  memcpy(&v, buf, 4);     CHECK_EQ(v, CODE_BASE + 0x40);
  memcpy(&v, buf + 4, 4); CHECK_EQ(v, DATA_BASE + 0x10);
}
