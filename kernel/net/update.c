/* Pulling new builds from the PC. See update.h. */

#include "kernel/app/capp.h"   /* CAPP_PROXY_DEFAULT */
#include "kernel/net/update.h"
#include "kernel/net/http.h"
#include "kernel/net/wifi.h"
#include "kernel/fs/fs.h"
#include "kernel/sys/env.h"
#include "kernel/ui/icons.h"
#include "kernel/app/launcher.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "update";

#define DEFAULT_BASE  CAPP_PROXY_DEFAULT
#define FIRMWARE_PATH "/update/firmware.bin"
#define MANIFEST_MAX  2048                   /* 24 apps at ~40 bytes a line */

static char s_error[96];
static char s_token[64];
static int  s_token_read;

const char *update_error(void) { return s_error; }

const char *update_base(void) {
  const char *v = env_get("PROXY");
  return (v && *v) ? v : DEFAULT_BASE;
}

/* Same file and the same trimming as apps/claude.c: a token with a newline
 * on the end matches nothing. */
const char *update_token(void) {
  int fd, n;
  if (s_token_read) return s_token;
  s_token_read = 1;
  s_token[0] = 0;
  fd = fs_open("/claude.token", FS_O_READ);
  if (fd < 0) return s_token;
  n = fs_read(fd, s_token, sizeof s_token - 1);
  fs_close(fd);
  if (n < 0) n = 0;
  s_token[n] = 0;
  while (n > 0 && (s_token[n - 1] == '\n' || s_token[n - 1] == '\r' ||
                   s_token[n - 1] == ' '))
    s_token[--n] = 0;
  return s_token;
}

static const char *bearer(void) {
  const char *t = update_token();
  return *t ? t : NULL;
}

/* FNV-1a over a file on the card, 1 KB at a time. -1 if it cannot be read. */
static int hash_file(const char *path, uint32_t *out) {
  static uint8_t buf[1024];
  uint32_t h = MANIFEST_FNV_INIT;
  int fd = fs_open(path, FS_O_READ), n;
  if (fd < 0) return -1;
  while ((n = fs_read(fd, buf, sizeof buf)) > 0)
    h = manifest_fnv1a(h, buf, (size_t)n);
  fs_close(fd);
  if (n < 0) return -1;
  *out = h;
  return 0;
}

static void say(UpdateLog log, void *ctx, const char *line) {
  if (log) log(ctx, line);
  ESP_LOGI(TAG, "%s", line);
}

/* ---- check ---------------------------------------------------------------- */

/* Where NAME.capp lives on the card, folders included; the top level if it
 * is not there at all. Matched on the file name, not the app's own name,
 * because that is what the manifest is keyed by. */
static void capp_path(const char *name, char *out, size_t size) {
  char want[40];
  size_t wl;
  int i;

  snprintf(want, sizeof want, "/%s.capp", name);
  wl = strlen(want);
  for (i = 0; i < icons_total(); i++) {
    const Icon *ic = icon_at(i);
    size_t pl;
    if (!ic || ic->kind != ICON_CAPP) continue;
    pl = strlen(ic->path);
    if (pl >= wl && strcmp(ic->path + pl - wl, want) == 0) {
      snprintf(out, size, "%s", ic->path);
      return;
    }
  }
  snprintf(out, size, "%s/%s.capp", ICONS_DIR, name);
}

int update_check(UpdateCheck *out) {
  static char text[MANIFEST_MAX];
  char url[160];
  ManifestLocal local;
  const esp_app_desc_t *self;
  int n, i;

  memset(out, 0, sizeof *out);
  s_error[0] = 0;

  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) {
    snprintf(s_error, sizeof s_error, "no network: %s", wifi_status());
    return -1;
  }

  snprintf(url, sizeof url, "%s/update", update_base());
  n = http_request("GET", url, NULL, NULL, bearer(), text, sizeof text, 15000);
  if (n < 0) {
    if (n == -403)
      snprintf(s_error, sizeof s_error, "the proxy wants a token (/claude.token)");
    else
      snprintf(s_error, sizeof s_error, "cannot reach %s (%d)", update_base(), n);
    return -2;
  }
  if (manifest_parse(text, &out->m) < 0) {
    snprintf(s_error, sizeof s_error, "that is not a manifest -- is webproxy.py current?");
    return -3;
  }

  self = esp_app_get_description();
  memset(&local, 0, sizeof local);
  local.own_sha = self ? self->app_elf_sha256 : NULL;
  /* The icon table is what knows which folder an app is in. Loaded here if
   * the launcher has not yet, as launchui's own need_icons does. */
  if (icons_total() == 0) icons_reload();
  for (i = 0; i < out->m.napps; i++) {
    char path[80];
    capp_path(out->m.app[i].name, path, sizeof path);
    if (hash_file(path, &local.app_hash[i]) == 0) local.have_app[i] = 1;
  }

  out->firmware_stale = manifest_diff(&out->m, &local, out->stale);
  for (i = 0; i < out->m.napps; i++) out->nstale_apps += out->stale[i] != 0;
  return 0;
}

/* ---- apps ----------------------------------------------------------------- */

static int install_app(const ManifestApp *a, UpdateLog log, void *ctx) {
  char url[160], tmp[88], dst[80], line[96];
  uint32_t hash = 0;
  FsStat st;
  int n;

  snprintf(url, sizeof url, "%s/update/app/%s", update_base(), a->name);
  capp_path(a->name, dst, sizeof dst);
  snprintf(tmp, sizeof tmp, "%s.new", dst);

  n = http_download_ex(url, tmp, bearer(), NULL, NULL, 30000);
  if (n < 0) {
    snprintf(line, sizeof line, "%s: download failed (%d)", a->name, n);
    say(log, ctx, line);
    fs_remove(tmp);
    return -1;
  }

  /* Measured and hashed before it is allowed to replace anything. */
  if (fs_stat(tmp, &st) != 0 || (uint32_t)st.size != a->size ||
      hash_file(tmp, &hash) != 0 || hash != a->hash) {
    snprintf(line, sizeof line, "%s: bad copy (%u of %u bytes)", a->name,
             (unsigned)(fs_stat(tmp, &st) == 0 ? st.size : 0), (unsigned)a->size);
    say(log, ctx, line);
    fs_remove(tmp);
    return -1;
  }

  fs_remove(dst);                      /* FAT rename does not overwrite */
  if (fs_rename(tmp, dst) != 0) {
    snprintf(line, sizeof line, "%s: could not replace the old one", a->name);
    say(log, ctx, line);
    return -1;
  }
  snprintf(line, sizeof line, "%s.capp %u bytes", a->name, (unsigned)a->size);
  say(log, ctx, line);
  return 0;
}

int update_apps(const UpdateCheck *c, UpdateLog log, void *ctx) {
  int i, done = 0;
  for (i = 0; i < c->m.napps; i++) {
    if (!c->stale[i]) continue;
    if (install_app(&c->m.app[i], log, ctx) == 0) done++;
  }
  if (done) icons_reload();
  return done;
}

/* ---- firmware ------------------------------------------------------------- */

typedef struct { UpdateLog log; void *ctx; int last; } Prog;

static void dl_progress(void *p, uint32_t done, uint32_t total) {
  Prog *pr = (Prog *)p;
  int pct = total ? (int)((uint64_t)done * 100 / total) : -1;
  char line[40];
  if (pct < 0 || pct / 10 == pr->last) return;
  pr->last = pct / 10;
  snprintf(line, sizeof line, "downloading %d%%", pct);
  say(pr->log, pr->ctx, line);
}

static void flash_progress(void *p, int pct) {
  Prog *pr = (Prog *)p;
  char line[40];
  if (pct / 10 == pr->last) return;
  pr->last = pct / 10;
  snprintf(line, sizeof line, "writing %d%%", pct);
  say(pr->log, pr->ctx, line);
}

int update_firmware(UpdateLog log, void *ctx) {
  char url[160], line[96];
  Prog pr = { log, ctx, -1 };
  LaunchResult r;
  int n;

  s_error[0] = 0;
  fs_mkdir("/update");
  snprintf(url, sizeof url, "%s/update/firmware", update_base());
  n = http_download_ex(url, FIRMWARE_PATH, bearer(), dl_progress, &pr, 120000);
  if (n < 0) {
    snprintf(s_error, sizeof s_error, "download failed (%d)", n);
    fs_remove(FIRMWARE_PATH);
    return -1;
  }
  snprintf(line, sizeof line, "%u KB on the card, flashing", (unsigned)(n / 1024));
  say(log, ctx, line);

  /* launcher_boot validates the image, verifies its SHA as it lands, arms
   * rollback and restarts. Only reached on failure. */
  pr.last = -1;
  r = launcher_boot(FIRMWARE_PATH, flash_progress, &pr);
  snprintf(s_error, sizeof s_error, "%s", launcher_strerror(r));
  return -2;
}
