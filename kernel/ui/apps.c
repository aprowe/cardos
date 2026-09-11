/* The built-in apps. See app.h. */

#include "kernel/ui/app.h"
#include "kernel/ui/draw.h"
#include "kernel/fs/fs.h"
#include "kernel/mem/mem.h"
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

/* The listing is cached rather than read in paint. Reading the card inside a
 * paint callback meant every cursor movement that crossed this window did SD
 * SPI I/O -- tens of milliseconds each -- which is what made the mouse lag.
 * A paint callback has to be cheap, because damage repaints it. */
#define FILES_MAX 24
#define FILES_NAME 26
typedef struct {
  int  top;
  int  count;
  char name[FILES_MAX][FILES_NAME];
} FilesState;
static FilesState s_files;

static void files_reload(FilesState *st) {
  FsDir d;
  FsEntry e;
  st->count = 0;
  if (fs_opendir("/", &d) != 0) return;
  while (st->count < FILES_MAX && fs_readdir(&d, &e) == 1) {
    snprintf(st->name[st->count], FILES_NAME, "%.24s%s", e.name,
             e.is_dir ? "/" : "");
    st->count++;
  }
  fs_closedir(&d);
}

static void files_paint(void *state, Rect c) {
  FilesState *st = (FilesState *)state;
  int rows = c.h / LINE_H, i;

  if (st->count == 0) { line(c, 0, "no card"); return; }
  for (i = 0; i < rows && st->top + i < st->count; i++)
    line(c, i, st->name[st->top + i]);
}

static int files_key(void *state, uint8_t k) {
  FilesState *st = (FilesState *)state;
  if ((k == 'j' || k == 0x81) && st->top + 1 < st->count) { st->top++; return 1; }
  if ((k == 'k' || k == 0x80) && st->top > 0) { st->top--; return 1; }
  if (k == 'r' || k == 'R') { files_reload(st); return 1; }   /* rescan */
  return 0;
}

static void files_open(void *state) {
  FilesState *st = (FilesState *)state;
  st->top = 0;
  files_reload(st);
}

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
