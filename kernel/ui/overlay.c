/* The voice panel. See overlay.h. */

#include "kernel/ui/overlay.h"

#include "kernel/ui/draw.h"
#include "kernel/ui/shell.h"
#include "kernel/drv/display.h"

#include <string.h>

/* Centred, and wide enough for forty characters of status at six pixels each.
 * Deliberately not full screen: seeing a sliver of what you were doing behind
 * it is what makes it read as "on top of" rather than "instead of". */
#define PW  208
#define PH   50
#define PX  ((DISPLAY_W - PW) / 2)
#define PY  ((DISPLAY_H - PH) / 2 - 6)

#define C_SHADOW_  RGB565(6, 8, 12)
#define C_BACK     RGB565(18, 22, 34)
#define C_EDGE     RGB565(96, 116, 170)
#define C_TITLE_   RGB565(238, 242, 250)
#define C_SUB      RGB565(150, 160, 180)
#define C_BAR_BACK RGB565(34, 40, 56)
#define C_REC      RGB565(232, 86, 76)     /* the recording red everything uses */
#define C_SEND     RGB565(96, 176, 232)
#define C_WORK     RGB565(226, 176, 110)

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* The frame, drawn once per state change and left alone while a meter moves
 * inside it. Redrawing the whole panel sixty times a second would flicker for
 * no gain, which is the same reason everything else here tracks damage. */
static void panel(const char *title, uint16_t accent) {
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));

  /* A shadow, because the panel is over an app and needs to look like it. */
  draw_rect(R(PX + 3, PY + 3, PW, PH), C_SHADOW_);
  draw_rect(R(PX, PY, PW, PH), C_BACK);
  draw_frame(R(PX, PY, PW, PH), C_EDGE);
  /* A stripe of the state's colour along the top, so which state it is in is
   * readable from further away than the words are. */
  draw_rect(R(PX + 1, PY + 1, PW - 2, 2), accent);

  draw_text((int16_t)(PX + 10), (int16_t)(PY + 9), title, C_TITLE_, C_BACK);
}

static void bar(int pct, uint16_t colour) {
  int w = PW - 20;
  int fill;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  fill = w * pct / 100;

  draw_rect(R(PX + 10, PY + 24, w, 10), C_BAR_BACK);
  if (fill > 0) draw_rect(R(PX + 10, PY + 24, fill, 10), colour);
  draw_frame(R(PX + 10, PY + 24, w, 10), C_EDGE);
}

static void sub(const char *s) {
  draw_rect(R(PX + 10, PY + 38, PW - 20, 8), C_BACK);
  if (s) draw_text_ellipsis((int16_t)(PX + 10), (int16_t)(PY + 38),
                            (int16_t)(PW - 20), s, C_SUB, C_BACK);
}

/* Which panel is currently on screen, so a moving meter does not redraw the
 * chrome around it. */
static int s_state;      /* 0 none, 1 listening, 2 sending, 3 working, 4 result */

void overlay_listening(int level) {
  if (s_state != 1) {
    panel("Listening", C_REC);
    sub("release the button to send");
    s_state = 1;
  }
  bar(level, C_REC);
}

void overlay_sending(int pct) {
  char line[32];
  if (s_state != 2) {
    panel("Sending", C_SEND);
    s_state = 2;
  }
  bar(pct, C_SEND);
  /* snprintf without stdio: two digits and a sign is all this needs. */
  line[0] = (char)('0' + (pct / 100) % 10);
  line[1] = (char)('0' + (pct / 10) % 10);
  line[2] = (char)('0' + pct % 10);
  line[3] = '%';
  line[4] = 0;
  sub(pct >= 100 ? "100%" : line + (pct < 10 ? 2 : 1));
}

void overlay_working(const char *what) {
  if (s_state != 3) {
    panel("Listening", C_WORK);
    s_state = 3;
  }
  bar(100, C_WORK);
  sub(what ? what : "working");
}

void overlay_result(const char *text) {
  panel("Heard", C_SEND);
  /* The result gets the space the meter had: it is the only thing on this
   * panel anyone wants to read. */
  draw_rect(R(PX + 10, PY + 24, PW - 20, 10), C_BACK);
  draw_text_ellipsis((int16_t)(PX + 10), (int16_t)(PY + 24),
                     (int16_t)(PW - 20), text ? text : "", C_TITLE_, C_BACK);
  sub(NULL);
  s_state = 4;
}

void overlay_close(void) {
  if (!s_state) return;
  s_state = 0;
  /* Only the shell underneath knows what was there. Same conclusion as the
   * pointer: the panel cannot read back what it covered, because the display
   * is three-wire and there is no MISO. */
  ui_repaint();
}
