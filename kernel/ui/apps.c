/* The built-in apps. See app.h. */

#include "ui/app.h"
#include "ui/draw.h"
#include "fs/fs.h"
#include "mem/mem.h"
#include "esp_system.h"

#include <stdio.h>
#include <string.h>

#define LINE_H 9

static void line(Rect c, int n, const char *s) {
  draw_text(c.x, (int16_t)(c.y + n * LINE_H), s, C_TEXT, C_WHITE);
}

/* ------------------------------------------------------------- About ---- */

static void about_paint(void *state, Rect c) {
  char buf[40];
  (void)state;
  line(c, 0, "CardOS 0.1");
  line(c, 1, "M5Stack Cardputer");
  snprintf(buf, sizeof buf, "heap %uK free",
           (unsigned)(esp_get_free_heap_size() / 1024));
  line(c, 2, buf);
  line(c, 3, "no PSRAM, no MMU");
}

/* ------------------------------------------------------------- Files ---- */

typedef struct { int top; } FilesState;
static FilesState s_files;

static void files_paint(void *state, Rect c) {
  FilesState *st = (FilesState *)state;
  FsDir d;
  FsEntry e;
  int i = 0, shown = 0;
  int rows = c.h / LINE_H;
  char buf[40];

  if (fs_opendir("/", &d) != 0) { line(c, 0, "no card"); return; }
  while (fs_readdir(&d, &e) == 1 && shown < rows) {
    if (i++ < st->top) continue;
    snprintf(buf, sizeof buf, "%.24s%s", e.name, e.is_dir ? "/" : "");
    line(c, shown++, buf);
  }
  fs_closedir(&d);
  if (shown == 0) line(c, 0, "(end)");
}

static int files_key(void *state, uint8_t k) {
  FilesState *st = (FilesState *)state;
  if (k == 'j' || k == 0x81 /*down*/) { st->top++; return 1; }
  if ((k == 'k' || k == 0x80 /*up*/) && st->top > 0) { st->top--; return 1; }
  return 0;
}

static void files_open(void *state) { ((FilesState *)state)->top = 0; }

/* ------------------------------------------------------------ Memory ---- */

static void mem_paint(void *state, Rect c) {
  MemStats st;
  char buf[40];
  (void)state;
  mem_stats(&st);
  snprintf(buf, sizeof buf, "heap   %uK", (unsigned)(st.heap_size / 1024));
  line(c, 0, buf);
  snprintf(buf, sizeof buf, "used   %u", (unsigned)(st.movable_used + st.fixed_used));
  line(c, 1, buf);
  snprintf(buf, sizeof buf, "handles %u/%u", (unsigned)st.handles_used,
           (unsigned)MEM_MAX_HANDLES);
  line(c, 2, buf);
  snprintf(buf, sizeof buf, "compact %u", (unsigned)st.compactions);
  line(c, 3, buf);
  snprintf(buf, sizeof buf, "evict   %u", (unsigned)st.evictions);
  line(c, 4, buf);
}

/* ------------------------------------------------------------- Notes ---- */
/* The one that proves keystrokes reach the focused window rather than the
 * desktop -- which is the difference between a picture of a window system and
 * a window system. */

#define NOTES_MAX 120
typedef struct { char buf[NOTES_MAX + 1]; int len; } NotesState;
static NotesState s_notes;

static void notes_paint(void *state, Rect c) {
  NotesState *st = (NotesState *)state;
  int cols = c.w / 6;
  int row = 0, i = 0;
  char tmp[42];

  if (cols <= 1) return;
  if (cols > (int)sizeof tmp - 1) cols = (int)sizeof tmp - 1;

  /* Wrap at the window width; a narrow window simply shows fewer columns. */
  while (i < st->len && row * LINE_H < c.h) {
    int n = st->len - i;
    if (n > cols) n = cols;
    memcpy(tmp, st->buf + i, (size_t)n);
    tmp[n] = '\0';
    line(c, row++, tmp);
    i += n;
  }
  if (st->len == 0) line(c, 0, "type here");
  /* A block cursor, so it is obvious where focus went. */
  {
    int cx = st->len % cols, cy = st->len / cols;
    Rect cur;
    cur.x = (int16_t)(c.x + cx * 6);
    cur.y = (int16_t)(c.y + cy * LINE_H);
    cur.w = 6; cur.h = 8;
    if (cur.y + cur.h <= c.y + c.h) draw_rect(cur, C_TEXT);
  }
}

static int notes_key(void *state, uint8_t k) {
  NotesState *st = (NotesState *)state;
  if (k == 0x08) {                     /* backspace */
    if (st->len > 0) { st->len--; st->buf[st->len] = '\0'; return 1; }
    return 0;
  }
  if (k == 0x0D) k = ' ';              /* one paragraph; enter is a space */
  if (k < 0x20 || k > 0x7E) return 0;
  if (st->len >= NOTES_MAX) return 0;
  st->buf[st->len++] = (char)k;
  st->buf[st->len] = '\0';
  return 1;
}

static void notes_open(void *state) {
  NotesState *st = (NotesState *)state;
  st->len = 0;
  st->buf[0] = '\0';
}

/* ---------------------------------------------------------- registry ---- */

static const AppDef APPS[] = {
  { "About",  about_paint, NULL,      NULL,       NULL },
  { "Files",  files_paint, files_key, files_open, &s_files },
  { "Memory", mem_paint,   NULL,      NULL,       NULL },
  { "Notes",  notes_paint, notes_key, notes_open, &s_notes },
};

int           app_count(void) { return (int)(sizeof APPS / sizeof APPS[0]); }
const AppDef *app_at(int i) {
  if (i < 0 || i >= app_count()) return &APPS[0];
  return &APPS[i];
}
