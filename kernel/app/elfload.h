/* Loader for CardOS app binaries (.capp). Device-only.
 *
 * A .capp is an ELF executable, fully linked at address 0 with its relocations
 * retained (ld -q). That choice is what makes this loader small: the linker has
 * already resolved every internal reference, and PC-relative ones do not change
 * when the whole image moves, so the only fixups left are absolute
 * R_XTENSA_32 relocations -- an add of the load address. Everything else in the
 * relocation table is SLOT0_OP (l32r and call operands), which is invariant and
 * skipped.
 *
 * The alternative, loading relocatable .o files, means implementing Xtensa's
 * instruction-slot relocations properly. This does not.
 *
 * Apps link against nothing: they receive a CardApi table and export exactly
 * one symbol, so there are no undefined symbols to resolve.
 *
 * Code and data are loaded into separate allocations because the chip insists.
 * The same SRAM is reachable through an instruction window that only permits
 * aligned 32-bit access and a data window that permits byte access; code must
 * be fetched through the first and strings read through the second. capp.ld
 * links the two halves at far-apart addresses so the loader can tell, from an
 * absolute value alone, which base belongs to it.
 */
#ifndef CARDOS_ELFLOAD_H
#define CARDOS_ELFLOAD_H

#include "kernel/app/capp.h"

typedef struct {
  void           *code;      /* executable RAM, instruction-window address */
  void           *data;      /* ordinary heap: rodata, data and bss */
  uint32_t        code_size;
  uint32_t        data_size;

  /* Read from the image without running it -- name, icon and flags are needed
   * to draw an icon, and executing a program to find out what it is called is
   * the wrong way round. */
  const CappInfo *info;

  /* The program. Called when it is actually run. */
  int (*main)(const CardApi *api, int argc, char **argv);
} LoadedApp;

typedef enum {
  CAPP_OK = 0,
  CAPP_ERR_OPEN,
  CAPP_ERR_TOO_BIG,
  CAPP_ERR_NOT_ELF,
  CAPP_ERR_TRUNCATED,       /* the file is shorter than it says it is */
  CAPP_ERR_WRONG_MACHINE,
  CAPP_ERR_NO_IMAGE,        /* no allocatable section */
  CAPP_ERR_NO_ENTRY,        /* capp_main or capp_info missing */
  CAPP_ERR_NO_MEMORY,       /* no executable RAM left */
  CAPP_ERR_RELOC,           /* a relocation type we cannot apply */
  CAPP_ERR_API              /* built against a different API version */
} CappResult;

CappResult capp_load(const char *path, LoadedApp *out);
void       capp_unload(LoadedApp *la);
const char *capp_strerror(CappResult r);

/* Free executable memory, which is what bounds app size. Worth reporting: it
 * is a different pool from the general heap and much smaller. */
uint32_t capp_exec_free(void);

#endif /* CARDOS_ELFLOAD_H */
