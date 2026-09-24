/* Small settings mirrored to the card. See prefs.h. */

#include "kernel/sys/prefs.h"
#include "kernel/sys/kvtext.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

typedef enum { P_U8, P_U16, P_STR } PrefType;

typedef struct {
  const char *key;          /* the NVS key in PREFS_NS, and the file's */
  PrefType    type;
} Pref;

/* What survives a reflash. A setting a person chose that is not in here is
 * lost with NVS -- add it, and call prefs_mirror() after writing it. */
static const Pref PREFS[] = {
  { "bright",   P_U8 },     /* kernel/drv/display.c */
  { "volume",   P_U8 },     /* kernel/drv/speaker.c */
  { "shell",    P_U8 },     /* kernel/ui/shell.c */
  { "autodesk", P_U8 },     /* kernel/ui/desktop.c */
  { "btboot",   P_U8 },     /* kernel/drv/bthid.c */
  { "pins",     P_STR },    /* kernel/ui/pins.c */
  { "dim_s",    P_U16 },    /* kernel/sys/power.c */
  { "off_s",    P_U16 },
};
#define NPREFS ((int)(sizeof PREFS / sizeof PREFS[0]))

#define FILE_MAX 1024
#define VAL_MAX  256

static const char *TAG = "prefs";

/* One key's value from NVS as text, or -1 if NVS has none. */
static int read_nvs(nvs_handle_t h, const Pref *p, char *out, size_t n) {
  switch (p->type) {
  case P_U8: {
    uint8_t v;
    if (nvs_get_u8(h, p->key, &v) != ESP_OK) return -1;
    snprintf(out, n, "%u", (unsigned)v);
    return 0;
  }
  case P_U16: {
    uint16_t v;
    if (nvs_get_u16(h, p->key, &v) != ESP_OK) return -1;
    snprintf(out, n, "%u", (unsigned)v);
    return 0;
  }
  default: {
    size_t len = n;
    if (nvs_get_str(h, p->key, out, &len) != ESP_OK) return -1;
    return 0;
  }
  }
}

static int write_nvs(nvs_handle_t h, const Pref *p, const char *value) {
  unsigned long v;
  char *end;
  if (p->type == P_STR) return nvs_set_str(h, p->key, value) == ESP_OK ? 0 : -1;
  v = strtoul(value, &end, 10);
  if (end == value || *end) return -1;                 /* not a number: leave it */
  if (p->type == P_U8) return v <= 255 && nvs_set_u8(h, p->key, (uint8_t)v) == ESP_OK ? 0 : -1;
  return v <= 65535 && nvs_set_u16(h, p->key, (uint16_t)v) == ESP_OK ? 0 : -1;
}

/* NAME.tmp, remove NAME, rename: a power cut leaves the old file or the new
 * one (apps/safefile.h is the same idea for apps). */
static void write_file(const char *text, size_t len) {
  const char *tmp = PREFS_FILE ".tmp";
  int fd = fs_open(tmp, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  if (fs_write(fd, text, len) != (int)len) { fs_close(fd); fs_remove(tmp); return; }
  fs_close(fd);
  fs_remove(PREFS_FILE);
  if (fs_rename(tmp, PREFS_FILE) != 0) ESP_LOGW(TAG, "could not replace %s", PREFS_FILE);
}

void prefs_mirror(void) {
  char *text;
  char val[VAL_MAX];
  size_t len = 0;
  nvs_handle_t h;
  int i;

  if (!fs_mounted() || (text = (char *)malloc(FILE_MAX)) == NULL) return;
  len = (size_t)snprintf(text, FILE_MAX, "%s",
                         "# CardOS settings: a copy of NVS, put back at boot if a flash wipes it\n");
  if (nvs_open(PREFS_NS, NVS_READONLY, &h) == ESP_OK) {
    for (i = 0; i < NPREFS; i++)
      if (read_nvs(h, &PREFS[i], val, sizeof val) == 0)
        kv_put(text, FILE_MAX, &len, PREFS[i].key, val);
    nvs_close(h);
  }
  fs_mkdir("/config");
  write_file(text, len);
  free(text);
}

static const Pref *find(const char *key) {
  int i;
  for (i = 0; i < NPREFS; i++) if (!strcmp(PREFS[i].key, key)) return &PREFS[i];
  return NULL;
}

int prefs_restore_from_card(void) {
  static char text[FILE_MAX + 1];        /* once, at boot: 1 KB of .bss -- move to the heap if RAM gets tight */
  char key[24], val[VAL_MAX], have[VAL_MAX];
  const char *p;
  nvs_handle_t h;
  int fd, n, restored = 0;

  if (!fs_mounted()) return 0;
  fd = fs_open(PREFS_FILE, FS_O_READ);
  if (fd < 0) {
    /* A cut between remove and rename leaves only the .tmp. */
    if (fs_rename(PREFS_FILE ".tmp", PREFS_FILE) != 0) return 0;
    if ((fd = fs_open(PREFS_FILE, FS_O_READ)) < 0) return 0;
  }
  n = fs_read(fd, text, FILE_MAX);
  fs_close(fd);
  if (n <= 0) return 0;
  text[n] = 0;

  if (nvs_open(PREFS_NS, NVS_READWRITE, &h) != ESP_OK) return 0;
  p = text;
  while (kv_next(&p, key, sizeof key, val, sizeof val)) {
    const Pref *pref = find(key);
    if (!pref || read_nvs(h, pref, have, sizeof have) == 0) continue;   /* NVS wins */
    if (write_nvs(h, pref, val) == 0) restored++;
  }
  if (restored) nvs_commit(h);
  nvs_close(h);
  if (restored) ESP_LOGI(TAG, "restored %d setting(s) from %s", restored, PREFS_FILE);
  return restored;
}

void prefs_forget_file(void) {
  fs_remove(PREFS_FILE);
  fs_remove(PREFS_FILE ".tmp");
}

int prefs_get_u16(const char *key, int def) {
  nvs_handle_t h;
  uint16_t v;
  int r = def;
  if (nvs_open(PREFS_NS, NVS_READONLY, &h) != ESP_OK) return def;
  if (nvs_get_u16(h, key, &v) == ESP_OK) r = v;
  nvs_close(h);
  return r;
}

void prefs_set_u16(const char *key, int value) {
  nvs_handle_t h;
  if (value < 0) value = 0;
  if (value > 65535) value = 65535;
  if (nvs_open(PREFS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u16(h, key, (uint16_t)value);
  nvs_commit(h);
  nvs_close(h);
  prefs_mirror();
}
