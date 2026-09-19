/* The file half of conf.h: thin over fs, on the device only. */

#include "kernel/sys/conf.h"
#include "kernel/fs/fs.h"
#include "kernel/app/capp.h"   /* CAPP_CONFIG */

#include <string.h>

#define TEXT_MAX 640   /* three Google lines are under 460 bytes */

int conf_read(const char *path, char *lines, int n, size_t width) {
  char text[TEXT_MAX];
  int fd, got;
  if (!fs_mounted()) return 0;
  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return 0;
  got = fs_read(fd, text, sizeof text - 1);
  fs_close(fd);
  if (got < 0) got = 0;
  text[got] = 0;
  return conf_split(text, lines, n, width);
}

int conf_write(const char *path, const char *const *values, int n) {
  char text[TEXT_MAX];
  int fd, len;
  if (!fs_mounted()) return -1;
  len = conf_join(values, n, text, sizeof text);
  if (len < 0) return -1;
  fs_mkdir(CAPP_CONFIG);
  fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return -1;
  fs_write(fd, text, (size_t)len);
  fs_close(fd);
  return 0;
}

void conf_remove(const char *path) {
  if (!fs_mounted()) return;
  fs_remove(path);
}
