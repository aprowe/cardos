/* The real Vim app (apps/vim/vim.c) painted into a framebuffer with the
 * real 6x8 font.
 *
 * Two things are checked. A full paint writes every pixel of the screen --
 * there is no framebuffer on the device, so a pixel nobody writes is
 * whatever was there before. And after each key, painting only what the app
 * marked as damaged leaves exactly the screen a full paint would: a row the
 * app forgot to mark is a stale row on the panel, and this finds it.
 *
 * VIM_DUMP=dir writes the screens there (vim_*.ppm), to look at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "kernel/console/font6x8.h"

#define capp_info vim_capp_info
#define capp_main vim_capp_main
#include "apps/vim/vim.c"
#undef capp_info
#undef capp_main

#define SW 240
#define SH 135
#define UNSET 0xF81F

static uint16_t fb[SH][SW], fb2[SH][SW];
static uint16_t (*tgt)[SW] = fb;
static CappUi VUI;
static CardApi VAPI;
static const CRect VWIN = { 0, 0, SW, SH };
static CRect s_clip;
static CRect s_dmg;
static int s_ndmg;

static void put(int x, int y, uint16_t c) {
  if (x < s_clip.x || y < s_clip.y || x >= s_clip.x + s_clip.w || y >= s_clip.y + s_clip.h) return;
  if (x >= 0 && y >= 0 && x < SW && y < SH) tgt[y][x] = c;
}
static void v_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++) for (x = r.x; x < r.x + r.w; x++) put(x, y, c);
}
static void v_frame(CRect r, uint16_t c) {
  v_fill(capp_rect(r.x, r.y, r.w, 1), c);
  v_fill(capp_rect(r.x, r.y + r.h - 1, r.w, 1), c);
  v_fill(capp_rect(r.x, r.y, 1, r.h), c);
  v_fill(capp_rect(r.x + r.w - 1, r.y, 1, r.h), c);
}
static void v_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 6) {
    int ch = (unsigned char)*s, i, j;
    const uint8_t *g = font6x8[(ch >= 32 && ch <= 126 ? ch : '?') - 32];
    for (i = 0; i < 6; i++)
      for (j = 0; j < 8; j++) put(x + i, y + j, (g[i] >> j) & 1 ? fg : bg);
  }
}
static void v_ui(const CappUi *ui) { VUI = *ui; }
static CRect v_area(void) { return s_clip; }
static void v_damage(CRect r) {
  if (!s_ndmg) s_dmg = r;
  else {
    int x0 = r.x < s_dmg.x ? r.x : s_dmg.x, y0 = r.y < s_dmg.y ? r.y : s_dmg.y;
    int x1 = r.x + r.w > s_dmg.x + s_dmg.w ? r.x + r.w : s_dmg.x + s_dmg.w;
    int y1 = r.y + r.h > s_dmg.y + s_dmg.h ? r.y + r.h : s_dmg.y + s_dmg.h;
    s_dmg = capp_rect(x0, y0, x1 - x0, y1 - y0);
  }
  s_ndmg++;
}
static int s_pick_mode = -1;
static int v_pick(const CappPick *p) { s_pick_mode = p->mode; return 0; }

static void full_paint(uint16_t (*into)[SW]) {
  int x, y;
  tgt = into;
  for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) into[y][x] = UNSET;
  s_clip = VWIN;
  VUI.paint(VUI.state, VWIN);
  tgt = fb;
}

static int unset(void) {
  int x, y, n = 0;
  for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) n += fb[y][x] == UNSET;
  return n;
}

static void dump(const char *what) {
  const char *dir = getenv("VIM_DUMP");
  char f[512];
  FILE *o;
  int x, y;
  if (!dir) return;
  snprintf(f, sizeof f, "%s/vim_%s.ppm", dir, what);
  if (!(o = fopen(f, "wb"))) return;
  fprintf(o, "P6 %d %d 255\n", SW, SH);
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++) {
      uint16_t c = (uint16_t)(fb[y][x] >> 8 | fb[y][x] << 8);
      fputc((c >> 11) << 3, o); fputc(((c >> 5) & 63) << 2, o); fputc((c & 31) << 3, o);
    }
  fclose(o);
}

/* A key, then a paint of only the damage over the last screen; it must be
 * the screen a full paint makes. The number of rows of pixels repainted. */
static int s_bad;
static int key_and_check(int k) {
  int x, y, rows;
  s_ndmg = 0;
  VUI.key(VUI.state, (uint8_t)k);
  if (VUI.tick) VUI.tick(VUI.state, 0);
  s_clip = s_ndmg ? s_dmg : VWIN;
  rows = s_clip.h;
  tgt = fb;
  VUI.paint(VUI.state, VWIN);
  full_paint(fb2);
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++)
      if (fb[y][x] != fb2[y][x]) { s_bad++; y = SH; break; }
  return rows;
}

static void keys(const char *s) { while (*s) key_and_check((uint8_t)*s++); }

static int s_room = -1;                /* big-mode room, when not fakeapi's */
static void boot(const char *path, const char *text) {
  fakeapi_init(&VAPI);
  fakefs_mem(&VAPI);
  if (s_room >= 0) fakeapi_big_room = s_room;
  VAPI.fill = v_fill; VAPI.frame = v_frame; VAPI.text = v_text; VAPI.ui = v_ui;
  VAPI.paint_area = v_area; VAPI.damage = v_damage; VAPI.pick = v_pick;
  if (text) fakefs_put(path, text);
  memset(&V, 0, sizeof V);
  s_picking = 0;
  s_bigblock = 0;
  {
    char *argv[2];
    argv[0] = "vim";
    argv[1] = (char *)path;
    vim_capp_main(&VAPI, path ? 2 : 1, argv);
  }
  full_paint(fb);
  s_bad = 0;
}

static const char *C_SRC =
  "#include <stdio.h>\n"
  "\n"
  "/* A comment that\n"
  "   runs on. */\n"
  "static int count = 42;\n"
  "\n"
  "int main(void) {\n"
  "\tfor (int i = 0; i < count; i++)\n"
  "    printf(\"%d\\n\", i); // print\n"
  "  return 0;\n"
  "}\n";

void test_vim_paint_a_c_file_fills_the_screen(void) {
  boot("/home/hello.c", C_SRC);
  CHECK(s_bigblock);                             /* fakeapi gives big-mode room */
  CHECK(V.t.cap > 14 * 1024);
  CHECK_EQ(unset(), 0);
  dump("c");
  /* The keyword "static" (line 5) is in the keyword colour, the comment in
   * the comment colour, the number in the number colour. */
  {
    int y = 4 * ROWH + 3, x, kw = 0, com = 0, num = 0;
    for (x = 0; x < SW; x++) { kw += fb[y][x] == C_KW; num += fb[y][x] == C_NUM; }
    for (x = 0; x < SW; x++) com += fb[3 * ROWH + 3][x] == C_COM;
    CHECK(kw > 0);
    CHECK(num > 0);
    CHECK(com > 0);
  }
  /* The status line names the file and the mode. */
  CHECK(fb[13 * ROWH + 4][2] == C_HEAD || fb[13 * ROWH + 4][3] == C_HEAD);
}

void test_vim_paint_damage_repaints_what_changed(void) {
  int r;
  boot("/home/hello.c", C_SRC);
  /* A move marks the rows it touched and the bars, not "everything"; the
   * shell unions marks into one rectangle, which may still be tall. */
  key_and_check('j');
  CHECK(s_ndmg > 0);
  r = key_and_check('l');
  CHECK(s_ndmg > 0);
  keys("jjwwe");
  keys("ihello ");
  r = key_and_check(VK_ESC);
  CHECK(s_ndmg > 0);
  (void)r;
  keys("x.u");
  keys("ggVjj");
  keys("d");
  keys("u");
  keys("/count\r");
  keys("n");
  keys("A /* x\x1b");                             /* opens a comment: rows below change */
  keys("u");
  keys("Go");
  keys("pri");
  key_and_check(VCTRL('n'));
  dump("complete");
  keys("\x1b");
  keys(":set nonu\r");
  keys(":%s/count/n/g\r");
  keys("G");
  keys("o1\r2\r3\r4\r5\r6\r7\r8\r9\x1b");
  keys("gg");
  key_and_check(VCTRL('d'));
  keys("vjl");
  dump("visual");
  keys("\x1b");
  keys(":");
  dump("cmdline");
  keys("\x1b");
  CHECK_EQ(s_bad, 0);
  CHECK_EQ(unset(), 0);
}

void test_vim_paint_opens_the_picker_with_no_file(void) {
  s_pick_mode = -1;
  boot(0, 0);
  CHECK_EQ(s_pick_mode, CAPP_PICK_OPEN);
  CHECK(s_picking);
  CHECK(!VUI.wants_text(VUI.state));
  CHECK_EQ(unset(), 0);
  dump("empty");
}

void test_vim_paint_write_with_no_name_asks_and_q_closes(void) {
  boot("/home/n.txt", "abc");
  keys(":q\r");
  CHECK_EQ(s_pick_mode, CAPP_PICK_OPEN);           /* closed: the picker for the next */
  CHECK_EQ(V.path[0], 0);
  s_picking = 0;
  s_pick_mode = -1;
  keys("ihi\x1b:w\r");
  CHECK_EQ(s_pick_mode, CAPP_PICK_SAVE);
  CHECK_EQ(s_bad, 0);
}

/* The small buffer, when big mode gives nothing. */
void test_vim_paint_without_big_mode(void) {
  s_room = 0;
  boot("/home/hello.c", C_SRC);
  s_room = -1;
  CHECK(!s_bigblock);
  CHECK_EQ(V.t.cap, POOL_TEXT);
  CHECK(vb_len(&V.t) > 0);
}
