#include "kernel/sys/tzreply.h"

#include <string.h>

/* The characters a POSIX TZ rule is made of: names, offsets, the M.w.d
 * dates, /time, and <+0530> quoted names. No slash-separated words -- that
 * is an IANA name, which the C library would silently read as UTC. */
static int rule_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == ',' || c == '.' || c == '+' ||
         c == '-' || c == '/' || c == ':' || c == '<' || c == '>';
}

/* The rest of the line after `key ` in `reply`, copied, or -1. */
static int field(const char *reply, const char *key, char *out, size_t n) {
  size_t klen = strlen(key), i = 0;
  const char *p = reply;
  while (p && *p) {
    if (!strncmp(p, key, klen) && p[klen] == ' ') {
      p += klen + 1;
      while (p[i] && p[i] != '\n' && p[i] != '\r') {
        if (i + 1 >= n) return -1;               /* too long: refuse, not cut */
        out[i] = p[i];
        i++;
      }
      out[i] = 0;
      return 0;
    }
    p = strchr(p, '\n');
    if (p) p++;
  }
  return -1;
}

int tzreply_parse(const char *reply, char *rule, size_t rn, char *zone, size_t zn) {
  size_t i;
  int letters = 0;

  if (!rule || rn < 2 || !zone || zn < 1) return -1;
  rule[0] = 0;
  zone[0] = 0;
  if (!reply || field(reply, "tz", rule, rn) != 0 || !rule[0]) return -1;

  for (i = 0; rule[i]; i++) {
    if (!rule_char(rule[i])) { rule[0] = 0; return -1; }
    if ((rule[i] >= 'A' && rule[i] <= 'Z') || (rule[i] >= 'a' && rule[i] <= 'z'))
      letters++;
  }
  /* An IANA name passes the character test ("America/Los_Angeles" has only
   * letters, a slash and an underscore -- the underscore fails it, but
   * "Asia/Tokyo" would not). A rule never has a slash before its first
   * comma: that is where a zone name has its one. */
  {
    const char *slash = strchr(rule, '/'), *comma = strchr(rule, ',');
    if (slash && (!comma || slash < comma)) { rule[0] = 0; return -1; }
  }
  if (letters < 3 && rule[0] != '<') { rule[0] = 0; return -1; }

  if (field(reply, "zone", zone, zn) != 0) zone[0] = 0;
  return 0;
}
