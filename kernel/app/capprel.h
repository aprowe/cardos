/* Relocation arithmetic for .capp images, portable so the host suite can
 * reach it. See elfload.h for why only R_XTENSA_32 is ever applied.
 *
 * Windowed because the flash cache relocates code through the 28 KB arena:
 * an app's code may not fit in one piece of RAM, which is the whole reason
 * for the cache. Each window is handed every relocation of its section;
 * those outside it are checked and skipped. */
#ifndef CARDOS_CAPPREL_H
#define CARDOS_CAPPREL_H

#include <stdint.h>

/* Where capp.ld links each half. Code at 0, data far away, so an absolute
 * value says which half it points into without any symbol lookup. */
#define CAPP_DATA_ORIGIN 0x10000000u

#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP   20

typedef struct {            /* Elf32_Rela */
  uint32_t r_offset, r_info;
  int32_t  r_addend;
} CappRela;

/* Apply `r` to the bytes [win_off, win_off + win_len) of a section that is
 * `limit` bytes long, held at `win`. into_code says which section: code
 * offsets are link addresses from 0, data offsets from CAPP_DATA_ORIGIN.
 * win_off and win_len are multiples of 4. Returns 0, or -1 for a type it
 * does not know or an offset that is out of range or unaligned -- checked
 * for every entry, in the window or not. */
int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                  int into_code, const CappRela *r, uint32_t n,
                  uint32_t code_base, uint32_t data_base);

#endif /* CARDOS_CAPPREL_H */
