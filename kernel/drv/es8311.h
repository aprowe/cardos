/* The Cardputer ADV's ES8311 audio codec. Device-only.
 *
 * The original Cardputer has a PDM microphone and an I2S amplifier, each
 * ready the moment its pins clock. The ADV puts both behind this codec on
 * the I2C bus (0x18), which does nothing until it is told what to do. The
 * register sequences are M5Stack's (M5Unified, the Cardputer ADV speaker and
 * microphone callbacks); MCLK is taken from BCLK (no MCLK pin), times 8, so
 * the I2S port must run 16-bit words in two slots: BCLK = 32 fs, MCLK = 256 fs.
 *
 * One direction at a time, as on the original: each sequence resets the
 * codec, and the microphone's turns the DAC's clocks off. Nothing here on the
 * original -- these do nothing and return -1.
 */
#ifndef CARDOS_ES8311_H
#define CARDOS_ES8311_H

int  es8311_speaker_on(void);
int  es8311_mic_on(void);
void es8311_mic_off(void);

#endif /* CARDOS_ES8311_H */
