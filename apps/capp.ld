/* Linker script for CardOS app binaries.
 *
 * Code and data are separate sections, and the split is forced by the
 * hardware rather than by taste. On the ESP32-S3 the same physical SRAM is
 * reachable through two address windows: an instruction window that only
 * permits aligned 32-bit access, and a data window that permits byte access.
 * Code has to be fetched through the first and character data has to be read
 * through the second, so one image at one base address cannot serve both --
 * a single-section build loads, relocates, and then faults on the first
 * string it touches.
 *
 * So: .code is linked at 0 and runs from the flash cache (or executable RAM);
 * .data at 0x10000000 lands in the arena (or the heap); .rodata at
 * 0x20000000 is the constants, which go in the flash cache beside the code,
 * read through the data bus, or into RAM after the data on every other
 * path. The gaps between the link addresses are what let the loader tell,
 * from an absolute value alone, which of the three bases to add -- no
 * symbol table lookup required (kernel/app/capprel.c).
 *
 * capp_info stays first in .data: the loader reads its version and flags
 * from the file before anything is placed, and tools/build_apps.py reads it
 * at offset 0. What it points to -- the help, the command tables, their
 * strings -- is ordinary constants, which the icon scan loads into RAM.
 *
 * Literals stay with the code on purpose. l32r reads them as aligned words
 * through the instruction bus, which is exactly what that window allows.
 *
 * Everything is fully linked, so every internal reference is already resolved
 * and the PC-relative ones stay correct wherever the code is placed. Linked
 * with -q so the relocations survive into the output; only the absolute
 * R_XTENSA_32 entries are applied at load time.
 */
ENTRY(capp_main)

SECTIONS {
  . = 0;
  .code : {
    *(.literal .literal.*)
    *(.text .text.*)
    . = ALIGN(4);
  }

  . = 0x10000000;
  .data : {
    /* The descriptor is read by the loader, never referenced by the code, so
     * --gc-sections discards it unless told otherwise. First in the section
     * because the loader and tools/build_apps.py read it there. */
    KEEP(*(.rodata.capp_info))
    *(.data .data.*)
    . = ALIGN(4);
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
  }

  /* The overlay region (docs/superpowers/specs/2026-10-10-app-data-overlays-design.md):
   * one output section per overlay, all at the same address after .data, so
   * the region is as big as the largest. NOBITS, so the file carries nothing
   * for them, and the loader's zeroed block is their start. They reach here
   * because nothing above names .sbss: a .bss. name would have gone into
   * .data with the rest. The linker drops an empty one, so an app without
   * overlays links exactly as before. */
  . = ALIGN(16);
  OVERLAY : {
    .ovl0 { *(.sbss.capp_ovl0) }
    .ovl1 { *(.sbss.capp_ovl1) }
    .ovl2 { *(.sbss.capp_ovl2) }
    .ovl3 { *(.sbss.capp_ovl3) }
  }

  /* The constants: never written, so they can run from flash
   * (docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md).
   * ALIGN(4) at the end because relocation windows are whole words. */
  . = 0x20000000;
  .rodata : {
    *(.rodata .rodata.*)
    . = ALIGN(4);
  }

  /* Toolchain metadata that means nothing once loaded. .xt.prop in particular
   * carries its own relocations against .text, which would otherwise have to
   * be understood only to be ignored. */
  /DISCARD/ : {
    *(.comment)
    *(.xt.*)
    *(.xtensa.info)
    *(.debug*)
    *(.note*)
  }
}
