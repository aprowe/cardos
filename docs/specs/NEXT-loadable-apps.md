# Loading a binary app into a window — options

Not a spec. This is the brainstorm you asked for, with a recommendation at the
end.

The question: load a binary file from the card and run it **inside** CardOS —
in a window or fullscreen — rather than chain-booting it and leaving.

## Why this is hard here, stated once

Three constraints shape every option:

1. **Code must execute from IRAM or flash-mapped instruction memory.** Ordinary
   heap (DRAM) is not executable. `heap_caps_malloc(n, MALLOC_CAP_EXEC)` gives
   IRAM, and internal SRAM has no instruction cache to invalidate, so freshly
   written code runs immediately. That part is genuinely easy on this chip.
2. **There is no MMU for internal RAM.** Whatever address the code is loaded at
   is the address it must be linked for — unless the code is position
   independent, or unless we relocate it at load time.
3. **IRAM is scarce.** Measure before committing: CardOS currently reports ~280
   KB of general heap, but the EXEC-capable portion is much smaller and is
   shared with the BLE stack when the radio is up.

Everything below is a different answer to constraint 2.

## Option A — fixed app slot, position-dependent code

Reserve a fixed region of IRAM inside CardOS (a static array in `.iram1`), and
build apps linked to that exact address. Loading is `fs_read` into the slot,
then call the entry point.

**Cheapest possible thing that works.** No relocation, no ELF parsing, no
symbol resolution — maybe 150 lines in CardOS plus a linker script for apps.

Against it: one app at a time per slot, and the slot's address changes whenever
CardOS is rebuilt and the linker moves things. That is survivable — emit the
address into a generated header as part of the build, and apps rebuild against
it — but it means an app binary is tied to a CardOS build, which is exactly the
brittleness we avoided with chain-booting.

## Option B — relocatable ELF object loader

Apps compile to an ordinary `.o` (relocatable object). CardOS parses ELF32,
allocates `.text` in EXEC RAM and `.data`/`.bss` in DRAM, applies the Xtensa
relocations, resolves undefined symbols against an export table, and calls
`app_main`.

This is what the existing ESP32 ELF-loader projects do, so the approach is
proven on this architecture. Roughly 600–900 lines. The fiddly part is Xtensa's
`l32r` literal relocations (`R_XTENSA_SLOT0_OP`) rather than the plain
`R_XTENSA_32` ones.

In its favour: apps are built independently of a particular CardOS binary,
several can be loaded at once, and it is the same machinery **sub-project 3
needs anyway** — an on-device compiler that emits native code has to solve
placement and linking regardless. Building it now means building it once.

## Option C — execute in place from flash

`esp_partition_mmap` with `SPI_FLASH_MMAP_INST` maps flash into the instruction
address space, and the flash cache demand-pages it for free. Zero RAM cost for
code, which on a board with 8 MB of flash against 512 KB of RAM is the single
biggest win available — it is why the kernel spec wanted `rom_map()` at all.

But the mapped address is chosen at runtime, so this still needs position
independent code or relocation. It is an optimisation to layer **on top of**
option B, not an alternative to it: relocate once, then map rather than copy.

## Option D — don't load native code at all

A bytecode VM sidesteps every constraint above, and an interpreter for a small
stack machine is a weekend. It is also a different project from the one the
roadmap describes, and slower by an order of magnitude. Worth naming only to
rule out: the roadmap's whole point is native code compiled on the device.

## The part that is already solved

Whichever option wins, **the thing a loaded app has to be is already defined**:
the `AppDef` contract in `kernel/ui/app.h` — a name plus `paint`, `key`,
`click` and `open` callbacks. A loaded binary is simply an `AppDef` whose
function pointers happen to live in memory that arrived from a file. Windowed
and fullscreen are the same mechanism; fullscreen is a window the size of the
screen.

That also settles how a loaded app talks to CardOS. Rather than resolving
symbols against the kernel, **pass it a table of function pointers** — draw,
filesystem, memory — as an argument to its entry point. The app links against
nothing. This removes the symbol-resolution half of option B entirely, and it
versions cleanly: the table carries a version number, and CardOS refuses a
binary built against a table it no longer provides.

## Recommendation

**Option B, with the API table, and option C later as an optimisation.**

Option A is tempting because it could be working in a day, but an app binary
tied to one CardOS build is a dead end, and the work does not carry forward.
Option B is three or four times the effort and is the thing sub-project 3 needs
regardless.

A concrete v1:

1. Define `CardApi` — a versioned struct of function pointers covering drawing,
   the filesystem, and memory. This is the only thing an app may touch.
2. Define the entry point: `AppDef *app_register(const CardApi *api)`.
3. Write the ELF loader: parse, allocate, relocate, call. Refuse anything whose
   API version does not match.
4. `.capp` files in `/desktop` load as windows next to the built-in apps, using
   the icon machinery that already exists.
5. A host-side test for the ELF parsing and relocation arithmetic, against
   fixtures built by the real toolchain. That part is pure logic and belongs in
   the host suite, like everything else that can be.

The measurement to take before starting, because it decides the app size
budget: how much `MALLOC_CAP_EXEC` memory is actually free at boot, and how
much of it survives the BLE radio starting.
