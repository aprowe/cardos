/* Booting a guest firmware. Device-only.
 *
 * Copies a validated app image from the SD card into the ota_0 partition,
 * points the bootloader at it, arms the rollback, and restarts. CardOS exits;
 * the guest owns the chip until the next reset, at which point the unconfirmed
 * rollback brings us back.
 *
 * See docs/specs/2026-09-10-app-launcher-design.md, and the sibling
 * CardLaunch project, which is where the awkward details came from.
 */
#ifndef CARDOS_LAUNCHER_H
#define CARDOS_LAUNCHER_H

#include <stdint.h>

#include "app/appimage.h"

typedef enum {
  LAUNCH_OK = 0,
  LAUNCH_ERR_IMAGE,      /* the validator refused it; see the AppImageResult */
  LAUNCH_ERR_NO_SLOT,    /* no ota_0 -- the partition table is wrong */
  LAUNCH_ERR_OPEN,
  LAUNCH_ERR_ERASE,
  LAUNCH_ERR_WRITE,
  LAUNCH_ERR_VERIFY,     /* esp_ota_end refused: the SHA-256 did not match */
  LAUNCH_ERR_SELECT,
  LAUNCH_ERR_ARM         /* copied and selected, but rollback is not armed */
} LaunchResult;

typedef void (*LaunchProgress)(void *ctx, int percent);

/* Validate `path` without touching flash. Fills `info` when it returns
 * LAUNCH_OK, and `why` with the validator's verdict either way. */
LaunchResult launcher_check(const char *path, AppImageInfo *info,
                            AppImageResult *why);

/* Copy, select, arm and restart. Does not return on success.
 *
 * If it returns at all, something failed and CardOS is still running -- the
 * guest slot may hold a partly written image, but the boot partition has not
 * been changed unless the result is LAUNCH_ERR_ARM. */
LaunchResult launcher_boot(const char *path, LaunchProgress cb, void *ctx);

const char *launcher_strerror(LaunchResult r);

typedef struct {
  char running[17];       /* label of the partition we are executing from */
  int  guest_valid;       /* ota_0 holds something that parses as an image */
  char guest_name[33];    /* its project name, if it has a descriptor */
  char guest_version[33];
  uint32_t guest_size;
} LauncherInfo;

void launcher_info(LauncherInfo *out);

#endif /* CARDOS_LAUNCHER_H */
