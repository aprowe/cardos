/* Infrared timings. See irproto.h. */
#include "kernel/sys/irproto.h"

#define HDR_MARK   9000
#define HDR_SPACE  4500
#define RPT_SPACE  2250
#define BIT_MARK   562
#define ZERO_SPACE 562
#define ONE_SPACE  1687
#define FRAME_LEN  68             /* header 2, 32 bits 64, closing mark and gap 2 */
#define RPT_LEN    4

/* The last space stretched so the frame lasts IR_NEC_FRAME_US. */
static void pad(uint32_t *d, int n) {
  uint32_t s = 0;
  int i;
  for (i = 0; i < n - 1; i++) s += d[i];
  d[n - 1] = IR_NEC_FRAME_US - s;
}

int ir_encode(int proto, uint32_t addr, uint32_t cmd, int repeats,
              uint32_t *out, int max) {
  uint32_t v;
  int i, k, n;
  if (repeats < 0 || repeats > 50) return -1;
  if (proto == IR_NEC) {
    if (addr > 0xFFFF || cmd > 0xFF) return -1;
    if (addr <= 0xFF) addr |= (~addr & 0xFF) << 8;
    v = addr | cmd << 16 | (~cmd & 0xFF) << 24;
  } else if (proto == IR_NEC_MSB) {
    /* Sent most significant first: reverse it into the LSB-first order. */
    v = 0;
    for (i = 0; i < 32; i++)
      if (cmd & (1u << i)) v |= 1u << (31 - i);
  } else {
    return -1;
  }
  n = FRAME_LEN + RPT_LEN * repeats;
  if (max < n) return -2;
  k = 0;
  out[k++] = HDR_MARK;
  out[k++] = HDR_SPACE;
  for (i = 0; i < 32; i++) {
    out[k++] = BIT_MARK;
    out[k++] = (v >> i) & 1 ? ONE_SPACE : ZERO_SPACE;
  }
  out[k++] = BIT_MARK;
  out[k++] = 0;
  pad(out, k);
  for (i = 0; i < repeats; i++) {
    out[k++] = HDR_MARK;
    out[k++] = RPT_SPACE;
    out[k++] = BIT_MARK;
    out[k++] = 0;
    pad(out + k - RPT_LEN, RPT_LEN);
  }
  return k;
}

uint32_t ir_carrier(int proto) {
  return proto == IR_NEC || proto == IR_NEC_MSB ? IR_NEC_HZ : 0;
}
