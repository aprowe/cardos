/* The built-in apps. See app.h. */

/* Every paint callback clears its own rectangle first.
 *
 * These used to rely on the desktop filling the content well before calling
 * them, which worked right up until the launcher started hosting them
 * fullscreen -- and then they drew over whatever was already on the panel. An
 * app that paints all of itself works under any host, which is the property
 * worth having. */
#include "kernel/ui/app.h"
#include "kernel/ui/settings.h"
#include "kernel/ui/draw.h"
#include "kernel/fs/fs.h"
#include "kernel/mem/mem.h"
#include "esp_system.h"
#include "esp_timer.h"

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
  draw_rect(c, C_WHITE);
  line(c, 0, "CardOS 0.1");
  line(c, 1, "M5Stack Cardputer");
  snprintf(buf, sizeof buf, "heap %uK free",
           (unsigned)(esp_get_free_heap_size() / 1024));
  line(c, 2, buf);
  line(c, 3, "no PSRAM, no MMU");
}

/* ------------------------------------------------------------- Files ---- */

/* The listing is cached rather than read in paint. Reading the card inside a
 * paint callback meant every cursor movement across this window did SD SPI
 * I/O -- tens of milliseconds each -- which is what made the mouse lag. A
 * paint callback has to be cheap, because damage is what calls it. */
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

  draw_rect(c, C_WHITE);
  if (st->count == 0) { line(c, 0, "no card"); return; }
  for (i = 0; i < rows && st->top + i < st->count; i++)
    line(c, i, st->name[st->top + i]);
}

static int files_key(void *state, uint8_t k) {
  FilesState *st = (FilesState *)state;
  if ((k == 'j' || k == 0x81) && st->top + 1 < st->count) { st->top++; return 1; }
  if ((k == 'k' || k == 0x80) && st->top > 0) { st->top--; return 1; }
  if (k == 'r' || k == 'R') { files_reload(st); return 1; }
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
  draw_rect(c, C_WHITE);
  kmem_stats(&st);
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

/* ---------------------------------------------------------- registry ---- */

/* Edit and Mines used to live here too. They are loadable .capp binaries now,
 * and carrying a second copy compiled in would mean two versions of the same
 * app drifting apart -- so the built-ins are the ones that cannot sensibly be
 * loadable: a file browser, a heap view, and an about box. */
static const AppDef APPS[] = {
  /* name    paint        key        click open        state     height w h text */
  { "Files",  files_paint, files_key, NULL,  files_open, &s_files, NULL, 0,0, NULL },
  { "Memory", mem_paint,   NULL,      NULL,  NULL,       NULL,     NULL, 0,0, NULL },
  { "About",  about_paint, NULL,      NULL,  NULL,       NULL,     NULL, 0,0, NULL },
};

/* Settings lives in its own file -- it reaches across the radio, the panel and
 * the desktop, and apps.c is meant to stay a collection of small self-contained
 * things -- so it is appended here rather than sitting in the table. */
#define NBUILTIN ((int)(sizeof APPS / sizeof APPS[0]))

int app_count(void) { return NBUILTIN + 1; }

const AppDef *app_at(int i) {
  if (i == NBUILTIN) return settings_app();
  if (i < 0 || i >= NBUILTIN) return &APPS[0];
  return &APPS[i];
}

int app_index_by_name(const char *name) {
  int i;
  for (i = 0; i < app_count(); i++)
    if (strcmp(app_at(i)->name, name) == 0) return i;
  return -1;
}
