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
 *   NAME/       a folder: one level of subdirectory, scanned the same way
 *
 * /firmware is scanned as well, for .bin only, and appears as a folder called
 * Firmware rather than as loose entries on the desktop.
 */
#ifndef CARDOS_ICONS_H
#define CARDOS_ICONS_H

#include <stdint.h>

#include "kernel/ui/app.h"

#define ICONS_DIR "/desktop"

/* Firmware images are scanned from here too. They were already living in
 * /firmware before CardOS existed -- put there by the tools that built them --
 * and asking the user to copy them into /desktop to see them would be asking
 * them to keep two copies of a 1.4 MB file in step. They are shown inside a
 * Firmware folder, so the desktop is apps and the one thing that is not an
 * app takes a deliberate step to reach. */
#define FIRMWARE_DIR "/firmware"

/* Where the IDE keeps assembly source, and where its example is seeded. */
#define ASM_DIR "/asm"
/* Everything the scan can hold: top-level entries, folders, and what is
 * inside them, all in one flat table.
 *
 * This was 24, and the app set grew past it -- four top-level entries, three
 * folders holding thirteen apps between them, and a Firmware folder with four
 * images is 25. The scan simply stopped when it ran out, so the last folder
 * looked empty and nobody was told. Sized with room now rather than exactly,
 * because the failure is silent. */
#define MAX_ICONS 48

typedef enum { ICON_BUILTIN, ICON_FIRMWARE, ICON_CAPP, ICON_FOLDER } IconKind;

typedef struct {
  char     name[20];    /* shown under the icon */
  char     path[80];    /* full path */
  IconKind kind;
  int      slot;        /* app registry index, or capprun slot */
  int      cli;         /* a command: runnable, but not shown anywhere */
  int      parent;      /* flat index of the folder this sits in, or -1 */

  /* The colour icon, read from the card the first time it is asked for.
   * colour_tried separates "no file" from "not looked yet", so a missing one
   * is not re-read on every repaint. */
  uint16_t *colour;
  int       colour_tried;
} Icon;

/* Seeds the folder if it is empty and loads every .capp it finds. Safe to call
 * again; unloads what it loaded first. */
void icons_reload(void);

/* What the shells draw: everything except the command-line tools, which are
 * sorted to the end so a carousel can walk 0..count-1 without gaps. */
int icons_count(void);

/* Everything, commands included -- what a name or path lookup searches. */
int icons_total(void);

const Icon *icon_at(int i);

/* One level of the tree. `folder` is a flat index of an ICON_FOLDER entry,
 * or -1 for the top. Only visible entries, so a carousel can walk 0..n-1.
 * icons_count() and icon_at() stay flat -- every level at once -- because
 * the desktop and every by-name lookup want exactly that. */
int         icons_in_count(int folder);
const Icon *icons_in_at(int folder, int i);

/* The flat index of an entry, for the calls that take one. -1 if not ours. */
int icon_index(const Icon *ic);

/* The app behind an icon, or NULL for firmware. */
const AppDef *icon_app(int i);

/* 16x16 1bpp, or NULL for an entry with no icon of its own. */
const uint8_t *icon_bitmap(int i);

/* 16x16 RGB565 from /desktop/icons/NAME.cic, or NULL if there is no such file.
 * Preferred over the 1bpp shape when present: a colour icon is the one someone
 * drew on purpose. */
const uint16_t *icon_colour(int i);

#define ICON_DIR ICONS_DIR "/icons"

/* Chain-boot a firmware entry. Does not return on success. Returns 0 if the
 * icon is not firmware or the image was refused. */
int icons_boot_firmware(int i);

#endif /* CARDOS_ICONS_H */
