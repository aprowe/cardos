/* Todo -- Google Tasks on the Cardputer.
 *
 * Reads the default task list, ticks things off, and adds new ones. The sign-in
 * happened once on a PC (tools/google_auth.py); this asks the kernel for an
 * access token and makes three kinds of request with it.
 *
 * Works offline, which is the point of a todo list you carry. Tasks are cached
 * to /todo.cache on the card and shown straight away at startup, so the list is
 * on screen before the radio has finished thinking about it. Anything added or
 * ticked while there is no network is queued in the cache and pushed on the
 * next sync -- a todo app that refuses to take a note until it has wifi is a
 * todo app you stop using.
 *
 * The JSON is read by looking for field names rather than parsed. A real parser
 * is several kilobytes to extract three strings per item from a document this
 * regular, and the memory is better spent on the items themselves.
 */

#include "kernel/app/capp.h"

#define MAX_ITEMS  40
#define TITLE_MAX  38
#define ID_MAX     56
#define ROW_H      11
#define REPLY_MAX  6000
#define CACHE_PATH "/todo.cache"

#define LIST_URL "https://tasks.googleapis.com/tasks/v1/users/@me/lists"
#define TASKS_URL "https://tasks.googleapis.com/tasks/v1/lists/%s/tasks?showCompleted=true&showHidden=false&maxResults=40"
#define TASK_URL "https://tasks.googleapis.com/tasks/v1/lists/%s/tasks/%s"
#define ADD_URL  "https://tasks.googleapis.com/tasks/v1/lists/%s/tasks"

#define CLR_BG      CAPP_RGB(22, 24, 30)
#define CLR_ROW     CAPP_RGB(30, 33, 40)
#define CLR_SEL     CAPP_RGB(48, 74, 110)
#define CLR_TEXT    CAPP_RGB(222, 228, 238)
#define CLR_DONE    CAPP_RGB(110, 120, 134)
#define CLR_TICK    CAPP_RGB(112, 208, 140)
#define CLR_BAR     CAPP_RGB(40, 66, 104)
#define CLR_BARFG   CAPP_RGB(232, 238, 248)
#define CLR_WARN    CAPP_RGB(240, 176, 80)
#define CLR_PEND    CAPP_RGB(150, 170, 255)

typedef enum { VIEW_LIST = 0, VIEW_ADD } View;

typedef struct {
  char id[ID_MAX];          /* empty means it has never reached Google */
  char title[TITLE_MAX + 1];
  int  done;
  int  dirty;               /* changed here, not yet pushed */
} Item;

static const CardApi *api;

static struct {
  View view;
  Item item[MAX_ITEMS];
  int  n;
  int  sel;

  char list_id[ID_MAX];
  char status[52];
  int  online;

  char draft[TITLE_MAX + 1];
  int  draft_len;

  char reply[REPLY_MAX];
} T;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *s) { api->fmt(T.status, sizeof T.status, "%s", s); }

/* ---- the smallest JSON reader that answers the question ------------------ */

/* The value of "name" as a string, starting the search at `from`. Returns a
 * pointer just past the value so the caller can walk a list, or NULL. */
static const char *json_str_at(const char *from, const char *name,
                               char *out, int n) {
  char pat[32];
  const char *p;
  int i = 0;

  api->fmt(pat, sizeof pat, "\"%s\"", name);
  p = from;
  for (;;) {
    const char *q = p;
    int j, plen = (int)api->str_len(pat);
    /* No strstr in the API table, and writing one here is four lines. */
    while (*q) {
      for (j = 0; j < plen && q[j] == pat[j]; j++) { }
      if (j == plen) break;
      q++;
    }
    if (!*q) return 0;
    p = q + api->str_len(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') continue;           /* a field of that name, wrong shape */
    p++;
    while (*p && *p != '"' && i + 1 < n) {
      if (*p == '\\' && p[1]) {
        /* Only the escapes Google actually emits in a title. */
        p++;
        if (*p == 'n' || *p == 't') { out[i++] = ' '; p++; continue; }
        if (*p == 'u') { out[i++] = '?'; p += 5; continue; }
      }
      out[i++] = *p++;
    }
    out[i] = 0;
    return *p ? p + 1 : p;
  }
}

/* ---- the cache ----------------------------------------------------------
 *
 * One line per item: done flag, dirty flag, id, then the title. The title is
 * last because it is the only field that can contain a space. */
static void cache_save(void) {
  char line[ID_MAX + TITLE_MAX + 16];
  int fd, i;

  fd = api->open(CACHE_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < T.n; i++) {
    int n = api->fmt(line, sizeof line, "%d %d %s %s\n",
                     T.item[i].done, T.item[i].dirty,
                     T.item[i].id[0] ? T.item[i].id : "-", T.item[i].title);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

static void cache_load(void) {
  char buf[512], line[ID_MAX + TITLE_MAX + 16];
  int fd, n, i, len = 0;

  T.n = 0;
  fd = api->open(CACHE_PATH, CAPP_O_READ);
  if (fd < 0) return;

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n && T.n < MAX_ITEMS; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      {
        Item *it = &T.item[T.n];
        int p = 0, q = 0;
        if (line[0] < '0' || line[0] > '1') continue;
        it->done = line[0] - '0';
        it->dirty = (line[2] == '1');
        p = 4;
        while (line[p] && line[p] != ' ' && q < ID_MAX - 1) it->id[q++] = line[p++];
        it->id[q] = 0;
        if (it->id[0] == '-' && !it->id[1]) it->id[0] = 0;
        while (line[p] == ' ') p++;
        api->fmt(it->title, sizeof it->title, "%s", line + p);
        if (it->title[0]) T.n++;
      }
    }
  }
  api->close(fd);
}

/* ---- Google -------------------------------------------------------------- */

/* The network first, then the token. Two separate things that can be missing,
 * and telling them apart is the difference between "turn the wifi on" and
 * "sign in on a PC". */
static const char *token(void) {
  const char *t;

  if (!api->net_ready()) {
    say("connecting to wifi...");
    if (api->net_connect(20000) != 0) {
      say(api->net_status());
      T.online = 0;
      return 0;
    }
  }
  t = api->google_token();
  if (!t) { say(api->google_status()); T.online = 0; }
  return t;
}

static int find_list(const char *tok) {
  char url[128];
  int n;

  if (T.list_id[0]) return 1;
  api->fmt(url, sizeof url, "%s", LIST_URL);
  n = api->http("GET", url, 0, 0, tok, T.reply, sizeof T.reply, 15000);
  if (n < 0) { say("cannot reach Google"); return 0; }

  /* The first list is the default one, which is what "my tasks" means to
   * anyone who has not made more of them. */
  if (!json_str_at(T.reply, "id", T.list_id, ID_MAX)) {
    say("no task lists on this account");
    return 0;
  }
  return 1;
}

/* Push anything changed here, then pull the list back. Push first on purpose:
 * a pull that ran first would overwrite a local tick with the server's older
 * answer, which is the one way an offline edit can be silently lost. */
static void sync_now(void) {
  const char *tok = token();
  char url[256], body[TITLE_MAX + 64];
  int i, n;

  if (!tok) return;
  say("syncing...");
  if (!find_list(tok)) return;

  for (i = 0; i < T.n; i++) {
    Item *it = &T.item[i];
    if (!it->dirty) continue;

    if (!it->id[0]) {
      api->fmt(url, sizeof url, ADD_URL, T.list_id);
      api->fmt(body, sizeof body, "{\"title\":\"%s\"}", it->title);
      n = api->http("POST", url, body, "application/json", tok,
                    T.reply, sizeof T.reply, 15000);
      if (n >= 0) {
        json_str_at(T.reply, "id", it->id, ID_MAX);
        it->dirty = 0;
      }
    } else {
      api->fmt(url, sizeof url, TASK_URL, T.list_id, it->id);
      api->fmt(body, sizeof body, "{\"status\":\"%s\"}",
               it->done ? "completed" : "needsAction");
      n = api->http("PATCH", url, body, "application/json", tok,
                    T.reply, sizeof T.reply, 15000);
      if (n >= 0) it->dirty = 0;
    }
    if (n < 0) { say("push failed, keeping it local"); cache_save(); return; }
  }

  api->fmt(url, sizeof url, TASKS_URL, T.list_id);
  n = api->http("GET", url, 0, 0, tok, T.reply, sizeof T.reply, 20000);
  if (n < 0) { say("pull failed, showing the cache"); return; }

  {
    const char *p = T.reply;
    char status[16];
    int count = 0;

    while (count < MAX_ITEMS) {
      Item *it = &T.item[count];
      const char *after_id = json_str_at(p, "id", it->id, ID_MAX);
      if (!after_id) break;
      if (!json_str_at(after_id, "title", it->title, TITLE_MAX + 1)) break;

      /* Google sends "status" after "title" for each task, so reading forward
       * from the title keeps the three fields on the same item. */
      status[0] = 0;
      json_str_at(after_id, "status", status, sizeof status);
      it->done = (status[0] == 'c');
      it->dirty = 0;

      p = after_id;
      if (it->title[0]) count++;
    }
    T.n = count;
  }

  if (T.sel >= T.n) T.sel = T.n ? T.n - 1 : 0;
  T.online = 1;
  api->fmt(T.status, sizeof T.status, "%d task%s", T.n, T.n == 1 ? "" : "s");
  cache_save();
}

/* ---- local edits --------------------------------------------------------- */

static void toggle(void) {
  if (T.sel < 0 || T.sel >= T.n) return;
  T.item[T.sel].done = !T.item[T.sel].done;
  T.item[T.sel].dirty = 1;
  cache_save();
  say("ticked -- s syncs");
}

static void add_draft(void) {
  Item *it;
  int i;

  if (!T.draft_len || T.n >= MAX_ITEMS) { T.view = VIEW_LIST; return; }
  /* New items go on top, where you can see the one you just wrote.
   * api->mem_cpy rather than a struct assignment: GCC turns one of those into
   * a call to memcpy, and an app links against nothing that could provide
   * it. */
  for (i = T.n; i > 0; i--)
    api->mem_cpy(&T.item[i], &T.item[i - 1], sizeof T.item[0]);
  it = &T.item[0];
  api->mem_set(it, 0, sizeof *it);
  api->fmt(it->title, sizeof it->title, "%s", T.draft);
  it->dirty = 1;
  T.n++;
  T.sel = 0;
  T.draft[0] = 0;
  T.draft_len = 0;
  T.view = VIEW_LIST;
  cache_save();
  say("added -- s syncs");
}

/* ---- painting ------------------------------------------------------------ */

static void paint_list(CRect c) {
  int rows = (c.h - ROW_H) / ROW_H;
  int top = 0, r;

  if (T.sel >= rows) top = T.sel - rows + 1;

  for (r = 0; r < rows; r++) {
    int i = top + r;
    short y = (short)(c.y + r * ROW_H);
    int sel = (i == T.sel);
    uint16_t bg = sel ? CLR_SEL : ((r & 1) ? CLR_ROW : CLR_BG);

    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    if (i >= T.n) continue;

    /* The box, and a tick drawn rather than lettered: an x reads as "delete"
     * and a check reads as "done". */
    api->frame(rect(c.x + 3, y + 2, 7, 7), CLR_DONE);
    if (T.item[i].done) {
      api->fill(rect(c.x + 4, y + 6, 2, 2), CLR_TICK);
      api->fill(rect(c.x + 5, y + 7, 2, 2), CLR_TICK);
      api->fill(rect(c.x + 6, y + 5, 2, 2), CLR_TICK);
      api->fill(rect(c.x + 7, y + 3, 2, 2), CLR_TICK);
    }

    api->text((short)(c.x + 14), (short)(y + 2), T.item[i].title,
              T.item[i].done ? CLR_DONE : CLR_TEXT, bg);

    /* A dot for anything the server has not seen yet. */
    if (T.item[i].dirty)
      api->fill(rect(c.x + c.w - 5, y + 4, 3, 3), CLR_PEND);
  }

  if (T.n == 0) {
    api->fill(rect(c.x, c.y, c.w, c.h - ROW_H), CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 6), "nothing to do", CLR_DONE, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 20), "a adds  s syncs", CLR_DONE, CLR_BG);
  }
}

static void paint_add(CRect c) {
  char shown[TITLE_MAX + 2];
  int i;

  api->fill(c, CLR_BG);
  api->text((short)(c.x + 6), (short)(c.y + 10), "New task", CLR_BARFG, CLR_BG);

  api->fill(rect(c.x + 5, c.y + 26, c.w - 10, 14), CLR_ROW);
  for (i = 0; i < T.draft_len; i++) shown[i] = T.draft[i];
  shown[T.draft_len] = '_';
  shown[T.draft_len + 1] = 0;
  api->text((short)(c.x + 8), (short)(c.y + 29), shown, CLR_TEXT, CLR_ROW);

  api->text((short)(c.x + 6), (short)(c.y + 48),
            "enter adds   backspace cancels", CLR_DONE, CLR_BG);
}

static void app_paint(void *st, CRect c) {
  char bar[64];
  (void)st;

  if (T.view == VIEW_ADD) { paint_add(c); return; }
  paint_list(c);

  api->fill(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H), CLR_BAR);
  api->fmt(bar, sizeof bar, "%s%s", T.online ? "" : "offline  ", T.status);
  api->text((short)(c.x + 3), (short)(c.y + c.h - ROW_H + 2), bar,
            T.online ? CLR_BARFG : CLR_WARN, CLR_BAR);
}

/* ---- input --------------------------------------------------------------- */

static int key_list(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (T.sel > 0) T.sel--; return 1;
  case CAPP_KEY_DOWN: if (T.sel + 1 < T.n) T.sel++; return 1;
  case CAPP_KEY_ENTER:
  case ' ':           toggle(); return 1;
  case 'a': case 'A':
    T.draft[0] = 0;
    T.draft_len = 0;
    T.view = VIEW_ADD;
    return 1;
  case 's': case 'S': sync_now(); return 1;
  default: return 0;
  }
}

static int key_add(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { add_draft(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (T.draft_len > 0) T.draft[--T.draft_len] = 0;
    else T.view = VIEW_LIST;
    return 1;
  }
  /* No quote or backslash: the title goes into a JSON body, and escaping two
   * characters properly is more code than refusing them is worth here. */
  if (k >= 32 && k < 127 && k != '"' && k != '\\' &&
      T.draft_len < TITLE_MAX) {
    T.draft[T.draft_len++] = (char)k;
    T.draft[T.draft_len] = 0;
    return 1;
  }
  return 0;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  return T.view == VIEW_ADD ? key_add(k) : key_list(k);
}

static int app_click(void *st, short x, short y, int button) {
  int i = y / ROW_H;
  (void)st; (void)button;
  if (T.view != VIEW_LIST) return 0;
  if (i < 0 || i >= T.n) return 0;
  /* A click on the box ticks; a click on the text selects. Two targets in one
   * row, which is what the box is for. */
  if (x < 14 && i == T.sel) { toggle(); return 1; }
  T.sel = i;
  if (x < 14) toggle();
  return 1;
}

static int app_wants_text(void *st) {
  (void)st;
  return T.view == VIEW_ADD;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Todo",
  /* 16x16: a clipboard with a tick. */
  { 0x07, 0xE0, 0x0C, 0x30, 0x1F, 0xF8, 0x30, 0x0C,
    0x30, 0x0C, 0x37, 0x8C, 0x33, 0x0C, 0x30, 0x0C,
    0x36, 0x0C, 0x33, 0x0C, 0x31, 0x8C, 0x30, 0xCC,
    0x30, 0x6C, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tmove\nenter\ttick it off\na\tadd a task\ns\tsync with Google\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;

  api->mem_set(&T, 0, sizeof T);
  T.view = VIEW_LIST;

  /* The cache first, so the list is on screen before the radio has finished
   * thinking about it. A todo list that shows nothing until it is online is a
   * todo list you cannot use on a train. */
  cache_load();
  api->fmt(T.status, sizeof T.status, "%d cached -- s syncs", T.n);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
