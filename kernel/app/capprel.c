/* Relocation arithmetic. See capprel.h. */

#include "kernel/app/capprel.h"

#include <string.h>

int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                  int into_code, const CappRela *r, uint32_t n,
                  uint32_t code_base, uint32_t data_base) {
  uint32_t j;
  if ((win_off & 3u) || (win_len & 3u)) return -1;
  for (j = 0; j < n; j++) {
    uint32_t type = r[j].r_info & 0xFFu, off, v;
    if (type == R_XTENSA_NONE || type == R_XTENSA_SLOT0_OP || type == R_XTENSA_ASM_EXPAND)
      continue;               /* PC-relative, or nothing at all */
    if (type != R_XTENSA_32) return -1;
    /* r_offset is a link-time address, so it carries its section's origin.
     * Written so it cannot wrap: an r_offset just below the origin gave an
     * `off` near 2^32, and `off + 4` came round to something small that
     * passed. */
    off = into_code ? r[j].r_offset : r[j].r_offset - CAPP_DATA_ORIGIN;
    if (off >= limit || limit - off < 4 || (off & 3u)) return -1;
    if (off < win_off || off - win_off >= win_len) continue;   /* another window's */
    /* memcpy, not a uint32_t store: the compiler may not narrow it into
     * accesses the instruction window refuses, and the host does not care. */
    memcpy(&v, win + (off - win_off), 4);
    v = (v >= CAPP_DATA_ORIGIN) ? (v - CAPP_DATA_ORIGIN) + data_base : v + code_base;
    memcpy(win + (off - win_off), &v, 4);
  }
  return 0;
}
