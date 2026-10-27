/* The IR LED. See irtx.h. */
#include "kernel/drv/irtx.h"
#include "kernel/sys/irproto.h"

#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"

#define IR_PIN      44
#define RES_HZ      1000000       /* a tick is a microsecond */
#define MAX_TICKS   32767         /* the most one half of an RMT symbol holds */
#define MAX_DUR     1024          /* durations in one send */
#define CODE_MAX    (68 + 4 * 50) /* a NEC frame and the most repeats */

static const char *TAG = "irtx";

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_copy;
static uint32_t s_carrier = 1;    /* none applied yet */

static int start(void) {
  rmt_tx_channel_config_t cfg = {
    .gpio_num = IR_PIN,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = RES_HZ,
    .mem_block_symbols = 48,      /* one block on the S3 */
    .trans_queue_depth = 2,
  };
  rmt_copy_encoder_config_t copy = {};     /* it has no fields */
  if (s_chan) return 0;
  if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK) {
    s_chan = NULL;
    return -1;
  }
  if (rmt_new_copy_encoder(&copy, &s_copy) != ESP_OK || rmt_enable(s_chan) != ESP_OK) {
    if (s_copy) rmt_del_encoder(s_copy);
    rmt_del_channel(s_chan);
    s_chan = NULL;
    s_copy = NULL;
    return -1;
  }
  ESP_LOGI(TAG, "IR out on G%d", IR_PIN);
  return 0;
}

static int set_carrier(uint32_t hz) {
  rmt_carrier_config_t c = {0};
  int rc;
  if (hz == s_carrier) return 0;
  c.frequency_hz = hz;
  c.duty_cycle = 0.33f;
  rmt_disable(s_chan);
  rc = rmt_apply_carrier(s_chan, hz ? &c : NULL) == ESP_OK ? 0 : -1;
  if (rmt_enable(s_chan) != ESP_OK) rc = -1;
  if (!rc) s_carrier = hz;
  return rc;
}

int irtx_send(const uint32_t *us, int n, uint32_t carrier_hz) {
  rmt_symbol_word_t *sym;
  rmt_transmit_config_t tx = {0};
  int i, halves = 0, ns, rc;
  uint32_t total = 0;
  if (!us || n <= 0 || n > MAX_DUR || carrier_hz > 100000) return -2;
  for (i = 0; i < n; i++) {
    halves += (int)((us[i] + MAX_TICKS - 1) / MAX_TICKS);
    total += us[i];
  }
  if (start() != 0 || set_carrier(carrier_hz) != 0) return -1;
  /* A duration longer than one half holds is split into several halves of
   * the same level; two halves to a symbol, the last padded with dark. */
  ns = (halves + 1) / 2;
  sym = calloc((size_t)ns, sizeof *sym);
  if (!sym) return -4;
  {
    int h = 0;
    for (i = 0; i < n; i++) {
      uint32_t left = us[i];
      unsigned level = (i & 1) ? 0 : 1;
      while (left) {
        uint32_t d = left > MAX_TICKS ? MAX_TICKS : left;
        rmt_symbol_word_t *s = &sym[h / 2];
        if (h & 1) { s->duration1 = d; s->level1 = level; }
        else       { s->duration0 = d; s->level0 = level; }
        left -= d;
        h++;
      }
    }
    if (h & 1) { sym[h / 2].duration1 = 1; sym[h / 2].level1 = 0; }
  }
  rc = rmt_transmit(s_chan, s_copy, sym, (size_t)ns * sizeof *sym, &tx) == ESP_OK ? 0 : -1;
  if (!rc && rmt_tx_wait_all_done(s_chan, (int)(total / 1000) + 200) != ESP_OK) rc = -1;
  free(sym);
  return rc;
}

int irtx_code(int proto, uint32_t addr, uint32_t cmd, int repeats) {
  uint32_t *d = malloc(CODE_MAX * sizeof *d);
  int n, rc;
  if (!d) return -4;
  n = ir_encode(proto, addr, cmd, repeats, d, CODE_MAX);
  rc = n > 0 ? irtx_send(d, n, ir_carrier(proto)) : -2;
  free(d);
  return rc;
}
