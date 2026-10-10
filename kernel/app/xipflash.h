/* The appcode partition, mapped once at boot into the instruction bus, as
 * the flash under xipcache.c. Device-only. A device without the partition
 * (one updated over the air from the old table) has no cache, and every
 * app's code loads into RAM as before. */
#ifndef CARDOS_XIPFLASH_H
#define CARDOS_XIPFLASH_H

#include <stdint.h>
#include "kernel/app/xipcache.h"

int       xipflash_init(void);
int       xipflash_ready(void);
XipCache *xipflash_cache(void);       /* NULL when not ready */
uint32_t  xipflash_base(void);        /* instruction address of offset 0 */
void      xipflash_forget(const char *path);   /* a .capp changed on the card */
void      xipflash_usage(uint32_t *live_sectors, uint32_t *total_sectors);

#endif /* CARDOS_XIPFLASH_H */
