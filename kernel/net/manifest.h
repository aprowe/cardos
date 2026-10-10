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
 *   app <name> <fnv1a32 hex, 8 chars> <size>
 *
 * Unknown first words are skipped, so the proxy can add kinds without the
 * device refusing the whole file.
 */
#ifndef CARDOS_MANIFEST_H
#define CARDOS_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

/* 24 was too few by 2026-10-01: the server listed 28 and the rest were
 * dropped unseen. Anything past this is counted in `dropped`. */
#define MANIFEST_MAX_APPS 48
#define MANIFEST_NAME_MAX 16

typedef struct {
  char     name[MANIFEST_NAME_MAX];   /* "pinball", without .capp */
  uint32_t hash;                      /* FNV-1a 32 of the file */
  uint32_t size;
  char     folder[MANIFEST_NAME_MAX]; /* "Games", or "" for the top level:
                                         where a first install goes */
} ManifestApp;

typedef struct {
  int      has_firmware;
  uint8_t  firmware_sha[32];
  uint32_t firmware_size;
  int      napps;
  int      dropped;                   /* app lines past MANIFEST_MAX_APPS */
  char     fw_version[32];            /* "version firmware v0.15.1": what is offered */
  char     server_version[32];        /* "version server v0.15.1-2-gabc": the server */
  ManifestApp app[MANIFEST_MAX_APPS];
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
} ManifestLocal;

/* Fills `stale_app[i]` for every app in `m` whose hash differs or which we
 * do not have, and returns whether the firmware differs. Order follows `m`. */
int manifest_diff(const Manifest *m, const ManifestLocal *local,
                  int *stale_app);

#endif /* CARDOS_MANIFEST_H */
