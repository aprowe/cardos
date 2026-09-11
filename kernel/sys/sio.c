/* Standard input and output. See sio.h. */

#include "kernel/sys/sio.h"
#include "kernel/console/console.h"
#include "kernel/fs/fs.h"

#include <stdlib.h>
#include <string.h>

typedef enum { OUT_CONSOLE = 0, OUT_BUFFER, OUT_FILE } OutKind;
typedef enum { IN_NONE = 0, IN_BUFFER } InKind;

static OutKind s_out_kind;
static char   *s_out_buf;
static size_t  s_out_cap, s_out_len;
static int     s_out_full;
static int     s_out_fd = -1;

static InKind s_in_kind;
static char  *s_in_buf;
static size_t s_in_pos;

void sio_reset(void) {
  if (s_out_kind == OUT_FILE && s_out_fd >= 0) fs_close(s_out_fd);
  s_out_fd = -1;
  free(s_out_buf);
  s_out_buf = NULL;
  s_out_cap = s_out_len = 0;
  s_out_full = 0;
  s_out_kind = OUT_CONSOLE;

  free(s_in_buf);
  s_in_buf = NULL;
  s_in_pos = 0;
  s_in_kind = IN_NONE;
}

/* ---- stdout ------------------------------------------------------------- */

void sio_write(const char *s) {
  size_t n;
  if (!s || !*s) return;
  n = strlen(s);

  switch (s_out_kind) {
  case OUT_BUFFER:
    if (s_out_len + n >= s_out_cap) {
      /* Fill what is left and remember that we did. Dropping the rest quietly
       * would make a long pipe look like a short one. */
      size_t room = (s_out_cap > s_out_len + 1) ? s_out_cap - s_out_len - 1 : 0;
      if (room) { memcpy(s_out_buf + s_out_len, s, room); s_out_len += room; }
      s_out_buf[s_out_len] = 0;
      s_out_full = 1;
      return;
    }
    memcpy(s_out_buf + s_out_len, s, n);
    s_out_len += n;
    s_out_buf[s_out_len] = 0;
    return;

  case OUT_FILE:
    if (s_out_fd >= 0) fs_write(s_out_fd, s, n);
    return;

  default:
    con_write(s);
    return;
  }
}

void sio_write_line(const char *s) {
  sio_write(s);
  sio_write("\n");
}

int sio_out_to_buffer(size_t cap) {
  free(s_out_buf);
  s_out_buf = (char *)malloc(cap);
  if (!s_out_buf) { s_out_kind = OUT_CONSOLE; return 0; }
  s_out_buf[0] = 0;
  s_out_cap = cap;
  s_out_len = 0;
  s_out_full = 0;
  s_out_kind = OUT_BUFFER;
  return 1;
}

int sio_out_overflowed(void) { return s_out_full; }

char *sio_take_buffer(size_t *len) {
  char *b = s_out_buf;
  if (len) *len = s_out_len;
  s_out_buf = NULL;
  s_out_cap = s_out_len = 0;
  s_out_kind = OUT_CONSOLE;
  return b;
}

int sio_out_to_file(const char *path, int append) {
  /* FS_O_APPEND rather than opening and seeking: without TRUNC the open still
   * maps to "w+b", which truncates -- so ">>" was overwriting the file every
   * time and looked like only the last line had been written. */
  int fd = fs_open(path, FS_O_WRITE | FS_O_CREATE |
                         (append ? FS_O_APPEND : FS_O_TRUNC));
  if (fd < 0) return -1;
  s_out_fd = fd;
  s_out_kind = OUT_FILE;
  return 0;
}

/* ---- stdin -------------------------------------------------------------- */

void sio_in_from_buffer(char *buf) {
  free(s_in_buf);
  s_in_buf = buf;
  s_in_pos = 0;
  s_in_kind = buf ? IN_BUFFER : IN_NONE;
}

int sio_in_from_file(const char *path) {
  int fd = fs_open(path, FS_O_READ);
  int size, got = 0;
  char *buf;

  if (fd < 0) return -1;
  size = fs_seek(fd, 0, FS_SEEK_END);
  fs_seek(fd, 0, FS_SEEK_SET);
  if (size < 0) { fs_close(fd); return -1; }

  /* Read whole. A file large enough for this to matter is a file this machine
   * has nowhere to put anyway, and the cap says so rather than thrashing. */
  if (size > 32 * 1024) size = 32 * 1024;
  buf = (char *)malloc((size_t)size + 1);
  if (!buf) { fs_close(fd); return -1; }

  while (got < size) {
    int n = fs_read(fd, buf + got, (size_t)(size - got));
    if (n <= 0) break;
    got += n;
  }
  buf[got] = 0;
  fs_close(fd);

  sio_in_from_buffer(buf);
  return 0;
}

int sio_has_input(void) {
  return s_in_kind == IN_BUFFER && s_in_buf && s_in_buf[s_in_pos];
}

int sio_read_line(char *buf, size_t size) {
  size_t n = 0;

  if (!buf || size < 2) return -1;
  if (s_in_kind != IN_BUFFER || !s_in_buf) return -1;
  if (!s_in_buf[s_in_pos]) return -1;

  while (s_in_buf[s_in_pos] && s_in_buf[s_in_pos] != '\n') {
    char c = s_in_buf[s_in_pos++];
    if (c == '\r') continue;
    if (n < size - 1) buf[n++] = c;
  }
  if (s_in_buf[s_in_pos] == '\n') s_in_pos++;
  buf[n] = 0;
  return (int)n;
}
