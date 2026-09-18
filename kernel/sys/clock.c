/* Wall-clock time, from the network. See clock.h. */

#include "kernel/sys/clock.h"

#include "kernel/sys/env.h"
#include "kernel/net/wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "clock";

/* pool.ntp.org rather than a vendor's: it is the answer that keeps working
 * when a company loses interest in a product. */
#define NTP_SERVER "pool.ntp.org"

static int s_synced;                 /* the network told us, this boot */
static int s_have;                   /* we have a time at all, if only a saved one */
static int s_started;
static uint32_t s_last_save;         /* uptime ms of the last write */

/* Where the time is kept across a power cut.
 *
 * NVS rather than the card, because this has to work on a machine with no
 * card in it and because a half-written four-byte key is a thing NVS already
 * knows how to avoid. It sits with the other settings -- brightness, PATH,
 * the shell -- which is where someone would look for it.
 *
 * Saved every ten minutes rather than continuously: the point is to be
 * roughly right after a power cut, and the cost of being wrong is bounded by
 * the interval, so ten minutes buys that for 144 writes a day. */
#define CLOCK_NS      "cardos"
#define CLOCK_KEY     "epoch"
#define SAVE_EVERY_MS (10 * 60 * 1000u)
#define EPOCH_FLOOR   1577836800u    /* 2020-01-01; anything below is garbage */

static uint32_t uptime_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void save_now(void) {
  nvs_handle_t h;
  uint32_t now;
  if (!s_have) return;
  now = (uint32_t)time(NULL);
  if (now < EPOCH_FLOOR) return;
  if (nvs_open(CLOCK_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, CLOCK_KEY, now);
  nvs_commit(h);
  nvs_close(h);
  s_last_save = uptime_ms();
}

/* The saved time, or 0. Restoring it is honest but not accurate: the device
 * was off for an unknown while, so the clock comes back BEHIND by exactly
 * however long that was. It is still worth doing -- a calendar that opens on
 * roughly today beats one that opens in 1970 -- which is why it is marked as
 * approximate rather than synchronised, and why the network is still asked. */
static uint32_t load_saved(void) {
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open(CLOCK_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  if (nvs_get_u32(h, CLOCK_KEY, &v) != ESP_OK) v = 0;
  nvs_close(h);
  return v >= EPOCH_FLOOR ? v : 0;
}

/* The zone, from env TZ. Public because it has to be re-applied the moment
 * TZ changes: it used to run only at boot and at each NTP sync, so `set
 * TZ=...` appeared to work -- env listed it, and the clock went on reporting
 * UTC until the next reboot. Every time on the device was UTC, which is how
 * an event at half five in the afternoon showed up as half past midnight the
 * next day. */
void clock_apply_zone(void) {
  const char *tz = env_get("TZ");
  setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
  tzset();
}

/* Whether a zone was actually chosen, as opposed to falling back to UTC.
 * "19:37" with no way to tell it is UTC is the part that wasted the time. */
int clock_zone_set(void) {
  const char *tz = env_get("TZ");
  return tz && tz[0];
}

const char *clock_zone(void) {
  const char *tz = env_get("TZ");
  return (tz && tz[0]) ? tz : "UTC0 (TZ is not set)";
}

void clock_init(void) {
  clock_apply_zone();

  /* A clock set in a previous life would still be running -- the RTC timer
   * survives a soft reset even though it does not survive power loss -- so
   * anything after 2020 means somebody already synchronised us. */
  {
    time_t now = time(NULL);
    if (now > (time_t)EPOCH_FLOOR) { s_synced = 1; s_have = 1; }
  }

  /* Cold start: nothing is running, so fall back to the last time we wrote
   * down. The device is behind by however long it was off, and says so --
   * clock_full prefixes an approximate time with a tilde -- but the date is
   * right unless it has been off for days, and the date is what a diary
   * needs. */
  if (!s_have) {
    uint32_t saved = load_saved();
    if (saved) {
      struct timeval tv;
      tv.tv_sec = (time_t)saved;
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);
      s_have = 1;
      ESP_LOGI(TAG, "restored the saved time; asking the network for a real one");
    }
  }
  s_last_save = uptime_ms();
}

/* Called from the shell's housekeeping, and cheap enough to call every pass:
 * it does nothing at all until the interval is up. */
void clock_persist_tick(void) {
  if (!s_have) return;
  if (uptime_ms() - s_last_save < SAVE_EVERY_MS) return;
  save_now();
}

int clock_synced(void) { return s_synced; }
int clock_have_time(void) { return s_have; }

int clock_sync(int timeout_ms) {
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);

  if (!wifi_is_connected()) return -1;

  clock_apply_zone();

  if (!s_started) {
    /* Started once and left running: SNTP re-polls on its own schedule after
     * this, which is how a device with no RTC keeps from drifting. */
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
      ESP_LOGW(TAG, "sntp would not start");
      return -1;
    }
    s_started = 1;
  }

  if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
    ESP_LOGW(TAG, "no answer from %s", NTP_SERVER);
    return -1;
  }

  s_synced = 1;
  s_have = 1;
  save_now();                        /* the one time we know it is right */
  {
    char when[40];
    clock_full(when, sizeof when);
    ESP_LOGI(TAG, "clock set: %s", when);
  }
  return 0;
}

void clock_hm(char *buf, size_t n) {
  time_t now;
  struct tm tm;

  if (!buf || n < 6) return;
  if (!s_have) { snprintf(buf, n, "--:--"); return; }

  now = time(NULL);
  localtime_r(&now, &tm);
  snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void clock_full(char *buf, size_t n) {
  time_t now;
  struct tm tm;

  if (!buf || n < 8) return;
  if (!s_have) { snprintf(buf, n, "not set"); return; }

  now = time(NULL);
  localtime_r(&now, &tm);
  /* A tilde on a time that came off the shelf rather than off the network.
   * A confident wrong clock is the thing worth avoiding, and one character
   * is the cheapest way to stay honest about which this is. */
  if (!s_synced) { buf[0] = '~'; strftime(buf + 1, n - 1, "%a %d %b %H:%M", &tm); }
  else strftime(buf, n, "%a %d %b %H:%M", &tm);
}

uint32_t clock_epoch(void) {
  return s_have ? (uint32_t)time(NULL) : 0;
}
