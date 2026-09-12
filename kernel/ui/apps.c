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

/* The file browser used to be here: a cached listing, arrow keys, and nothing
 * else. It is apps/files.c now -- a .capp with two input modes, folders,
 * renaming and moving -- for the same reason Edit and Mines left: an app that
 * can be loaded should be, and two apps called Files is one too many.
 *
 * What stays built in is what cannot sensibly be loadable: a heap view and an
 * about box, both of which report on the kernel that would be loading them.
 */

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

/* Edit, Mines and now Files used to live here. They are loadable .capp
 * binaries, and carrying a second copy compiled in would mean two versions of
 * the same app drifting apart -- so what stays built in is what cannot
 * sensibly be loaded: a heap view and an about box, both of which report on
 * the kernel that would be doing the loading. */
static const AppDef APPS[] = {
  { .name = "Memory", .paint = mem_paint },
  { .name = "About",  .paint = about_paint },
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
