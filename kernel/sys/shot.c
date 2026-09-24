/* Screenshots. See shot.h for why it works the way it does. */

#include "kernel/sys/shot.h"
#include "kernel/app/capp.h"   /* the card layout */

#include "kernel/drv/display.h"
#include "kernel/fs/fs.h"
#include "kernel/net/http.h"
#include "kernel/net/update.h"    /* update_base: the proxy's address */

#include <stdio.h>
#include <string.h>

#define SHOT_DIR CAPP_HOME "/shots"

static int  s_fd = -1;
static char s_path[48];
static char s_error[80];

const char *shot_error(void)     { return s_error; }
const char *shot_last_path(void) { return s_path; }

/* One blit, mirrored: each row of it lands at the same place in the file
 * as on the panel. Rows rather than the block, because the file is
 * row-major and the block is not. */
static void tap(int x, int y, int w, int h, const uint16_t *px) {
  int r;
  if (s_fd < 0) return;
  if (x < 0 || y < 0 || x + w > DISPLAY_W || y + h > DISPLAY_H) return;
  for (r = 0; r < h; r++) {
    int32_t off = ((int32_t)(y + r) * DISPLAY_W + x) * 2;
    if (fs_seek(s_fd, off, FS_SEEK_SET) != off) return;
    fs_write(s_fd, px + (size_t)r * (size_t)w, (size_t)w * 2);
  }
}

int shot_take(const char *name, void (*repaint)(void)) {
  static const uint16_t zero[DISPLAY_W];
  char url[160], reply[64];
  int y, n;

  s_error[0] = 0;
  if (!name || !*name) name = "shot";
  if (!fs_mounted()) { snprintf(s_error, sizeof s_error, "no card"); return -1; }
  if (fs_mkdir(SHOT_DIR) != 0) { /* already there */ }
  snprintf(s_path, sizeof s_path, SHOT_DIR "/%s.565", name);

  s_fd = fs_open(s_path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (s_fd < 0) { snprintf(s_error, sizeof s_error, "cannot write %s", s_path); return -1; }

  /* Sized first, so a repaint that skips a region -- which a shell with
   * damage tracking is entitled to do -- still leaves a file of the right
   * length, black where nothing was drawn. */
  for (y = 0; y < DISPLAY_H; y++) fs_write(s_fd, zero, sizeof zero);

  display_set_tap(tap);
  if (repaint) repaint();
  display_set_tap(NULL);
  fs_close(s_fd);
  s_fd = -1;

  snprintf(url, sizeof url, "%s/shot?name=%s", update_base(), name);
  /* With the device's shared secret, as voice and update send it: a remote
   * server answers 403 without one, and the shot stayed on the card. */
  {
    const char *t = update_token();
    n = http_post_file_progress(url, s_path, "application/octet-stream",
                                *t ? t : NULL, reply, sizeof reply, 15000, NULL);
  }
  if (n < 0) snprintf(s_error, sizeof s_error, "on the card, but the proxy said %d", n);
  return 0;
}
