/* Notes -- the server's notes (server/notes.py), kept as files on the card.
 *
 * Each note is /home/notes/NAME.md: Edit opens it, with its preview, its
 * printing and everything else Edit does, and Notes keeps the files and the
 * server in step. The dashboard edits the same notes from a browser.
 *
 * Sync is by hash. /home/notes/.sync remembers, for every note, its id on
 * the server, its file, and the FNV-1a hash of its text when the two last
 * agreed. Against that, a sync sees what changed where:
 *
 *   changed here only          sent up
 *   changed on the server only fetched down
 *   changed on both            the server's wins the file; ours is kept as a
 *                              new note, NAME (conflict), so nothing is lost
 *   deleted on one side        deleted on the other, unless it was changed
 *                              on that other side, when it comes back
 *   new on either side         copied to the other
 *
 * The plan is worked out at once, then carried out an operation a tick so
 * the screen says how far it got.
 *
 * Voice: G0 held on the list makes the words a new note (CappUi.button).
 * m turns a voice memo into a note: the server transcribes it (Whisper) and
 * keeps it, and the next sync brings it here. `notes memo PATH` does the
 * same for the Memo app.
 *
 * Keys are the shared vocabulary (CLAUDE.md): r syncs (s too, as in Todo
 * and Calendar), d deletes after asking, p or fn-p prints the note, Escape
 * stops a sync. Every operation is also a menu item (fn-b, or a mouse).
 *
 * The sync at open waits for the first paint: it is a blocking request, and
 * starting it in capp_main left the screen empty until the server answered.
 * Each operation after that is one blocking request a tick. Turning them
 * into http_start/http_poll would make every case of do_op a state of its
 * own; a note is small and the server is near, so it has not been worth it.
 */

#include "kernel/app/capp.h"
#include "apps/safefile.h"
#include "apps/toolbar.h"
#include "apps/footer.h"
#include "apps/confirm.h"

static const CardApi *api;

#define DIR        "/home/notes"
#define INDEX      DIR "/.sync"
#define MEMO_DIR   "/home/memos"
#define MAX_NOTES  48
#define MAX_OPS    (MAX_NOTES * 2)
/* A note longer than this is not synced. It was 16 KB, and Notes' data --
 * this buffer most of it -- was one 40 KB block, which the heap had stopped
 * having in one piece after a few hours of the radios coming and going:
 * Notes would not open at all (2026-10-02). 8 KB of text is a long note. */
#define TEXT_MAX   8192
#define FILE_MAX   48
#define TITLE_MAX  40
#define ROW_H      14
#define TOP_H      16
#define KEY_DEL    0x7F                 /* Delete on a Bluetooth keyboard */

#define CLR_BG     CAPP_RGB(18, 18, 22)
#define CLR_TEXT   CAPP_RGB(232, 232, 236)
#define CLR_DIM    CAPP_RGB(128, 132, 146)
#define CLR_SEL    CAPP_RGB(48, 52, 70)
#define CLR_ACC    CAPP_RGB(250, 200, 90)
#define CLR_BAD    CAPP_RGB(240, 110, 96)

typedef struct {
  char id[12];
  char file[FILE_MAX];          /* the name in DIR, with .md */
  char hash[9];                 /* the text's hash when both sides last agreed */
} Idx;

typedef struct {
  char id[12];
  char hash[9];
  char title[TITLE_MAX + 1];
} Srv;

enum { OP_DOWN = 1, OP_DOWN_NEW, OP_UP, OP_UP_NEW, OP_DEL_SRV, OP_DEL_LOCAL, OP_CONFLICT };

typedef struct {
  uint8_t op;
  int8_t  i;                    /* into idx, or -1 */
  int8_t  s;                    /* into srv, or -1 */
  char    file[FILE_MAX];       /* OP_UP_NEW from a file not yet in the index */
} Op;

static struct {
  Idx  idx[MAX_NOTES];
  int  nidx;
  Srv  srv[MAX_NOTES];
  int  nsrv;
  Op   op[MAX_OPS];
  int  nop, at;                 /* the plan, and how far into it */
  int  syncing;

  char rows[MAX_NOTES][TITLE_MAX + 1];  /* what the list shows: each file's first line */
  char rowfile[MAX_NOTES][FILE_MAX];
  int  nrows, sel, top;

  int  picking;                 /* the memo picker is up */
  int  ask_delete;
  int  printing;                /* a print is out; its progress is the status */
  int  sync_soon;               /* sync on the next tick, after a paint */
  int  memo_soon;               /* send N.memo on the next tick */
  char memo[96];                /* the memo the picker chose */
  int  bad;
  char status[60];
  char text[TEXT_MAX];
  char line[160];
  CappEntry ent[MAX_NOTES + 8];
  CRect full;                   /* the app's rectangle, menu bar included */
  CRect content;                /* below the menu bar */
  int   have_at;                /* painted at least once: damage can be marked */
  int   fit;                    /* how many rows fit, from the last paint */
} N;

/* ---- small things -------------------------------------------------------------- */

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int ends_md(const char *s) {
  int n = (int)api->str_len(s);
  return n > 3 && s[n - 3] == '.' && s[n - 2] == 'm' && s[n - 1] == 'd';
}

/* ---- what changed -------------------------------------------------------------
 *
 * The list was repainted whole for every key and every tick of a sync. Each
 * change marks what it touched and the shell clips the next paint to that
 * (apps/files.c is the pattern). Nothing is marked before the first paint --
 * there is nothing on screen yet, and the host tests never paint. */

static void damage_all(void) { if (N.have_at) api->damage(N.content); }

/* The title line, which carries the status. */
static void damage_top(void) {
  if (N.have_at) api->damage(rect(N.content.x, N.content.y, N.content.w, TOP_H));
}

static void damage_footer(void) {
  if (N.have_at)
    api->damage(rect(N.content.x, N.content.y + N.content.h - FOOT_H, N.content.w, FOOT_H));
}

static void damage_row(int i) {
  if (!N.have_at || i < N.top || i >= N.top + N.fit) return;
  api->damage(rect(N.content.x, N.content.y + TOP_H + (i - N.top) * ROW_H,
                   N.content.w, ROW_H));
}

static void say(int bad, const char *s) {
  N.bad = bad;
  api->fmt(N.status, sizeof N.status, "%s", s);
  damage_top();
}

/* FNV-1a 32 as eight hex digits: server/notes.py's fnv(). */
static void fnv_hex(const char *s, int n, char out[9]) {
  static const char HEX[] = "0123456789abcdef";
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  for (i = 7; i >= 0; i--) { out[i] = HEX[h & 15]; h >>= 4; }
  out[8] = 0;
}

/* Field `i` of a tab-separated line. */
static void field(const char *line, int i, char *out, int n) {
  int k = 0;
  while (i > 0 && *line && *line != '\n') { if (*line++ == '\t') i--; }
  while (*line && *line != '\t' && *line != '\n' && k < n - 1) out[k++] = *line++;
  out[k] = 0;
}

static void path_of(const char *file, char *out, int n) {
  api->fmt(out, (size_t)n, "%s/%s", DIR, file);
}

/* A file's text into N.text: its length, or -1 (missing, or too long). */
static int read_file(const char *file) {
  char path[96];
  int fd, n = 0, r;
  path_of(file, path, sizeof path);
  if ((fd = safe_open_read(api, path)) < 0) return -1;
  while (n < TEXT_MAX - 1 && (r = api->read(fd, N.text + n, (size_t)(TEXT_MAX - 1 - n))) > 0) n += r;
  r = api->read(fd, N.line, 1);         /* anything left: too long to carry */
  api->close(fd);
  N.text[n] = 0;
  return r > 0 ? -1 : n;
}

static int write_file(const char *file, const char *text) {
  SafeFile f;
  char path[96];
  path_of(file, path, sizeof path);
  if (safe_begin(&f, api, path) != 0) return -1;
  safe_line(&f, text);
  return safe_commit(&f);
}

static int file_exists(const char *file) {
  CappStat st;
  char path[96];
  path_of(file, path, sizeof path);
  return api->stat(path, &st) == 0;
}

/* A file name from a title: letters, digits, spaces, - and _, and unique. */
static void name_for(const char *title, const char *suffix, char *out, int n) {
  char base[TITLE_MAX + 1];
  int k = 0, i, tries;
  for (i = 0; title[i] && k < 32; i++) {
    char c = title[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == ' ' || c == '-' || c == '_')
      base[k++] = c;
  }
  while (k && base[k - 1] == ' ') k--;
  base[k] = 0;
  if (!base[0]) api->fmt(base, sizeof base, "Untitled");
  api->fmt(out, (size_t)n, "%s%s.md", base, suffix);
  for (tries = 2; file_exists(out) && tries < 100; tries++)
    api->fmt(out, (size_t)n, "%s%s %d.md", base, suffix, tries);
}

/* ---- the index ------------------------------------------------------------------- */

static void load_index(void) {
  int fd, n, len = 0, i;
  char buf[256], ln[96];
  N.nidx = 0;
  if ((fd = safe_open_read(api, INDEX)) < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0)
    for (i = 0; i < n; i++) {
      if (buf[i] != '\n') { if (len < (int)sizeof ln - 1) ln[len++] = buf[i]; continue; }
      ln[len] = 0;
      len = 0;
      if (N.nidx < MAX_NOTES && ln[0]) {
        Idx *x = &N.idx[N.nidx];
        field(ln, 0, x->id, sizeof x->id);
        field(ln, 1, x->hash, sizeof x->hash);
        field(ln, 2, x->file, sizeof x->file);
        if (x->id[0] && x->file[0]) N.nidx++;
      }
    }
  api->close(fd);
}

static void save_index(void) {
  SafeFile f;
  int i;
  if (safe_begin(&f, api, INDEX) != 0) return;
  for (i = 0; i < N.nidx; i++) {
    api->fmt(N.line, sizeof N.line, "%s\t%s\t%s\n", N.idx[i].id, N.idx[i].hash, N.idx[i].file);
    safe_line(&f, N.line);
  }
  safe_commit(&f);
}

static int idx_by_id(const char *id) {
  int i;
  for (i = 0; i < N.nidx; i++) if (same(N.idx[i].id, id)) return i;
  return -1;
}

static int idx_by_file(const char *file) {
  int i;
  for (i = 0; i < N.nidx; i++) if (same(N.idx[i].file, file)) return i;
  return -1;
}

static void idx_set(const char *id, const char *file, const char *hash) {
  int i = idx_by_id(id);
  if (i < 0) {
    if (N.nidx >= MAX_NOTES) return;
    i = N.nidx++;
  }
  api->fmt(N.idx[i].id, sizeof N.idx[i].id, "%s", id);
  api->fmt(N.idx[i].file, sizeof N.idx[i].file, "%s", file);
  api->fmt(N.idx[i].hash, sizeof N.idx[i].hash, "%s", hash);
}

static void idx_drop(int i) {
  if (i < 0 || i >= N.nidx) return;
  api->mem_move(&N.idx[i], &N.idx[i + 1], (size_t)(N.nidx - i - 1) * sizeof N.idx[0]);
  N.nidx--;
}

/* ---- the list ---------------------------------------------------------------------- */

static void first_line(const char *file, char *out, int n) {
  char path[96], buf[64];
  int fd, r, k = 0, i = 0;
  path_of(file, path, sizeof path);
  out[0] = 0;
  if ((fd = safe_open_read(api, path)) < 0) return;
  r = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (r <= 0) return;
  buf[r] = 0;
  while (buf[i] == '#' || buf[i] == ' ') i++;
  while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && k < n - 1) out[k++] = buf[i++];
  out[k] = 0;
}

static void load_rows(void) {
  int n, i, j;
  N.nrows = 0;
  n = api->list_ex(DIR, N.ent, MAX_NOTES + 8);
  for (i = 0; i < n && N.nrows < MAX_NOTES; i++) {
    if (N.ent[i].is_dir || !ends_md(N.ent[i].name)) continue;
    api->fmt(N.rowfile[N.nrows], FILE_MAX, "%s", N.ent[i].name);
    first_line(N.ent[i].name, N.rows[N.nrows], TITLE_MAX + 1);
    if (!N.rows[N.nrows][0]) api->fmt(N.rows[N.nrows], TITLE_MAX + 1, "%s", N.ent[i].name);
    N.nrows++;
  }
  /* A to Z by what the list shows. */
  for (i = 1; i < N.nrows; i++)
    for (j = i; j > 0; j--) {
      const char *a = N.rows[j - 1], *b = N.rows[j];
      int k = 0;
      char ca, cb, t[TITLE_MAX + 1], tf[FILE_MAX];
      while (a[k] && ((a[k] | 32) == (b[k] | 32))) k++;
      ca = (char)(a[k] | 32); cb = (char)(b[k] | 32);
      if (ca <= cb) break;
      api->mem_cpy(t, N.rows[j], sizeof t);
      api->mem_cpy(N.rows[j], N.rows[j - 1], sizeof t);
      api->mem_cpy(N.rows[j - 1], t, sizeof t);
      api->mem_cpy(tf, N.rowfile[j], sizeof tf);
      api->mem_cpy(N.rowfile[j], N.rowfile[j - 1], sizeof tf);
      api->mem_cpy(N.rowfile[j - 1], tf, sizeof tf);
    }
  if (N.sel >= N.nrows) N.sel = N.nrows ? N.nrows - 1 : 0;
  damage_all();
}

/* ---- the server ------------------------------------------------------------------- */

static int ask(const char *method, const char *rel, const char *body, char *out, int n) {
  char url[128];
  api->fmt(url, sizeof url, "%s%s", api->proxy(), rel);
  return api->http(method, url, body, body ? "text/plain" : 0, "", out, (size_t)n, 20000);
}

/* Did the note just fetched come whole? Its hash against the server's: a
 * note longer than TEXT_MAX arrives cut off, and written to the card, then
 * edited and sent back, the cut copy would have replaced the whole one. */
static int whole(const Srv *s) {
  char h[9];
  fnv_hex(N.text, (int)api->str_len(N.text), h);
  if (same(h, s->hash)) return 1;
  api->fmt(N.status, sizeof N.status, "%.24s: too long for here", s->title);
  N.bad = 1;
  return 0;
}

static int fetch_list(void) {
  const char *p;
  int r = ask("GET", "/notes", 0, N.text, TEXT_MAX);
  if (r < 0) return r;
  N.nsrv = 0;
  for (p = N.text; *p && N.nsrv < MAX_NOTES; ) {
    Srv *s = &N.srv[N.nsrv];
    field(p, 0, s->id, sizeof s->id);
    field(p, 1, s->hash, sizeof s->hash);
    field(p, 3, s->title, sizeof s->title);
    if (s->id[0]) N.nsrv++;
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  return 0;
}

static int srv_by_id(const char *id) {
  int i;
  for (i = 0; i < N.nsrv; i++) if (same(N.srv[i].id, id)) return i;
  return -1;
}

/* ---- the plan --------------------------------------------------------------------- */

static void add_op(int op, int i, int s, const char *file) {
  Op *o;
  if (N.nop >= MAX_OPS) return;
  o = &N.op[N.nop++];
  o->op = (uint8_t)op;
  o->i = (int8_t)i;
  o->s = (int8_t)s;
  api->fmt(o->file, sizeof o->file, "%s", file ? file : "");
}

static void plan(void) {
  int i, n;
  char h[9];
  N.nop = N.at = 0;
  for (i = 0; i < N.nidx; i++) {
    int s = srv_by_id(N.idx[i].id), len = read_file(N.idx[i].file);
    if (len < 0 && !file_exists(N.idx[i].file)) {              /* deleted here */
      if (s >= 0 && same(N.srv[s].hash, N.idx[i].hash)) add_op(OP_DEL_SRV, i, s, 0);
      else if (s >= 0) add_op(OP_DOWN, i, s, 0);                /* changed there: back */
      else add_op(OP_DEL_LOCAL, i, -1, 0);                      /* gone from both */
      continue;
    }
    if (len < 0) continue;                                      /* too long to carry */
    fnv_hex(N.text, len, h);
    if (s < 0) {                                                /* deleted there */
      if (same(h, N.idx[i].hash)) add_op(OP_DEL_LOCAL, i, -1, 0);
      else add_op(OP_UP_NEW, i, -1, N.idx[i].file);             /* changed here: back */
      continue;
    }
    {
      int here = !same(h, N.idx[i].hash), there = !same(N.srv[s].hash, N.idx[i].hash);
      if (here && there && !same(h, N.srv[s].hash)) add_op(OP_CONFLICT, i, s, 0);
      else if (here && !there) add_op(OP_UP, i, s, 0);
      else if (there && !here) add_op(OP_DOWN, i, s, 0);
      else if (here) api->fmt(N.idx[i].hash, sizeof N.idx[i].hash, "%s", h);  /* the same edit */
    }
  }
  for (i = 0; i < N.nsrv; i++)
    if (idx_by_id(N.srv[i].id) < 0) add_op(OP_DOWN_NEW, -1, i, 0);
  n = api->list_ex(DIR, N.ent, MAX_NOTES + 8);
  for (i = 0; i < n; i++)
    if (!N.ent[i].is_dir && ends_md(N.ent[i].name) && idx_by_file(N.ent[i].name) < 0)
      add_op(OP_UP_NEW, -1, -1, N.ent[i].name);
}

/* One operation. 0, or <0 with the status saying why. */
static int do_op(Op *o) {
  char rel[64], h[9], file[FILE_MAX];
  int r, len;
  Srv *s = o->s >= 0 ? &N.srv[o->s] : 0;
  Idx *x = o->i >= 0 ? &N.idx[o->i] : 0;

  switch (o->op) {
  case OP_DOWN:
  case OP_DOWN_NEW:
    api->fmt(rel, sizeof rel, "/notes/note?id=%s", s->id);
    if ((r = ask("GET", rel, 0, N.text, TEXT_MAX)) < 0) return r;
    if (!whole(s)) return 0;               /* too long for here: left on the server */
    if (o->op == OP_DOWN_NEW) name_for(s->title, "", file, sizeof file);
    else api->fmt(file, sizeof file, "%s", x->file);
    if (write_file(file, N.text) != 0) return -2;
    fnv_hex(N.text, (int)api->str_len(N.text), h);
    idx_set(s->id, file, h);
    return 0;

  case OP_UP:
    if ((len = read_file(x->file)) < 0) return -2;
    api->fmt(rel, sizeof rel, "/notes/note?id=%s", x->id);
    if ((r = ask("POST", rel, N.text, N.line, sizeof N.line)) < 0) return r;
    field(N.line, 1, h, sizeof h);
    api->fmt(x->hash, sizeof x->hash, "%s", h);
    return 0;

  case OP_UP_NEW: {
    char id[12];
    api->fmt(file, sizeof file, "%s", o->file);
    if ((len = read_file(file)) < 0) return -2;
    if ((r = ask("POST", "/notes/note", N.text, N.line, sizeof N.line)) < 0) return r;
    field(N.line, 0, id, sizeof id);
    field(N.line, 1, h, sizeof h);
    if (x) idx_drop(o->i);                  /* its old id is gone from the server */
    idx_set(id, file, h);
    return 0;
  }

  case OP_DEL_SRV:
    api->fmt(rel, sizeof rel, "/notes/note?id=%s", x->id);
    if ((r = ask("DELETE", rel, 0, N.line, sizeof N.line)) < 0 && r != -404) return r;
    idx_drop(o->i);
    return 0;

  case OP_DEL_LOCAL: {
    char path[96];
    path_of(x->file, path, sizeof path);
    api->remove(path);
    idx_drop(o->i);
    return 0;
  }

  case OP_CONFLICT: {
    char id[12], base[TITLE_MAX + 1];
    int k;
    /* Ours goes up as a note of its own, under a name that says so... */
    if ((len = read_file(x->file)) < 0) return -2;
    if ((r = ask("POST", "/notes/note", N.text, N.line, sizeof N.line)) < 0) return r;
    field(N.line, 0, id, sizeof id);
    field(N.line, 1, h, sizeof h);
    api->fmt(base, sizeof base, "%s", x->file);
    for (k = 0; base[k]; k++) if (base[k] == '.') { base[k] = 0; break; }
    name_for(base, " (conflict)", file, sizeof file);
    if (write_file(file, N.text) != 0) return -2;
    idx_set(id, file, h);
    /* ...and the server's takes the name. */
    api->fmt(rel, sizeof rel, "/notes/note?id=%s", s->id);
    if ((r = ask("GET", rel, 0, N.text, TEXT_MAX)) < 0) return r;
    if (!whole(s)) return 0;
    if (write_file(x->file, N.text) != 0) return -2;
    fnv_hex(N.text, (int)api->str_len(N.text), h);
    api->fmt(x->hash, sizeof x->hash, "%s", h);
    return 0;
  }
  }
  return 0;
}

static void sync_begin(void) {
  int r;
  if (N.syncing) return;
  if (!api->net_ready() && api->net_connect(15000) != 0) {
    say(1, "offline: showing what is here");
    return;
  }
  say(0, "syncing...");
  load_index();
  if ((r = fetch_list()) < 0) {
    api->fmt(N.status, sizeof N.status, "cannot reach the server (%d)", r);
    N.bad = 1;
    return;
  }
  plan();
  if (!N.nop) {
    save_index();
    say(0, "in step with the server");
    load_rows();
    return;
  }
  N.syncing = 1;
}

/* One operation a tick, so the screen keeps up. 1 to repaint. */
static int sync_tick(void) {
  int r;
  if (!N.syncing) return 0;
  if (N.at >= N.nop) {
    N.syncing = 0;
    save_index();
    load_rows();
    api->fmt(N.status, sizeof N.status, "synced: %d change%s", N.nop, N.nop == 1 ? "" : "s");
    N.bad = 0;
    return 1;
  }
  r = do_op(&N.op[N.at]);
  if (r < 0) {
    N.syncing = 0;
    save_index();
    load_rows();
    api->fmt(N.status, sizeof N.status, r == -2 ? "the card refused a note" :
             "sync stopped: server said %d", r);
    N.bad = 1;
    return 1;
  }
  /* An index entry dropped mid-plan shifts the rest: replan rather than act
   * on stale positions. Cheap -- a plan is reads of small files. */
  if (N.op[N.at].op == OP_DEL_SRV || N.op[N.at].op == OP_DEL_LOCAL ||
      (N.op[N.at].op == OP_UP_NEW && N.op[N.at].i >= 0)) {
    save_index();
    if (fetch_list() < 0) { N.syncing = 0; say(1, "sync stopped: server gone"); return 1; }
    plan();
    if (!N.nop) { N.syncing = 0; save_index(); load_rows(); say(0, "synced"); return 1; }
    return 1;
  }
  N.at++;
  api->fmt(N.status, sizeof N.status, "syncing %d/%d", N.at, N.nop);
  damage_top();
  return 1;
}

/* ---- making notes ------------------------------------------------------------------ */

/* A new note from words: its first line is its title, and its name. */
static int new_note(const char *text, char *file_out, int n) {
  char title[TITLE_MAX + 1];
  int k = 0;
  while (text[k] && text[k] != '\n' && k < TITLE_MAX) { title[k] = text[k]; k++; }
  title[k] = 0;
  name_for(title, "", file_out, n);
  api->fmt(N.text, TEXT_MAX, "%s%s", text, text[0] && text[api->str_len(text) - 1] == '\n' ? "" : "\n");
  return write_file(file_out, N.text);
}

/* Words said or sent as a note: a heading with when, then the words. The
 * words alone were a one-line note, and a note's first line is its title --
 * a sentence arrived as a title over an empty page. Same shape as the
 * server's notes from voice memos. */
static int words_note(const char *kind, const char *words, char *file_out, int n) {
  static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  static char text[1200];        /* fifteen seconds of speech, or a command's words */
  CappTime t;
  api->now(&t);
  if (t.synced && t.month >= 1 && t.month <= 12)
    api->fmt(text, sizeof text, "# %s, %d %s %02d:%02d\n\n%s", kind, t.day, MON[t.month - 1],
             t.hour, t.min, words);
  else
    api->fmt(text, sizeof text, "# %s\n\n%s", kind, words);
  return new_note(text, file_out, n);
}

/* A voice memo, transcribed on the server into a note there; the sync
 * brings it here. The reply names it: id, hash, title. */
static int memo_to_note(const char *path, char *out, int n) {
  char url[128];
  int r;
  if (!api->http_upload) { api->fmt(out, (size_t)n, "this firmware cannot send files: update os"); return -1; }
  if (!api->net_ready() && api->net_connect(15000) != 0) { api->fmt(out, (size_t)n, "offline"); return -1; }
  {
    /* Its name -- MMDD-HHMMSS.wav, Memo's clock -- titles the note. */
    const char *base = path, *p;
    for (p = path; *p; p++) if (*p == '/') base = p + 1;
    api->fmt(url, sizeof url, "%s/notes/audio?name=%s", api->proxy(), base);
  }
  r = api->http_upload(url, path, "audio/wav", N.line, sizeof N.line, 120000);
  if (r < 0) {
    if (N.line[0] == 'e') api->fmt(out, (size_t)n, "%s", N.line + 6);
    else api->fmt(out, (size_t)n, "the server did not take it (%d)", r);
    return -1;
  }
  field(N.line, 2, N.text, 64);
  api->fmt(out, (size_t)n, "noted: %s", N.text);
  return 0;
}

/* ---- the screen ------------------------------------------------------------------- */

static void app_paint(void *st, CRect full) {
  CRect c;
  int y, i, rows;
  (void)st;

  /* Only the dropdown moved, or only the bar: draw that and nothing else.
   * Repainting the list first is what makes a menu flicker. */
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  c = toolbar_rest(full);
  N.full = full;
  N.content = c;
  N.have_at = 1;

  api->fill(rect(c.x, c.y, c.w, TOP_H), CLR_BG);
  api->text((int16_t)(c.x + 6), (int16_t)(c.y + 4), "Notes", CLR_ACC, CLR_BG);
  if (N.status[0]) {
    /* Right of the title and cut to the room there: a long reason from the
     * server used to run back over the title. */
    char s[40];
    int room = (c.w - 54) / 6, w;
    if (room > (int)sizeof s - 1) room = (int)sizeof s - 1;
    if (room < 1) room = 1;
    api->fmt(s, (size_t)room + 1, "%s", N.status);
    w = (int)api->str_len(s) * 6;
    api->text((int16_t)(c.x + c.w - 6 - w), (int16_t)(c.y + 4), s,
              N.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  }
  y = c.y + TOP_H;
  rows = (c.h - TOP_H - FOOT_H) / ROW_H;
  if (rows < 1) rows = 1;
  N.fit = rows;
  if (N.sel < N.top) N.top = N.sel;
  if (N.sel >= N.top + rows) N.top = N.sel - rows + 1;
  for (i = N.top; i < N.nrows && i - N.top < rows; i++, y += ROW_H) {
    uint16_t bg = i == N.sel ? CLR_SEL : CLR_BG;
    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    api->text((int16_t)(c.x + 8), (int16_t)(y + 3), N.rows[i], CLR_TEXT, bg);
  }
  if (!N.nrows) {
    api->fill(rect(c.x, y, c.w, ROW_H * 2), CLR_BG);
    api->text((int16_t)(c.x + 8), (int16_t)(y + 3), "no notes: n for one, or hold G0", CLR_DIM, CLR_BG);
    y += ROW_H * 2;
  }
  if (y < c.y + c.h - FOOT_H) api->fill(rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);

  if (N.ask_delete && N.sel < N.nrows) {
    /* The title cut short enough that the question still fits. */
    char t[24], line[FOOT_CHARS + 1];
    api->fmt(t, sizeof t, "%s", N.rows[N.sel]);
    api->fmt(line, sizeof line, "delete %s? y / n", t);
    footer_paint(api, c, 0);
    api->text((int16_t)(c.x + 4), (int16_t)(c.y + c.h - FOOT_H + 2), line, CLR_BAD, FOOT_BG);
  } else {
    footer_paint(api, c, "enter edit  n new  d delete  r sync");
  }

  /* Last: a dropdown is drawn over the list it covers. */
  toolbar_paint_menu(full);
}

/* ---- doing things ------------------------------------------------------------------ */

static void open_note(const char *file) {
  char path[96];
  path_of(file, path, sizeof path);
  api->run("edit", path);
}

/* The row it left and the row it reached; a scroll is every row. */
static void select_row(int i) {
  if (i < 0 || i >= N.nrows || i == N.sel) return;
  damage_row(N.sel);
  N.sel = i;
  damage_row(N.sel);
  if (N.sel < N.top || N.sel >= N.top + N.fit) damage_all();
}

/* A sync is a blocking request, so it starts on the next tick: by then the
 * screen has been painted, saying "syncing..." rather than nothing. `tell`
 * says so now; a sync that follows a delete or a memo leaves what that said. */
static void request_sync(int tell) {
  if (N.syncing) return;
  N.sync_soon = 1;
  if (tell) say(0, "syncing...");
}

/* Escape during a sync stops it after the operation in hand. What was done
 * is in the index; the rest is worked out again by the next sync, which
 * plans from the files and the server, not from where this one stopped. */
static void stop_sync(void) {
  N.syncing = 0;
  save_index();
  load_rows();
  say(0, "sync stopped: r starts it");
}

static void cancel_delete(void) {
  if (!N.ask_delete) return;
  N.ask_delete = 0;
  damage_footer();
}

static void delete_selected(void) {
  char path[96];
  cancel_delete();
  if (N.sel >= N.nrows) return;
  path_of(N.rowfile[N.sel], path, sizeof path);
  api->remove(path);
  load_rows();
  request_sync(1);                      /* the server forgets it now, not later */
}

/* The note on paper, set as Edit sets it: the file is markdown-shaped
 * already, so `# headings` and `[ ] tasks` print as they read. The job loads
 * the fonts, so leaving Notes mid-print is fine. */
static void print_note(void) {
  int rc;
  if (N.sel >= N.nrows) return;
  if (read_file(N.rowfile[N.sel]) < 0) { say(1, "cannot read that note"); return; }
  rc = api->print_fonts(N.text, "print24", "print24b", "print34b");
  if (rc == 0)       { N.printing = 1; say(0, "printing..."); }
  else if (rc == -1) say(1, "still printing the last one");
  else if (rc == -2) say(1, "no printer: print scan");
  else               say(1, "could not print: no memory");
}

/* ---- the action table -------------------------------------------------------------
 *
 * One table for the menus (fn-b, or a mouse), fn-p, and the commands below.
 * Only print has a chord: a chord is matched before the key handler in
 * every state, and fn-p is the same everywhere. */

enum {
  ACT_SYNC = 1, ACT_MEMO, ACT_ADD, ACT_LIST,
  ACT_OPEN, ACT_NEW, ACT_PICK, ACT_DELETE, ACT_PRINT
};

static const CappParam P_PATH[] = { { "memo", CAPP_ARG_TEXT, "the memo's path, e.g. /home/memos/1001-1324.wav" } };
static const CappParam P_TEXT[] = { { "text", CAPP_ARG_TEXT, "what the note says; its first words are its title" } };

static const CappAction ACTIONS[] = {
  { "open", "Open in Edit", "Note", 0, ACT_OPEN },
  { "new", "New note", "Note", 0, ACT_NEW },
  { "memo.pick", "Memo to note...", "Note", 0, ACT_PICK },
  { "delete", "Delete", "Note", 0, ACT_DELETE },
  { "print", "Print", "Note", CAPP_KEY_PRINT, ACT_PRINT },     /* fn-p */
  { "sync", "Sync now", "Sync", 0, ACT_SYNC, "bring the notes and the server into step", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "memo", "Memo to note", 0, 0, ACT_MEMO, "transcribe a voice memo into a new note", P_PATH, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "add", "New note", 0, 0, ACT_ADD, "a new note with this text", P_TEXT, 1, CAPP_CMD_YES },
  { "list", "List", 0, 0, ACT_LIST, "every note's title", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* The one place that knows what anything does, however it was asked. */
static int do_action(int a) {
  cancel_delete();
  /* Changing the notes while a sync works through its plan would pull files
   * out from under it. Reading and printing are fine. */
  if (N.syncing && (a == ACT_NEW || a == ACT_PICK || a == ACT_DELETE || a == ACT_SYNC)) {
    say(0, "syncing: esc stops it");
    return 1;
  }
  switch (a) {
  case ACT_OPEN:
    if (N.sel < N.nrows) open_note(N.rowfile[N.sel]);
    return 1;
  case ACT_NEW: {
    char file[FILE_MAX];
    if (new_note("New note", file, sizeof file) == 0) { load_rows(); open_note(file); }
    else say(1, "the card refused it");
    return 1;
  }
  case ACT_PICK: {
    static const CappPick p = { CAPP_PICK_OPEN, "Memo to note", MEMO_DIR, "wav", 0 };
    if (api->pick(&p) == 0) N.picking = 1;
    return 1;
  }
  case ACT_DELETE:
    if (N.nrows) { N.ask_delete = 1; damage_footer(); }
    return 1;
  case ACT_PRINT: print_note(); return 1;
  case ACT_SYNC:  request_sync(1); return 1;
  default: return 0;
  }
}

/* ---- keys and the pointer ---------------------------------------------------------- */

/* The bar first: while it has the keyboard it answers for every key. */
static int menu_key(uint8_t k, int *handled) {
  int a = toolbar_key(k);
  *handled = 1;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  *handled = 0;
  return 0;
}

static int app_key(void *st, uint8_t k) {
  int handled, r;
  (void)st;
  r = menu_key(k, &handled);
  if (handled) return r;
  if (N.picking) return 1;
  if (N.syncing) {
    /* A sync in progress is a level of its own, and Escape is how you back
     * out of a level. It used to be swallowed with everything else. */
    if (k == CAPP_KEY_ESC) stop_sync();
    return 1;
  }
  if (N.ask_delete) {
    /* y deletes; n, Escape, Backspace is a no; anything else waits. */
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) delete_selected();
    else if (a == CONFIRM_NO) cancel_delete();
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:    select_row(N.sel - 1); return 1;
  case CAPP_KEY_DOWN:  select_row(N.sel + 1); return 1;
  case CAPP_KEY_ENTER:
  case 'e': case 'E':  return do_action(ACT_OPEN);
  case 'n': case 'N':  return do_action(ACT_NEW);
  case 'm': case 'M':  return do_action(ACT_PICK);
  case 'd': case 'D':
  case KEY_DEL:        return do_action(ACT_DELETE);
  case 'p': case 'P':  return do_action(ACT_PRINT);
  case 'r': case 'R':
  case 's': case 'S':  return do_action(ACT_SYNC);
  default: return 0;
  }
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a, row;
  (void)st;
  if (!N.have_at) return 1;
  /* The first sign of a mouse brings the menu bar, which moves everything. */
  if (toolbar_saw_mouse()) api->damage(N.full);
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  y = (int16_t)(y - toolbar_h());
  if (N.syncing || N.picking) return 1;
  if (N.ask_delete) { cancel_delete(); return 1; }
  if (y < TOP_H || y >= N.content.h - FOOT_H) return 1;
  row = N.top + (y - TOP_H) / ROW_H;
  if (row >= N.nrows) return 1;
  /* A second click on the selected row opens it, as in Files. */
  if (row == N.sel || button == CAPP_BTN_RIGHT) return do_action(ACT_OPEN);
  select_row(row);
  return 1;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int changed = 0;
  (void)st; (void)buttons;
  if (!N.have_at) return 0;
  if (toolbar_saw_mouse()) { api->damage(N.full); changed = 1; }
  if (toolbar_hover(x, y)) changed = 1;
  if (wheel && N.nrows) {
    int want = N.sel - wheel;
    if (want < 0) want = 0;
    if (want >= N.nrows) want = N.nrows - 1;
    select_row(want);
    changed = 1;
  }
  return changed;
}

/* G0 held on the list: the words are a new note. */
static int app_button(void *st, int event, const char *text) {
  char file[FILE_MAX];
  (void)st;
  if (event == CAPP_G0_ASK)
    return N.syncing || N.picking || N.ask_delete ? CAPP_G0_NONE : CAPP_G0_WORDS;
  if (event == CAPP_G0_HEARD && text && text[0]) {
    if (words_note("Voice note", text, file, sizeof file) == 0) {
      load_rows();
      request_sync(1);
    }
    return 1;
  }
  return 0;
}

static int app_tick(void *st, uint32_t now) {
  int changed = 0;
  (void)st; (void)now;
  if (N.picking) {
    int r = api->pick_poll(N.memo, sizeof N.memo);
    if (r != CAPP_PICK_PENDING) {
      N.picking = 0;
      /* The upload waits a tick, like a sync, so "transcribing" is on
       * screen before the wait rather than after it. */
      if (r == 1) { N.memo_soon = 1; say(0, "transcribing..."); }
      return 1;
    }
  }
  if (N.memo_soon) {
    N.memo_soon = 0;
    N.bad = memo_to_note(N.memo, N.status, sizeof N.status) != 0;
    damage_top();
    if (!N.bad) request_sync(0);
    return 1;
  }
  if (N.sync_soon && N.have_at) {
    N.sync_soon = 0;
    sync_begin();
    return 1;
  }
  if (N.printing) {
    /* Its progress is the status, until it says something final. */
    const char *ps = api->print_status();
    if (!(ps[0] == 's' || ps[0] == 'c' || (ps[0] == 'p' && ps[1] && ps[5] == 'i')))
      N.printing = 0;
    if (!same(ps, N.status)) { say(0, ps); changed = 1; }
  }
  changed |= sync_tick();
  return changed;
}

/* ---- commands ----------------------------------------------------------------------- */

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  char file[FILE_MAX];
  size_t o = 0;
  int i;
  (void)st; (void)argc;
  switch (action) {
  case ACT_SYNC:
    sync_begin();
    while (N.syncing) sync_tick();
    api->fmt(out, n, "%s", N.status);
    return N.bad ? -1 : 0;
  case ACT_MEMO:
    if (memo_to_note(argv[0], out, (int)n) != 0) return -1;
    return 0;
  case ACT_ADD:
    if (words_note("Note", argv[0], file, sizeof file) != 0) { api->fmt(out, n, "the card refused it"); return -1; }
    api->fmt(out, n, "new note %s (synced next time Notes opens)", file);
    return 0;
  case ACT_LIST:
    load_rows();
    if (!N.nrows) { api->fmt(out, n, "no notes"); return 0; }
    for (i = 0; i < N.nrows && o + 8 < n; i++)
      o += (size_t)api->fmt(out + o, n - o, "%s%s", i ? "\n" : "", N.rows[i]);
    return 0;
  }
  api->fmt(out, n, "notes has no command %d", action);
  return -1;
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Notes",
  /* 16x16: a page with lines and a folded corner. */
  { 0x3F, 0xC0, 0x20, 0x60, 0x20, 0x50, 0x2F, 0x78,
    0x20, 0x08, 0x2F, 0xE8, 0x20, 0x08, 0x2F, 0xE8,
    0x20, 0x08, 0x2F, 0x88, 0x20, 0x08, 0x20, 0x08,
    0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "up down\tmove\n"
  "enter e\topen the note in Edit\n"
  "n\ta new note\n"
  "m\ta voice memo, transcribed into a note\n"
  "d\tdelete the note, here and on the server: y yes, n or esc no\n"
  "r s\tsync with the server\n"
  "esc\tstop a sync\n"
  "p\tprint the note\n"
  "hold G0\tsay a new note\n"
  "fn-b\tthe menus\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  CappStat st;
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&N, 0, sizeof N);
  if (api->stat(DIR, &st) != 0) api->mkdir(DIR);
  load_rows();
  /* Not now: see the note at the top. The first tick after the first paint. */
  if (!api->headless()) request_sync(1);
  toolbar_init(api, ACTIONS, NACT, 0, 0);
  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  UI.button = app_button;
  api->ui(&UI);
  return 0;
}
