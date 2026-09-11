/* Structural validation of an ESP32-S3 application image. See appimage.h. */

#include "kernel/app/appimage.h"

#include <string.h>

/* Offsets within esp_image_header_t. */
#define H_MAGIC          0u
#define H_SEGMENT_COUNT  1u
#define H_ENTRY_ADDR     4u
#define H_CHIP_ID       12u
#define H_HASH_APPENDED 23u

/* Offsets within esp_app_desc_t, which sits at the start of segment 0. */
#define D_MAGIC          0u
#define D_VERSION       16u
#define D_PROJECT_NAME  48u
#define D_DATE          96u
#define D_IDF_VER      112u
#define D_SIZE         256u

/* A single segment larger than this is a corrupt header, not a real image:
 * the whole chip has 8 MB of flash. Bounding it before the walk is what stops
 * a wild length from overflowing the running total. */
#define MAX_SEGMENT_LEN (16u * 1024u * 1024u)

static uint16_t rd_u16(const unsigned char *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The fields are fixed-width and not NUL-terminated on the wire, so a name
 * that exactly fills its field would run into the next one. */
static void copy_field(char *dst, size_t dst_size,
                       const unsigned char *src, size_t field_len) {
  size_t n = field_len < dst_size - 1 ? field_len : dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

AppImageResult appimage_parse(AppImageRead read, void *ctx,
                              uint32_t file_size, uint32_t max_size,
                              AppImageInfo *out) {
  return appimage_parse_ex(read, ctx, file_size, max_size, 0, out);
}

AppImageResult appimage_parse_ex(AppImageRead read, void *ctx,
                                 uint32_t file_size, uint32_t max_size,
                                 int allow_trailing, AppImageInfo *out) {
  unsigned char hdr[APPIMAGE_HEADER_SIZE];
  unsigned char desc[D_SIZE];
  uint32_t pos, total;
  unsigned i;

  memset(out, 0, sizeof *out);

  if (file_size < APPIMAGE_HEADER_SIZE) return APPIMAGE_ERR_TOO_SMALL;
  if (read(ctx, 0, hdr, sizeof hdr) != 0) return APPIMAGE_ERR_READ;

  if (hdr[H_MAGIC] != APPIMAGE_MAGIC) return APPIMAGE_ERR_MAGIC;

  out->chip_id       = rd_u16(hdr + H_CHIP_ID);
  out->entry_addr    = rd_u32(hdr + H_ENTRY_ADDR);
  out->segment_count = hdr[H_SEGMENT_COUNT];
  out->hash_appended = hdr[H_HASH_APPENDED] ? 1u : 0u;

  /* Wrong chip would hard-fault at the first instruction, so refuse it here
   * rather than discover it as a boot loop. */
  if (out->chip_id != APPIMAGE_CHIP_ESP32S3) return APPIMAGE_ERR_CHIP;

  if (out->segment_count == 0 || out->segment_count > APPIMAGE_MAX_SEGMENTS)
    return APPIMAGE_ERR_SEGMENTS;

  /* Walk the segment table. Each segment header is followed by its data, so
   * the only way to learn the image length is to step through all of them --
   * which is also the only way to notice a truncated file, because the header
   * of a truncated image still looks perfect. */
  pos = APPIMAGE_HEADER_SIZE;
  for (i = 0; i < out->segment_count; i++) {
    unsigned char seg[APPIMAGE_SEG_HEADER_SIZE];
    uint32_t data_len;

    if (pos + APPIMAGE_SEG_HEADER_SIZE > file_size) return APPIMAGE_ERR_TRUNCATED;
    if (read(ctx, pos, seg, sizeof seg) != 0) return APPIMAGE_ERR_READ;

    data_len = rd_u32(seg + 4);
    if (data_len > MAX_SEGMENT_LEN) return APPIMAGE_ERR_SEGMENTS;

    pos += APPIMAGE_SEG_HEADER_SIZE;

    if (i == 0 && data_len >= D_SIZE && pos + D_SIZE <= file_size) {
      if (read(ctx, pos, desc, sizeof desc) != 0) return APPIMAGE_ERR_READ;
      if (rd_u32(desc + D_MAGIC) == APPIMAGE_APP_DESC_MAGIC) {
        out->has_app_desc = 1;
        copy_field(out->version,      sizeof out->version,      desc + D_VERSION,      32);
        copy_field(out->project_name, sizeof out->project_name, desc + D_PROJECT_NAME, 32);
        copy_field(out->date,         sizeof out->date,         desc + D_DATE,         16);
        copy_field(out->idf_ver,      sizeof out->idf_ver,      desc + D_IDF_VER,      32);
      }
    }

    pos += data_len;
    if (pos > file_size) return APPIMAGE_ERR_TRUNCATED;
  }

  /* One checksum byte, positioned so the image length is a multiple of 16,
   * then the optional SHA-256. */
  total = (pos + 1u + 15u) & ~15u;
  if (out->hash_appended) total += 32u;

  if (total > file_size) return APPIMAGE_ERR_TRUNCATED;
  if (!allow_trailing && file_size - total > APPIMAGE_MAX_TRAILER)
    return APPIMAGE_ERR_TRAILING;
  if (total > max_size)  return APPIMAGE_ERR_TOO_BIG;

  out->image_size = total;
  return APPIMAGE_OK;
}

/* ------------------------------------------------------ buffer wrapper --- */

typedef struct { const unsigned char *buf; uint32_t len; } BufCtx;

static int buf_read(void *ctx, uint32_t offset, void *dst, size_t n) {
  BufCtx *b = (BufCtx *)ctx;
  if (offset > b->len || (uint32_t)(offset + n) > b->len) return -1;
  memcpy(dst, b->buf + offset, n);
  return 0;
}

AppImageResult appimage_parse_buffer(const void *buf, uint32_t len,
                                     uint32_t max_size, AppImageInfo *out) {
  BufCtx ctx;
  ctx.buf = (const unsigned char *)buf;
  ctx.len = len;
  return appimage_parse(buf_read, &ctx, len, max_size, out);
}

const char *appimage_strerror(AppImageResult r) {
  switch (r) {
  case APPIMAGE_OK:            return "ok";
  case APPIMAGE_ERR_TOO_SMALL: return "file is too short to be a firmware image";
  case APPIMAGE_ERR_MAGIC:     return "not an ESP32 firmware image";
  case APPIMAGE_ERR_CHIP:      return "built for a different chip, not the ESP32-S3";
  case APPIMAGE_ERR_SEGMENTS:  return "corrupt image header";
  case APPIMAGE_ERR_TRUNCATED: return "image is truncated -- the copy is incomplete";
  case APPIMAGE_ERR_TOO_BIG:   return "image is too large for the guest partition";
  case APPIMAGE_ERR_TRAILING:  return "not an app image (a full-flash dump?)";
  case APPIMAGE_ERR_READ:      return "could not read the image";
  }
  return "unknown error";
}
