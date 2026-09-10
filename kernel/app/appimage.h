/* Structural validation of an ESP32-S3 application image.
 *
 * Portable C11 -- no ESP-IDF -- so the parser that decides whether a foreign
 * firmware is safe to flash runs under the host test suite. That matters more
 * than usual: the failure mode of a bad image reaching
 * esp_ota_set_boot_partition() is a device that boot-loops, recoverable only
 * by a USB reflash.
 *
 * This layer does NOT verify the appended SHA-256. esp_ota_end() already does
 * that correctly and refuses to finalise a bad image, so reimplementing it
 * here would add a crypto dependency for no gain. What this catches is the
 * cheap, likely stuff -- a truncated SD copy, an image for the wrong chip, a
 * file that is not an image at all -- before spending four seconds and 4 MB
 * of flash writes finding out.
 */
#ifndef CARDOS_APPIMAGE_H
#define CARDOS_APPIMAGE_H

#include <stddef.h>
#include <stdint.h>

#define APPIMAGE_MAGIC          0xE9u
#define APPIMAGE_CHIP_ESP32S3   0x0009u
#define APPIMAGE_APP_DESC_MAGIC 0xABCD5432u

#define APPIMAGE_HEADER_SIZE    24u
#define APPIMAGE_SEG_HEADER_SIZE 8u
#define APPIMAGE_MAX_SEGMENTS   16u

typedef enum {
  APPIMAGE_OK = 0,
  APPIMAGE_ERR_TOO_SMALL,   /* file cannot even hold a header */
  APPIMAGE_ERR_MAGIC,       /* not an ESP32 image */
  APPIMAGE_ERR_CHIP,        /* built for a different chip */
  APPIMAGE_ERR_SEGMENTS,    /* absurd segment count or length */
  APPIMAGE_ERR_TRUNCATED,   /* segment table runs past the end of the file */
  APPIMAGE_ERR_TOO_BIG,     /* will not fit the guest partition */
  APPIMAGE_ERR_READ         /* the reader failed */
} AppImageResult;

typedef struct {
  uint32_t image_size;      /* bytes the image actually occupies */
  uint32_t entry_addr;
  uint16_t chip_id;
  uint8_t  segment_count;
  uint8_t  hash_appended;

  int  has_app_desc;        /* an esp_app_desc_t was found and looked sane */
  char project_name[33];    /* the fields are not NUL-terminated on the wire */
  char version[33];
  char date[17];
  char idf_ver[33];
} AppImageInfo;

/* Read `n` bytes at `offset`. Returns 0 on success. Reading through a
 * callback rather than from a buffer means the device can walk a multi-
 * megabyte image on the SD card eight bytes at a time, while the host tests
 * hand it a synthetic image in RAM. */
typedef int (*AppImageRead)(void *ctx, uint32_t offset, void *buf, size_t n);

AppImageResult appimage_parse(AppImageRead read, void *ctx,
                              uint32_t file_size, uint32_t max_size,
                              AppImageInfo *out);

/* Convenience wrapper for a whole image already in memory. */
AppImageResult appimage_parse_buffer(const void *buf, uint32_t len,
                                     uint32_t max_size, AppImageInfo *out);

const char *appimage_strerror(AppImageResult r);

#endif /* CARDOS_APPIMAGE_H */
