/* ESP-NOW under kernel/net/linkproto.c: two Cardputers, no router.
 *
 * ESP-NOW is the WiFi radio sending frames straight to another device's MAC
 * (or to everyone), up to 250 bytes, a few milliseconds each way. It needs
 * the radio started but not joined to anything, and it shares the radio's
 * one channel with WiFi:
 *   - joined to a router, the channel is the router's and cannot move, so
 *     HELLO says so (LP_F_FIXED) and the other side comes to it;
 *   - not joined, this side sits on channel 1 and, while it hears nobody,
 *     walks the other channels in turn looking for someone fixed elsewhere.
 * Two devices on different routers on different channels cannot meet: said
 * in `why`.
 *
 * Received frames arrive on the WiFi task; they go through a small ring
 * (copied, with the sender) and lp_rx sees them on the shell's loop in
 * link_tick, the same task every app call comes from, so linkproto needs no
 * lock of its own. The link is the app's that opened it and closes with it
 * (capprun -> link_release_owner). While open, the radio stays up
 * (wifi_use) and awake (wifi_fast): modem sleep would sleep through frames.
 */
#include "kernel/net/link.h"
#include "kernel/net/linkproto.h"
#include "kernel/net/wifi.h"
#include "kernel/app/capprun.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "link";

#define RING        16
#define HOME_CH     1
#define HOME_MS     1200          /* on channel 1 this long ... */
#define HOP_MS      120           /* ... then this long on each of the others */

typedef struct { uint8_t mac[6]; uint8_t len; uint8_t f[LP_FRAME]; } RxFrame;

/* The protocol's state and the ring, about 7 KB together: from the heap
 * while a link is open, not held in .bss for the uptime by a feature one
 * game uses. One block, so one allocation to fail or succeed. */
typedef struct { LinkProto lp; RxFrame ring[RING]; } LinkMem;
static LinkMem    *s_mem;
static LinkProto  *s_lp;           /* &s_mem->lp while open */
static RxFrame    *s_ring;         /* s_mem->ring while open; NULL otherwise */
static int         s_open;
static const void *s_owner;
static int         s_role;         /* the last link's, kept past its close */
static volatile int s_head, s_tail;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int         s_fixed;       /* on a router's channel */
static uint8_t     s_ch;
static uint32_t    s_hop_at;
static char        s_why[48];
static const uint8_t ALL[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- the radio ------------------------------------------------------------------ */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  int next;
  if (!info || !info->src_addr || len <= 0 || len > LP_FRAME) return;
  portENTER_CRITICAL(&s_mux);
  next = (s_head + 1) % RING;
  if (s_ring && next != s_tail) {              /* full: dropped, and resent */
    memcpy(s_ring[s_head].mac, info->src_addr, 6);
    s_ring[s_head].len = (uint8_t)len;
    memcpy(s_ring[s_head].f, data, (size_t)len);
    s_head = next;
  }
  portEXIT_CRITICAL(&s_mux);
}

static void ensure_peer(const uint8_t *mac) {
  esp_now_peer_info_t p;
  if (esp_now_is_peer_exist(mac)) return;
  memset(&p, 0, sizeof p);
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;                               /* whatever the radio is on */
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  if (esp_now_add_peer(&p) != ESP_OK) {
    /* The table is small (20); a game needs two. Make room. */
    esp_now_peer_num_t n;
    if (esp_now_get_peer_num(&n) == ESP_OK && n.total_num >= 10) {
      esp_now_peer_info_t q;
      if (esp_now_fetch_peer(true, &q) == ESP_OK) esp_now_del_peer(q.peer_addr);
      esp_now_add_peer(&p);
    }
  }
}

static void tx(void *ctx, const uint8_t *mac, const uint8_t *f, int len) {
  (void)ctx;
  if (!mac) mac = ALL;
  ensure_peer(mac);
  esp_now_send(mac, f, (size_t)len);
}

static void set_channel(uint8_t ch) {
  if (ch == s_ch) return;
  if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) s_ch = ch;
}

/* Where to listen: the router's channel if joined, else home or the walk. */
static void tune(uint32_t now) {
  uint8_t primary = 0;
  wifi_second_chan_t second;
  s_fixed = wifi_is_connected();
  if (s_fixed) {
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK) s_ch = primary;
    s_lp->flags = LP_F_FIXED;
    return;
  }
  s_lp->flags = 0;
  /* Someone heard, or a game on: stay put. */
  if (s_lp->state != LP_LOOKING || s_lp->npeers > 0) { s_hop_at = now + HOME_MS; return; }
  if ((int32_t)(now - s_hop_at) < 0) return;
  /* home for a while, then 2, 3 ... 13 briefly each, then home again */
  if (s_ch >= 13) { set_channel(HOME_CH); s_hop_at = now + HOME_MS; }
  else { set_channel((uint8_t)(s_ch + 1)); s_hop_at = now + HOP_MS; }
}

/* ---- the name this device goes by ------------------------------------------------ */

static void my_name(char *out, size_t n) {
  int fd = fs_open("/config/chat.txt", FS_O_READ), r = -1, i;   /* the Chat name */
  if (fd >= 0) {
    r = fs_read(fd, out, n - 1);
    fs_close(fd);
  }
  if (r > 0) {
    out[r] = 0;
    for (i = 0; out[i]; i++) if (out[i] == '\r' || out[i] == '\n') { out[i] = 0; break; }
    if (out[0]) return;
  }
  {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, n, "Cardputer %02X%02X", mac[4], mac[5]);
  }
}

/* ---- the kernel's face ---------------------------------------------------------- */

int link_open(const char *game, const char *me) {
  uint8_t mac[6];
  char name[LP_NAME_MAX];
  LinkMem *m;
  if (s_open) link_close();
  s_why[0] = 0;
  if ((m = (LinkMem *)calloc(1, sizeof *m)) == NULL) {
    snprintf(s_why, sizeof s_why, "not enough memory (%u bytes)", (unsigned)sizeof *m);
    return -1;
  }
  wifi_use();                                  /* before start: no release between */
  if (wifi_start() != 0) {
    snprintf(s_why, sizeof s_why, "the radio would not start (%s)", wifi_status());
    wifi_unuse();
    free(m);
    return -1;
  }
  if (esp_now_init() != ESP_OK) {
    snprintf(s_why, sizeof s_why, "ESP-NOW would not start");
    wifi_unuse();
    free(m);
    return -1;
  }
  portENTER_CRITICAL(&s_mux);
  s_mem = m;
  s_lp = &m->lp;
  s_ring = m->ring;
  s_head = s_tail = 0;
  portEXIT_CRITICAL(&s_mux);
  esp_now_register_recv_cb(on_recv);
  wifi_fast(1);
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  lp_init(s_lp, mac, tx, NULL, (uint32_t)esp_timer_get_time() ^ mac[5]);
  s_ch = 0;
  if (!wifi_is_connected()) set_channel(HOME_CH);
  s_hop_at = now_ms() + HOME_MS;
  if (me && *me) snprintf(name, sizeof name, "%s", me);
  else my_name(name, sizeof name);
  tune(now_ms());
  lp_open(s_lp, game ? game : "", name, now_ms());
  s_open = 1;
  s_owner = capprun_caller();
  ESP_LOGI(TAG, "open: %s as %s, channel %u%s", game, name, s_ch, s_fixed ? " (router's)" : "");
  return 0;
}

void link_close(void) {
  if (!s_open) return;
  lp_close(s_lp);
  vTaskDelay(pdMS_TO_TICKS(20));               /* let the goodbyes leave */
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  wifi_fast(0);
  wifi_unuse();
  /* What an app may still ask after the close, kept; then the memory goes. */
  s_role = s_lp->role;
  if (!s_why[0]) snprintf(s_why, sizeof s_why, "%s", s_lp->why);
  s_open = 0;
  s_owner = NULL;
  portENTER_CRITICAL(&s_mux);
  s_ring = NULL;                               /* a late frame finds no ring */
  s_lp = NULL;
  portEXIT_CRITICAL(&s_mux);
  free(s_mem);
  s_mem = NULL;
}

void link_release_owner(const void *owner) {
  if (owner && owner == s_owner) link_close();
}

int link_active(void) { return s_open; }

void link_tick(void) {
  uint32_t now;
  if (!s_open) return;
  now = now_ms();
  for (;;) {
    RxFrame fr;
    int have = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_tail != s_head) {
      fr = s_ring[s_tail];
      s_tail = (s_tail + 1) % RING;
      have = 1;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!have) break;
    lp_rx(s_lp, fr.mac, fr.f, fr.len, now);
  }
  tune(now);
  lp_tick(s_lp, now);
}

int link_state(void) { return s_open ? s_lp->state : LP_OFF; }
int link_peer_count(void) { return s_open && s_lp->state == LP_LOOKING ? s_lp->npeers : 0; }

const char *link_peer_name(int i) {
  if (!s_open) return "";
  if (i < 0) return s_lp->peer_name;
  return i < s_lp->npeers ? s_lp->peers[i].name : "";
}

int link_invite(int i) { return s_open ? lp_invite(s_lp, i, now_ms()) : -1; }
int link_answer(int yes) { return s_open ? lp_answer(s_lp, yes, now_ms()) : -1; }
int link_role(void) { return s_open ? s_lp->role : s_role; }
int link_send(const void *buf, int len) { return s_open ? lp_send(s_lp, buf, len, now_ms()) : -1; }
int link_recv(void *buf, int max) { return s_open ? lp_recv(s_lp, buf, max) : 0; }
void link_look(void) { if (s_open) lp_look(s_lp, now_ms()); }

const char *link_why(void) {
  if (s_why[0]) return s_why;
  return s_open ? s_lp->why : "";
}

int link_channel(void) { return s_ch; }
