/* The update manifest: what the proxy has, and what of it is newer than ours.
 *
 * Portable C -- no ESP-IDF, no filesystem -- so the parser and the diff run
 * under the host test suite. The device-side wrapper (update.c) fetches the
 * text and hashes the files; this decides.
 *
 * The format is one line per item, whitespace separated, because the device
 * has no JSON parser and one line per thing is all a manifest needs:
 *
 *   firmware <sha256 hex, 64 chars> <size>
 *   app <name> <fnv1a32 hex, 8 chars> <size> [folder]
 *   icon <fnv1a32 hex> <size> <name, to the end of the line>
 *
 * Unknown first words are skipped, so the proxy can add kinds without the
 * device refusing the whole file. `icon` lines (2026-10-16) are the colour
 * icons, /apps/icons/NAME.cic, named as the launcher looks them up -- the
 * app's own name, which may have a space in it, so it comes last. The server
 * sends them only to a device that asks (/update?icons=1): firmware from
 * before them would skip the lines, but it reads the manifest into a 4 KB
 * buffer that the icons would overflow.
 */
#ifndef CARDOS_MANIFEST_H
#define CARDOS_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

/* 24 was too few by 2026-10-01: the server listed 28 and the rest were
 * dropped unseen. Anything past this is counted in `dropped`. */
#define MANIFEST_MAX_APPS 64          /* 48 was full on 2026-10-27, with Crossword and Lamp */
#define MANIFEST_NAME_MAX 16
/* 52 icons on 2026-10-16: every app's, generic, the folders' and firmware. */
#define MANIFEST_MAX_ICONS 80
/* "folder-" and a folder, or an app's name as the launcher keeps it (19). */
#define MANIFEST_ICON_NAME_MAX 24

typedef struct {
  char     name[MANIFEST_NAME_MAX];   /* "pinball", without .capp */
  uint32_t hash;                      /* FNV-1a 32 of the file */
  uint32_t size;
  char     folder[MANIFEST_NAME_MAX]; /* "Games", or "" for the top level:
                                         where a first install goes */
} ManifestApp;

typedef struct {
  char     name[MANIFEST_ICON_NAME_MAX]; /* "Dashboard Link", without .cic */
  uint32_t hash;                         /* FNV-1a 32 of the file */
  uint32_t size;
} ManifestIcon;

typedef struct {
  int      has_firmware;
  uint8_t  firmware_sha[32];
  uint32_t firmware_size;
  int      napps;
  int      dropped;                   /* app lines past MANIFEST_MAX_APPS */
  char     fw_version[32];            /* "version firmware v0.15.1": what is offered */
  char     server_version[32];        /* "version server v0.15.1-2-gabc": the server */
  ManifestApp app[MANIFEST_MAX_APPS];
  int      nicons;
  int      icons_dropped;             /* icon lines past MANIFEST_MAX_ICONS */
  ManifestIcon icon[MANIFEST_MAX_ICONS];
} Manifest;

/* Parse `text`. Returns the number of items understood (firmware counts as
 * one), or -1 if nothing in it was a manifest line at all -- an HTML error
 * page, say. A partly-understood file is not an error: the lines that made
 * sense are kept. */
int manifest_parse(const char *text, Manifest *out);

/* FNV-1a 32, the same function icons.c uses for the blob stamp. Feed it in
 * pieces: start from MANIFEST_FNV_INIT. */
#define MANIFEST_FNV_INIT 2166136261u
uint32_t manifest_fnv1a(uint32_t h, const uint8_t *data, size_t n);

/* What the device has, for the diff. An app that is not on the card at all
 * has have_app[i] == 0. */
typedef struct {
  const uint8_t *own_sha;             /* our running firmware's, or NULL */
  int      have_app[MANIFEST_MAX_APPS];
  uint32_t app_hash[MANIFEST_MAX_APPS];
  uint8_t  have_icon[MANIFEST_MAX_ICONS];
  uint32_t icon_hash[MANIFEST_MAX_ICONS];
} ManifestLocal;

/* Fills `stale_app[i]` for every app in `m` whose hash differs or which we
 * do not have, and returns whether the firmware differs. Order follows `m`. */
int manifest_diff(const Manifest *m, const ManifestLocal *local,
                  int *stale_app);

/* Fills `stale_icon[i]` for every icon in `m` that differs or that we do
 * not have, and returns how many. */
int manifest_diff_icons(const Manifest *m, const ManifestLocal *local,
                        uint8_t *stale_icon);

#endif /* CARDOS_MANIFEST_H */
