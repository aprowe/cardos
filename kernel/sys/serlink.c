/* The serial link. See serlink.h; the frames are serframe.c.
 *
 * Buffers are taken when a frame starts and given back when it is answered
 * -- about 19 KB for the moment it takes, never held -- except the last
 * reply, which `re` may ask for again.
 */
#include "kernel/sys/serlink.h"
#include "kernel/sys/serframe.h"
#include "kernel/sys/shot.h"
#include "kernel/console/console.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define DATA_MAX   6144                      /* sh output, a listing */
#define REPLY_MAX  (DATA_MAX / 3 * 4 + 96)
#define FIELDS     5

static SerlinkHooks s_hooks;
static uint8_t s_keys[64];
static int     s_kh, s_kt;                    /* queued by `key` */
static char   *s_last;                        /* the last reply, for `re` */
static int     s_last_n;

void serlink_init(const SerlinkHooks *hooks) { s_hooks = *hooks; }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* In pieces: the driver's ring buffer is 256 bytes, and a write larger than
 * the ring never fits -- it is refused outright, every time, so any reply
 * over 256 bytes simply never went. */
static void send(const char *buf, int n) {
  int o = 0, k, m;
  uint32_t until = now_ms() + 5000;
  while (o < n && now_ms() < until) {
    m = n - o > 128 ? 128 : n - o;
    k = usb_serial_jtag_write_bytes(buf + o, (size_t)m, pdMS_TO_TICKS(100));
    if (k > 0) o += k;
  }
}

static void answer(const char *id, int ok, const uint8_t *data, int n) {
  char *out = malloc(REPLY_MAX);
  int len;
  if (!out) return;
  len = sf_reply(out, REPLY_MAX, id, ok ? "ok" : "err", data, n);
  if (len < 0) len = sf_reply(out, REPLY_MAX, id, "err", (const uint8_t *)"reply too big", 13);
  send(out, len);
  free(s_last);
  s_last = malloc((size_t)len);               /* its own size, not REPLY_MAX */
  s_last_n = s_last ? len : 0;
  if (s_last) memcpy(s_last, out, (size_t)len);
  free(out);
}

static void answer_text(const char *id, int ok, const char *text) {
  answer(id, ok, (const uint8_t *)text, (int)strlen(text));
}

static long num(const char *s) { return s ? strtol(s, NULL, 10) : 0; }

static void do_ls(const char *id, const char *path, char *data) {
  FsDir d;
  FsEntry e;
  int o = 0, n = 0, more = 0;
  if (fs_opendir(path, &d) != 0) { answer_text(id, 0, "cannot list it"); return; }
  data[0] = 0;
  while (fs_readdir(&d, &e) == 1) {
    if (o + (int)strlen(e.name) + 40 >= DATA_MAX) { more++; continue; }
    o += snprintf(data + o, DATA_MAX - o, "%s\t%lu\t%c\t%lu\n", e.name, (unsigned long)e.size,
                  e.is_dir ? 'd' : 'f', (unsigned long)e.mtime);
    n++;
  }
  fs_closedir(&d);
  if (more) o += snprintf(data + o, DATA_MAX - o, "+%d more\n", more);
  answer(id, 1, (const uint8_t *)data, o);
}

static void do_get(const char *id, const char *path, long off, long len, uint8_t *data) {
  int fd, n;
  if (len <= 0 || len > SF_CHUNK) len = SF_CHUNK;
  if ((fd = fs_open(path, FS_O_READ)) < 0) { answer_text(id, 0, "cannot open it"); return; }
  if (off && fs_seek(fd, (int32_t)off, FS_SEEK_SET) != off) {
    fs_close(fd);
    answer_text(id, 0, "cannot seek there");
    return;
  }
  n = fs_read(fd, data, (size_t)len);
  fs_close(fd);
  if (n < 0) { answer_text(id, 0, "read failed"); return; }
  answer(id, 1, data, n);
}

static void do_put(const char *id, const char *path, long off, const char *b64, uint8_t *data) {
  char part[FS_PATH_MAX + 8], msg[48];
  FsStat st;
  int n = sf_b64_decode(b64, (int)strlen(b64), data, SF_PUT), fd;
  if (n < 0) { answer_text(id, 0, "bad base64"); return; }
  snprintf(part, sizeof part, "%s.part", path);
  if (off == 0) {
    fd = fs_open(part, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  } else {
    if (fs_stat(part, &st) != 0) { answer_text(id, 0, "no upload under way"); return; }
    /* The reply to this chunk was lost and it was sent again: it is there. */
    if ((long)st.size == off + n) { answer_text(id, 1, ""); return; }
    if ((long)st.size != off) {
      snprintf(msg, sizeof msg, "the upload is at %lu", (unsigned long)st.size);
      answer_text(id, 0, msg);
      return;
    }
    fd = fs_open(part, FS_O_WRITE | FS_O_APPEND);
  }
  if (fd < 0) { answer_text(id, 0, "cannot write it"); return; }
  if (n && fs_write(fd, data, (size_t)n) != n) { fs_close(fd); answer_text(id, 0, "write failed (card full?)"); return; }
  fs_close(fd);
  answer_text(id, 1, "");
}

static void do_commit(const char *id, const char *path, long size) {
  char part[FS_PATH_MAX + 8], msg[64];
  FsStat st;
  snprintf(part, sizeof part, "%s.part", path);
  if (fs_stat(part, &st) != 0) { answer_text(id, 0, "no upload under way"); return; }
  if ((long)st.size != size) {
    snprintf(msg, sizeof msg, "got %lu bytes of %ld", (unsigned long)st.size, size);
    answer_text(id, 0, msg);
    return;
  }
  fs_remove(path);                         /* the card's rename will not replace */
  if (fs_rename(part, path) != 0) { answer_text(id, 0, "rename failed"); return; }
  answer_text(id, 1, "");
}

static void handle(char *body) {
  char *f[FIELDS], *data;
  int n = sf_split(body, f, FIELDS);
  const char *id = f[0], *verb = n > 1 ? f[1] : "";
  const char *a = n > 2 ? f[2] : "", *b = n > 3 ? f[3] : NULL, *c = n > 4 ? f[4] : NULL;

  if (!strcmp(verb, "re")) {
    if (s_last) send(s_last, s_last_n);
    else answer_text(id, 0, "nothing to repeat");
    return;
  }
  if ((data = malloc(DATA_MAX + 1)) == NULL) { answer_text(id, 0, "out of memory"); return; }

  if (!strcmp(verb, "ping")) answer_text(id, 1, "serlink 1");
  else if (!strcmp(verb, "state")) {
    data[0] = 0;
    if (s_hooks.state) s_hooks.state(data, DATA_MAX);
    answer_text(id, 1, data);
  } else if (!strcmp(verb, "sh")) {
    int rc;
    data[0] = 0;
    rc = s_hooks.shell ? s_hooks.shell(a, data, DATA_MAX) : -1;
    answer_text(id, rc == 0, data);
  } else if (!strcmp(verb, "ls")) do_ls(id, *a ? a : "/", data);
  else if (!strcmp(verb, "stat")) {
    FsStat st;
    if (fs_stat(a, &st) != 0) answer_text(id, 0, "not there");
    else {
      snprintf(data, DATA_MAX, "%lu\t%c\t%lu", (unsigned long)st.size, st.is_dir ? 'd' : 'f',
               (unsigned long)st.mtime);
      answer_text(id, 1, data);
    }
  } else if (!strcmp(verb, "get")) do_get(id, a, num(b), num(c), (uint8_t *)data);
  else if (!strcmp(verb, "put")) do_put(id, a, num(b), c ? c : "", (uint8_t *)data);
  else if (!strcmp(verb, "commit")) do_commit(id, a, num(b));
  else if (!strcmp(verb, "rm")) answer_text(id, fs_remove(a) == 0, "");
  else if (!strcmp(verb, "mv")) answer_text(id, b && fs_rename(a, b) == 0, "");
  else if (!strcmp(verb, "mkdir")) answer_text(id, fs_mkdir(a) == 0, "");
  else if (!strcmp(verb, "open")) {
    int rc = s_hooks.open ? s_hooks.open(a, b) : -1;
    answer_text(id, rc == 0, rc == 0 ? "" : "no such app");
  } else if (!strcmp(verb, "key")) {
    uint8_t k[64];
    int i, m = sf_b64_decode(a, (int)strlen(a), k, sizeof k);
    for (i = 0; i < m && (s_kt + 1) % 64 != s_kh; i++) { s_keys[s_kt] = k[i]; s_kt = (s_kt + 1) % 64; }
    answer_text(id, m >= 0, m >= 0 ? "" : "bad base64");
  } else if (!strcmp(verb, "shot")) {
    if (shot_capture("link", s_hooks.repaint) != 0) answer_text(id, 0, shot_error());
    else answer_text(id, 1, shot_last_path());
  } else answer_text(id, 0, "no such request");
  free(data);
}

/* Read the rest of a frame whose STX has arrived. The PC sends a frame in
 * one write, so it comes in a burst; a sender that stops halfway is given
 * up on after a second and a half of silence. */
static void read_frame(void) {
  SfReader r;
  uint8_t chunk[256];
  char *buf = malloc(SF_MAX);
  uint32_t last = now_ms();
  int i, k, rc;
  if (!buf) return;
  sf_reader_init(&r, buf, SF_MAX);
  sf_feed(&r, SF_STX);
  while (now_ms() - last < 1500) {
    k = usb_serial_jtag_read_bytes(chunk, sizeof chunk, pdMS_TO_TICKS(20));
    if (k <= 0) continue;
    last = now_ms();
    for (i = 0; i < k; i++) {
      rc = sf_feed(&r, chunk[i]);
      if (rc == SF_FRAME) {
        /* Bytes after ETX in the same burst are keys; the PC never sends
         * any, so they are dropped rather than queued out of order. */
        handle(buf);
        free(buf);
        return;
      }
      if (rc == SF_BAD) { free(buf); return; }   /* the PC times out and resends */
    }
  }
  free(buf);
}

int serlink_key(int *activity) {
  int c;
  if (s_kh != s_kt) {
    c = s_keys[s_kh];
    s_kh = (s_kh + 1) % 64;
    return c;
  }
  c = con_serial_key();
  if (c != SF_STX) return c;
  read_frame();
  if (activity) *activity = 1;
  return 0;
}
