#include "tinytest.h"
#include "kernel/app/appimage.h"
#include <string.h>

/* ---- synthetic image builder -------------------------------------------
 * Builds a byte-accurate ESP32-S3 app image so the parser is tested against
 * the real layout rather than against itself.
 *
 *   0  esp_image_header_t         24 bytes
 *  24  segment header             8 bytes (load_addr, data_len)
 *  32  segment data               -- for segment 0 this begins with a
 *                                    256-byte esp_app_desc_t
 *      ... further segments ...
 *      padding + 1 checksum byte, aligned so the total is a multiple of 16
 *      optional 32-byte SHA-256
 */

static unsigned char g_img[70000];
static uint32_t      g_len;

static void put_u16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static uint32_t build(uint8_t segs, uint16_t chip, int hash_appended, int with_desc) {
  uint32_t pos = APPIMAGE_HEADER_SIZE, i, total;
  memset(g_img, 0, sizeof g_img);
  g_img[0] = APPIMAGE_MAGIC;
  g_img[1] = segs;
  put_u32(g_img + 4, 0x40378000u);            /* entry_addr */
  put_u16(g_img + 12, chip);
  g_img[23] = (unsigned char)(hash_appended ? 1 : 0);

  for (i = 0; i < segs; i++) {
    uint32_t dlen = (i == 0) ? 512u : 256u;
    put_u32(g_img + pos, 0x3C000000u);        /* load_addr */
    put_u32(g_img + pos + 4, dlen);
    pos += APPIMAGE_SEG_HEADER_SIZE;
    if (i == 0 && with_desc) {
      put_u32(g_img + pos, APPIMAGE_APP_DESC_MAGIC);
      memcpy(g_img + pos + 16, "1.2.3", 5);          /* version[32]      */
      memcpy(g_img + pos + 48, "bruce", 5);          /* project_name[32] */
      memcpy(g_img + pos + 96, "Sep 10 2026", 11);   /* date[16]         */
      memcpy(g_img + pos + 112, "v5.2.1", 6);        /* idf_ver[32]      */
    }
    pos += dlen;
  }
  total = (pos + 1u + 15u) & ~15u;            /* checksum byte, 16-aligned */
  if (hash_appended) total += 32u;
  g_len = total;
  return total;
}

/* Reads straight out of the synthetic image, the way a flash partition reader
 * does -- no file length to bound it. */
static int img_read(void *ctx, uint32_t offset, void *dst, size_t n) {
  (void)ctx;
  if (offset + n > sizeof g_img) return -1;
  memcpy(dst, g_img + offset, n);
  return 0;
}

/* ---- tests -------------------------------------------------------------- */

void test_valid_s3_image_parses(void) {
  AppImageInfo info;
  uint32_t total = build(3, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info), APPIMAGE_OK);
  CHECK_EQ(info.image_size, total);
  CHECK_EQ(info.segment_count, 3);
  CHECK_EQ(info.chip_id, APPIMAGE_CHIP_ESP32S3);
  CHECK_EQ(info.entry_addr, 0x40378000u);
  CHECK_EQ(info.hash_appended, 1);
}

void test_app_descriptor_fields_are_extracted(void) {
  AppImageInfo info;
  build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info), APPIMAGE_OK);
  CHECK_EQ(info.has_app_desc, 1);
  CHECK_EQ(strcmp(info.project_name, "bruce"), 0);
  CHECK_EQ(strcmp(info.version, "1.2.3"), 0);
  CHECK_EQ(strcmp(info.date, "Sep 10 2026"), 0);
  CHECK_EQ(strcmp(info.idf_ver, "v5.2.1"), 0);
}

void test_image_without_a_descriptor_is_still_valid(void) {
  AppImageInfo info;
  build(2, APPIMAGE_CHIP_ESP32S3, 1, 0);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info), APPIMAGE_OK);
  CHECK_EQ(info.has_app_desc, 0);
  CHECK_EQ(info.project_name[0], 0);   /* reported as unknown, not as garbage */
}

void test_a_full_length_name_is_still_terminated(void) {
  AppImageInfo info;
  build(1, APPIMAGE_CHIP_ESP32S3, 0, 1);
  /* 32 characters with no NUL: the on-the-wire worst case. */
  memcpy(g_img + 32 + 48, "0123456789012345678901234567890123", 32);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info), APPIMAGE_OK);
  CHECK_EQ(strlen(info.project_name), 32);
  CHECK_EQ(info.project_name[32], 0);
}

void test_not_an_image_is_rejected(void) {
  AppImageInfo info;
  build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  g_img[0] = 0x7F;                    /* an ELF, say */
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_MAGIC);
}

void test_image_for_the_wrong_chip_is_rejected(void) {
  AppImageInfo info;
  build(2, 0x0000, 1, 1);             /* 0x0000 is the original ESP32 */
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_CHIP);
  build(2, 0x0005, 1, 1);             /* ESP32-C3 */
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_CHIP);
}

void test_a_file_too_short_for_a_header_is_rejected(void) {
  AppImageInfo info;
  build(1, APPIMAGE_CHIP_ESP32S3, 0, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, 12, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TOO_SMALL);
  CHECK_EQ(appimage_parse_buffer(g_img, 0, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TOO_SMALL);
}

/* The most likely real-world failure: the copy to the SD card was cut short.
 * The header still looks perfect, so only walking the segment table finds it. */
void test_a_truncated_image_is_rejected(void) {
  AppImageInfo info;
  uint32_t total = build(3, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, total - 1, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TRUNCATED);
  CHECK_EQ(appimage_parse_buffer(g_img, total / 2, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TRUNCATED);
  CHECK_EQ(appimage_parse_buffer(g_img, total, 4u * 1024 * 1024, &info),
           APPIMAGE_OK);              /* exactly the right length is fine */
}

void test_absurd_segment_counts_are_rejected(void) {
  AppImageInfo info;
  build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  g_img[1] = 0;
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_SEGMENTS);
  g_img[1] = 17;
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_SEGMENTS);
}

void test_a_wild_segment_length_cannot_overflow_the_walk(void) {
  AppImageInfo info;
  build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  put_u32(g_img + 24 + 4, 0xFFFFFF00u);   /* segment claims ~4 GB */
  CHECK(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &info) != APPIMAGE_OK);
}

void test_an_image_too_large_for_the_partition_is_rejected(void) {
  AppImageInfo info;
  uint32_t total = build(4, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, total - 1, &info),
           APPIMAGE_ERR_TOO_BIG);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, total, &info), APPIMAGE_OK);
}

void test_hash_appended_accounts_for_the_extra_32_bytes(void) {
  AppImageInfo with, without;
  uint32_t a = build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &with), APPIMAGE_OK);
  {
    uint32_t b = build(2, APPIMAGE_CHIP_ESP32S3, 0, 1);
    CHECK_EQ(appimage_parse_buffer(g_img, g_len, 4u * 1024 * 1024, &without),
             APPIMAGE_OK);
    CHECK_EQ(a - b, 32);
  }
  CHECK_EQ(with.image_size - without.image_size, 32);
  CHECK_EQ(without.hash_appended, 0);
}

void test_every_result_has_a_message(void) {
  int r;
  for (r = APPIMAGE_OK; r <= APPIMAGE_ERR_READ; r++) {
    const char *m = appimage_strerror((AppImageResult)r);
    CHECK(m != NULL);
    CHECK(m[0] != 0);
  }
}

/* CardLaunch's notes warn that people feed launchers full-flash dumps instead
 * of app images. Its own guard is a first-byte 0xE9 check, which does not
 * actually catch that: a full-flash dump begins with the *bootloader* image,
 * and that starts 0xE9 too. What distinguishes them is that the contained
 * image is a tiny fraction of the file. */
void test_a_full_flash_dump_is_rejected(void) {
  AppImageInfo info;
  uint32_t total = build(3, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(g_img[0], APPIMAGE_MAGIC);          /* a dump looks fine here ... */
  /* ... but the file is vastly larger than the image inside it. */
  CHECK_EQ(appimage_parse_buffer(g_img, 64000, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TRAILING);
  CHECK_EQ(appimage_parse_buffer(g_img, total, 4u * 1024 * 1024, &info),
           APPIMAGE_OK);
}

/* A signed image carries a 4 KB signature block after the image proper, so a
 * little trailing data has to stay acceptable. */
void test_a_small_trailer_is_tolerated(void) {
  AppImageInfo info;
  uint32_t total = build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, total + 4096, 4u * 1024 * 1024, &info),
           APPIMAGE_OK);
}

/* Reading an image out of a flash partition: the rest of the partition sits
 * after it, which is not a dump. */
void test_trailing_data_is_allowed_when_reading_a_partition(void) {
  AppImageInfo info;
  uint32_t total = build(2, APPIMAGE_CHIP_ESP32S3, 1, 1);
  CHECK_EQ(appimage_parse_buffer(g_img, 64000, 4u * 1024 * 1024, &info),
           APPIMAGE_ERR_TRAILING);
  CHECK_EQ(appimage_parse_ex(img_read, NULL, 64000, 4u * 1024 * 1024, 1,
                             &info), APPIMAGE_OK);
  CHECK_EQ(info.image_size, total);
}
