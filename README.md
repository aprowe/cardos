# CardOS

A tiny custom operating system for the M5Stack Cardputer.

Standalone by design: no PC, no network dependency, no Lua, no Arduino.
Apps are written in a custom language and compiled **on the device** to native
Xtensa machine code.

- Handle-based memory manager with **SD-backed swap**, because the ESP32-S3
  has no MMU and 320 KB of usable heap
- Cooperative scheduler with per-task stacks and a hand-written Xtensa
  context switch
- FAT32 filesystem, readable on any PC
- A shell today; a windowed desktop, an on-device compiler and a Bluetooth
  mouse next

Start with [`docs/specs/2026-09-10-kernel-core-design.md`](docs/specs/2026-09-10-kernel-core-design.md).

## Roadmap

| | | |
|---|---|---|
| 1 | Kernel core — memory, swap, tasks, FS, shell | **designed** |
| 2 | Window system + desktop shell | |
| 3 | CardLang — language, on-device compiler, native codegen | |
| 4 | Editor + toolchain | |
| 5 | Bluetooth HID mouse | |
