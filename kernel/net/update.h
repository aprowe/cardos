/* Pulling new builds from the PC. Device-only; the decisions live in
 * manifest.h, which is portable and tested.
 *
 * The proxy (tools/webproxy.py) serves what it last built under /update:
 * a manifest, the firmware, and each .capp. This fetches the manifest,
 * hashes what is on the card, reads the running firmware's own SHA, and says
 * what is stale; then installs it. Apps are downloaded beside the old one and
 * renamed over it, so a failed download costs nothing. The firmware goes to
 * the card and then through launcher_boot(), which validates it, writes the
 * OTA slot we are not running from, arms rollback and restarts. A new CardOS
 * confirms itself once its shell is up (src/main.c); one that crashes first
 * is rolled back by the bootloader.
 *
 * Everything here blocks. The callers are the console and the Claude
 * terminal's tick, both of which say what is happening first.
 */
#ifndef CARDOS_UPDATE_H
#define CARDOS_UPDATE_H

#include <stdint.h>
#include "kernel/net/manifest.h"

typedef struct {
  Manifest m;
  int firmware_stale;
  int stale[MANIFEST_MAX_APPS];
  int nstale_apps;
} UpdateCheck;

/* The proxy's base URL: the PROXY variable, or the compiled-in default. */
const char *update_base(void);

/* The bearer token from /claude.token, or "" if there is none. */
const char *update_token(void);

/* Fetch and compare. 0 on success, negative on failure; update_error() says
 * why in words. */
int update_check(UpdateCheck *out);

/* One line of progress at a time -- "pinball.capp 19672 bytes" -- for
 * whichever screen is listening. */
typedef void (*UpdateLog)(void *ctx, const char *line);

/* Install every stale app in `c`. Returns how many were installed; the log
 * gets a line per failure. Rescans the launcher's list afterwards. */
int update_apps(const UpdateCheck *c, UpdateLog log, void *ctx);

/* Download and install the firmware. Does not return on success -- the chip
 * restarts into the new image. On failure, returns negative and the old
 * firmware carries on. */
int update_firmware(UpdateLog log, void *ctx);

const char *update_error(void);

#endif /* CARDOS_UPDATE_H */
