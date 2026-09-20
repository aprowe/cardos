/* PDM microphone capture. See mic.h. */

#include "kernel/drv/mic.h"
#include "kernel/fs/fs.h"

#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PIN_MIC_CLK 43
#define PIN_MIC_DAT 46

/* 512 samples: 32 ms at 16 kHz. Small enough that releasing the button feels
 * instant, large enough that the file is not written a handful of bytes at a
 * time onto a card that would rather have blocks. */
#define BLOCK_SAMPLES 512

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx;

int mic_open(void) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_pdm_rx_config_t pdm = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                               I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = PIN_MIC_CLK,
      .din = PIN_MIC_DAT,
      .invert_flags = { .clk_inv = false },
    },
  };

  if (s_rx) return 0;

  if (i2s_new_channel(&chan, NULL, &s_rx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed");
    s_rx = NULL;
    return -1;
  }
  if (i2s_channel_init_pdm_rx_mode(s_rx, &pdm) != ESP_OK) {
    ESP_LOGE(TAG, "pdm rx init failed");
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return -1;
  }
  if (i2s_channel_enable(s_rx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable failed");
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return -1;
  }
  return 0;
}

void mic_close(void) {
  if (!s_rx) return;
  i2s_channel_disable(s_rx);
  i2s_del_channel(s_rx);
  s_rx = NULL;
}

/* A 44-byte canonical WAV header. Written twice: once with zero lengths so the
 * audio can stream out behind it, and again at the end when the lengths are
 * known. Whisper reads the header, so the second write is not optional. */
static void wav_header(uint8_t *h, uint32_t data_bytes) {
  uint32_t rate = MIC_RATE;
  uint32_t byte_rate = rate * 2;         /* mono, 16-bit */

  memcpy(h + 0, "RIFF", 4);
  h[4] = (uint8_t)((data_bytes + 36) & 0xFF);
  h[5] = (uint8_t)(((data_bytes + 36) >> 8) & 0xFF);
  h[6] = (uint8_t)(((data_bytes + 36) >> 16) & 0xFF);
  h[7] = (uint8_t)(((data_bytes + 36) >> 24) & 0xFF);
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;      /* fmt chunk size */
  h[20] = 1;  h[21] = 0;                            /* PCM */
  h[22] = 1;  h[23] = 0;                            /* mono */
  h[24] = (uint8_t)(rate & 0xFF);
  h[25] = (uint8_t)((rate >> 8) & 0xFF);
  h[26] = (uint8_t)((rate >> 16) & 0xFF);
  h[27] = (uint8_t)((rate >> 24) & 0xFF);
  h[28] = (uint8_t)(byte_rate & 0xFF);
  h[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
  h[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
  h[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
  h[32] = 2;  h[33] = 0;                            /* block align */
  h[34] = 16; h[35] = 0;                            /* bits per sample */
  memcpy(h + 36, "data", 4);
  h[40] = (uint8_t)(data_bytes & 0xFF);
  h[41] = (uint8_t)((data_bytes >> 8) & 0xFF);
  h[42] = (uint8_t)((data_bytes >> 16) & 0xFF);
  h[43] = (uint8_t)((data_bytes >> 24) & 0xFF);
}

static int loudness(const int16_t *s, int n) {
  /* Peak, not RMS: this drives a meter that has to move while someone speaks,
   * and RMS over 32 ms of speech barely twitches. */
  int i, peak = 0;
  for (i = 0; i < n; i++) {
    int v = s[i] < 0 ? -s[i] : s[i];
    if (v > peak) peak = v;
  }
  return peak * 100 / 32768;
}

int mic_record_wav(const char *path, int max_ms,
                   int (*stop)(void), void (*level)(int pct)) {
  static int16_t block[BLOCK_SAMPLES];
  uint8_t header[44];
  uint32_t total = 0;
  int64_t started;
  int fd;

  if (max_ms <= 0) max_ms = MIC_MAX_MS;
  if (max_ms > MIC_HARD_MAX_MS) max_ms = MIC_HARD_MAX_MS;
  if (mic_open() != 0) return -1;

  fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) { mic_close(); return -1; }

  wav_header(header, 0);
  if (fs_write(fd, header, sizeof header) != (int)sizeof header) {
    fs_close(fd);
    mic_close();
    return -1;
  }

  started = esp_timer_get_time();
  for (;;) {
    size_t got = 0;
    int n;

    if (i2s_channel_read(s_rx, block, sizeof block, &got, 200) != ESP_OK) break;
    n = (int)(got / sizeof block[0]);
    if (n <= 0) continue;

    if (fs_write(fd, block, got) != (int)got) break;
    total += (uint32_t)got;

    if (level) level(loudness(block, n));
    if (stop && stop()) break;
    if ((esp_timer_get_time() - started) / 1000 >= max_ms) break;
  }

  /* Back to the top to write the lengths in. */
  wav_header(header, total);
  if (fs_seek(fd, 0, FS_SEEK_SET) >= 0)
    fs_write(fd, header, sizeof header);
  fs_close(fd);
  mic_close();

  ESP_LOGI(TAG, "recorded %u bytes (%u ms)", (unsigned)total,
           (unsigned)(total / (MIC_RATE * 2 / 1000)));
  return (int)total;
}
