/* Todo -- Google Tasks on the Cardputer.
 *
 * Reads the account's task lists, ticks things off, and adds new ones. The sign-in
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
 * More than one list, but one at a time: only the current list's tasks are in
 * memory, and each list has its own cache file under /todo. Left and Right
 * step between lists, `l` shows them all. Lists are made and named on a
 * phone or a PC; there is no request type here for that, on purpose.
 *
 * ONE SWEEP, AT OPEN. Opening the app fetches the lists, pushes and pulls the
 * list on screen, then pulls every other list into its own cache file -- and
 * then stops. Nothing starts on its own after that; `s` runs the sweep again.
 * It used to pull the current list every ten minutes, which meant the app
 * reached out and rearranged the screen while you were reading it, and the
 * list you were not looking at was never fetched at all, so `o` had nothing
 * real to show. A list off screen is absorbed straight into its file rather
 * than into the item array, because nothing on screen depends on it and the
 * array is the one on display.
 *
 * `o` is that overview: every list's open tasks under its own heading, the
 * way the calendar's agenda groups by day. It is read from the cache files,
 * so it costs one small array and no requests of its own.
 *
 * The JSON is read by looking for field names rather than parsed. A real parser
 * is several kilobytes to extract three strings per item from a document this
 * regular, and the memory is better spent on the items themselves.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"

#define MAX_ITEMS  40
#define TITLE_MAX  38
#define ID_MAX     56
#define ROW_H      11
#define REPLY_MAX  6000
#define MAX_LISTS  8
#define NAME_MAX   24

/* One file per list, named by the list's id, so switching lists is a file
 * swap rather than a bigger array. The caches are caches -- /cache/todo can
 * be wiped and the next sync rebuilds them -- but the list index is state:
 * lose /var/todo/lists and the lists are gone until Google is asked again. */
#define CACHE_DIR  CAPP_CACHE "/todo"
#define CACHE_FMT  CAPP_CACHE "/todo/%s.cache"
#define STATE_DIR  CAPP_VAR "/todo"
#define LISTS_PATH CAPP_VAR "/todo/lists"

/* `fields=` is not an optimisation here, it is the difference between a list
 * that syncs and one that silently loses tasks.
 *
 * A Google task carries kind, etag, selfLink, position, updated, links and
 * more -- three hundred bytes of it -- and forty of those is twelve kilobytes
 * against a six-kilobyte buffer and the kernel's eight. Nothing reports that:
 * the HTTP layer fills the buffer, sees it full, and cannot tell a body that
 * ended from one that was cut off, so the app parses as far as the cut and
 * shows a short list as though it were the whole one. Asking for the three
 * fields actually read brings a task to about sixty bytes, and forty of them
 * to well under the buffer. apps/calendar.c says the same thing about the
 * same problem; Todo simply never had it done. */
#define LIST_URL "https://tasks.googleapis.com/tasks/v1/users/@me/lists?fields=items(id,title)"
#define TASKS_URL "https://tasks.googleapis.com/tasks/v1/lists/%s/tasks?showCompleted=true&showHidden=false&maxResults=40&fields=items(id,title,status)"
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

typedef enum { VIEW_LIST = 0, VIEW_ADD, VIEW_LISTS, VIEW_ALL } View;

/* The sync runs on the OS's request task and is collected from tick, so the
 * screen never stops. See CardApi.http_start. */
/* The sweep, in order: the lists themselves, then the pending edits of the
 * list on screen, then its tasks, then every other list in turn. */
enum { SYNC_IDLE = 0, SYNC_LIST, SYNC_PUSH, SYNC_PULL, SYNC_SWEEP };

/* The overview's rows. Capped rather than sized to the lists, because eight
 * lists of forty tasks is more than fits on a 135-pixel screen many times
 * over -- and this array is charged to an app that already holds forty items
 * and a six-kilobyte reply. What does not fit says so on the last line. */
#define OVER_MAX   48
#define PAGE_MAX   3072   /* 40 tasks or 48 overview rows, with headings */

/* When a sync could not even be attempted -- no radio yet, no token yet --
 * try again soon rather than in ten minutes. Opening the app during the few
 * seconds it takes WiFi to come up is the common case, not the rare one. */
#define RETRY_MS      (15u * 1000u)

typedef struct {
  char id[ID_MAX];          /* empty means it has never reached Google */
  char title[TITLE_MAX + 1];
  int  done;
  int  dirty;               /* changed here, not yet pushed */
  int  deleted;             /* gone here, not yet gone at Google */
} Item;

typedef struct {
  char id[ID_MAX];
  char name[NAME_MAX];
} TList;

/* One line of the overview. The title is copied rather than pointed at
 * because it comes out of a cache file that is read and closed. */
typedef struct {
  char title[TITLE_MAX + 1];
  unsigned char list;               /* index into lists[] */
  unsigned char head;               /* first row of that list: draw a heading */
} OverRow;

static const CardApi *api;

static struct {
  View view;
  Item item[MAX_ITEMS];
  int  n;
  int  sel;

  /* The lists, and which one the items above belong to. `cur` is an index
   * into lists[] or -1 while none is known; list_id is the current id either
   * way, because it is remembered on the card before the lists are. */
  TList lists[MAX_LISTS];
  int   nlists;
  int   cur;
  int   lists_fresh;                /* fetched this run, not just from the card */
  int   psel;                       /* the picker's highlighted row */
  int   discard;                    /* the reply in flight is another list's */
  int   sweep;                      /* the list being pulled off screen, or -1 */
  int   swept;                      /* how many the sweep has finished */
  int   synced_once;                /* the sweep has run; do not start another */
  int   cmd_waiting;                /* a `sync` command wants to hear the end */

  OverRow over[OVER_MAX];
  int     nover;
  int     osel;
  int     over_full;                /* there was more than the array holds */
  /* Where paint actually put each row. A heading makes the row under it two
   * rows tall, so a click cannot be divided back into an index the way the
   * flat list's can -- it is matched against what was drawn, for the same
   * reason paint_list keeps shown_top. -1 means "not on screen". */
  short   oy[OVER_MAX];

  CRect content;                    /* what paint was last given, for damage */
  char  last_status[52];            /* to know whether the strip changed */
  char  list_id[ID_MAX];
  char status[52];
  int  online;

  int      stage;                   /* SYNC_* */
  int      pushing;                 /* the item being sent */
  uint32_t next_auto;
  int      tried_once;

  char draft[TITLE_MAX + 1];
  int  draft_len;

  char reply[REPLY_MAX];
  int shown_top, shown_rows;  /* what the last paint put on screen */

  /* Paper. The page is built here rather than in reply[] because a sync may
   * be filling that at the same moment; printing copies it at once, so it
   * is only busy for the length of one call. */
  char page[PAGE_MAX];
  int  printing;              /* mirror the job's status into the strip */
} T;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *s) { api->fmt(T.status, sizeof T.status, "%s", s); }

/* To /cache/app.log, via the kernel. A sync that fails once a day cannot be
 * caught on a USB serial port that is not plugged in -- which is exactly when
 * it fails. Sparingly: this writes to an SD card, so it is a handful of lines
 * per sweep and never one per tick.
 *
 * A macro because the API has fmt but no vfmt, and adding one would move
 * every .capp to a new API version to save four lines here. */
#define logf(...) do { char lb_[120];                        api->fmt(lb_, sizeof lb_, __VA_ARGS__);                        api->log(lb_); } while (0)

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
 * One line per item: done, deleted and dirty flags, the id (or `-` for an
 * item that has never reached Google), then the title. The title is last
 * because it is the only field that can contain a space. A deleted item is
 * kept: it is still waiting to be deleted at Google, and dropping it here
 * would bring it back on the next pull. */
static const char *cache_path(void) {
  static char path[ID_MAX + 16];
  api->fmt(path, sizeof path, CACHE_FMT, T.list_id[0] ? T.list_id : "_");
  return path;
}

static void cache_save(void) {
  char line[ID_MAX + TITLE_MAX + 16];
  int fd, i;

  fd = api->open(cache_path(), CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < T.n; i++) {
    int n = api->fmt(line, sizeof line, "%d %d %d %s %s\n",
                     T.item[i].done, T.item[i].deleted, T.item[i].dirty,
                     T.item[i].id[0] ? T.item[i].id : "-", T.item[i].title);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

static void cache_load(void) {
  char buf[512], line[ID_MAX + TITLE_MAX + 16];
  int fd, n, i, len = 0;

  T.n = 0;
  fd = api->open(cache_path(), CAPP_O_READ);
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
        /* The three flags, each one digit and a space: the shape the saver
         * writes, checked rather than assumed. */
        if (line[0] < '0' || line[0] > '1' || line[1] != ' ') continue;
        if (line[2] < '0' || line[2] > '1' || line[3] != ' ') continue;
        if (line[4] < '0' || line[4] > '1' || line[5] != ' ') continue;
        it->done    = line[0] - '0';
        it->deleted = line[2] - '0';
        it->dirty   = line[4] - '0';
        p = 6;
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

/* ---- the lists -----------------------------------------------------------
 *
 * /todo/lists: the current list's id on the first line, then "id name" per
 * list. On the card so that Left and Right work on a train, and so the app
 * opens on the list it was closed on. */
static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return !*a && !*b;
}

static void lists_save(void) {
  char line[ID_MAX + NAME_MAX + 4];
  int fd, i, n;

  fd = api->open(LISTS_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  n = api->fmt(line, sizeof line, "%s\n", T.list_id[0] ? T.list_id : "-");
  api->write(fd, line, (size_t)n);
  for (i = 0; i < T.nlists; i++) {
    n = api->fmt(line, sizeof line, "%s %s\n", T.lists[i].id, T.lists[i].name);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

static void lists_load(void) {
  char buf[256], line[ID_MAX + NAME_MAX + 4];
  int fd, n, i, len = 0, row = 0;

  T.nlists = 0;
  T.cur = -1;
  fd = api->open(LISTS_PATH, CAPP_O_READ);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      if (row++ == 0) {
        if (line[0] == '-' && !line[1]) line[0] = 0;
        api->fmt(T.list_id, sizeof T.list_id, "%s", line);
      } else if (T.nlists < MAX_LISTS) {
        TList *l = &T.lists[T.nlists];
        int p = 0, q = 0;
        while (line[p] && line[p] != ' ' && q < ID_MAX - 1) l->id[q++] = line[p++];
        l->id[q] = 0;
        while (line[p] == ' ') p++;
        api->fmt(l->name, sizeof l->name, "%s", line + p);
        if (l->id[0]) T.nlists++;
      }
    }
  }
  api->close(fd);
  for (i = 0; i < T.nlists; i++)
    if (same(T.lists[i].id, T.list_id)) T.cur = i;
}

/* The lists reply. The current list stays current if it is still there;
 * otherwise -- first run, or a list deleted elsewhere -- the first one is,
 * and whatever tasks are in memory go with it: on a first run they are the
 * ones added before any sync, and they belong to the only list there was. */
static void absorb_lists(void) {
  const char *p = T.reply;
  int count = 0, i;

  while (count < MAX_LISTS) {
    TList *l = &T.lists[count];
    const char *after_id = json_str_at(p, "id", l->id, ID_MAX);
    if (!after_id) break;
    if (!json_str_at(after_id, "title", l->name, NAME_MAX)) break;
    p = after_id;
    if (l->id[0]) count++;
  }
  T.nlists = count;
  T.lists_fresh = 1;
  if (!count) return;

  T.cur = -1;
  for (i = 0; i < count; i++)
    if (same(T.lists[i].id, T.list_id)) T.cur = i;
  if (T.cur < 0) {
    /* The tasks in memory were filed under a list that is not here (or under
     * none). They are refiled under the first list: saved to its file and
     * the old file dropped, so a restart does not find them twice. */
    if (T.n) api->remove(cache_path());
    T.cur = 0;
    api->fmt(T.list_id, sizeof T.list_id, "%s", T.lists[0].id);
    if (T.n) cache_save();
  }
  lists_save();
}

/* Make list i current: file the current tasks, load that list's. A pull or a
 * lists fetch in flight is simply dropped when it lands (sync_tick). A push
 * is not: its reply is what clears an item's dirty flag, and discarding it
 * means the item is pushed again -- twice at Google -- so that one is
 * waited for. It is one small request, not a whole sync. */
static void select_list(int i) {
  if (T.nlists == 0) { say("no lists yet -- s syncs"); return; }
  if (T.stage == SYNC_PUSH) { say("sending -- wait a moment"); return; }
  if (T.stage == SYNC_SWEEP) { say("syncing -- wait a moment"); return; }
  if (i < 0) i = T.nlists - 1;
  if (i >= T.nlists) i = 0;
  if (i == T.cur) return;
  if (T.stage != SYNC_IDLE) T.discard = 1;

  cache_save();
  T.cur = i;
  api->fmt(T.list_id, sizeof T.list_id, "%s", T.lists[i].id);
  cache_load();
  T.sel = 0;
  lists_save();
  api->fmt(T.status, sizeof T.status, "%d cached -- s syncs", T.n);
  /* Sync it as soon as the tick comes round, not in ten minutes. */
  T.tried_once = 0;
}

/* ---- the overview -------------------------------------------------------
 *
 * Read from the cache files rather than the network: every list has one by
 * the time the sweep has run once, and a view that costs nothing to open is
 * a view you will actually use. Done tasks are left out -- the question this
 * view answers is "what is there to do", and a finished task is not part of
 * it. Deleted ones too: they are on their way out.
 */
static void over_add(int list, const char *title, int first) {
  OverRow *r;
  if (T.nover >= OVER_MAX) { T.over_full = 1; return; }
  r = &T.over[T.nover++];
  api->fmt(r->title, sizeof r->title, "%s", title);
  r->list = (unsigned char)list;
  r->head = (unsigned char)(first ? 1 : 0);
}

/* One list's open tasks, straight off the card. The parsing is cache_load's,
 * deliberately: one format, read the same way in both places. */
static void over_scan(int li) {
  char buf[256], line[ID_MAX + TITLE_MAX + 16];
  char path[ID_MAX + 16];
  int fd, n, i, len = 0, first = 1;

  api->fmt(path, sizeof path, CACHE_FMT, T.lists[li].id);
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) return;

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      if (line[0] < '0' || line[0] > '1' || line[1] != ' ') continue;
      if (line[2] < '0' || line[2] > '1' || line[3] != ' ') continue;
      if (line[4] < '0' || line[4] > '1' || line[5] != ' ') continue;
      if (line[0] == '1' || line[2] == '1') continue;    /* done, or going */
      {
        int p = 6;
        while (line[p] && line[p] != ' ') p++;           /* past the id */
        while (line[p] == ' ') p++;
        if (!line[p]) continue;
        over_add(li, line + p, first);
        first = 0;
      }
    }
  }
  api->close(fd);
}

static void over_build(void) {
  int i;
  T.nover = 0;
  T.over_full = 0;
  /* The list on screen is the live one: its array may hold edits that are
   * not in its file yet, so it is read from memory and the rest from disk. */
  for (i = 0; i < T.nlists; i++) {
    if (i == T.cur) {
      int j, first = 1;
      for (j = 0; j < T.n; j++) {
        if (T.item[j].done || T.item[j].deleted) continue;
        over_add(i, T.item[j].title, first);
        first = 0;
      }
    } else {
      over_scan(i);
    }
  }
  if (T.osel >= T.nover) T.osel = T.nover ? T.nover - 1 : 0;
}

/* ---- Google -------------------------------------------------------------- */

/* Removing a row from the array. Used once the delete has reached Google, and
 * straight away for something that never got there. */
static void drop(int i) {
  int j;
  for (j = i; j + 1 < T.n; j++)
    api->mem_cpy(&T.item[j], &T.item[j + 1], sizeof T.item[0]);
  T.n--;
  if (T.sel >= T.n) T.sel = T.n ? T.n - 1 : 0;
}

/* Marked rather than removed, so a delete made offline still happens when the
 * network comes back -- and marked rather than done, so pressing the key
 * again takes it back. Until the sweep sends it, a delete is a local opinion
 * and there is no reason the user cannot change it; before, the only way back
 * from a mis-hit `d` was to retype the task.
 *
 * An item that never reached Google is the exception: there is nothing at the
 * other end to delete, so it simply goes, and there is nothing to restore. */
static void delete_selected(void) {
  Item *it;

  if (T.sel < 0 || T.sel >= T.n) return;
  it = &T.item[T.sel];

  if (it->deleted) {
    it->deleted = 0;
    it->dirty = 1;
    cache_save();
    say("kept -- s syncs");
    return;
  }
  if (!it->id[0]) {
    drop(T.sel);
    cache_save();
    say("deleted");
    return;
  }
  it->deleted = 1;
  it->dirty = 1;
  cache_save();
  say("deleted -- d undoes, s syncs");
}

/* ---- syncing, without stopping the world ---------------------------------
 *
 * Four states: find the list, push each local change, pull the list back,
 * idle. Push before pull on purpose -- a pull that ran first would overwrite
 * a local tick with the server's older answer, which is the one way an
 * offline edit can be silently lost.
 *
 * Each step starts a request and returns. sync_tick collects it on a later
 * pass, so the shell keeps drawing and reading keys throughout, and draws its
 * own spinner while anything is in the air.
 */

static int next_dirty(void) {
  int i;
  for (i = 0; i < T.n; i++) if (T.item[i].dirty) return i;
  return -1;
}

static int start_list(const char *tok) {
  return api->http_start("GET", LIST_URL, 0, 0, tok, 15000);
}

static int start_pull(const char *tok) {
  char url[256];
  api->fmt(url, sizeof url, TASKS_URL, T.list_id);
  return api->http_start("GET", url, 0, 0, tok, 20000);
}

static int start_push(const char *tok, int i) {
  Item *it = &T.item[i];
  char url[256], body[TITLE_MAX + 64];

  if (it->deleted && it->id[0]) {
    api->fmt(url, sizeof url, TASK_URL, T.list_id, it->id);
    return api->http_start("DELETE", url, 0, 0, tok, 15000);
  }
  if (!it->id[0]) {
    api->fmt(url, sizeof url, ADD_URL, T.list_id);
    api->fmt(body, sizeof body, "{\"title\":\"%s\"}", it->title);
    return api->http_start("POST", url, body, "application/json", tok, 15000);
  }
  api->fmt(url, sizeof url, TASK_URL, T.list_id, it->id);
  api->fmt(body, sizeof body, "{\"status\":\"%s\"}",
           it->done ? "completed" : "needsAction");
  return api->http_start("PATCH", url, body, "application/json", tok, 15000);
}

static void absorb_tasks(void) {
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
    it->deleted = 0;

    p = after_id;
    if (it->title[0]) count++;
  }
  T.n = count;
  if (T.sel >= T.n) T.sel = T.n ? T.n - 1 : 0;
  T.online = 1;
  api->fmt(T.status, sizeof T.status, "%d task%s", T.n, T.n == 1 ? "" : "s");
  cache_save();
}

/* http_start said no: the device runs one request at a time, and someone
 * else -- the Claude terminal mid-answer, another window's sync -- has it.
 * Say so and try again soon. Returning silently left the old status on
 * screen and the next attempt ten minutes off, which reads as "broken". */
/* A list nobody is looking at, written straight to its file.
 *
 * The item array is what is on screen, so a sweep that loaded each list into
 * it would rearrange the display three times while you read. This walks the
 * reply and writes the cache lines as it goes: no array, no repaint, and the
 * file is exactly what cache_load would later read back. Pending edits in
 * that file are not overwritten blindly -- there are none, because a list
 * with pending edits is pushed when you switch to it, and its dirty flag
 * survives in the file until then.
 */
static int absorb_into(const char *list_id, int *count_out) {
  const char *p = T.reply;
  char path[ID_MAX + 16], line[ID_MAX + TITLE_MAX + 16];
  char id[ID_MAX], title[TITLE_MAX + 1], status[16];
  int fd, count = 0;

  api->fmt(path, sizeof path, CACHE_FMT, list_id);
  fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return -1;

  while (count < MAX_ITEMS) {
    const char *after_id = json_str_at(p, "id", id, ID_MAX);
    int n, done;
    if (!after_id) break;
    if (!json_str_at(after_id, "title", title, TITLE_MAX + 1)) break;
    status[0] = 0;
    json_str_at(after_id, "status", status, sizeof status);
    done = (status[0] == 'c');
    p = after_id;
    if (!title[0]) continue;
    n = api->fmt(line, sizeof line, "%d %d %d %s %s\n",
                 done, 0, 0, id[0] ? id : "-", title);
    api->write(fd, line, (size_t)n);
    count++;
  }
  api->close(fd);
  if (count_out) *count_out = count;
  return 0;
}

static void start_refused(void) {
  T.online = 0;
  say("busy -- another request is running, retrying");
  T.next_auto = api->ticks_ms() + RETRY_MS;
}

/* The next list the sweep has not done, skipping the one on screen (already
 * pulled) and anything with pending edits (pushed when you switch to it).
 * Returns -1 when the sweep is finished. */
static int sweep_next(void) {
  int i;
  for (i = T.swept; i < T.nlists; i++) {
    T.swept = i + 1;
    if (i == T.cur) continue;
    return i;
  }
  return -1;
}

static void sweep_done(void) {
  T.stage = SYNC_IDLE;
  T.sweep = -1;
  T.online = 1;
  over_build();
  api->fmt(T.status, sizeof T.status, "%d task%s", T.n, T.n == 1 ? "" : "s");
  logf("sweep done, %d list%s", T.nlists, T.nlists == 1 ? "" : "s");
}

/* Start the next off-screen pull, or finish. */
static void sweep_step(const char *tok) {
  int i = sweep_next();
  char url[256];

  if (i < 0) { sweep_done(); return; }
  api->fmt(url, sizeof url, TASKS_URL, T.lists[i].id);
  if (api->http_start("GET", url, 0, 0, tok, 20000) != 0) {
    /* The queue is busy. One list missing from the overview is not worth
     * wedging the sweep over: stop here and let `s` try again. */
    logf("sweep: %s refused, stopping", T.lists[i].name);
    sweep_done();
    return;
  }
  T.sweep = i;
  T.stage = SYNC_SWEEP;
  api->fmt(T.status, sizeof T.status, "syncing %s (%d/%d)",
           T.lists[i].name, T.swept, T.nlists);
}

static void sync_begin(void) {
  const char *tok;

  if (T.stage != SYNC_IDLE) return;
  T.tried_once = 1;

  /* A sync that could not start is not a sync that succeeded: it is tried
   * again shortly rather than being counted as the one sweep this open
   * gets. Opening the app a second before the radio comes up is the common
   * case, not the rare one. */
  if (!api->net_ready()) {
    T.online = 0;
    say("offline -- showing the cache");
    T.next_auto = api->ticks_ms() + RETRY_MS;
    logf("offline: %s", api->net_status ? api->net_status() : "no radio");
    return;
  }
  tok = api->google_token();
  if (!tok || !tok[0]) {
    T.online = 0;
    say(api->google_status());
    T.next_auto = api->ticks_ms() + RETRY_MS;
    logf("no token: %s", api->google_status());
    return;
  }

  /* Always the lists first, even on a second sweep: a list renamed or added
   * on a phone should show up without restarting the app. */
  if (start_list(tok) != 0) { start_refused(); return; }
  T.stage = SYNC_LIST;
  T.swept = 0;
  T.sweep = -1;
  say("syncing...");
  logf("sweep begins (%s)", T.synced_once ? "asked" : "opened");
}

static void sync_tick(void) {
  const char *tok;
  int n, i;

  if (T.stage == SYNC_IDLE) return;
  n = api->http_poll(T.reply, sizeof T.reply);
  if (n == CAPP_HTTP_PENDING) return;

  /* The list changed under it. The lists themselves are not per-list and
   * are kept; anything else was the old list's and is dropped. select_list
   * left tried_once clear, so the next tick syncs the new list. */
  if (T.discard) {
    T.discard = 0;
    if (T.stage == SYNC_LIST && n >= 0) absorb_lists();
    T.stage = SYNC_IDLE;
    return;
  }

  tok = api->google_token();

  if (T.stage == SYNC_LIST) {
    if (n >= 0) absorb_lists();
    if (n < 0 || !T.nlists) {
      T.online = 0;
      say(n < 0 ? "cannot reach Google" : "no task lists on this account");
      logf("lists failed (%d)", n);
      T.stage = SYNC_IDLE;
      return;
    }
    logf("%d list%s", T.nlists, T.nlists == 1 ? "" : "s");
    i = next_dirty();
    if (i >= 0 && tok && start_push(tok, i) == 0) {
      T.pushing = i; T.stage = SYNC_PUSH; return;
    }
    if (tok && start_pull(tok) == 0) { T.stage = SYNC_PULL; return; }
    T.stage = SYNC_IDLE;
    return;
  }

  if (T.stage == SYNC_PUSH) {
    /* 404 on a delete means it is already gone, which is the outcome asked
     * for. Anything else that failed stays dirty and is retried next time. */
    if (n >= 0 || n == -404) {
      if (T.pushing >= 0 && T.pushing < T.n) {
        Item *it = &T.item[T.pushing];
        if (it->deleted) drop(T.pushing);
        else {
          if (!it->id[0]) json_str_at(T.reply, "id", it->id, ID_MAX);
          it->dirty = 0;
        }
      }
      cache_save();
    } else {
      /* Kept local and still dirty, so the next sweep tries again. The pull
       * below is skipped for this list on purpose: it would overwrite the
       * edit that just failed to send with the server's older answer. */
      T.online = 0;
      say("push failed, keeping it local");
      logf("push failed (%d), keeping it local", n);
      T.synced_once = 1;
      if (tok) sweep_step(tok);
      else sweep_done();
      return;
    }
    i = next_dirty();
    if (i >= 0 && tok && start_push(tok, i) == 0) { T.pushing = i; return; }
    if (tok && start_pull(tok) == 0) { T.stage = SYNC_PULL; return; }
    T.stage = SYNC_IDLE;
    return;
  }

  if (T.stage == SYNC_SWEEP) {
    /* One list of the sweep came back. A failure here is logged and stepped
     * past: the others are the only chance they get until the user asks
     * again, and abandoning them for one 403 is how the overview ends up
     * half empty with nothing saying why. */
    int got = 0;
    if (n < 0) {
      logf("%s: pull failed (%d)", T.sweep >= 0 ? T.lists[T.sweep].name : "?", n);
    } else if (T.sweep >= 0) {
      absorb_into(T.lists[T.sweep].id, &got);
      logf("%s: %d task%s", T.lists[T.sweep].name, got, got == 1 ? "" : "s");
    }
    if (tok) sweep_step(tok);
    else sweep_done();
    return;
  }

  /* SYNC_PULL: the list on screen. Then the rest of them. */
  if (n < 0) {
    T.online = 0;
    /* -4 is the kernel refusing to start the request at all, and it knows
     * why -- "not enough memory: 37 KB free, TLS needs about 33" is
     * something the owner of the device can act on, where "pull failed" is
     * not. See kernel/net/http.c. */
    if (n == -4) {
      api->fmt(T.status, sizeof T.status, "%s", api->net_status());
      T.next_auto = api->ticks_ms() + RETRY_MS;
      T.synced_once = 0;             /* transient: let the retry sweep */
    } else {
      say("pull failed, showing the cache");
    }
    logf("%s: pull failed (%d) %s", T.lists[T.cur >= 0 ? T.cur : 0].name, n,
         n == -4 ? api->net_status() : "");
    T.stage = SYNC_IDLE;
    if (n != -4) T.synced_once = 1;
    return;
  }
  absorb_tasks();
  logf("%s: %d task%s", T.cur >= 0 ? T.lists[T.cur].name : "?", T.n,
       T.n == 1 ? "" : "s");
  T.synced_once = 1;
  if (tok) sweep_step(tok);
  else sweep_done();
}

/* ---- local edits --------------------------------------------------------- */

static void toggle(void) {
  if (T.sel < 0 || T.sel >= T.n) return;
  if (T.item[T.sel].deleted) return;
  T.item[T.sel].done = !T.item[T.sel].done;
  T.item[T.sel].dirty = 1;
  cache_save();
  say("ticked -- s syncs");
}

/* A new task on top of the current list, saved and marked for the next
 * sync to push. What Add does with its draft and what the `add` command
 * does with its argument: one path, so the two cannot disagree. -1 if the
 * list is full or there is nothing to add.
 *
 * A double quote or a backslash would break the JSON the push sends, which
 * is why the draft's keyboard refuses them; text from a command gets the
 * same rule by replacement instead. */
static int add_text(const char *text) {
  Item *it;
  int i, j;

  if (!text || !text[0] || T.n >= MAX_ITEMS) return -1;
  /* New items go on top, where you can see the one you just wrote.
   * api->mem_cpy rather than a struct assignment: GCC turns one of those into
   * a call to memcpy, and an app links against nothing that could provide
   * it. */
  for (i = T.n; i > 0; i--)
    api->mem_cpy(&T.item[i], &T.item[i - 1], sizeof T.item[0]);
  it = &T.item[0];
  api->mem_set(it, 0, sizeof *it);
  for (i = 0, j = 0; text[i] && j < TITLE_MAX; i++)
    it->title[j++] = (text[i] == '"' || text[i] == '\\') ? '\'' : text[i];
  it->title[j] = 0;
  it->dirty = 1;
  T.n++;
  T.sel = 0;
  cache_save();
  return 0;
}

static void add_draft(void) {
  if (T.draft_len && add_text(T.draft) == 0) say("added -- s syncs");
  T.draft[0] = 0;
  T.draft_len = 0;
  T.view = VIEW_LIST;
}

/* ---- painting ------------------------------------------------------------ */

static void paint_list(CRect c) {
  int rows = (c.h - ROW_H) / ROW_H;
  int top = 0, r;

  if (T.sel >= rows) top = T.sel - rows + 1;

  /* Kept for the click handler. It has to undo exactly the arithmetic done
   * here, and the only way to be sure it matches is for it to be this. */
  T.shown_top = top;
  T.shown_rows = rows;

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
              (T.item[i].done || T.item[i].deleted) ? CLR_DONE : CLR_TEXT, bg);

    /* Struck through rather than hidden: an item deleted with no network is
     * still on the list until the delete reaches Google, and it should look
     * like something on its way out rather than something still to do. */
    if (T.item[i].deleted) {
      short w = (short)(api->str_len(T.item[i].title) * 6);
      api->fill(rect(c.x + 14, y + 5, w, 1), CLR_DONE);
    }

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

/* Every list's open tasks under its own heading, the way the calendar's
 * agenda groups by day. Read-only on purpose: ticking and adding belong to a
 * list, so there is one place that knows how to push them, and this stays a
 * report you can trust. Enter goes to the list the row belongs to. */
static void paint_all(CRect c) {
  int rows = (c.h - ROW_H) / ROW_H;
  int top = 0, r, y;

  /* The heading a row carries is drawn above it, so a row with a heading is
   * two rows tall -- which the scroll has to know about or the selection
   * walks off the bottom. Counted the simple way: from the top until the
   * selected row fits. */
  while (top < T.osel) {
    int used = 0, i;
    for (i = top; i <= T.osel && i < T.nover; i++)
      used += T.over[i].head ? 2 : 1;
    if (used <= rows) break;
    top++;
  }

  api->fill(rect(c.x, c.y, c.w, c.h - ROW_H), CLR_BG);
  for (r = 0; r < T.nover; r++) T.oy[r] = -1;
  y = c.y;
  for (r = top; r < T.nover && y + ROW_H <= c.y + c.h - ROW_H; r++) {
    OverRow *o = &T.over[r];
    uint16_t bg;
    if (o->head) {
      if (y + 2 * ROW_H > c.y + c.h - ROW_H) break;
      api->text((short)(c.x + 2), (short)(y + 1),
                T.lists[o->list].name, CLR_PEND, CLR_BG);
      y += ROW_H;
    }
    bg = (r == T.osel) ? CLR_SEL : CLR_BG;
    T.oy[r] = (short)y;
    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    api->frame(rect(c.x + 8, y + 2, 7, 7), CLR_DONE);
    api->text((short)(c.x + 19), (short)(y + 2), o->title, CLR_TEXT, bg);
    y += ROW_H;
  }

  if (!T.nover)
    api->text((short)(c.x + 6), (short)(c.y + 6),
              T.nlists ? "nothing to do anywhere" : "no lists yet -- s syncs",
              CLR_DONE, CLR_BG);

  api->fill(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H), CLR_BAR);
  {
    char bar[52];
    api->fmt(bar, sizeof bar, "%d task%s%s   enter opens", T.nover,
             T.nover == 1 ? "" : "s", T.over_full ? "+" : "");
    api->text((short)(c.x + 3), (short)(c.y + c.h - ROW_H + 2), bar,
              CLR_BARFG, CLR_BAR);
  }
}

static void paint_lists(CRect c) {
  int rows = (c.h - ROW_H) / ROW_H, r;

  api->fill(c, CLR_BG);
  for (r = 0; r < rows && r < T.nlists; r++) {
    short y = (short)(c.y + r * ROW_H);
    int sel = (r == T.psel);
    uint16_t bg = sel ? CLR_SEL : ((r & 1) ? CLR_ROW : CLR_BG);
    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    /* A dot marks the one whose tasks are on the card right now. */
    if (r == T.cur) api->fill(rect(c.x + 5, y + 4, 3, 3), CLR_TICK);
    api->text((short)(c.x + 14), (short)(y + 2), T.lists[r].name, CLR_TEXT, bg);
  }
  if (!T.nlists)
    api->text((short)(c.x + 6), (short)(c.y + 6), "no lists yet -- s syncs",
              CLR_DONE, CLR_BG);
  api->fill(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H), CLR_BAR);
  api->text((short)(c.x + 3), (short)(c.y + c.h - ROW_H + 2),
            "enter opens   backspace cancels", CLR_BARFG, CLR_BAR);
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

/* Everything this app can be asked to do, stated once.
 *
 * The ctrl chords, the menu bar, the help panel and the names a script or a
 * model would use all come out of this table -- see CappUi.actions. Before
 * it, the same facts were written three times and nothing checked they
 * agreed.
 *
 * Ctrl belongs entirely to the app now (kernel/drv/keyboard.h), so these
 * chords are safe to claim: the desktop used to take ctrl-S for its Start
 * menu, which is why an editor's save did nothing in a window. */
enum { ACT_ADD = 1, ACT_TICK, ACT_DELETE, ACT_SYNC, ACT_LISTS, ACT_ALL,
       ACT_SAVE, ACT_CANCEL, ACT_OPEN, ACT_GOTO, ACT_PRINT,
       ACT_DONE, ACT_LIST };

/* Commands as well as actions (CAPP_CMD_YES): what `do todo ...`, voice and
 * an AI can ask for, with no screen. `done` and `list` have no menu entry --
 * Tick acts on the row under the cursor, which is not a thing a sentence can
 * name, so the command takes the title. See app_command. */
static const CappParam P_TEXT[]  = { { "text",  CAPP_ARG_TEXT, "what the task says" } };
static const CappParam P_TITLE[] = { { "title", CAPP_ARG_TEXT,
                                       "the task, or enough of it to match" } };

static const CappAction LIST_ACTIONS[] = {
  { "add",    "Add",      "Task", 0x01, ACT_ADD,       /* ctrl-a */
    "add a task to the current list", P_TEXT, 1, CAPP_CMD_YES },
  { "tick",   "Tick",     "Task", 0x14, ACT_TICK },    /* ctrl-t */
  { "delete", "Delete",   "Task", 0x04, ACT_DELETE },  /* ctrl-d */
  { "sync",   "Sync now", "List", 0x13, ACT_SYNC,      /* ctrl-s */
    "sync every list with Google", 0, 0, CAPP_CMD_YES | CAPP_CMD_NET },
  { "lists",  "Lists...", "List", 0x0C, ACT_LISTS },   /* ctrl-l */
  { "all",    "All lists", "List", 0x0F, ACT_ALL },   /* ctrl-o */
  { "print",  "Print",    "List", CAPP_KEY_PRINT, ACT_PRINT },  /* fn-p */
  { "done",   "Done",     0,      0,    ACT_DONE,
    "tick off the task whose title matches", P_TITLE, 1, CAPP_CMD_YES },
  { "list",   "List",     0,      0,    ACT_LIST,
    "the open tasks in the current list", 0, 0, CAPP_CMD_YES },
};

/* The overview. Reading, and a way into the list a row belongs to. */
static const CappAction ALL_ACTIONS[] = {
  { "goto",   "Open list", "All", 0, ACT_GOTO },
  { "print",  "Print",     "All", CAPP_KEY_PRINT, ACT_PRINT },  /* fn-p */
  { "cancel", "Back",      "All", 0, ACT_CANCEL },
};

/* Choosing a list. */
static const CappAction PICK_ACTIONS[] = {
  { "open",   "Open",   "Lists", 0, ACT_OPEN },
  { "cancel", "Cancel", "Lists", 0, ACT_CANCEL },
};

/* While typing, the only two things that apply. */
static const CappAction ADD_ACTIONS[] = {
  { "save",   "Save",   "Edit", 0, ACT_SAVE },
  { "cancel", "Cancel", "Edit", 0, ACT_CANCEL },
};

static const TbIcon LIST_ICONS[] = { { "+", ACT_ADD } };

#define NLIST ((int)(sizeof LIST_ACTIONS / sizeof LIST_ACTIONS[0]))
#define NADD  ((int)(sizeof ADD_ACTIONS / sizeof ADD_ACTIONS[0]))
#define NPICK ((int)(sizeof PICK_ACTIONS / sizeof PICK_ACTIONS[0]))
#define NALL  ((int)(sizeof ALL_ACTIONS / sizeof ALL_ACTIONS[0]))

static void use_list_menus(void) { toolbar_set(LIST_ACTIONS, NLIST, LIST_ICONS, 1); }
static void use_add_menus(void)  { toolbar_set(ADD_ACTIONS, NADD, 0, 0); }
static void use_pick_menus(void) { toolbar_set(PICK_ACTIONS, NPICK, 0, 0); }
static void use_all_menus(void)  { toolbar_set(ALL_ACTIONS, NALL, 0, 0); }

static int do_action(int a);

/* ---- paper --------------------------------------------------------------- */

/* One line onto the page: a prefix ("# ", "[ ] ", "") and the text. Refuses
 * quietly at the end, so a page that stops short is still a page. */
static int page_line(int at, const char *prefix, const char *text) {
  int n;
  if (at >= PAGE_MAX - 1) return at;
  n = api->fmt(T.page + at, (size_t)(PAGE_MAX - at), "%s%s\n", prefix, text);
  if (n < 0) return at;
  if (at + n > PAGE_MAX - 1) { T.page[at] = 0; return at; }
  return at + n;
}

static int page_footer(int at) {
  CappTime t;
  char when[32];
  static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  api->now(&t);
  if (t.synced && t.month >= 1 && t.month <= 12)
    api->fmt(when, sizeof when, "%d %s %02d:%02d", t.day, MON[t.month - 1], t.hour, t.min);
  else
    api->fmt(when, sizeof when, "%s", "undated");
  at = page_line(at, "---", "");
  at = page_line(at, "printed ", when);
  return at;
}

/* The list on screen: open tasks as boxes to tick on paper, then the done
 * ones under a rule, ticked -- they are part of the list's story, and paper
 * has no grey to fade them into. */
static void page_list(void) {
  int i, at = 0, done = 0;
  at = page_line(at, "# ", T.cur >= 0 ? T.lists[T.cur].name : "Todo");
  for (i = 0; i < T.n; i++) {
    if (T.item[i].deleted || T.item[i].done) { done += T.item[i].done; continue; }
    at = page_line(at, "[ ] ", T.item[i].title);
  }
  if (done) {
    at = page_line(at, "---", "");
    for (i = 0; i < T.n; i++)
      if (T.item[i].done && !T.item[i].deleted)
        at = page_line(at, "[x] ", T.item[i].title);
  }
  page_footer(at);
}

/* Every list's open tasks under its own heading, as the overview shows. */
static void page_all(void) {
  int i, at = 0;
  at = page_line(at, "# ", "All lists");
  for (i = 0; i < T.nover; i++) {
    if (T.over[i].head)
      at = page_line(at, "## ", T.lists[T.over[i].list].name);
    at = page_line(at, "[ ] ", T.over[i].title);
  }
  if (T.over_full) at = page_line(at, "...", "");
  page_footer(at);
}

static void print_page(void) {
  int rc;
  if (T.view == VIEW_ALL) page_all(); else page_list();
  rc = api->print(T.page);
  if (rc == 0)       { T.printing = 1; say("printing..."); }
  else if (rc == -1) say("still printing the last one");
  else if (rc == -2) say("no printer: print scan in the console");
  else               say("could not print: no memory");
}

static void app_paint(void *st, CRect c) {
  char bar[64];
  (void)st;

  {
    CRect full = c;
    /* Only the dropdown moved: draw it and nothing else. Repainting the
     * content underneath first is what made the menu flicker. */
    if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
    /* Only the bar changed -- the busy dots ticking over. Leave the content. */
    if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
    toolbar_paint_bar(full);
    c = toolbar_rest(full);

    T.content = c;               /* what damage() has to be expressed in */
    if (T.view == VIEW_ADD) paint_add(c);
    else if (T.view == VIEW_ALL) paint_all(c);
    else if (T.view == VIEW_LISTS) paint_lists(c);
    else {
      paint_list(c);

      api->fill(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H), CLR_BAR);
      api->fmt(bar, sizeof bar, "%s%s%s%s", T.online ? "" : "offline  ",
               T.cur >= 0 ? T.lists[T.cur].name : "",
               T.cur >= 0 ? "  " : "", T.status);
      api->text((short)(c.x + 3), (short)(c.y + c.h - ROW_H + 2), bar,
                T.online ? CLR_BARFG : CLR_WARN, CLR_BAR);
    }
    /* Last: a dropdown is drawn over the content it covers. */
    toolbar_paint_menu(full);
  }
}

/* ---- input --------------------------------------------------------------- */

static int app_key(void *st, unsigned char k);

/* The one place that knows what anything does. */
static int do_action(int a) {
  switch (a) {
  case ACT_ADD:
    T.draft[0] = 0;
    T.draft_len = 0;
    T.view = VIEW_ADD;
    use_add_menus();
    return 1;
  case ACT_TICK:   toggle(); return 1;
  case ACT_DELETE: delete_selected(); return 1;
  case ACT_SYNC:   sync_begin(); return 1;
  case ACT_LISTS:
    T.psel = T.cur < 0 ? 0 : T.cur;
    T.view = VIEW_LISTS;
    use_pick_menus();
    return 1;
  case ACT_OPEN:
    select_list(T.psel);
    T.view = VIEW_LIST;
    use_list_menus();
    return 1;
  case ACT_ALL:
    over_build();
    T.view = VIEW_ALL;
    use_all_menus();
    return 1;
  case ACT_GOTO:
    /* Into the list the highlighted row belongs to, with that list's own
     * selection left where it was: the overview is a way in, not a cursor
     * shared between views. */
    if (T.osel >= 0 && T.osel < T.nover) select_list(T.over[T.osel].list);
    T.view = VIEW_LIST;
    use_list_menus();
    return 1;
  case ACT_PRINT:  print_page(); return 1;
  case ACT_SAVE:
    add_draft();                       /* returns to the list itself */
    use_list_menus();
    return 1;
  case ACT_CANCEL:
    T.view = VIEW_LIST;
    use_list_menus();
    return 1;
  default: return 0;
  }
}

/* Only what is not in the action table: movement, and the bare letters this
 * list has always answered to. The ctrl chords never reach here -- the shell
 * matches them against the table first. */
static int key_list(unsigned char k) {
  /* A held arrow keeps moving; a held space or `d` must not keep toggling
   * or deleting. Everything below the arrows is a decision, not a step. */
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 0;
  switch (k) {
  case CAPP_KEY_UP:   if (T.sel > 0) T.sel--; return 1;
  case CAPP_KEY_DOWN: if (T.sel + 1 < T.n) T.sel++; return 1;
  case CAPP_KEY_LEFT:  select_list(T.cur - 1); return 1;
  case CAPP_KEY_RIGHT: select_list(T.cur + 1); return 1;
  case CAPP_KEY_ENTER:
  case ' ':           return do_action(ACT_TICK);
  case 'a': case 'A': return do_action(ACT_ADD);
  case 's': case 'S': return do_action(ACT_SYNC);
  case 'l': case 'L': return do_action(ACT_LISTS);
  case 'o': case 'O': return do_action(ACT_ALL);
  case 'p': case 'P': return do_action(ACT_PRINT);
  case 'd': case 'D':
  case 0x7F:          return do_action(ACT_DELETE);
  default: return 0;
  }
}

/* Typing. Letters are letters here -- that is the whole reason actions and
 * keys are separate. */
static int key_add(unsigned char k) {
  if (k == CAPP_KEY_ENTER) return do_action(ACT_SAVE);
  /* Escape abandons the draft, not the app. It used to leave Todo entirely
   * from here, which is a poor answer to "no, not that one". */
  if (k == CAPP_KEY_ESC) return do_action(ACT_CANCEL);
  if (k == CAPP_KEY_BACK) {
    if (T.draft_len > 0) { T.draft[--T.draft_len] = 0; return 1; }
    return do_action(ACT_CANCEL);
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

/* The picker. Escape reaches an app before the shell acts on it now, so it
 * means "back to the list" here -- and the shell only leaves the app when a
 * view declines it, which the list view does. */
static int key_pick(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (T.psel > 0) T.psel--; return 1;
  case CAPP_KEY_DOWN: if (T.psel + 1 < T.nlists) T.psel++; return 1;
  case CAPP_KEY_ENTER: return do_action(ACT_OPEN);
  case CAPP_KEY_ESC:
  case CAPP_KEY_BACK:
  case 'l': case 'L':  return do_action(ACT_CANCEL);
  default: return 0;
  }
}

static int key_all(unsigned char k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 0;
  switch (k) {
  case CAPP_KEY_UP:   if (T.osel > 0) T.osel--; return 1;
  case CAPP_KEY_DOWN: if (T.osel + 1 < T.nover) T.osel++; return 1;
  case CAPP_KEY_ENTER: return do_action(ACT_GOTO);
  case CAPP_KEY_ESC:
  case CAPP_KEY_BACK:
  case 'o': case 'O':  return do_action(ACT_CANCEL);
  case 's': case 'S':  return do_action(ACT_SYNC);
  case 'p': case 'P':  return do_action(ACT_PRINT);
  default: return 0;
  }
}

/* The bar first, and it answers for every key while it has them. fn-b puts
 * the keyboard in it; an item chosen there becomes an action, the same one a
 * click would have produced. */
static int menu_key(unsigned char k, int *handled) {
  int a = toolbar_key(k);
  *handled = 1;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  *handled = 0;
  return 0;
}

static int app_key(void *st, unsigned char k) {
  int handled, r;
  (void)st;
  r = menu_key(k, &handled);
  if (handled) return r;
  if (T.view == VIEW_ADD) return key_add(k);
  if (T.view == VIEW_LISTS) return key_pick(k);
  if (T.view == VIEW_ALL) return key_all(k);
  /* The list is the top level: it declines Escape, and the shell takes that
   * as "leave the app". Every view above returns 1 for it and goes back a
   * step instead, which is the whole point of the app seeing it first. */
  return key_list(k);
}

/* What the shell calls for a chord out of the table, and what the menu bar
 * and any script reach through. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
}

/* Lower-case compare of `needle` somewhere in `hay`. */
static int contains(const char *hay, const char *needle) {
  int i, j;
  for (i = 0; hay[i]; i++) {
    for (j = 0; needle[j]; j++) {
      char a = hay[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) break;
    }
    if (!needle[j]) return 1;
    if (!hay[i + j]) return 0;
  }
  return 0;
}

static const char *list_name(void) {
  return T.cur >= 0 ? T.lists[T.cur].name : "the list";
}

/* The commands (see LIST_ACTIONS): the same functions the screen uses, with
 * arguments instead of a draft or a cursor. The OS has already checked the
 * count and the types. Everything here is local -- the card is the truth and
 * a dirty item is pushed by the next sync -- except `sync`, which is the one
 * that needs the network and so answers PENDING and finishes from tick. */
static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  int i, hit = -1, hits = 0;
  size_t o = 0;
  (void)st;
  (void)argc;

  switch (action) {
  case ACT_ADD:
    if (add_text(argv[0]) != 0) {
      api->fmt(out, n, "%s is full (%d tasks)", list_name(), MAX_ITEMS);
      return -1;
    }
    api->fmt(out, n, "added \"%s\" to %s", T.item[0].title, list_name());
    return 0;

  case ACT_LIST:
    for (i = 0; i < T.n && o + 4 < n; i++) {
      if (T.item[i].done || T.item[i].deleted) continue;
      o += (size_t)api->fmt(out + o, n - o, "- %s\n", T.item[i].title);
    }
    if (!o) api->fmt(out, n, "nothing left to do in %s", list_name());
    return 0;

  case ACT_DONE:
    for (i = 0; i < T.n; i++) {
      if (T.item[i].done || T.item[i].deleted) continue;
      if (contains(T.item[i].title, argv[0])) { hit = i; hits++; }
    }
    if (!hits) {
      api->fmt(out, n, "no open task matches \"%s\"", argv[0]);
      return -1;
    }
    if (hits > 1) {
      api->fmt(out, n, "%d tasks match \"%s\"; say more of it", hits, argv[0]);
      return -1;
    }
    T.item[hit].done = 1;
    T.item[hit].dirty = 1;
    cache_save();
    api->fmt(out, n, "ticked off \"%s\"", T.item[hit].title);
    return 0;

  case ACT_SYNC:
    if (T.stage != SYNC_IDLE) { api->fmt(out, n, "already syncing"); return -1; }
    sync_begin();
    if (T.stage == SYNC_IDLE) {                 /* could not start: says why */
      api->fmt(out, n, "%s", T.status);
      return -1;
    }
    T.cmd_waiting = 1;
    return CAPP_CMD_PENDING;
  }
  api->fmt(out, n, "todo has no command %d", action);
  return -1;
}

static int app_click(void *st, short x, short y, int button) {
  int row;
  int i;
  (void)st; (void)button;

  /* The strip first. A hit is an action, so this works identically whether
   * the app is showing a list or a text field. */
  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;             /* opened or closed a menu */
    if (a != TB_NONE) return do_action(a);
  }
  y = (short)(y - toolbar_h());
  row = y / ROW_H;

  if (T.view == VIEW_ALL) {
    int r;
    for (r = 0; r < T.nover; r++) {
      if (T.oy[r] < 0) continue;
      if (y < T.oy[r] || y >= T.oy[r] + ROW_H) continue;
      /* The first click selects, a click on the row already selected opens
       * it -- the same two-step the flat list uses for its tick box, so a
       * mis-aimed tap does not change which list you are looking at. */
      if (r == T.osel) return do_action(ACT_GOTO);
      T.osel = r;
      return 1;
    }
    return 0;
  }
  if (T.view == VIEW_LISTS) {
    if (row < 0 || row >= T.nlists) return 0;
    T.psel = row;
    return do_action(ACT_OPEN);
  }
  if (T.view != VIEW_LIST) return 0;
  /* The bottom strip is the status line, not a task. */
  if (row >= T.shown_rows) return 0;
  /* Through the scroll offset the list was last painted with -- without this
   * a click on a scrolled list ticks off a different task than the one under
   * the pointer, which is a nasty way to lose a to-do. */
  i = T.shown_top + row;
  if (i < 0 || i >= T.n) return 0;
  /* A click on the box ticks; a click on the text selects. Two targets in one
   * row, which is what the box is for. */
  if (x < 14 && i == T.sel) { toggle(); return 1; }
  T.sel = i;
  if (x < 14) toggle();
  return 1;
}

/* The strip appears the moment a mouse does, which is also the moment the
 * layout shifts down -- hence the repaint. */
/* Collects an in-flight request and starts one when it is due. The first is
 * on open, because the reason to open a list is to see what is on it. */
/* The status strip, and nothing else.
 *
 * This is the flicker the user saw, and it was the OS doing exactly what it
 * was told: an app that asks for a repaint without marking anything gets its
 * whole rectangle (kernel/app/capprun.c), and toolbar_damage_bar marks
 * nothing until a mouse has appeared and the bar exists. So a sync redrew
 * every task, several times a second, and in a window the frame with them --
 * which is why it was worst there. The strip is the only thing that changes
 * while a sync runs, so the strip is what gets marked. */
static void damage_status(void) {
  CRect c = T.content;
  if (c.w <= 0 || c.h < ROW_H) return;
  api->damage(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H));
}

static int status_changed(void) {
  const char *a = T.status, *b = T.last_status;
  while (*a && *a == *b) { a++; b++; }
  if (!*a && !*b) return 0;
  api->fmt(T.last_status, sizeof T.last_status, "%s", T.status);
  return 1;
}

static int app_tick(void *st, uint32_t now_ms) {
  int was = T.stage, n = T.n, redraw = 0;
  (void)st;

  sync_tick();
  toolbar_busy(T.stage != SYNC_IDLE);

  /* A `sync` command asked, and the sweep has ended: its status is the
   * answer ("synced", or why not). */
  if (T.cmd_waiting && T.stage == SYNC_IDLE) {
    T.cmd_waiting = 0;
    api->command_done(0, T.status);
  }

  /* While a page is printing its progress is the strip; the last word --
   * "printed", or why not -- stays until the next sync writes over it. */
  if (T.printing) {
    const char *ps = api->print_status();
    say(ps);
    if (!(ps[0] == 's' || ps[0] == 'c' || (ps[0] == 'p' && ps[5] == 'i')))
      T.printing = 0;                 /* not starting/connecting/printing */
  }

  /* One sweep, at open. Everything the app is going to fetch, it fetches
   * now: the lists, the list on screen, then the rest into their files. What
   * used to happen instead was a pull of the current list every ten minutes,
   * which rearranged the screen under whoever was reading it and still left
   * every other list unfetched. `s` is how you ask for another.
   *
   * The exception is a sweep that could not start at all -- no radio yet, no
   * token yet -- which is not a sweep and is retried shortly. */
  /* Not when started only to run a command: `add` is local, and a headless
   * instance that swept every list would be the slow thing commands exist to
   * avoid. The `sync` command starts one itself. */
  if (T.stage == SYNC_IDLE && !T.synced_once && !api->headless() &&
      (!T.tried_once || (int32_t)(now_ms - T.next_auto) >= 0))
    sync_begin();

  if (was != T.stage || n != T.n) redraw = 1;
  if (status_changed()) { damage_status(); redraw = 1; }
  /* The bar's dots animate off the clock, so while one is in the air the bar
   * asks for itself back -- and only the bar. When there is no bar (no mouse
   * has appeared) this marks nothing and asks for nothing, which is the
   * whole fix. */
  if (T.stage != SYNC_IDLE && toolbar_bar_rect().w) {
    toolbar_damage_bar();
    redraw = 1;
  }
  return redraw;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons; (void)wheel;
  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  return changed;
}

static int app_wants_text(void *st) {
  (void)st;
  /* Not while the menu has the keyboard: a spoken sentence or a Bluetooth
   * keyboard would otherwise type into a draft that is not on screen. */
  if (toolbar_has_keys()) return 0;
  return T.view == VIEW_ADD;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_NET,   /* a window by default; the title bar's box fills the screen */
  "Todo",
  /* 16x16: a clipboard with a tick. */
  { 0x07, 0xE0, 0x0C, 0x30, 0x1F, 0xF8, 0x30, 0x0C,
    0x30, 0x0C, 0x37, 0x8C, 0x33, 0x0C, 0x30, 0x0C,
    0x36, 0x0C, 0x33, 0x0C, 0x31, 0x8C, 0x30, 0xCC,
    0x30, 0x6C, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tmove\nenter\ttick it off\na\tadd a task\nd\tdelete / undo\n"
  "s\tsync every list\nleft/right\tnext list\nl\tchoose a list\n"
  "o\tall lists at once\np\tprint it\nescape\tback a level\nfn-`\tleave\n",
  LIST_ACTIONS,
  sizeof LIST_ACTIONS / sizeof LIST_ACTIONS[0],
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
  api->mkdir(CACHE_DIR);
  api->mkdir(STATE_DIR);
  T.sweep = -1;
  lists_load();
  cache_load();
  /* From the cache files, so `o` shows everything the last sweep left even
   * on a device that has not been online since. */
  over_build();
  api->fmt(T.status, sizeof T.status, "%d cached -- s syncs", T.n);

  toolbar_init(api, LIST_ACTIONS, NLIST, LIST_ICONS, 1);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.actions = LIST_ACTIONS;
  UI.nactions = NLIST;
  UI.action = app_action;
  UI.command = app_command;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
