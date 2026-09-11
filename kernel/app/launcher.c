/* Booting a guest firmware. See launcher.h. */

#include "kernel/app/launcher.h"
#include "kernel/fs/fs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "launch";

#define GUEST_SLOT_BYTES (3u * 1024 * 1024)   /* ota_0 in partitions.csv */
#define COPY_CHUNK 4096

/* ------------------------------------------------------- image reading -- */

static int fs_reader(void *ctx, uint32_t offset, void *buf, size_t n) {
  int fd = *(int *)ctx;
  if (fs_seek(fd, (int32_t)offset, FS_SEEK_SET) < 0) return -1;
  return fs_read(fd, buf, n) == (int)n ? 0 : -1;
}

LaunchResult launcher_check(const char *path, AppImageInfo *info,
                            AppImageResult *why) {
  FsStat st;
  AppImageResult r;
  int fd;

  if (why) *why = APPIMAGE_ERR_READ;
  if (fs_stat(path, &st) != 0 || st.is_dir) return LAUNCH_ERR_OPEN;

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return LAUNCH_ERR_OPEN;

  r = appimage_parse(fs_reader, &fd, st.size, GUEST_SLOT_BYTES, info);
  fs_close(fd);

  if (why) *why = r;
  return (r == APPIMAGE_OK) ? LAUNCH_OK : LAUNCH_ERR_IMAGE;
}

/* --------------------------------------------------------- arming ------- */

/* otadata holds two of these, one per 4 KB sector. The CRC covers seq alone,
 * so state can be rewritten without recomputing it. */
typedef struct {
  uint32_t seq;
  uint8_t  label[20];
  uint32_t state;
  uint32_t crc;
} OtaSelect;

#define OTA_STATE_NEW  0x0u          /* ESP_OTA_IMG_NEW */
#define OTA_SEQ_EMPTY  0xFFFFFFFFu
#define OTA_SECTOR     0x1000u

/* Mark the freshly selected app as NEW, so the bootloader runs it as
 * PENDING_VERIFY. Nothing ever confirms it -- a foreign firmware has never
 * heard of esp_ota_mark_app_valid_cancel_rollback -- so the next reset rolls
 * back to the factory partition, which is CardOS.
 *
 * esp_ota_set_boot_partition() does NOT do this by itself. That was wrong in
 * the first draft of the spec, and without this the promise that reset brings
 * you home simply does not hold.
 *
 * Flash bits only go 1 -> 0 and NEW is zero, so the field is written in place.
 * Erasing here would destroy the selection just made. */
static int arm_rollback(void) {
  const esp_partition_t *od = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  OtaSelect a, b;
  size_t offset;
  uint32_t state = OTA_STATE_NEW;
  int a_live, b_live;

  if (!od) return -1;
  if (esp_partition_read(od, 0, &a, sizeof a) != ESP_OK) return -1;
  if (esp_partition_read(od, OTA_SECTOR, &b, sizeof b) != ESP_OK) return -1;

  /* set_boot_partition just bumped one entry; the live one has the higher
   * sequence number. An unwritten entry reads as all ones. */
  a_live = (a.seq != OTA_SEQ_EMPTY);
  b_live = (b.seq != OTA_SEQ_EMPTY);
  if (a_live && b_live) offset = (b.seq > a.seq) ? OTA_SECTOR : 0;
  else if (b_live)      offset = OTA_SECTOR;
  else if (a_live)      offset = 0;
  else                  return -1;          /* neither written: nothing to arm */

  if (esp_partition_write(od, offset + offsetof(OtaSelect, state),
                          &state, sizeof state) != ESP_OK)
    return -1;

  ESP_LOGI(TAG, "armed slot %u as pending-verify", (unsigned)(offset ? 1 : 0));
  return 0;
}

/* ---------------------------------------------------------- booting ----- */

LaunchResult launcher_boot(const char *path, LaunchProgress cb, void *ctx) {
  AppImageInfo info;
  AppImageResult why;
  const esp_partition_t *target;
  esp_ota_handle_t handle = 0;
  uint8_t *buf;
  uint32_t done = 0;
  int fd, last_pct = -1;
  LaunchResult r;

  /* Validate before erasing anything. An erase is irreversible and takes
   * seconds; a bad image should cost neither. */
  r = launcher_check(path, &info, &why);
  if (r != LAUNCH_OK) return r;

  target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (!target) return LAUNCH_ERR_NO_SLOT;

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return LAUNCH_ERR_OPEN;

  /* Allocated rather than static: this runs once, seconds before the chip
     restarts, and a permanent 4 KB reservation for it is 4 KB the rest of
     CardOS never gets back. */
  buf = malloc(COPY_CHUNK);
  if (!buf) { fs_close(fd); return LAUNCH_ERR_ERASE; }

  if (esp_ota_begin(target, info.image_size, &handle) != ESP_OK) {
    fs_close(fd);
    free(buf);
    return LAUNCH_ERR_ERASE;
  }

  while (done < info.image_size) {
    uint32_t want = info.image_size - done;
    int got;
    if (want > COPY_CHUNK) want = COPY_CHUNK;
    got = fs_read(fd, buf, want);
    if (got <= 0) break;
    if (esp_ota_write(handle, buf, (size_t)got) != ESP_OK) {
      fs_close(fd);
      free(buf);
      esp_ota_abort(handle);
      return LAUNCH_ERR_WRITE;
    }
    done += (uint32_t)got;
    if (cb) {
      int pct = (int)((uint64_t)done * 100 / info.image_size);
      if (pct != last_pct) { last_pct = pct; cb(ctx, pct); }
    }
  }
  fs_close(fd);
  free(buf);

  /* esp_ota_end verifies the appended SHA-256, which is why this layer does
   * not carry its own crypto. */
  if (esp_ota_end(handle) != ESP_OK) return LAUNCH_ERR_VERIFY;
  if (esp_ota_set_boot_partition(target) != ESP_OK) return LAUNCH_ERR_SELECT;

  if (arm_rollback() != 0) {
    /* The guest will boot, but reset will not bring us home. Say so rather
     * than restarting into a device that looks bricked. */
    return LAUNCH_ERR_ARM;
  }

  esp_restart();
  return LAUNCH_OK;                 /* not reached */
}

const char *launcher_strerror(LaunchResult r) {
  switch (r) {
  case LAUNCH_OK:          return "ok";
  case LAUNCH_ERR_IMAGE:   return "not a bootable image";
  case LAUNCH_ERR_NO_SLOT: return "no ota_0: the partition table is wrong";
  case LAUNCH_ERR_OPEN:    return "cannot open";
  case LAUNCH_ERR_ERASE:   return "could not erase the guest slot";
  case LAUNCH_ERR_WRITE:   return "write failed partway";
  case LAUNCH_ERR_VERIFY:  return "image failed verification";
  case LAUNCH_ERR_SELECT:  return "could not set the boot partition";
  case LAUNCH_ERR_ARM:     return "booted but reset will not return here";
  }
  return "unknown error";
}

/* ------------------------------------------------------------- info ----- */

/* Read the guest slot straight out of flash. */
static int part_reader(void *ctx, uint32_t offset, void *buf, size_t n) {
  const esp_partition_t *p = (const esp_partition_t *)ctx;
  return esp_partition_read(p, offset, buf, n) == ESP_OK ? 0 : -1;
}

void launcher_info(LauncherInfo *out) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *guest = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  AppImageInfo info;

  memset(out, 0, sizeof *out);
  if (running) {
    strncpy(out->running, running->label, sizeof out->running - 1);
  }
  if (!guest) return;

  /* allow_trailing: an image in a partition is followed by the rest of the
   * partition, which is not a full-flash dump. */
  if (appimage_parse_ex(part_reader, (void *)guest, guest->size,
                        GUEST_SLOT_BYTES, 1, &info) == APPIMAGE_OK) {
    out->guest_valid = 1;
    out->guest_size = info.image_size;
    strncpy(out->guest_name, info.project_name, sizeof out->guest_name - 1);
    strncpy(out->guest_version, info.version, sizeof out->guest_version - 1);
  }
}
