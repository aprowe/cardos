/* Roku on the host: which key sends which ECP name, how a typed character is
 * spelled, the queue that keeps a burst of volume presses, the address and
 * the device-info reply, and what the commands send and say. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info roku_capp_info
#define capp_main roku_capp_main
#include "apps/roku.c"
#undef capp_info
#undef capp_main

static int t_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap; int r;
  va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
  return r;
}
static size_t t_strlen(const char *s) { return strlen(s); }
static int t_ready(void) { return 1; }

static char s_method[8], s_url[200];
static int s_status;              /* what the fake TV answers: 0 ok, or negative */
static int s_calls;
static int t_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  (void)body; (void)ct; (void)bearer; (void)ms;
  snprintf(s_method, sizeof s_method, "%s", m);
  snprintf(s_url, sizeof s_url, "%s", url);
  s_calls++;
  if (n) out[0] = 0;
  return s_status;
}

static int s_started, s_pending;
static int t_start(const char *m, const char *url, const char *body, const char *ct,
                   const char *bearer, int ms) {
  (void)body; (void)ct; (void)bearer; (void)ms;
  if (s_pending) return -1;
  snprintf(s_method, sizeof s_method, "%s", m);
  snprintf(s_url, sizeof s_url, "%s", url);
  s_started++;
  s_pending = 1;
  return 0;
}
static int t_poll(char *out, size_t n) {
  if (!s_pending) return -1;
  s_pending = 0;
  if (n) out[0] = 0;
  return s_status;
}

static CardApi TF;

static void topen(void) {
  memset(&TF, 0, sizeof TF);
  TF.fmt = t_fmt; TF.str_len = t_strlen; TF.net_ready = t_ready;
  TF.http = t_http; TF.http_start = t_start; TF.http_poll = t_poll;
  api = &TF;
  memset(&G, 0, sizeof G);
  s_status = 0; s_calls = 0; s_started = 0; s_pending = 0;
  s_method[0] = s_url[0] = 0;
}

void test_roku_keys_name_the_ecp_buttons(void) {
  topen();
  CHECK(!strcmp(ecp_for_key('p'), "Power"));
  CHECK(!strcmp(ecp_for_key('+'), "VolumeUp"));
  CHECK(!strcmp(ecp_for_key('='), "VolumeUp"));      /* + unshifted */
  CHECK(!strcmp(ecp_for_key('-'), "VolumeDown"));
  CHECK(!strcmp(ecp_for_key('m'), "VolumeMute"));
  CHECK(!strcmp(ecp_for_key('1'), "InputHDMI1"));
  CHECK(!strcmp(ecp_for_key('4'), "InputHDMI4"));
  CHECK(!strcmp(ecp_for_key('t'), "InputTuner"));
  CHECK(!strcmp(ecp_for_key('a'), "InputAV1"));
  CHECK(!strcmp(ecp_for_key(CAPP_KEY_UP), "Up"));
  CHECK(!strcmp(ecp_for_key(CAPP_KEY_ENTER), "Select"));
  CHECK(!strcmp(ecp_for_key(CAPP_KEY_BACK), "Back"));
  CHECK(!strcmp(ecp_for_key('h'), "Home"));
  CHECK(ecp_for_key('z') == 0);
  CHECK(ecp_for_key('5') == 0);
}

void test_roku_typed_characters_are_url_encoded(void) {
  char k[16];
  topen();
  lit_key('a', k, sizeof k);  CHECK(!strcmp(k, "Lit_a"));
  lit_key('Z', k, sizeof k);  CHECK(!strcmp(k, "Lit_Z"));
  lit_key('7', k, sizeof k);  CHECK(!strcmp(k, "Lit_7"));
  lit_key(' ', k, sizeof k);  CHECK(!strcmp(k, "Lit_%20"));
  lit_key('&', k, sizeof k);  CHECK(!strcmp(k, "Lit_%26"));
  lit_key('/', k, sizeof k);  CHECK(!strcmp(k, "Lit_%2F"));
}

void test_roku_queue_keeps_a_burst_in_order_and_drops_past_full(void) {
  int i;
  char k[16];
  topen();
  for (i = 0; i < QUEUE_MAX; i++) {
    t_fmt(k, sizeof k, "K%d", i);
    CHECK_EQ(q_push(k), 0);
  }
  CHECK_EQ(q_push("over"), -1);
  for (i = 0; i < QUEUE_MAX; i++) {
    char want[16];
    t_fmt(want, sizeof want, "K%d", i);
    CHECK(!strcmp(q_front(), want));
    q_pop();
  }
  CHECK(q_front() == 0);
}

void test_roku_address_must_be_four_numbers(void) {
  char a[24];
  topen();
  CHECK(valid_ip("192.168.1.50"));
  CHECK(valid_ip("10.0.0.1"));
  CHECK(!valid_ip("192.168.1"));
  CHECK(!valid_ip("300.1.1.1"));
  CHECK(!valid_ip("1.2.3.4x"));
  CHECK(!valid_ip("1..3.4"));
  CHECK(!valid_ip(""));
  /* What the file holds: trimmed of the line's end and any spaces. */
  take_ip(" 192.168.1.50 \r\n", a, sizeof a);
  CHECK(!strcmp(a, "192.168.1.50"));
}

void test_roku_device_info_names_the_tv(void) {
  char name[TVNAME_MAX];
  topen();
  tv_name("<device-info><model-name>55S425</model-name>"
          "<friendly-device-name>TCL Roku TV</friendly-device-name>"
          "<user-device-name>Living room</user-device-name></device-info>",
          name, sizeof name);
  CHECK(!strcmp(name, "Living room"));
  tv_name("<model-name>55S425</model-name>"
          "<friendly-device-name>TCL Roku TV</friendly-device-name>"
          "<user-device-name></user-device-name>", name, sizeof name);
  CHECK(!strcmp(name, "TCL Roku TV"));
  tv_name("<model-name>55S425</model-name>", name, sizeof name);
  CHECK(!strcmp(name, "55S425"));
  tv_name("garbage", name, sizeof name);
  CHECK(!strcmp(name, "Roku"));
}

void test_roku_a_key_is_posted_from_tick(void) {
  topen();
  t_fmt(G.ip, sizeof G.ip, "192.168.1.50");
  CHECK_EQ(app_key(0, '+'), 1);
  CHECK_EQ(app_key(0, '+'), 1);
  CHECK_EQ(s_started, 0);                     /* nothing sent from the handler */
  app_tick(0, 0);
  CHECK_EQ(s_started, 1);
  CHECK(!strcmp(s_method, "POST"));
  CHECK(!strcmp(s_url, "http://192.168.1.50:8060/keypress/VolumeUp"));
  app_tick(0, 0);                             /* the answer, then the second press */
  app_tick(0, 0);
  CHECK_EQ(s_started, 2);
  CHECK(q_front() == 0);
}

void test_roku_a_refusal_says_where_to_allow_it(void) {
  topen();
  t_fmt(G.ip, sizeof G.ip, "192.168.1.50");
  app_key(0, 'm');
  app_key(0, 'm');
  s_status = -403;
  app_tick(0, 0);
  app_tick(0, 0);
  CHECK(G.bad);
  CHECK(strstr(G.status, "mobile apps") != 0);
  CHECK(q_front() == 0);                      /* the rest of the burst is dropped */
}

void test_roku_commands_send_and_say_what_they_did(void) {
  char out[96];
  const char *argv[1];
  topen();
  argv[0] = "on";
  CHECK_EQ(app_command(0, ACT_POWER, 1, argv, out, sizeof out), -1);
  CHECK(strstr(out, "address") != 0);         /* no TV yet */
  CHECK_EQ(s_calls, 0);

  t_fmt(G.ip, sizeof G.ip, "192.168.1.50");
  CHECK_EQ(app_command(0, ACT_POWER, 1, argv, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://192.168.1.50:8060/keypress/PowerOn"));
  argv[0] = "hdmi2";
  CHECK_EQ(app_command(0, ACT_INPUT, 1, argv, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://192.168.1.50:8060/keypress/InputHDMI2"));
  argv[0] = "down";
  CHECK_EQ(app_command(0, ACT_VOLUME, 1, argv, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://192.168.1.50:8060/keypress/VolumeDown"));
  CHECK_EQ(app_command(0, ACT_MUTE, 0, argv, out, sizeof out), 0);
  CHECK(!strcmp(s_url, "http://192.168.1.50:8060/keypress/VolumeMute"));

  s_status = -3;
  CHECK_EQ(app_command(0, ACT_MUTE, 0, argv, out, sizeof out), -1);
  CHECK(strstr(out, "192.168.1.50") != 0);
}
