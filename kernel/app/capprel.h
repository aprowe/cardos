/* Relocation arithmetic for .capp images, portable so the host suite can
 * reach it. See elfload.h for why only R_XTENSA_32 is ever applied.
 *
 * Windowed because the flash cache relocates code and constants through the 28 KB arena:
 * an app's code may not fit in one piece of RAM, which is the whole reason
 * for the cache. Each window is handed every relocation of its section;
 * those outside it are checked and skipped. */
#ifndef CARDOS_CAPPREL_H
#define CARDOS_CAPPREL_H

#include <stdint.h>

/* Where capp.ld links each part. Code at 0, data and constants far away, so
 * an absolute value says which part it points into without any symbol
 * lookup. */
#define CAPP_DATA_ORIGIN   0x10000000u
#define CAPP_RODATA_ORIGIN 0x20000000u

#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP   20

typedef struct {            /* Elf32_Rela */
  uint32_t r_offset, r_info;
  int32_t  r_addend;
} CappRela;

/* Where each part landed: code (the flash cache's instruction view, or
 * executable RAM), data (the arena or the heap), constants (the flash
 * cache's data view, or RAM). */
typedef struct { uint32_t code, data, rodata; } CappBases;

/* Apply `r` to the bytes [win_off, win_off + win_len) of a section that is
 * `limit` bytes long, linked at `origin` (0, CAPP_DATA_ORIGIN or
 * CAPP_RODATA_ORIGIN), held at `win`. A value is moved by the base of the
 * part it was linked in: below CAPP_DATA_ORIGIN code, below
 * CAPP_RODATA_ORIGIN data, above that the constants. win_off and win_len
 * are multiples of 4. Returns 0, or -1 for a type it does not know, an
 * offset that is out of range, below `origin` or unaligned -- checked for
 * every entry, in the window or not -- or no bases. */
int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                  uint32_t origin, const CappRela *r, uint32_t n, const CappBases *b);

#endif /* CARDOS_CAPPREL_H */
