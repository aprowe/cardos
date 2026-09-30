/* Several saved networks. What matters: the old one-network file still reads,
 * joining a second does not lose the first, and the one tried first is the
 * one that is actually in range. */
#include <string.h>
#include "tinytest.h"
#include "kernel/net/wifilist.h"

void test_wifilist_reads_the_old_one_network_file(void) {
  WifiList l;
  wifilist_parse(&l, "ChrandyBBYeahh\r\nhunter2  \n");
  CHECK_EQ(l.n, 1);
  CHECK(!strcmp(l.net[0].ssid, "ChrandyBBYeahh"));
  CHECK(!strcmp(l.net[0].pass, "hunter2"));
}

void test_wifilist_round_trips_pairs(void) {
  WifiList l, m;
  char text[256];
  memset(&l, 0, sizeof l);
  wifilist_remember(&l, "Home", "pw1");
  wifilist_remember(&l, "Office WiFi", "");            /* open, and a space */
  CHECK(wifilist_format(&l, text, sizeof text) > 0);
  CHECK(!strcmp(text, "Office WiFi\n\nHome\npw1\n"));
  wifilist_parse(&m, text);
  CHECK_EQ(m.n, 2);
  CHECK(!strcmp(m.net[0].ssid, "Office WiFi"));
  CHECK(!strcmp(m.net[0].pass, ""));
  CHECK(!strcmp(m.net[1].pass, "pw1"));
}

void test_wifilist_joining_again_moves_to_the_front(void) {
  WifiList l;
  memset(&l, 0, sizeof l);
  wifilist_remember(&l, "A", "1");
  wifilist_remember(&l, "B", "2");
  wifilist_remember(&l, "C", "3");
  wifilist_remember(&l, "A", "new");                     /* rejoined, new password */
  CHECK_EQ(l.n, 3);
  CHECK(!strcmp(l.net[0].ssid, "A"));
  CHECK(!strcmp(l.net[0].pass, "new"));
  CHECK(!strcmp(l.net[1].ssid, "C"));
  CHECK(!strcmp(l.net[2].ssid, "B"));
}

void test_wifilist_full_pushes_out_the_oldest(void) {
  WifiList l;
  char name[8];
  int i;
  memset(&l, 0, sizeof l);
  for (i = 0; i < WIFILIST_MAX + 2; i++) {
    name[0] = (char)('a' + i); name[1] = 0;
    wifilist_remember(&l, name, "x");
  }
  CHECK_EQ(l.n, WIFILIST_MAX);
  CHECK_EQ(l.net[0].ssid[0], 'a' + WIFILIST_MAX + 1);    /* newest first */
  CHECK_EQ(l.net[WIFILIST_MAX - 1].ssid[0], 'c');       /* a and b gone */
}

void test_wifilist_forget_one(void) {
  WifiList l;
  memset(&l, 0, sizeof l);
  wifilist_remember(&l, "A", "1");
  wifilist_remember(&l, "B", "2");
  CHECK_EQ(wifilist_forget(&l, "A"), 1);
  CHECK_EQ(wifilist_forget(&l, "A"), 0);
  CHECK_EQ(l.n, 1);
  CHECK(!strcmp(l.net[0].ssid, "B"));
}

/* In range beats recent: the scan saw Work strong and Home weak, and Phone
 * not at all -- try Work, then Home. */
void test_wifilist_tries_what_is_in_range_strongest_first(void) {
  WifiList l;
  const char *seen[] = { "Cafe", "Home", "Work" };
  const int8_t rssi[] = { -40, -80, -55 };
  int order[WIFILIST_MAX], n;
  memset(&l, 0, sizeof l);
  wifilist_remember(&l, "Work", "w");
  wifilist_remember(&l, "Phone", "p");
  wifilist_remember(&l, "Home", "h");                   /* most recent */
  n = wifilist_order(&l, seen, rssi, 3, order);
  CHECK_EQ(n, 2);
  CHECK(!strcmp(l.net[order[0]].ssid, "Work"));
  CHECK(!strcmp(l.net[order[1]].ssid, "Home"));
  /* none of them in the scan (hidden, or no scan): all, most recent first */
  n = wifilist_order(&l, NULL, NULL, 0, order);
  CHECK_EQ(n, 3);
  CHECK(!strcmp(l.net[order[0]].ssid, "Home"));
  CHECK(!strcmp(l.net[order[2]].ssid, "Work"));
}
