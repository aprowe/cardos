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
/* The key's path hash: of the normalised path, so "/apps//jar.capp" and
 * "/apps/jar.capp" are one entry and forget finds what the loader wrote. */
uint32_t  xipflash_path_hash(const char *path);

/* Every operation on xipflash_cache() is made under this lock. A file
 * change is reported on whichever task wrote it (the share server's, say),
 * and a forget programming live=0 into a sector the shell has just erased
 * for new code leaves a zero word inside that code. Recursive, so a log
 * line written under it cannot deadlock. No-ops without the partition. */
void      xipflash_lock(void);
void      xipflash_unlock(void);

#endif /* CARDOS_XIPFLASH_H */
