/* See env.h. */

#include "kernel/sys/env.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS "cardosenv"

/* The names that have been set, comma-separated, so they can be read back.
 * NVS can enumerate keys, but only by walking every namespace on the partition
 * and filtering -- an index of our own is smaller and says exactly what it
 * means. Without it, `set FOO=bar` wrote a value nothing ever looked for
 * again, which is a setting that silently vanishes at the next reboot. */
#define KEY_NAMES "_names"

typedef struct {
  char name[ENV_NAME_MAX];
  char value[ENV_VALUE_MAX];
} Var;

static Var s_var[ENV_MAX];
static int s_n;

/* /desktop first because that is where the apps actually are, then the folders
 * they are grouped into, then /bin because that is where anyone would put one
 * next. A PATH entry that does not exist costs one failed open.
 *
 * The graphical apps live in folders and the CLI ones do not, which is why
 * /desktop still comes first: `grep` and `cat` are found without walking any
 * of the rest. Nothing here is load-bearing -- the launcher's own list is
 * searched after PATH, so an app resolves by name even on a card whose saved
 * PATH predates the folders. */
static const char *DEFAULT_PATH =
    "/desktop:/desktop/Tools:/desktop/Net:/desktop/Games:/bin";

static int find(const char *name) {
  int i;
  for (i = 0; i < s_n; i++) if (strcmp(s_var[i].name, name) == 0) return i;
  return -1;
}

static void save(const char *name, const char *value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (value && value[0]) nvs_set_str(h, name, value);
  else nvs_erase_key(h, name);
  nvs_commit(h);
  nvs_close(h);
}

static void load_one(const char *name, const char *fallback) {
  nvs_handle_t h;
  char buf[ENV_VALUE_MAX];
  size_t n = sizeof buf;

  buf[0] = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_str(h, name, buf, &n) != ESP_OK) buf[0] = 0;
    nvs_close(h);
  }
  if (!buf[0] && fallback) snprintf(buf, sizeof buf, "%s", fallback);
  if (!buf[0]) return;

  if (s_n < ENV_MAX) {
    snprintf(s_var[s_n].name, ENV_NAME_MAX, "%s", name);
    snprintf(s_var[s_n].value, ENV_VALUE_MAX, "%s", buf);
    s_n++;
  }
}

/* Rewrite the index from what is currently in memory. */
static void save_names(void) {
  nvs_handle_t h;
  char names[ENV_MAX * (ENV_NAME_MAX + 1) + 1];
  int i, n = 0;

  names[0] = 0;
  for (i = 0; i < s_n; i++)
    n += snprintf(names + n, sizeof names - n, "%s%s",
                  i ? "," : "", s_var[i].name);

  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (names[0]) nvs_set_str(h, KEY_NAMES, names);
  else nvs_erase_key(h, KEY_NAMES);
  nvs_commit(h);
  nvs_close(h);
}

void env_init(void) {
  nvs_handle_t h;
  char names[ENV_MAX * (ENV_NAME_MAX + 1) + 1];
  size_t len = sizeof names;
  int i = 0;

  s_n = 0;
  names[0] = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_str(h, KEY_NAMES, names, &len) != ESP_OK) names[0] = 0;
    nvs_close(h);
  }

  while (names[i] && s_n < ENV_MAX) {
    char name[ENV_NAME_MAX];
    int k = 0;
    while (names[i] == ',') i++;
    while (names[i] && names[i] != ',' && k < ENV_NAME_MAX - 1) name[k++] = names[i++];
    name[k] = 0;
    if (k) load_one(name, NULL);
  }

  /* The ones CardOS itself relies on, filled in if they were never set. */
  if (!env_get("PATH")) load_one("PATH", DEFAULT_PATH);
  if (!env_get("HOME")) load_one("HOME", "/");
  if (!env_get("EDITOR")) load_one("EDITOR", "edit");
}

const char *env_get(const char *name) {
  int i = name ? find(name) : -1;
  return i < 0 ? NULL : s_var[i].value;
}

int env_set(const char *name, const char *value) {
  int i;

  if (!name || !name[0]) return -1;
  i = find(name);

  if (!value || !value[0]) {
    if (i < 0) return 0;
    for (; i + 1 < s_n; i++) s_var[i] = s_var[i + 1];
    s_n--;
    save(name, NULL);
    save_names();
    return 0;
  }

  if (i < 0) {
    if (s_n >= ENV_MAX) return -1;
    i = s_n++;
    snprintf(s_var[i].name, ENV_NAME_MAX, "%s", name);
  }
  snprintf(s_var[i].value, ENV_VALUE_MAX, "%s", value);
  save(s_var[i].name, s_var[i].value);
  save_names();
  return 0;
}

int env_count(void) { return s_n; }
const char *env_name_at(int i)  { return (i < 0 || i >= s_n) ? "" : s_var[i].name; }
const char *env_value_at(int i) { return (i < 0 || i >= s_n) ? "" : s_var[i].value; }

int env_path_next(int *iter, char *out, int out_size) {
  const char *p = env_get("PATH");
  int i, n = 0;

  if (!p || !iter || !out || out_size < 2) return 0;
  i = *iter;
  while (p[i] == ':') i++;
  if (!p[i]) return 0;

  while (p[i] && p[i] != ':' && n < out_size - 1) out[n++] = p[i++];
  out[n] = 0;
  *iter = i;
  return n > 0;
}
