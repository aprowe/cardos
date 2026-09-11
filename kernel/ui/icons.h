/* What is on the desktop: the contents of /desktop, resolved. Device-only.
 *
 * Shared by the desktop shell and the launcher, which are two presentations of
 * the same list. Scanning it twice would mean loading every .capp twice, and
 * executable RAM is not a thing to spend on a duplicate.
 *
 * Three kinds of entry, told apart by extension:
 *
 *   NAME.app    a built-in app, by name
 *   NAME.capp   a loadable app binary, which carries its own name and icon
 *   NAME.bin    a firmware image to chain-boot, replacing CardOS
 */
#ifndef CARDOS_ICONS_H
#define CARDOS_ICONS_H

#include <stdint.h>

#include "kernel/ui/app.h"

#define ICONS_DIR "/desktop"
#define MAX_ICONS 24

typedef enum { ICON_BUILTIN, ICON_FIRMWARE, ICON_CAPP } IconKind;

typedef struct {
  char     name[20];    /* shown under the icon */
  char     path[80];    /* full path */
  IconKind kind;
  int      slot;        /* app registry index, or capprun slot */
} Icon;

/* Seeds the folder if it is empty and loads every .capp it finds. Safe to call
 * again; unloads what it loaded first. */
void icons_reload(void);

int         icons_count(void);
const Icon *icon_at(int i);

/* The app behind an icon, or NULL for firmware. */
const AppDef *icon_app(int i);

/* 16x16 1bpp, or NULL for an entry with no icon of its own. */
const uint8_t *icon_bitmap(int i);

/* Chain-boot a firmware entry. Does not return on success. Returns 0 if the
 * icon is not firmware or the image was refused. */
int icons_boot_firmware(int i);

#endif /* CARDOS_ICONS_H */
