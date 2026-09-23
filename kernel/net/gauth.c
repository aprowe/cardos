/* Google OAuth on the device. See gauth.h. */

#include "kernel/net/gauth.h"
#include "kernel/net/http.h"
#include "kernel/sys/busy.h"
#include "kernel/sys/conf.h"
#include "kernel/app/capp.h"   /* CAPP_CONFIG */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gauth";

#define NVS_NS      "cardosg"
#define KEY_ID      "cid"
#define KEY_SECRET  "csec"
#define KEY_REFRESH "rtok"

#define TOKEN_URL "https://oauth2.googleapis.com/token"

static char s_token[GAUTH_TOKEN_MAX];
static uint32_t s_expires_at_ms;      /* monotonic, not wall clock */
static char s_detail[96] = "not configured";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int load(const char *key, char *out, size_t n) {
  nvs_handle_t h;
  size_t len = n;
  out[0] = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
  if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = 0;
  nvs_close(h);
  return out[0] ? 0 : -1;
}

static void store(const char *key, const char *value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (value && *value) nvs_set_str(h, key, value);
  else nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

/* The three values mirrored to the card, in this order, one per line, so a
 * flash that wipes NVS does not mean the browser dance again. */
#define GOOGLE_CONF CAPP_CONFIG "/google.txt"

static void mirror_to_card(void) {
  char id[GAUTH_ID_MAX], secret[GAUTH_SECRET_MAX], refresh[GAUTH_REFRESH_MAX];
  const char *lines[3] = { id, secret, refresh };
  load(KEY_ID, id, sizeof id);
  load(KEY_SECRET, secret, sizeof secret);
  load(KEY_REFRESH, refresh, sizeof refresh);
  if (!id[0] && !secret[0] && !refresh[0]) { conf_remove(GOOGLE_CONF); return; }
  conf_write(GOOGLE_CONF, lines, 3);
}

int gauth_restore_from_card(void) {
  char lines[3][GAUTH_REFRESH_MAX];
  if (gauth_configured()) {
    /* NVS has it: it wins. But a login older than the mirror (2026-09-19)
     * has no file on the card, and the day NVS goes it is gone for good --
     * which is how "not configured" kept coming back. Write the mirror now
     * if it is missing, so every login is covered, not only new ones. */
    if (conf_read(GOOGLE_CONF, &lines[0][0], 3, GAUTH_REFRESH_MAX) < 3)
      mirror_to_card();
    return 0;
  }
  if (conf_read(GOOGLE_CONF, &lines[0][0], 3, GAUTH_REFRESH_MAX) < 3) return 0;
  if (!lines[0][0] || !lines[1][0] || !lines[2][0]) return 0;
  store(KEY_ID, lines[0]);
  store(KEY_SECRET, lines[1]);
  store(KEY_REFRESH, lines[2]);
  snprintf(s_detail, sizeof s_detail, "%s", "configured from card");
  return 1;
}

int gauth_set(const char *client_id, const char *client_secret,
              const char *refresh_token) {
  if (client_id)     store(KEY_ID, client_id);
  if (client_secret) store(KEY_SECRET, client_secret);
  if (refresh_token) store(KEY_REFRESH, refresh_token);
  mirror_to_card();

  /* Any change invalidates whatever was cached: the old access token belongs
   * to the old credentials. */
  s_token[0] = 0;
  s_expires_at_ms = 0;
  snprintf(s_detail, sizeof s_detail, "%s",
           gauth_configured() ? "configured" : "incomplete");
  return 0;
}

void gauth_forget(void) {
  store(KEY_ID, NULL);
  store(KEY_SECRET, NULL);
  store(KEY_REFRESH, NULL);
  conf_remove(GOOGLE_CONF);
  s_token[0] = 0;
  s_expires_at_ms = 0;
  snprintf(s_detail, sizeof s_detail, "%s", "forgotten");
}

int gauth_configured(void) {
  char buf[GAUTH_REFRESH_MAX];
  if (load(KEY_ID, buf, sizeof buf) != 0) return 0;
  if (load(KEY_SECRET, buf, sizeof buf) != 0) return 0;
  if (load(KEY_REFRESH, buf, sizeof buf) != 0) return 0;
  return 1;
}

/* Pull a JSON string or number out by field name. The replies here are small,
 * flat and machine-generated, and a real parser is several kilobytes to
 * extract two fields from them. */
static int json_str(const char *hay, const char *name, char *out, size_t n) {
  char pat[40];
  const char *p;
  size_t i = 0;

  snprintf(pat, sizeof pat, "\"%s\"", name);
  p = strstr(hay, pat);
  if (!p) return -1;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return -1;
  p++;
  while (*p && *p != '"' && i + 1 < n) {
    if (*p == '\\' && p[1]) p++;      /* an escape: take the next byte as-is */
    out[i++] = *p++;
  }
  out[i] = 0;
  return i ? 0 : -1;
}

static int json_int(const char *hay, const char *name) {
  char pat[40];
  const char *p;
  int v = 0, seen = 0;

  snprintf(pat, sizeof pat, "\"%s\"", name);
  p = strstr(hay, pat);
  if (!p) return -1;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); seen = 1; }
  return seen ? v : -1;
}

/* Percent-encode for a form body. The refresh token and client secret are
 * base64url-ish and usually need no escaping, but "usually" is not a thing to
 * build on -- one unescaped '+' turns into a space and the exchange fails with
 * an error that says nothing about why. */
static int form_encode(const char *s, char *out, size_t n) {
  static const char *HEX = "0123456789ABCDEF";
  size_t i = 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      if (i + 1 >= n) return -1;
      out[i++] = (char)c;
    } else {
      if (i + 3 >= n) return -1;
      out[i++] = '%';
      out[i++] = HEX[c >> 4];
      out[i++] = HEX[c & 15];
    }
  }
  out[i] = 0;
  return 0;
}

const char *gauth_token(void) {
  static char id[GAUTH_ID_MAX], secret[GAUTH_SECRET_MAX];
  static char refresh[GAUTH_REFRESH_MAX];
  static char body[GAUTH_ID_MAX + GAUTH_SECRET_MAX + GAUTH_REFRESH_MAX + 128];
  static char reply[1200];
  static char enc[GAUTH_REFRESH_MAX * 3];
  int n, secs;

  /* A minute of slack: a token that expires while the request is in flight is
   * a request that fails for no visible reason. */
  if (s_token[0] && (int32_t)(s_expires_at_ms - now_ms()) > 60000)
    return s_token;

  if (load(KEY_ID, id, sizeof id) != 0 ||
      load(KEY_SECRET, secret, sizeof secret) != 0 ||
      load(KEY_REFRESH, refresh, sizeof refresh) != 0) {
    snprintf(s_detail, sizeof s_detail, "%s", "not configured");
    return NULL;
  }

  n = snprintf(body, sizeof body, "grant_type=refresh_token");
  if (form_encode(id, enc, sizeof enc) == 0)
    n += snprintf(body + n, sizeof body - n, "&client_id=%s", enc);
  if (form_encode(secret, enc, sizeof enc) == 0)
    n += snprintf(body + n, sizeof body - n, "&client_secret=%s", enc);
  if (form_encode(refresh, enc, sizeof enc) == 0)
    snprintf(body + n, sizeof body - n, "&refresh_token=%s", enc);

  busy_begin("signing in");
  n = http_request_quiet("POST", TOKEN_URL, body,
                         "application/x-www-form-urlencoded", NULL,
                         reply, sizeof reply, 15000);
  busy_end();
  if (n < 0) {
    char why[48];
    /* Google answers a rejected refresh with a JSON error, and that error is
     * the whole diagnosis: invalid_grant means the token was revoked or the
     * account changed its password, and no amount of retrying fixes it. */
    if (json_str(reply, "error", why, sizeof why) == 0)
      snprintf(s_detail, sizeof s_detail, "refused: %s", why);
    else
      snprintf(s_detail, sizeof s_detail, "token request failed (%d)", n);
    ESP_LOGW(TAG, "%s", s_detail);
    return NULL;
  }

  if (json_str(reply, "access_token", s_token, sizeof s_token) != 0) {
    snprintf(s_detail, sizeof s_detail, "%s", "no access_token in the reply");
    return NULL;
  }

  secs = json_int(reply, "expires_in");
  if (secs < 60) secs = 3600;          /* Google says 3600; do not depend on it */
  s_expires_at_ms = now_ms() + (uint32_t)secs * 1000u;
  snprintf(s_detail, sizeof s_detail, "token good for %d min", secs / 60);
  ESP_LOGI(TAG, "%s", s_detail);
  return s_token;
}

const char *gauth_status(void) { return s_detail; }
