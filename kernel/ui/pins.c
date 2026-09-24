/* The desktop's pinned apps. See pins.h. */

#include "kernel/sys/prefs.h"
#include "kernel/ui/pins.h"

#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NS   "cardos"
#define NVS_KEY  "pins"

/* One string, names separated by newlines, because the alternative is a
 * length-prefixed blob and this one can be read with `nvs` in a debugger.
 * Twenty apps at fifteen characters is comfortably inside it. */
#define PINS_MAX 512

static char s_pins[PINS_MAX];
static int  s_active;

static void save(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_KEY, s_pins);
  nvs_commit(h);
  nvs_close(h);
  prefs_mirror();              /* and /config/settings.txt: see prefs.h */
}

void pins_init(void) {
  nvs_handle_t h;
  size_t len = sizeof s_pins;

  s_pins[0] = 0;
  s_active = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  if (nvs_get_str(h, NVS_KEY, s_pins, &len) == ESP_OK) s_active = 1;
  else s_pins[0] = 0;
  nvs_close(h);
}

int pins_active(void) { return s_active; }

/* A whole-line match. Without the line boundaries "Web" would match inside
 * "Webcam", and the two would pin and unpin each other. */
static char *find(const char *name) {
  size_t n;
  char *p = s_pins;

  if (!name || !*name) return 0;
  n = strlen(name);
  while (*p) {
    char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && strncmp(p, name, n) == 0) return p;
    if (!end) break;
    p = end + 1;
  }
  return 0;
}

int pins_has(const char *name) {
  if (!s_active) return 1;            /* no list yet: everything is on */
  return find(name) != 0;
}

void pins_add(const char *name) {
  size_t have, add;

  if (!name || !*name) return;
  if (!s_active) {
    /* The first deliberate act creates the list. Adding to "everything" would
     * otherwise be a no-op that silently turned every other app off. */
    s_active = 1;
  }
  if (find(name)) return;

  have = strlen(s_pins);
  add = strlen(name);
  if (have + add + 2 > sizeof s_pins) return;
  snprintf(s_pins + have, sizeof s_pins - have, "%s\n", name);
  save();
}

void pins_remove(const char *name) {
  char *at;

  if (!s_active) {
    /* Removing one from "everything" has to write the rest down first, which
     * only the caller can do -- it is the one with the list of apps. It calls
     * pins_add for each, then this. Until then, removing is meaningless. */
    return;
  }
  at = find(name);
  if (!at) return;
  {
    char *end = strchr(at, '\n');
    if (end) memmove(at, end + 1, strlen(end + 1) + 1);
    else *at = 0;
  }
  save();
}
