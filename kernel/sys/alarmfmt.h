/* Alarms: the lines of /config/alarms.txt, and when each one rings.
 *
 * Shared by the kernel, which rings them (kernel/sys/alarm.c), and the Clock
 * app, which edits them -- so header-only and free of the C library, since an
 * app links against nothing. One alarm a line, readable and typeable:
 *
 *     on 07:00 -MTWTF- Work       weekdays; a letter rings that day, '-' not
 *     off 09:30 SMTWTFS           every day, switched off
 *     on 06:15 once Flight        the next 06:15, then switched off
 *     on 07:09 snooze Work        a snooze: rings once, then removed
 *
 * The seven places are Sunday to Saturday. The time is 24-hour local, as the
 * device's clock reads it (TZ applied). The label is the rest of the line.
 */
#ifndef CARDOS_ALARMFMT_H
#define CARDOS_ALARMFMT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define AF_OPT __attribute__((unused))
#else
#define AF_OPT
#endif

#define ALARM_FILE      "/config/alarms.txt"
#define ALARM_LABEL_MAX 24
#define ALARM_MAX       16
#define ALARM_SNOOZE_MIN 9

enum { ALARM_REPEAT = 0, ALARM_ONCE, ALARM_SNOOZE };

typedef struct {
  uint8_t on;
  uint8_t hour, min;
  uint8_t days;                  /* bit 0 Sunday .. bit 6 Saturday; REPEAT only */
  uint8_t kind;                  /* ALARM_* */
  char    label[ALARM_LABEL_MAX];
} Alarm;

static AF_OPT int af_word(const char **p, const char *w) {
  const char *s = *p;
  while (*w) { if (*s != *w) return 0; s++; w++; }
  if (*s && *s != ' ' && *s != '\t') return 0;
  *p = s;
  return 1;
}

static AF_OPT void af_skip(const char **p) { while (**p == ' ' || **p == '\t') (*p)++; }

static AF_OPT int af_num(const char **p, int max_digits, int *out) {
  int v = 0, n = 0;
  while (**p >= '0' && **p <= '9' && n < max_digits) { v = v * 10 + (**p - '0'); (*p)++; n++; }
  if (!n) return 0;
  *out = v;
  return 1;
}

/* One line into *a. 1, or 0 for anything that is not an alarm (a comment, a
 * blank, a typo): the kernel skips it rather than ringing at a guess. */
static AF_OPT int alarm_parse(const char *line, Alarm *a) {
  const char *p = line;
  int h, m, i, n;
  a->on = 0; a->hour = 0; a->min = 0; a->days = 0; a->kind = ALARM_REPEAT; a->label[0] = 0;
  af_skip(&p);
  if (af_word(&p, "on")) a->on = 1;
  else if (!af_word(&p, "off")) return 0;
  af_skip(&p);
  if (!af_num(&p, 2, &h) || *p != ':') return 0;
  p++;
  if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')) return 0;
  af_num(&p, 2, &m);
  if (h > 23 || m > 59 || (*p && *p != ' ' && *p != '\t')) return 0;
  a->hour = (uint8_t)h; a->min = (uint8_t)m;
  af_skip(&p);
  if (af_word(&p, "once")) a->kind = ALARM_ONCE;
  else if (af_word(&p, "snooze")) a->kind = ALARM_SNOOZE;
  else {
    for (i = 0; i < 7; i++) {
      if (!p[i] || p[i] == ' ' || p[i] == '\t') return 0;
      if (p[i] != '-') a->days |= (uint8_t)(1u << i);
    }
    if (p[7] && p[7] != ' ' && p[7] != '\t' && p[7] != '\r') return 0;
    p += 7;
  }
  af_skip(&p);
  for (n = 0; p[n] && p[n] != '\r' && p[n] != '\n' && n < ALARM_LABEL_MAX - 1; n++)
    a->label[n] = p[n];
  while (n > 0 && (a->label[n - 1] == ' ' || a->label[n - 1] == '\t')) n--;
  a->label[n] = 0;
  return 1;
}

static AF_OPT int af_put(char *out, int n, int at, const char *s) {
  while (*s && at < n - 1) out[at++] = *s++;
  out[at < n ? at : n - 1] = 0;
  return at;
}

/* The line for *a, NUL-terminated. Returns its length. */
static AF_OPT int alarm_format(const Alarm *a, char *out, int n) {
  static const char LETTER[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };
  char t[6], d[8];
  int at = 0, i;
  t[0] = (char)('0' + a->hour / 10); t[1] = (char)('0' + a->hour % 10); t[2] = ':';
  t[3] = (char)('0' + a->min / 10);  t[4] = (char)('0' + a->min % 10);  t[5] = 0;
  at = af_put(out, n, at, a->on ? "on " : "off ");
  at = af_put(out, n, at, t);
  at = af_put(out, n, at, " ");
  if (a->kind == ALARM_ONCE) at = af_put(out, n, at, "once");
  else if (a->kind == ALARM_SNOOZE) at = af_put(out, n, at, "snooze");
  else {
    for (i = 0; i < 7; i++) d[i] = (a->days >> i) & 1 ? LETTER[i] : '-';
    d[7] = 0;
    at = af_put(out, n, at, d);
  }
  if (a->label[0]) { at = af_put(out, n, at, " "); at = af_put(out, n, at, a->label); }
  return at;
}

/* Does *a ring at this minute? `wday` 0 = Sunday. */
static AF_OPT int alarm_rings_at(const Alarm *a, int wday, int hour, int min) {
  if (!a->on || a->hour != hour || a->min != min) return 0;
  if (a->kind != ALARM_REPEAT) return 1;
  return (a->days >> wday) & 1;
}

/* Minutes from now (wday, hour:min) to its next ring, 1 .. 7 days; an alarm
 * set for this very minute is a week away (or a day, once), since this
 * minute's ring is already happening. -1 if it will not ring. */
static AF_OPT int alarm_minutes_until(const Alarm *a, int wday, int hour, int min) {
  int now = hour * 60 + min, at = a->hour * 60 + a->min, d;
  if (!a->on) return -1;
  if (a->kind != ALARM_REPEAT) return at > now ? at - now : at - now + 1440;
  for (d = 0; d <= 7; d++) {
    int day = (wday + d) % 7, mins = d * 1440 + at - now;
    if (mins <= 0) continue;
    if ((a->days >> day) & 1) return mins;
  }
  return -1;
}

/* "every day", "weekdays", "weekends", "once", or "Mon Wed Fri". */
static AF_OPT void alarm_days_text(const Alarm *a, char *out, int n) {
  static const char *const NAME[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  int at = 0, i;
  out[0] = 0;
  if (a->kind == ALARM_ONCE)   { af_put(out, n, 0, "once"); return; }
  if (a->kind == ALARM_SNOOZE) { af_put(out, n, 0, "snoozed"); return; }
  if (a->days == 0x7F)         { af_put(out, n, 0, "every day"); return; }
  if (a->days == 0x3E)         { af_put(out, n, 0, "weekdays"); return; }
  if (a->days == 0x41)         { af_put(out, n, 0, "weekends"); return; }
  if (!a->days)                { af_put(out, n, 0, "no days"); return; }
  for (i = 0; i < 7; i++)
    if ((a->days >> i) & 1) {
      if (at) at = af_put(out, n, at, " ");
      at = af_put(out, n, at, NAME[i]);
    }
}

/* A time as people say it: "7", "7:30", "07:30", "7am", "7:15pm", "19:00",
 * and "7.30", which is what a recogniser makes of a colon. 1, or 0. */
static AF_OPT int alarm_parse_time(const char *s, int *hour, int *min) {
  const char *p = s;
  int h, m = 0, pm = -1;
  af_skip(&p);
  if (!af_num(&p, 2, &h)) return 0;
  if (*p == ':' || *p == '.') {
    p++;
    if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9')) return 0;
    af_num(&p, 2, &m);
  }
  af_skip(&p);
  if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 'm' || p[1] == 'M')) { pm = 0; p += 2; }
  else if ((p[0] == 'p' || p[0] == 'P') && (p[1] == 'm' || p[1] == 'M')) { pm = 1; p += 2; }
  af_skip(&p);
  if (*p || m > 59) return 0;
  if (pm >= 0) {
    if (h < 1 || h > 12) return 0;
    h = h % 12 + (pm ? 12 : 0);
  } else if (h > 23) {
    return 0;
  }
  *hour = h;
  *min = m;
  return 1;
}

#endif /* CARDOS_ALARMFMT_H */
