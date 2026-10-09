/* apps/toolbar.h: while a request is out, the busy dots ask for a repaint
 * only when they step, and only for themselves. */

#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/toolbar.h"

static CardApi TA;
static int s_marks;
static CRect s_last;
static void tb_damage(CRect r) { s_marks++; s_last = r; }

static const CappAction TB_ACTS[] = {
  { "sync", "Sync", "File", 0, 1 },
};

void test_toolbar_busy_dots_mark_once_a_step_and_only_themselves(void) {
  CRect app = { 0, 0, 240, 135 };
  int i, asked = 0;
  fakeapi_init(&TA);
  TA.damage = tb_damage;
  toolbar_init(&TA, TB_ACTS, 1, 0, 0);
  CHECK_EQ(toolbar_damage_bar(), 0);         /* no bar (no mouse yet): nothing */
  toolbar_saw_mouse();
  toolbar_busy(1);
  fakeapi_ticks = 1000;
  toolbar_paint_bar(app);
  s_marks = 0;
  /* 420 ms of 5 ms ticks: three steps of the dots, so three marks. */
  for (i = 0; i < 84; i++) {
    fakeapi_ticks += 5;
    asked += toolbar_damage_bar();
  }
  CHECK_EQ(asked, 3);
  CHECK_EQ(s_marks, 3);
  CHECK(s_last.w == 12 && s_last.h == 3);    /* the dots, not the titles */
  CHECK(s_last.y >= app.y && s_last.y + s_last.h <= app.y + TB_H);
  toolbar_busy(0);
}
