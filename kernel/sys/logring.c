/* See logring.h. */

#include "kernel/sys/logring.h"

static int put(char *out, size_t n, size_t i, char c) {
  if (i + 1 < n) out[i] = c;
  return 1;
}

/* Right-aligned seconds with three decimals, so lines stay in a column and
 * the eye can see a two-second gap without reading the digits. */
static size_t put_stamp(char *out, size_t n, size_t i, uint32_t ms) {
  uint32_t secs = ms / 1000u, frac = ms % 1000u;
  char buf[16];
  int len = 0, j;

  if (!secs) buf[len++] = '0';
  while (secs && len < (int)sizeof buf) { buf[len++] = (char)('0' + secs % 10); secs /= 10; }

  i += put(out, n, i, '[');
  for (j = len; j < 5; j++) i += put(out, n, i, ' ');   /* pad to five */
  for (j = len - 1; j >= 0; j--) i += put(out, n, i, buf[j]);
  i += put(out, n, i, '.');
  i += put(out, n, i, (char)('0' + (frac / 100) % 10));
  i += put(out, n, i, (char)('0' + (frac / 10) % 10));
  i += put(out, n, i, (char)('0' + frac % 10));
  i += put(out, n, i, ']');
  i += put(out, n, i, ' ');
  return i;
}

int logring_line(char *out, size_t n, uint32_t ms, const char *tag,
                 const char *msg) {
  size_t i = 0;

  if (!out || n < 2) return 0;

  i = put_stamp(out, n, i, ms);
  if (tag && *tag) {
    while (*tag) i += put(out, n, i, *tag++);
    i += put(out, n, i, ':');
    i += put(out, n, i, ' ');
  }
  if (msg) {
    /* A newline inside a message would make one entry look like two, and a
     * log you cannot count the entries in is a log you cannot trust. */
    while (*msg) {
      char c = *msg++;
      i += put(out, n, i, (c == '\n' || c == '\r') ? ' ' : c);
    }
  }

  /* The newline is not optional: it is what makes the next line a line. If
   * the message filled the buffer, it is cut short to make room. */
  if (i > n - 2) i = n - 2;
  out[i++] = '\n';
  out[i] = 0;
  return (int)i;
}

int logring_rotate_needed(uint32_t size, int add, uint32_t cap) {
  if (add < 0) add = 0;
  return size + (uint32_t)add > cap;
}
