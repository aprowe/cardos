/* CardOS filesystem over FAT32 on the SD card. Device-only.
 *
 * The Cardputer's card is on SPI: CS 12, MOSI 14, MISO 39, SCK 40. The sibling
 * project runs this bus at 20 MHz reliably, so that is what we use rather than
 * guessing higher.
 *
 * CardOS paths are absolute and rooted at "/"; they are mapped onto the VFS
 * mount point here so nothing above this file knows or cares where FAT is
 * actually mounted.
 */

#include "kernel/fs/fs.h"
#include "kernel/fs/path.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SD_CS   12
#define PIN_SD_MOSI 14
#define PIN_SD_MISO 39
#define PIN_SD_SCK  40

#define SD_HOST   SPI3_HOST
#define SD_FREQ_KHZ 20000        /* 20 MHz: proven on this board */

#define MOUNT_POINT "/sd"

static const char *TAG = "fs";
static sdmmc_card_t *s_card;
static int           s_mounted;
static FILE         *s_open[FS_MAX_OPEN];

/* The share server opens files from its own task while the shell may be
 * opening one too. FatFs itself is built reentrant; this table was the only
 * unguarded thing. A critical section rather than a mutex because a slot
 * claim is eight pointer reads. */
static portMUX_TYPE  s_open_lock = portMUX_INITIALIZER_UNLOCKED;

/* Map a CardOS path onto the VFS mount point. */
static int real_path(const char *cardos_path, char *out, size_t out_size) {
  char norm[FS_PATH_MAX];
  size_t mp = strlen(MOUNT_POINT);
  size_t n;

  if (path_normalize(cardos_path, norm, sizeof norm) != 0) return -1;
  n = strlen(norm);
  if (mp + n + 1 > out_size) return -1;

  memcpy(out, MOUNT_POINT, mp);
  /* "/" maps to the mount point itself, not to "/sd/". */
  if (n == 1 && norm[0] == '/') { out[mp] = '\0'; return 0; }
  memcpy(out + mp, norm, n + 1);
  return 0;
}

int fs_mount(void) {
  esp_err_t err;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  esp_vfs_fat_sdmmc_mount_config_t cfg = {
    /* Do not format on failure. An unreadable card is far more likely to be
     * the user's card in the wrong slot than a card that wants erasing, and
     * silently reformatting someone's SD card is unforgivable. */
    .format_if_mount_failed = false,
    .max_files = FS_MAX_OPEN,
    .allocation_unit_size = 16 * 1024,
  };
  spi_bus_config_t bus = {
    .mosi_io_num = PIN_SD_MOSI,
    .miso_io_num = PIN_SD_MISO,
    .sclk_io_num = PIN_SD_SCK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,
  };

  if (s_mounted) return 0;

  host.slot = SD_HOST;
  host.max_freq_khz = SD_FREQ_KHZ;

  err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
    return -1;
  }

  slot.gpio_cs = PIN_SD_CS;
  slot.host_id = SD_HOST;

  err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &cfg, &s_card);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mount: %s", esp_err_to_name(err));
    return -1;
  }
  s_mounted = 1;
  return 0;
}

int  fs_mounted(void) { return s_mounted; }

void fs_unmount(void) {
  if (!s_mounted) return;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
  s_mounted = 0;
  s_card = NULL;
}

void fs_space(uint64_t *total, uint64_t *freebytes) {
  FATFS *fatfs;
  DWORD free_clusters;
  if (total) *total = 0;
  if (freebytes) *freebytes = 0;
  if (!s_mounted) return;
  if (f_getfree("0:", &free_clusters, &fatfs) != FR_OK) return;
  if (total)
    *total = (uint64_t)(fatfs->n_fatent - 2) * fatfs->csize * FF_SS_SDCARD;
  if (freebytes)
    *freebytes = (uint64_t)free_clusters * fatfs->csize * FF_SS_SDCARD;
}

/* ------------------------------------------------------------- files ---- */

#define FD_CLAIMED ((FILE *)1)

static int alloc_fd(void) {
  int i, fd = -1;
  taskENTER_CRITICAL(&s_open_lock);
  for (i = 0; i < FS_MAX_OPEN; i++)
    if (!s_open[i]) { s_open[i] = FD_CLAIMED; fd = i; break; }
  taskEXIT_CRITICAL(&s_open_lock);
  return fd;
}

static void free_fd(int fd) {
  taskENTER_CRITICAL(&s_open_lock);
  s_open[fd] = NULL;
  taskEXIT_CRITICAL(&s_open_lock);
}

int fs_open(const char *path, int flags) {
  char real[FS_PATH_MAX + 8];
  const char *mode;
  int fd;
  FILE *f;

  if (!s_mounted) return -1;
  if (real_path(path, real, sizeof real) != 0) return -1;

  fd = alloc_fd();
  if (fd < 0) return -1;

  if (flags & FS_O_APPEND)      mode = (flags & FS_O_READ) ? "a+b" : "ab";
  else if (flags & FS_O_TRUNC)  mode = (flags & FS_O_READ) ? "w+b" : "wb";
  else if (flags & FS_O_WRITE)  mode = (flags & FS_O_CREATE) ? "w+b" : "r+b";
  else                          mode = "rb";

  f = fopen(real, mode);
  if (!f) { free_fd(fd); return -1; }

  s_open[fd] = f;
  return fd;
}

static FILE *file_of(int fd) {
  if (fd < 0 || fd >= FS_MAX_OPEN || s_open[fd] == FD_CLAIMED) return NULL;
  return s_open[fd];
}

int fs_read(int fd, void *buf, size_t n) {
  FILE *f = file_of(fd);
  size_t got;
  if (!f) return -1;
  got = fread(buf, 1, n, f);
  if (got == 0 && ferror(f)) return -1;
  return (int)got;
}

int fs_write(int fd, const void *buf, size_t n) {
  FILE *f = file_of(fd);
  size_t put;
  if (!f) return -1;
  put = fwrite(buf, 1, n, f);
  if (put != n) return -1;
  return (int)put;
}

int fs_seek(int fd, int32_t off, int whence) {
  FILE *f = file_of(fd);
  int w = (whence == FS_SEEK_CUR) ? SEEK_CUR
        : (whence == FS_SEEK_END) ? SEEK_END : SEEK_SET;
  if (!f) return -1;
  if (fseek(f, (long)off, w) != 0) return -1;
  return (int)ftell(f);
}

void fs_close(int fd) {
  FILE *f = file_of(fd);
  if (!f) return;
  fclose(f);
  free_fd(fd);
}

/* -------------------------------------------------------- directories --- */

int fs_stat(const char *path, FsStat *out) {
  char real[FS_PATH_MAX + 8];
  struct stat st;
  if (!s_mounted) return -1;
  if (real_path(path, real, sizeof real) != 0) return -1;
  if (stat(real, &st) != 0) return -1;
  out->size = (uint32_t)st.st_size;
  out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  out->mtime = (uint32_t)st.st_mtime;
  return 0;
}

int fs_opendir(const char *path, FsDir *d) {
  char real[FS_PATH_MAX + 8];
  DIR *dir;
  d->impl = NULL;
  if (!s_mounted) return -1;
  if (real_path(path, real, sizeof real) != 0) return -1;
  if (real[0] == '\0') strcpy(real, MOUNT_POINT);
  dir = opendir(real);
  if (!dir) return -1;
  d->impl = dir;
  /* Remember the directory so readdir can stat each child. */
  strncpy(d->prefix, real, sizeof d->prefix - 1);
  d->prefix[sizeof d->prefix - 1] = '\0';
  return 0;
}

int fs_readdir(FsDir *d, FsEntry *out) {
  struct dirent *e;
  struct stat st;
  char child[FS_PATH_MAX * 2];

  if (!d->impl) return -1;
  e = readdir((DIR *)d->impl);
  if (!e) return 0;

  strncpy(out->name, e->d_name, FS_NAME_MAX);
  out->name[FS_NAME_MAX] = '\0';
  out->size = 0;
  out->is_dir = 0;
  out->mtime = 0;
  if (snprintf(child, sizeof child, "%s/%s", d->prefix, e->d_name) > 0 &&
      stat(child, &st) == 0) {
    out->size = (uint32_t)st.st_size;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->mtime = (uint32_t)st.st_mtime;
  }
  return 1;
}

void fs_closedir(FsDir *d) {
  if (!d->impl) return;
  closedir((DIR *)d->impl);
  d->impl = NULL;
}

int fs_list(const char *dir, FsEntry *out, int max) {
  FsDir d;
  int n = 0;
  if (fs_opendir(dir, &d) != 0) return -1;
  while (n < max && fs_readdir(&d, &out[n]) == 1) n++;
  fs_closedir(&d);
  return n;
}

int fs_mkdir(const char *path) {
  char real[FS_PATH_MAX + 8];
  if (!s_mounted) return -1;
  if (real_path(path, real, sizeof real) != 0) return -1;
  if (mkdir(real, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

int fs_remove(const char *path) {
  char real[FS_PATH_MAX + 8];
  FsStat st;
  if (!s_mounted) return -1;
  if (real_path(path, real, sizeof real) != 0) return -1;
  if (fs_stat(path, &st) == 0 && st.is_dir) return rmdir(real) == 0 ? 0 : -1;
  return unlink(real) == 0 ? 0 : -1;
}

int fs_rename(const char *from, const char *to) {
  char rf[FS_PATH_MAX + 8], rt[FS_PATH_MAX + 8];
  if (!s_mounted) return -1;
  if (real_path(from, rf, sizeof rf) != 0) return -1;
  if (real_path(to, rt, sizeof rt) != 0) return -1;
  return rename(rf, rt) == 0 ? 0 : -1;
}

int fs_ensure_layout(void) {
  static const char *dirs[] = { "/cardos", "/cardos/apps", "/cardos/src", "/home" };
  size_t i;
  int bad = 0;
  if (!s_mounted) return -1;
  for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    if (fs_mkdir(dirs[i]) != 0) bad++;
  return bad ? -1 : 0;
}
