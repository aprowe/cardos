/* The saved networks. See wifilist.h. */

#include "kernel/net/wifilist.h"

#include <string.h>

static void copy_trim(char *out, size_t size, const char *s, size_t n) {
  while (n && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  if (n > size - 1) n = size - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

void wifilist_parse(WifiList *l, const char *text) {
  const char *p = text;
  char ssid[WIFILIST_SSID];
  int line = 0;
  memset(l, 0, sizeof *l);
  while (p && *p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (line % 2 == 0) {
      copy_trim(ssid, sizeof ssid, p, (size_t)(e - p));
    } else if (ssid[0] && l->n < WIFILIST_MAX) {
      memcpy(l->net[l->n].ssid, ssid, sizeof ssid);
      copy_trim(l->net[l->n].pass, sizeof l->net[0].pass, p, (size_t)(e - p));
      l->n++;
    }
    line++;
    p = *e ? e + 1 : e;
  }
  /* An SSID on the last line with no password line after it: an open network
   * written by hand. */
  if (line % 2 == 1 && ssid[0] && l->n < WIFILIST_MAX) {
    memcpy(l->net[l->n].ssid, ssid, sizeof ssid);
    l->net[l->n].pass[0] = 0;
    l->n++;
  }
}

int wifilist_format(const WifiList *l, char *out, size_t size) {
  size_t at = 0;
  int i;
  if (!size) return -1;
  out[0] = 0;
  for (i = 0; i < l->n; i++) {
    size_t a = strlen(l->net[i].ssid), b = strlen(l->net[i].pass);
    if (at + a + b + 3 > size) return -1;
    memcpy(out + at, l->net[i].ssid, a); at += a; out[at++] = '\n';
    memcpy(out + at, l->net[i].pass, b); at += b; out[at++] = '\n';
    out[at] = 0;
  }
  return (int)at;
}

static int find(const WifiList *l, const char *ssid) {
  int i;
  for (i = 0; i < l->n; i++) if (!strcmp(l->net[i].ssid, ssid)) return i;
  return -1;
}

void wifilist_remember(WifiList *l, const char *ssid, const char *pass) {
  WifiSaved w;
  int at = find(l, ssid), i;
  if (!ssid || !ssid[0]) return;
  memset(&w, 0, sizeof w);
  strncpy(w.ssid, ssid, sizeof w.ssid - 1);
  strncpy(w.pass, pass ? pass : "", sizeof w.pass - 1);
  if (at < 0) at = l->n < WIFILIST_MAX ? l->n++ : WIFILIST_MAX - 1;   /* the oldest goes */
  for (i = at; i > 0; i--) l->net[i] = l->net[i - 1];
  l->net[0] = w;
}

int wifilist_forget(WifiList *l, const char *ssid) {
  int at = find(l, ssid), i;
  if (at < 0) return 0;
  for (i = at; i + 1 < l->n; i++) l->net[i] = l->net[i + 1];
  l->n--;
  memset(&l->net[l->n], 0, sizeof l->net[0]);
  return 1;
}

int wifilist_order(const WifiList *l, const char *const *seen, const int8_t *rssi,
                   int nseen, int *order) {
  int n = 0, i, j, best;
  int8_t strength[WIFILIST_MAX];
  for (i = 0; i < l->n; i++) {
    for (j = 0; j < nseen; j++) if (!strcmp(seen[j], l->net[i].ssid)) break;
    if (j < nseen) { order[n] = i; strength[n] = rssi[j]; n++; }
  }
  if (!n) {
    for (i = 0; i < l->n; i++) order[i] = i;
    return l->n;
  }
  /* strongest first: a selection sort over at most eight */
  for (i = 0; i < n; i++) {
    best = i;
    for (j = i + 1; j < n; j++) if (strength[j] > strength[best]) best = j;
    if (best != i) {
      int t = order[i]; int8_t s = strength[i];
      order[i] = order[best]; strength[i] = strength[best];
      order[best] = t; strength[best] = s;
    }
  }
  return n;
}
