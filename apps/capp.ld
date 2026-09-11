/* Linker script for CardOS app binaries.
 *
 * Two output sections, and the split is forced by the hardware rather than by
 * taste. On the ESP32-S3 the same physical SRAM is reachable through two
 * address windows: an instruction window that only permits aligned 32-bit
 * access, and a data window that permits byte access. Code has to be fetched
 * through the first and character data has to be read through the second, so
 * one image at one base address cannot serve both -- a single-section build
 * loads, relocates, and then faults on the first string it touches.
 *
 * So: .code is linked at 0 and lands in executable RAM; .data is linked at
 * 0x10000000 and lands in ordinary heap. The gap between the two link
 * addresses is what lets the loader tell, from an absolute value alone, which
 * of the two bases to add -- no symbol table lookup required.
 *
 * Literals stay with the code on purpose. l32r reads them as aligned words
 * through the instruction bus, which is exactly what that window allows.
 *
 * Everything is fully linked, so every internal reference is already resolved
 * and the PC-relative ones stay correct wherever the code is placed. Linked
 * with -q so the relocations survive into the output; only the absolute
 * R_XTENSA_32 entries are applied at load time.
 */
ENTRY(capp_register)

SECTIONS {
  . = 0;
  .code : {
    *(.literal .literal.*)
    *(.text .text.*)
    . = ALIGN(4);
  }

  . = 0x10000000;
  .data : {
    *(.rodata .rodata.*)
    *(.data .data.*)
    . = ALIGN(4);
    *(.bss .bss.*)
    *(COMMON)
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
