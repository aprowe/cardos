/* The speaker. See speaker.h. */
#include "kernel/drv/speaker.h"
#include "kernel/drv/mic.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "speaker";

#define PIN_BCLK  41
#define PIN_DATA  42
#define PIN_LRCLK 43

#define BLOCK_SAMPLES 512            /* 16 ms at 16 kHz; 1 KB on the stack */

static i2s_chan_handle_t s_tx;
static char s_error[48];

/* Remembered like the brightness: same NVS namespace, read once on the first
 * ask. 60 is the default because the NS4168 is loud. */
#define NVS_NS     "cardos"
#define NVS_VOLUME "volume"
static int  s_volume = 60;
static int  s_volume_loaded;

static void load_volume(void) {
  nvs_handle_t h;
  uint8_t v;
  if (s_volume_loaded) return;
  s_volume_loaded = 1;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  if (nvs_get_u8(h, NVS_VOLUME, &v) == ESP_OK && v <= 100) s_volume = v;
  nvs_close(h);
}

static void fail(const char *why) {
  snprintf(s_error, sizeof s_error, "%s", why);
  ESP_LOGW(TAG, "%s", why);
}

const char *speaker_error(void) { return s_error; }
void speaker_set_volume(int pct) {
  nvs_handle_t h;
  load_volume();
  s_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_VOLUME, (uint8_t)s_volume);
  nvs_commit(h);
  nvs_close(h);
}
int  speaker_volume(void) { load_volume(); return s_volume; }

/* ---- the file ------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Walk the chunks rather than assume the 44-byte layout: a WAV from a PC
 * often carries a LIST chunk before the data. */
int speaker_wav_info(const char *path, WavInfo *out, const char **why) {
  uint8_t h[12], c[8], fmt[16];
  int fd;
  uint32_t off = 12;
  int have_fmt = 0;
  const char *reason = "not a WAV file";

  memset(out, 0, sizeof *out);
  fd = fs_open(path, FS_O_READ);
  if (fd < 0) { if (why) *why = "cannot open"; return -1; }
  if (fs_read(fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto bad;

  for (;;) {
    uint32_t size;
    if (fs_seek(fd, (int32_t)off, FS_SEEK_SET) < 0) goto bad;
    if (fs_read(fd, c, 8) != 8) goto bad;
    size = le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      if (size < 16 || fs_read(fd, fmt, 16) != 16) goto bad;
      if (le16(fmt) != 1) { reason = "not PCM"; goto bad; }
      out->channels = le16(fmt + 2);
      out->rate = le32(fmt + 4);
      out->bits = le16(fmt + 14);
      have_fmt = 1;
    } else if (!memcmp(c, "data", 4)) {
      out->data_offset = off + 8;
      out->data_bytes = size;
      break;
    }
    off += 8 + size + (size & 1);
    if (off > 4096) goto bad;                /* headers do not run this long */
  }
  fs_close(fd);
  if (!have_fmt) { if (why) *why = "no format chunk"; return -1; }
  if (out->bits != 16) { if (why) *why = "not 16-bit"; return -1; }
  if (out->channels < 1 || out->channels > 2) { if (why) *why = "not mono or stereo"; return -1; }
  if (out->rate < 8000 || out->rate > 48000) { if (why) *why = "rate out of range"; return -1; }
  return 0;
bad:
  fs_close(fd);
  if (why) *why = reason;
  return -1;
}

/* ---- the channel --------------------------------------------------------- */

static int open_tx(uint32_t rate, int channels) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_std_config_t std = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                  channels == 2 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = PIN_BCLK,
      .ws   = PIN_LRCLK,
      .dout = PIN_DATA,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (s_tx) return 0;
  mic_close();                                   /* G43 is ours now */
  if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) { fail("no I2S channel"); return -1; }
  if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) {
    i2s_del_channel(s_tx); s_tx = NULL;
    fail("I2S init failed");
    return -1;
  }
  if (i2s_channel_enable(s_tx) != ESP_OK) {
    i2s_del_channel(s_tx); s_tx = NULL;
    fail("I2S enable failed");
    return -1;
  }
  return 0;
}

static void close_tx(void) {
  if (!s_tx) return;
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_tx);
  s_tx = NULL;
}

int speaker_play_wav(const char *path, int (*stop)(void),
                     void (*progress)(uint32_t bytes)) {
  static int16_t block[BLOCK_SAMPLES];
  WavInfo w;
  const char *why = "";
  int fd, n;
  uint32_t left, played = 0;

  s_error[0] = 0;
  if (speaker_wav_info(path, &w, &why) != 0) { fail(why); return -1; }
  fd = fs_open(path, FS_O_READ);
  if (fd < 0) { fail("cannot open"); return -1; }
  if (fs_seek(fd, (int32_t)w.data_offset, FS_SEEK_SET) < 0) { fs_close(fd); fail("cannot seek"); return -1; }
  if (open_tx(w.rate, w.channels) != 0) { fs_close(fd); return -1; }

  left = w.data_bytes;
  while (left > 0) {
    size_t want = left < sizeof block ? (size_t)left : sizeof block, wrote = 0;
    int i;
    int gain = speaker_volume() * 256 / 100;   /* 8.8; read per block so a
                                                  change lands mid-playback */
    n = fs_read(fd, block, want);
    if (n <= 0) break;
    for (i = 0; i < n / 2; i++) block[i] = (int16_t)((block[i] * gain) >> 8);
    /* Blocks until the DMA takes it, which is what paces the loop. */
    if (i2s_channel_write(s_tx, block, (size_t)n, &wrote, 500) != ESP_OK) { fail("I2S write failed"); break; }
    left -= (uint32_t)n;
    played += (uint32_t)n;
    if (progress) progress(played);
    if (stop && stop()) break;
  }
  /* Let the last block drain before the channel goes, or it is clipped. */
  {
    static const int16_t silence[BLOCK_SAMPLES];
    size_t wrote;
    i2s_channel_write(s_tx, silence, sizeof silence, &wrote, 200);
  }
  fs_close(fd);
  close_tx();
  return s_error[0] ? -1 : 0;
}
