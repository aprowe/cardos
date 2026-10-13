/* The real Chat (apps/chat/chat.c) painted into a framebuffer: a dog emoji
 * that arrives is a picture four rows tall where the text would be, the
 * picker sits over the bottom of the log, and between them every pixel of
 * the screen is written by a full paint -- there is no framebuffer on the
 * device, so a pixel nobody writes is whatever was there before.
 *
 * CHAT_DUMP=dir writes the screens there (chat_*.ppm), to look at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info chat_capp_info
#define capp_main chat_capp_main
#include "apps/chat/chat.c"

#define SW 240
#define SH 135
#define UNSET 0xF81F                  /* magenta: nobody drew here */

static uint16_t fb[SH][SW];
static CappUi CUI;
static CardApi CAPI;
static const CRect CWIN = { 0, 0, SW, SH };
static const char *s_feed;            /* what the next GET answers */
static char s_posted[256];

static void put(int x, int y, uint16_t c) { if (x >= 0 && y >= 0 && x < SW && y < SH) fb[y][x] = c; }
static void c_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++) for (x = r.x; x < r.x + r.w; x++) put(x, y, c);
}
static void c_pixels(CRect r, const uint16_t *p) {
  int x, y;
  for (y = 0; y < r.h; y++) for (x = 0; x < r.w; x++) put(r.x + x, r.y + y, p[y * r.w + x]);
}
/* Text as blocks: a 6x8 cell of fg for a letter, bg for a space. */
static void c_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 6) {
    int i, j;
    for (j = 0; j < 8; j++)
      for (i = 0; i < 6; i++) put(x + i, y + j, *s != ' ' && i < 5 && j < 7 && j > 0 ? fg : bg);
  }
}
static void c_ui(const CappUi *ui) { CUI = *ui; }
static CRect c_area(void) { return CWIN; }
static void c_damage(CRect r) { (void)r; }
static int c_start(const char *method, const char *url, const char *body, const char *ct,
                   const char *bearer, int ms) {
  (void)url; (void)ct; (void)bearer; (void)ms;
  if (!strcmp(method, "POST")) snprintf(s_posted, sizeof s_posted, "%s", body);
  return 0;
}
static int c_poll(char *out, size_t n) {
  const char *t = s_feed ? s_feed : "";
  snprintf(out, n, "%s", t);
  s_feed = 0;
  return (int)strlen(out);
}

static void paint(void) {
  int x, y;
  for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) fb[y][x] = UNSET;
  CUI.paint(CUI.state, CWIN);
}

static int unset(void) {
  int x, y, n = 0;
  for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) n += fb[y][x] == UNSET;
  return n;
}

static void dump(const char *what) {
  const char *dir = getenv("CHAT_DUMP");
  char f[512];
  FILE *o;
  int x, y;
  if (!dir) return;
  snprintf(f, sizeof f, "%s/chat_%s.ppm", dir, what);
  if (!(o = fopen(f, "wb"))) return;
  fprintf(o, "P6 %d %d 255\n", SW, SH);
  for (y = 0; y < SH; y++)
    for (x = 0; x < SW; x++) {
      uint16_t c = (uint16_t)(fb[y][x] >> 8 | fb[y][x] << 8);     /* the panel's order back */
      fputc((c >> 11) << 3, o); fputc(((c >> 5) & 63) << 2, o); fputc((c & 31) << 3, o);
    }
  fclose(o);
}

static void boot(void) {
  fakeapi_init(&CAPI);
  fakefs_mem(&CAPI);
  CAPI.fill = c_fill; CAPI.pixels = c_pixels; CAPI.text = c_text; CAPI.ui = c_ui;
  CAPI.paint_area = c_area; CAPI.damage = c_damage;
  CAPI.http_start = c_start; CAPI.http_poll = c_poll;
  fakefs_put("/config/chat.txt", "alex\nmarcy\n");
  s_posted[0] = 0;
  chat_capp_main(&CAPI, 0, 0);
}

/* One poll: start, then the answer. */
static void exchange(const char *reply_text) {
  fakeapi_ticks += 10000;
  CUI.tick(CUI.state, fakeapi_ticks);           /* starts it */
  s_feed = reply_text;
  CUI.tick(CUI.state, fakeapi_ticks);           /* takes the answer */
}

static void key(uint8_t k) { CUI.key(CUI.state, k); }

void test_chat_paint_an_emoji_is_a_picture(void) {
  boot();
  exchange("1\t0\thome\thello\n2\t0\thome\t:arty-love:\n");
  paint();
  dump("emoji");
  CHECK_EQ(unset(), 0);
  /* The greeting (two lines), hello, then the picture, four rows, after
   * "home:" (five letters and a space). */
  CHECK_EQ(C.log.n, 2 + 1 + EMO_ROWS);
  {
    int top = TL_BAR_H + 3 * TL_ROW_H, x = 2 + 6 * 6, i, red = 0;
    uint16_t row[EMO_W * 2];
    emo_row(1, 0, 2, CLR_BG, row);
    for (i = 0; i < EMO_W * 2; i++) CHECK_EQ(fb[top][x + i], row[i]);
    for (i = 0; i < EMO_W * 2 * 36; i++) red += fb[top + i / (EMO_W * 2)][x + i % (EMO_W * 2)] == EMO_PAL[EMO_PIX[1][7 * EMO_W + 4]];
    CHECK(red > 0);
  }
}

void test_chat_paint_the_picker_sends_the_chosen_one(void) {
  boot();
  exchange("");
  key(KEY_TAB);
  CHECK(C.picking);
  CHECK_EQ(C.dog, 1);                            /* chat.txt said marcy */
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_RIGHT);
  paint();
  dump("picker");
  CHECK_EQ(unset(), 0);
  /* The chosen one sits on the highlight. */
  CHECK_EQ(fb[SH - TL_IN_H - PANEL_H + 12][3 + 2 * SLOT], CLR_SEL);
  key(KEY_TAB);                                   /* Arty, kept for next time */
  CHECK_EQ(C.dog, 0);
  CHECK(strstr(fakefs_get("/config/chat.txt"), "arty") != 0);
  key(CAPP_KEY_ENTER);
  CHECK(!C.picking);
  CHECK(!strcmp(C.outbox, ":arty-sad:"));
  exchange("");                                   /* the send goes out */
  CHECK(!strcmp(s_posted, ":arty-sad:"));
  paint();
  CHECK_EQ(unset(), 0);
}

void test_chat_paint_a_letter_closes_the_picker_and_types(void) {
  boot();
  key(KEY_TAB);
  key('h');
  CHECK(!C.picking);
  CHECK(!strcmp(C.input, "h"));
  key(CAPP_KEY_ESC);                              /* clears the typing */
  key(KEY_TAB);
  key(CAPP_KEY_ESC);                              /* closes the picker, nothing sent */
  CHECK(!C.picking);
  CHECK_EQ(C.outbox[0], 0);
}
