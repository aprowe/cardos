/* The ADV's ES8311 codec. See es8311.h. */
#include "kernel/drv/es8311.h"
#include "kernel/drv/board.h"

#define ES8311 0x18

typedef struct { unsigned char reg, val; } Reg;

static int send(const Reg *seq, int n) {
  int i;
  if (board() != BOARD_ADV) return -1;
  for (i = 0; i < n; i++)
    if (board_i2c_write_reg(ES8311, seq[i].reg, seq[i].val) != 0) return -1;
  return 0;
}

int es8311_speaker_on(void) {
  static const Reg SEQ[] = {
    { 0x00, 0x80 },   /* reset, then power on */
    { 0x01, 0xB5 },   /* clock manager: MCLK from BCLK, DAC clocks on */
    { 0x02, 0x18 },   /* MCLK multiplier 8 */
    { 0x0D, 0x01 },   /* analog power up */
    { 0x12, 0x00 },   /* DAC up */
    { 0x13, 0x10 },   /* out to the headphone driver (the amp takes it from there) */
    { 0x32, 0xBF },   /* DAC volume 0 dB: ours is applied to the samples */
    { 0x37, 0x08 },   /* DAC equaliser bypassed */
  };
  return send(SEQ, (int)(sizeof SEQ / sizeof SEQ[0]));
}

int es8311_mic_on(void) {
  static const Reg SEQ[] = {
    { 0x00, 0x80 },
    { 0x01, 0xBA },   /* MCLK from BCLK, ADC clocks on */
    { 0x02, 0x18 },
    { 0x0D, 0x01 },
    { 0x0E, 0x02 },   /* analog PGA and ADC modulator on */
    { 0x14, 0x10 },   /* Mic1p-Mic1n, PGA gain at its least */
    { 0x17, 0xBF },   /* ADC volume 0 dB */
    { 0x1C, 0x6A },   /* ADC equaliser bypassed, DC offset cancelled */
  };
  return send(SEQ, (int)(sizeof SEQ / sizeof SEQ[0]));
}

void es8311_mic_off(void) {
  static const Reg SEQ[] = { { 0x0D, 0xFC }, { 0x0E, 0x6A }, { 0x00, 0x00 } };
  send(SEQ, (int)(sizeof SEQ / sizeof SEQ[0]));
}
