/* A menu bar for apps, for when there is a mouse.
 *
 * Named menus on the left that drop down when clicked, small icon buttons on
 * the right for the one or two things worth a single click.
 *
 * THE MENUS ARE NOT DECLARED HERE. They are derived from the app's
 * CappAction table -- the same one that supplies its ctrl chords and its help
 * panel -- by walking it and collecting the distinct `menu` names in the
 * order they first appear. An app states each action once and gets the
 * shortcut, the menu item and the help line out of it. A menu item shows its
 * chord beside its label, which is how anybody ever learns a shortcut.
 *
 * AN ITEM IS AN ACTION, NOT A KEYSTROKE. The first version of this had each
 * button send the key the keyboard would, which was tidy right up until you
 * were typing: pressing Del while editing a to-do inserted a 'd', because in
 * a text field that is all a 'd' can be. Synthesising input is the wrong
 * direction.
 *
 * The set can change with what the app is doing -- toolbar_set swaps the
 * table -- so an editor offers Save and Cancel while typing and something
 * else the rest of the time. A menu item that cannot apply is worse than an
 * absent one.
 *
 * It appears once a mouse has actually moved, which is the rule the launcher
 * uses to decide whether to draw a pointer (s_pointer_on in
 * kernel/ui/launchui.c). So the bar and the cursor arrive together, and no
 * API was needed to ask whether a mouse exists -- the arrival of a mouse
 * event IS the answer.
 *
 * AND IT IS A KEYBOARD MENU TOO. fn-b shows the bar and puts the keyboard in
 * it; left and right walk the menu names, down opens one, up and down walk
 * its items, Enter runs the highlighted one and Escape steps back out to the
 * app with the bar still showing. fn-b again hides it. The same table draws
 * both, so a menu reached by keyboard and a menu reached by pointer cannot
 * disagree about what the app can do -- and every item shows its chord, which
 * is how anybody ever learns one without a mouse.
 *
 * WHILE THE KEYBOARD IS IN THE BAR, THE APP GETS NO KEYS. toolbar_key()
 * answers TB_CONSUMED for everything it is given, arrows and letters alike.
 * Letting unclaimed keys through meant typing into the app underneath an open
 * menu, which is how you end up with a stray letter in a to-do.
 *
 * PAINTING IS TWO CALLS, and the order matters. The bar goes before the app's
 * content and the open dropdown after it, because a dropdown overlaps what it
 * is drawn over; painting both up front would put the app's own drawing on
 * top of the open menu.
 *
 *     toolbar_paint_bar(c);
 *     ... paint into toolbar_rest(c) ...
 *     toolbar_paint_menu(c);
 *
 * And two fast paths, because an app that answers "repaint" without saying
 * what changed gets its whole window redrawn: toolbar_only_menu() for a
 * dropdown whose highlight moved, toolbar_only_bar() for the busy dots
 * ticking over. Both were flicker before they were functions.
 *
 * Coordinates: paint is handed the app's rectangle in SCREEN coordinates,
 * while click and mouse arrive local to the app. Extents are recorded local
 * during paint and the hit tests take local coordinates. Getting this
 * backwards works perfectly fullscreen and misses by the window's origin in a
 * window.
 */
#ifndef CARDOS_TOOLBAR_H
#define CARDOS_TOOLBAR_H

#include "kernel/app/capp.h"

/* Every helper here is static, so an app that uses only some would otherwise
 * fail -Werror=unused-function on the rest. MSVC builds these same files for
 * the host tests and does not know the syntax. */
#if defined(__GNUC__)
#define TB_OPT __attribute__((unused))
#else
#define TB_OPT
#endif

#define TB_H        11         /* the bar */
#define TB_ROW      10         /* one dropdown row */
#define TB_TITLES   4          /* distinct menu names */
#define TB_ICONS    4
#define TB_PAD      4
#define TB_CHARW    6

#define TB_NONE      (-1)      /* the click was not ours */
#define TB_CONSUMED  (-2)      /* ours, but it only opened or closed a menu */

#define TB_BG       CAPP_RGB(44, 48, 58)
#define TB_OPEN     CAPP_RGB(78, 116, 168)
#define TB_DROP     CAPP_RGB(54, 59, 72)
#define TB_HOT      CAPP_RGB(78, 116, 168)
#define TB_TEXT     CAPP_RGB(226, 232, 242)
#define TB_EDGE     CAPP_RGB(20, 22, 28)
#define TB_ICONBG   CAPP_RGB(62, 68, 82)

typedef struct {
  const char *label;           /* one or two characters: "+", "S" */
  int         action;          /* an app action value, never a key */
} TbIcon;

static struct {
  const CardApi    *api;
  const CappAction *act;
  int               nact;
  const char       *title[TB_TITLES];   /* distinct menu names, in order */
  int               nmenu;
  const TbIcon     *icon;
  int               nicon;

  int seen_mouse;
  int shown;                   /* asked for with fn-b, with or without a mouse */
  int focus;                   /* the keyboard is in the bar, not the app */
  int busy;
  int open;                    /* index of the open menu, or -1 */
  int hot_menu, hot_item, hot_icon;

  int mx0[TB_TITLES], mx1[TB_TITLES];   /* local extents, from paint */
  int ix0[TB_ICONS], ix1[TB_ICONS];
  int w;                                /* the app's width, from paint */
  CRect rect;                           /* the app's rectangle, from paint */
} TB;

static int tb_streq(const char *a, const char *b) {
  if (!a || !b) return a == b;
  while (*a && *a == *b) { a++; b++; }
  return *a == 0 && *b == 0;
}

static TB_OPT void toolbar_set(const CappAction *actions, int n,
                               const TbIcon *icons, int nicon) {
  int i, j;

  TB.act = actions;
  TB.nact = n;
  TB.nmenu = 0;
  for (i = 0; i < n; i++) {
    if (!actions[i].menu) continue;
    for (j = 0; j < TB.nmenu; j++)
      if (tb_streq(TB.title[j], actions[i].menu)) break;
    if (j == TB.nmenu && TB.nmenu < TB_TITLES)
      TB.title[TB.nmenu++] = actions[i].menu;
  }
  TB.icon = icons;
  TB.nicon = nicon > TB_ICONS ? TB_ICONS : nicon;
  TB.open = -1;
  TB.hot_menu = TB.hot_item = TB.hot_icon = -1;
}

static TB_OPT void toolbar_init(const CardApi *a, const CappAction *actions,
                                int n, const TbIcon *icons, int nicon) {
  TB.api = a;
  TB.seen_mouse = 0;
  TB.shown = 0;
  TB.focus = 0;
  TB.busy = 0;
  toolbar_set(actions, n, icons, nicon);
}

/* Call from the app's mouse callback. Returns 1 the first time, which is the
 * moment the layout changes and the app needs a full repaint. */
static TB_OPT int toolbar_saw_mouse(void) {
  if (TB.seen_mouse) return 0;
  TB.seen_mouse = 1;
  return 1;
}

/* The app says when it is waiting on something. The dots live in the bar
 * rather than in an overlay drawn by the shell: an overlay over an app's own
 * rectangle is two writers to the same pixels, and they flicker against each
 * other. Here there is one writer. */
static TB_OPT void toolbar_busy(int on) { TB.busy = on; }

static TB_OPT int toolbar_h(void) {
  return (TB.seen_mouse || TB.shown) && (TB.nmenu || TB.nicon) ? TB_H : 0;
}

static TB_OPT CRect toolbar_rest(CRect c) {
  int h = toolbar_h();
  c.y = (short)(c.y + h);
  c.h = (short)(c.h - h);
  return c;
}

static int tb_w(const char *s) {
  return (int)TB.api->str_len(s) * TB_CHARW + TB_PAD * 2;
}

static int tb_menu_count(int m) {
  int i, n = 0;
  for (i = 0; i < TB.nact; i++)
    if (tb_streq(TB.act[i].menu, TB.title[m])) n++;
  return n;
}

static const CappAction *tb_menu_item(int m, int want) {
  int i, n = 0;
  for (i = 0; i < TB.nact; i++) {
    if (!tb_streq(TB.act[i].menu, TB.title[m])) continue;
    if (n++ == want) return &TB.act[i];
  }
  return 0;
}

/* "Save  ^s", "Print  fn-p" -- the label, and the chord it answers to. */
static void tb_item_text(const CappAction *a, char *out, int n) {
  if (a->key >= 1 && a->key <= 26)
    TB.api->fmt(out, (size_t)n, "%s  ^%c", a->label, 'a' + a->key - 1);
  else if (a->key >= 0xE0 && a->key <= 0xF9)
    TB.api->fmt(out, (size_t)n, "%s  fn-%c", a->label, 'a' + a->key - 0xE0);
  else
    TB.api->fmt(out, (size_t)n, "%s", a->label);
}

/* ---- painting -------------------------------------------------------------- */

static TB_OPT void toolbar_paint_bar(CRect c) {
  CRect r;
  int i, x;

  if (!toolbar_h()) return;
  /* Remembered so the hit tests need no rectangle: click and mouse arrive
   * without one, and they must agree with where this drew. */
  TB.w = c.w;
  TB.rect = c;

  r.x = c.x; r.y = c.y; r.w = c.w; r.h = TB_H;
  TB.api->fill(r, TB_BG);
  r.y = (short)(c.y + TB_H - 1); r.h = 1;
  TB.api->fill(r, TB_EDGE);

  x = 1;
  for (i = 0; i < TB.nmenu; i++) {
    int w = tb_w(TB.title[i]);
    uint16_t bg = (i == TB.open) ? TB_OPEN : (i == TB.hot_menu ? TB_HOT : TB_BG);
    if (x + w > c.w - 2) { TB.mx0[i] = TB.mx1[i] = -1; continue; }
    TB.mx0[i] = x;
    TB.mx1[i] = x + w;
    if (bg != TB_BG) {
      r.x = (short)(c.x + x); r.y = c.y; r.w = (short)w; r.h = TB_H - 1;
      TB.api->fill(r, bg);
    }
    TB.api->text((short)(c.x + x + TB_PAD), (short)(c.y + 2), TB.title[i],
                 TB_TEXT, bg);
    x += w;
  }
  for (; i < TB_TITLES; i++) TB.mx0[i] = TB.mx1[i] = -1;

  x = c.w - 1;
  for (i = 0; i < TB.nicon; i++) {
    int w = tb_w(TB.icon[i].label);
    uint16_t bg = (i == TB.hot_icon) ? TB_HOT : TB_ICONBG;
    if (x - w < 2) { TB.ix0[i] = TB.ix1[i] = -1; continue; }
    x -= w;
    TB.ix0[i] = x;
    TB.ix1[i] = x + w;
    r.x = (short)(c.x + x); r.y = (short)(c.y + 1);
    r.w = (short)(w - 1);   r.h = TB_H - 3;
    TB.api->fill(r, bg);
    TB.api->text((short)(c.x + x + TB_PAD), (short)(c.y + 2), TB.icon[i].label,
                 TB_TEXT, bg);
    x -= 1;
  }
  for (; i < TB_ICONS; i++) TB.ix0[i] = TB.ix1[i] = -1;

  /* Three dots where the icons end, animated off the millisecond clock. */
  if (TB.busy) {
    int phase = (int)(TB.api->ticks_ms() / 140u) % 3, d;
    for (d = 0; d < 3; d++) {
      r.x = (short)(c.x + x - 13 + d * 4);
      r.y = (short)(c.y + 4);
      r.w = 3; r.h = 3;
      if (r.x > c.x) TB.api->fill(r, d == phase ? TB_TEXT : TB_ICONBG);
    }
  }
}

/* The bar itself, in screen coordinates. */
static TB_OPT CRect toolbar_bar_rect(void) {
  CRect r;
  r.x = TB.rect.x; r.y = TB.rect.y; r.w = (short)TB.w; r.h = TB_H;
  if (!toolbar_h()) { r.w = 0; r.h = 0; }
  return r;
}

/* Ask for the bar back and nothing else. The busy dots animate off the clock,
 * so something has to keep asking for a repaint -- and an app that answers
 * "repaint" without saying what changed gets its whole window redrawn several
 * times a second, which is a flicker far worse than the reassurance. */
static TB_OPT void toolbar_damage_bar(void) {
  CRect r = toolbar_bar_rect();
  if (r.w) TB.api->damage(r);
}

static TB_OPT int toolbar_only_bar(void) {
  CRect clip = TB.api->paint_area();
  CRect b = toolbar_bar_rect();
  if (!b.w) return 0;
  return clip.x >= b.x && clip.y >= b.y &&
         clip.x + clip.w <= b.x + b.w && clip.y + clip.h <= b.y + b.h;
}

/* Where the open dropdown sits, in screen coordinates. Zero width when none
 * is open. Beside the paint that draws it, because the two have to agree
 * about the on-screen clamping. */
static TB_OPT CRect toolbar_menu_rect(void) {
  CRect r;
  int i, n, w = 0, x;
  char buf[40];

  r.x = 0; r.y = 0; r.w = 0; r.h = 0;
  if (!toolbar_h() || TB.open < 0 || TB.mx0[TB.open] < 0) return r;
  n = tb_menu_count(TB.open);
  if (!n) return r;
  for (i = 0; i < n; i++) {
    const CappAction *a = tb_menu_item(TB.open, i);
    int iw;
    if (!a) continue;
    tb_item_text(a, buf, sizeof buf);
    iw = tb_w(buf);
    if (iw > w) w = iw;
  }
  if (w < 44) w = 44;
  x = TB.mx0[TB.open];
  if (x + w > TB.w) x = TB.w - w;
  if (x < 0) x = 0;

  r.x = (short)(TB.rect.x + x);
  r.y = (short)(TB.rect.y + TB_H);
  r.w = (short)w;
  r.h = (short)(n * TB_ROW + 2);
  return r;
}

/* Is this paint only the dropdown? Moving down an open menu changes one
 * highlighted row and nothing else, but an app that answers "repaint" gets
 * its whole rectangle: the content under the menu is painted, then the menu
 * over it, and at mouse-move rates that reads as the two flickering against
 * each other. The hover marks just the dropdown, and this says when the paint
 * can skip everything else. */
static TB_OPT int toolbar_only_menu(void) {
  CRect clip = TB.api->paint_area();
  CRect m = toolbar_menu_rect();
  if (!m.w) return 0;
  return clip.x >= m.x && clip.y >= m.y &&
         clip.x + clip.w <= m.x + m.w && clip.y + clip.h <= m.y + m.h;
}

static TB_OPT void toolbar_paint_menu(CRect c) {
  CRect m = toolbar_menu_rect(), r;
  int i, n;
  char buf[40];
  (void)c;

  if (!m.w) return;
  n = tb_menu_count(TB.open);

  TB.api->fill(m, TB_DROP);
  TB.api->frame(m, TB_EDGE);

  for (i = 0; i < n; i++) {
    const CappAction *a = tb_menu_item(TB.open, i);
    short yy = (short)(m.y + 1 + i * TB_ROW);
    uint16_t bg = (i == TB.hot_item) ? TB_HOT : TB_DROP;
    if (!a) continue;
    if (bg != TB_DROP) {
      r.x = (short)(m.x + 1); r.y = yy;
      r.w = (short)(m.w - 2); r.h = TB_ROW;
      TB.api->fill(r, bg);
    }
    tb_item_text(a, buf, sizeof buf);
    TB.api->text((short)(m.x + TB_PAD), (short)(yy + 1), buf, TB_TEXT, bg);
  }
}

/* ---- the keyboard ---------------------------------------------------------
 *
 * Call this FIRST from the app's key handler and act on the answer: TB_NONE
 * means the key was not the bar's and the app should handle it as usual,
 * TB_CONSUMED means the bar used it and wants a repaint, anything else is an
 * app action to run -- the same three answers toolbar_click gives, so an app
 * handles a menu item the same way whichever way it was reached.
 *
 * Damage is marked here rather than left to the app: moving down an open
 * dropdown changes a dropdown-sized piece of the screen, and an app that
 * answers "repaint" without saying what changed gets its whole window
 * redrawn, which is the flicker this file already has two fast paths for. */
static TB_OPT void toolbar_damage_all(CRect before) {
  CRect bar = toolbar_bar_rect(), now = toolbar_menu_rect();
  if (bar.w) TB.api->damage(bar);
  if (before.w) TB.api->damage(before);
  if (now.w) TB.api->damage(now);
}

/* Leave the bar to the app, keeping it on screen. */
static TB_OPT void toolbar_unfocus(void) {
  TB.focus = 0;
  TB.open = -1;
  TB.hot_menu = TB.hot_item = -1;
}

static TB_OPT int toolbar_key(uint8_t k) {
  CRect before;

  if (!TB.nmenu && !TB.nicon) return TB_NONE;

  /* The one key that works whether or not the bar has the keyboard. */
  if (k == CAPP_KEY_MENU) {
    if (TB.shown || TB.focus) {
      /* Hidden outright rather than merely unfocused: the same key that
       * summoned it puts it away, and the app gets its eleven pixels back. */
      toolbar_unfocus();
      TB.shown = 0;
    } else {
      TB.shown = 1;
      TB.focus = 1;
      TB.hot_menu = TB.nmenu ? 0 : -1;
      TB.hot_item = -1;
      TB.open = -1;
    }
    return TB_CONSUMED;      /* the layout moved: the app repaints in full */
  }

  if (!TB.focus) return TB_NONE;

  before = toolbar_menu_rect();

  switch (k) {
  case CAPP_KEY_LEFT:
  case CAPP_KEY_RIGHT: {
    int d = (k == CAPP_KEY_RIGHT) ? 1 : -1;
    if (TB.nmenu) {
      TB.hot_menu = (TB.hot_menu + d + TB.nmenu) % TB.nmenu;
      /* A dropdown that was open follows along, which is what every menu bar
       * does and the only way to read across them without reopening each. */
      if (TB.open >= 0) { TB.open = TB.hot_menu; TB.hot_item = 0; }
    }
    break;
  }

  case CAPP_KEY_DOWN:
    if (TB.open < 0) { TB.open = TB.hot_menu; TB.hot_item = 0; }
    else {
      int n = tb_menu_count(TB.open);
      if (n) TB.hot_item = (TB.hot_item + 1) % n;
    }
    break;

  case CAPP_KEY_UP:
    if (TB.open >= 0) {
      int n = tb_menu_count(TB.open);
      /* Up off the top closes the menu rather than wrapping to the bottom:
       * the way back to the bar has to be the way you came. */
      if (TB.hot_item <= 0) { TB.open = -1; TB.hot_item = -1; }
      else if (n) TB.hot_item--;
    }
    break;

  case CAPP_KEY_ENTER:
    if (TB.open < 0) { TB.open = TB.hot_menu; TB.hot_item = 0; break; }
    {
      const CappAction *a = tb_menu_item(TB.open, TB.hot_item);
      /* Running an item gives the keyboard back: the thing you asked for is
       * about to happen and it is the app's business, not the menu's. */
      toolbar_unfocus();
      toolbar_damage_all(before);
      return a ? a->action : TB_CONSUMED;
    }

  case CAPP_KEY_ESC:
    /* Outwards one step at a time, the same as everywhere else in the OS:
     * the dropdown, then the bar, and only then is Escape the app's. */
    if (TB.open >= 0) { TB.open = -1; TB.hot_item = -1; break; }
    toolbar_unfocus();
    break;

  default:
    /* Swallowed. See the note at the top of the file: an unclaimed key that
     * fell through reached the app underneath an open menu. */
    break;
  }

  toolbar_damage_all(before);
  return TB_CONSUMED;
}

/* Is the keyboard in the bar? An app asks before deciding that a key means
 * something to it -- and wants_text answers no while a menu is up, so voice
 * and the matrix do not type into a list that is not listening. */
static TB_OPT int toolbar_has_keys(void) { return TB.focus; }

/* ---- hit testing ----------------------------------------------------------- */

/* The whole rectangle counts, borders included. Testing against the rows as
 * drawn left a one-pixel band along the top of the dropdown belonging to
 * neither the bar nor any row: the highlight cleared, the menu repainted, and
 * a click there closed it. A dead zone between a menu title and its first
 * item is a menu that fights you. */
static int tb_item_at(int16_t x, int16_t y) {
  CRect m = toolbar_menu_rect();
  int n, row, mx;

  if (!m.w) return -1;
  mx = m.x - TB.rect.x;                 /* back into local coordinates */
  if (x < mx || x >= mx + m.w) return -1;
  n = tb_menu_count(TB.open);
  if (y < TB_H || y >= TB_H + n * TB_ROW + 2) return -1;
  row = (y - (TB_H + 1)) / TB_ROW;
  if (row < 0) row = 0;
  if (row >= n) row = n - 1;
  return row;
}

/* Local coordinates. Returns an app action, TB_CONSUMED for a click that only
 * worked the menus, or TB_NONE for one that belongs to the app. */
static TB_OPT int toolbar_click(int16_t x, int16_t y) {
  int i;

  if (!toolbar_h()) return TB_NONE;

  /* An open dropdown gets first refusal: it is drawn over the app, and a
   * click on it must not also reach what is underneath. */
  if (TB.open >= 0) {
    int row = tb_item_at(x, y);
    if (row >= 0) {
      const CappAction *a = tb_menu_item(TB.open, row);
      TB.open = -1;
      TB.hot_item = -1;
      return a ? a->action : TB_CONSUMED;
    }
    if (y >= TB_H) { TB.open = -1; return TB_CONSUMED; }
  }

  if (y < 0 || y >= TB_H) return TB_NONE;

  for (i = 0; i < TB.nicon; i++)
    if (TB.ix0[i] >= 0 && x >= TB.ix0[i] && x < TB.ix1[i]) {
      TB.open = -1;
      return TB.icon[i].action;
    }

  for (i = 0; i < TB.nmenu; i++)
    if (TB.mx0[i] >= 0 && x >= TB.mx0[i] && x < TB.mx1[i]) {
      TB.open = (TB.open == i) ? -1 : i;      /* a second click closes it */
      return TB_CONSUMED;
    }

  TB.open = -1;
  return TB_CONSUMED;                          /* bare bar: swallow it */
}

/* Highlight under the pointer. Returns 1 when anything changed. */
static TB_OPT int toolbar_hover(int16_t x, int16_t y) {
  int wm = TB.hot_menu, wi = TB.hot_item, wc = TB.hot_icon, i;
  int was_open = TB.open;

  TB.hot_menu = TB.hot_item = TB.hot_icon = -1;
  if (toolbar_h()) {
    if (TB.open >= 0) TB.hot_item = tb_item_at(x, y);
    if (y >= 0 && y < TB_H) {
      for (i = 0; i < TB.nmenu; i++)
        if (TB.mx0[i] >= 0 && x >= TB.mx0[i] && x < TB.mx1[i]) TB.hot_menu = i;
      for (i = 0; i < TB.nicon; i++)
        if (TB.ix0[i] >= 0 && x >= TB.ix0[i] && x < TB.ix1[i]) TB.hot_icon = i;
      /* Sliding along an open bar moves the open menu, the way a menu bar
       * behaves everywhere else. */
      if (TB.open >= 0 && TB.hot_menu >= 0) TB.open = TB.hot_menu;
    }
  }

  if (wm == TB.hot_menu && wi == TB.hot_item && wc == TB.hot_icon &&
      was_open == TB.open)
    return 0;

  /* Only the row under the pointer moved: ask for the dropdown back and
   * nothing else. Anything on the bar, or a change of which menu is open,
   * still wants the lot. */
  if (wm == TB.hot_menu && wc == TB.hot_icon && was_open == TB.open &&
      TB.open >= 0) {
    CRect m = toolbar_menu_rect();
    if (m.w) { TB.api->damage(m); return 1; }
  }
  return 1;
}

#endif /* CARDOS_TOOLBAR_H */
