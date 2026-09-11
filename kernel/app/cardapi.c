/* The API table handed to a loaded program.
 *
 * A program links against nothing and resolves no symbols; it calls through
 * this table. That removes symbol resolution from the loader entirely, and it
 * versions cleanly -- a program built against a table CardOS no longer
 * provides is refused at load rather than crashing at the first call.
 */

#include "kernel/app/capp.h"
#include "kernel/ui/draw.h"
#include "kernel/fs/fs.h"
#include "kernel/net/http.h"
#include "kernel/net/wifi.h"
#include "kernel/net/gauth.h"
#include "kernel/sys/sio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

/* Defined by capprun.c, which knows which program is currently running. */
void capprun_install_ui(const CappUi *ui);

static Rect to_rect(CRect r) {
  Rect o;
  o.x = r.x; o.y = r.y; o.w = r.w; o.h = r.h;
  return o;
}

static void api_fill(CRect r, uint16_t c)  { draw_rect(to_rect(r), c); }
static void api_frame(CRect r, uint16_t c) { draw_frame(to_rect(r), c); }
static void api_bevel(CRect r, uint16_t f, uint16_t tl, uint16_t br) {
  draw_bevel(to_rect(r), f, tl, br);
}
static void api_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  draw_text(x, y, s, fg, bg);
}

/* Raw blit, clipped like everything else -- a program must not be able to draw
 * over another window's chrome. */
static void api_pixels(CRect r, const uint16_t *px) {
  Rect want = to_rect(r);
  Rect vis = rect_intersect(want, draw_clip());
  int16_t y;
  if (rect_is_empty(vis) || !px) return;
  for (y = vis.y; y < vis.y + vis.h; y++) {
    const uint16_t *row = px + (size_t)(y - want.y) * (size_t)want.w
                             + (size_t)(vis.x - want.x);
    display_blit(vis.x, y, vis.w, 1, row);
  }
}

static int api_open(const char *p, int f)             { return fs_open(p, f); }
static int api_read(int fd, void *b, size_t n)        { return fs_read(fd, b, n); }
static int api_write(int fd, const void *b, size_t n) { return fs_write(fd, b, n); }
static int api_seek(int fd, int32_t o, int w)         { return fs_seek(fd, o, w); }
static void api_close(int fd)                         { fs_close(fd); }

/* Flattened listing: names packed into a caller-supplied array of fixed-width
 * slots, so a program needs no allocator and no knowledge of FsEntry. */
static int api_list(const char *dir, char *out, int max_entries, int name_len) {
  FsDir d;
  FsEntry e;
  int n = 0;
  if (!out || max_entries <= 0 || name_len <= 1) return -1;
  if (fs_opendir(dir, &d) != 0) return -1;
  while (n < max_entries && fs_readdir(&d, &e) == 1) {
    snprintf(out + (size_t)n * (size_t)name_len, (size_t)name_len, "%s", e.name);
    n++;
  }
  fs_closedir(&d);
  return n;
}

static void *api_memset(void *d, int c, size_t n)          { return memset(d, c, n); }
static void *api_memcpy(void *d, const void *s, size_t n)  { return memcpy(d, s, n); }
static void *api_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static size_t api_strlen(const char *s)                    { return strlen(s); }

static int api_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

static uint32_t api_ticks(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void api_log(const char *m) { ESP_LOGI("app", "%s", m ? m : ""); }

static void api_out(const char *s)              { sio_write(s); }
static void api_out_line(const char *s)         { sio_write_line(s); }
static int  api_in_line(char *b, size_t n)      { return sio_read_line(b, n); }
static int  api_has_input(void)                 { return sio_has_input(); }

static int api_http_get(const char *url, char *buf, size_t n, int timeout_ms) {
  return http_get(url, buf, n, timeout_ms);
}
static int api_net_ready(void) { return wifi_is_connected(); }
static const char *api_net_status(void) { return wifi_status(); }
static int api_http_download(const char *url, const char *path, int t) {
  return http_download(url, path, t);
}
static int api_net_connect(int timeout_ms) {
  if (wifi_is_connected()) return 0;
  return wifi_connect_saved(timeout_ms);
}

static int api_http(const char *method, const char *url, const char *body,
                    const char *content_type, const char *bearer,
                    char *out, size_t out_size, int timeout_ms) {
  return http_request(method, url, body, content_type, bearer, out, out_size,
                      timeout_ms);
}

static const char *api_google_token(void)  { return gauth_token(); }
static const char *api_google_status(void) { return gauth_status(); }

static void api_ui(const CappUi *ui) { capprun_install_ui(ui); }

static const CardApi API = {
  CAPP_API_VERSION,
  api_fill, api_frame, api_bevel, api_text, api_pixels,
  api_open, api_read, api_write, api_seek, api_close, api_list,
  api_memset, api_memcpy, api_memmove, api_strlen, api_fmt,
  api_ticks, api_log,
  api_out, api_out_line, api_in_line, api_has_input,
  api_http_get, api_net_ready, api_net_connect, api_net_status,
  api_http_download, api_http,
  api_google_token, api_google_status,
  api_ui,
};

const CardApi *cardos_api(void) { return &API; }
