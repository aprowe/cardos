# App constants in flash — design

Status: **approved in outline by alex 2026-10-10** ("do the overlay and the
flash constants"); this spec records the details. Builds on
`2026-10-09-xip-app-code-design.md`, and is planned together with
`2026-10-10-app-data-overlays-design.md`.

## Why

An app's constants (strings, tables, help, menus, icons) are 84 KB across
the 43 apps (18% of all app data). In Jar's three apps they are 30–47%.
Today they sit in the data block, so the app on screen spends arena on bytes
that never change. The flash cache already holds each app's relocated code.
It can hold the constants beside it, mapped through the **data** bus, which
allows byte reads, unlike the instruction bus.

## Design

### 1. A third link origin (`apps/capp.ld`, `capprel.c`)

| half | link origin | where it lives |
|---|---|---|
| code | `0x00000000` | flash (IROM), or exec RAM |
| data | `0x10000000` | arena, or heap |
| **constants** | **`0x20000000`** (`CAPP_RODATA_ORIGIN`) | **flash (DROM)**, or RAM |

`.rodata*` moves out of `.data` into its own output section `.rodata`
at the new origin. **`capp_info` and everything it points to stay in
`.data`.** The icon scan reads them without the flash cache, so they must be
in the data block. That covers `.rodata.capp_info`, already KEEP'd there,
plus the help text and command tables it points to; the build checks that
no pointer in `capp_info` lands in `.rodata` (§4).

`capprel_apply` classifies a value three ways instead of two:
- below `0x10000000`: code base;
- below `0x20000000`: data base;
- otherwise: rodata base.

It also relocates a third section: constants can hold pointers (a
`const char *const names[]` table), so `.rela.rodata` is applied to the
constants before they are written to flash.

### 2. Where constants go

- **XIP path** (data in the arena, partition present, no
  `CAPP_CODE_IN_RAM`): the cache entry becomes header, code, then constants.
  The constants start 16-byte aligned after the code. The code is relocated
  with `rodata base = drom_base + entry_off + rodata_off`; so are the
  constants themselves and the data.
- **Every other path** (heap data, RAM code, no partition, info-only load):
  constants go in RAM, appended to the data block after `.data` and the
  overlay region, relocated there. That is today's behaviour, so nothing
  that works now stops working.
- The **info-only load** (icon scan) needs only `capp_info`, which is in
  `.data`. It skips constants entirely.

### 3. The second mapping (`xipflash.c`)

`appcode` is mapped twice at boot:
- as INST, as now;
- as DATA (`ESP_PARTITION_MMAP_DATA`), at `drom_base`.

IDF maps flash with `ESP_MMU_MMAP_FLAG_PADDR_SHARED` (`flash_mmap.c:302`),
so the same range may be mapped twice. 512 KB is 8 more 64 KB pages, of
which there are plenty. Reads go through the 32 KB data cache. The flash
write path already flushes caches over the range it writes; the read-back
check (`xip_verify`) reads the flash itself. The DROM view is checked on the
device as the code view was.

### 4. The cache entry and its key (`xipcache.c`)

- The header records `code_size` and `rodata_size`; the entry covers both,
  and the CRC covers both.
- `XipKey` gains `drom_base`, taken from the header's reserved words, so the
  header stays 128 bytes.
- `XIP_MAGIC` becomes `"XIP2"`, so entries written by the current firmware
  read as free space and are rewritten on first launch. No wipe is needed.

### 5. The budget (`build_apps.py`)

The constants no longer count against the arena on the XIP path:
`data + bss + largest overlay` ≤ 28 KB. On the fallback paths (no partition,
console tools, headless commands) constants join the data block, which then
goes to the heap if it does not fit the arena. That is how a device without
`appcode` already behaves for big apps. `sizes.txt` gains a `rodata`
column. The build refuses:
- a `capp_info` pointer into `.rodata`;
- a constant section that is writable.

### 6. API

There is no new call, so no API bump of its own. It lands with the overlays'
API 37 and needs the same rebuild.

## Testing

Host:
- `test_capprel.c`: three-way classification; `.rela.rodata` applied;
  windowed equals whole with the new origin. Fixtures are rebuilt.
- `test_xipcache.c`: an entry with constants (size, CRC, the key's
  `drom_base`); an old-magic entry reads as free.
- `build_apps.py` checks on the fixture apps: constants at `0x20000000`;
  `capp_info`'s pointers all in `.data`.

Device:
- Every app opens from a fresh cache (`xip wipe`, then open each one).
- `mem` shows the arena holding less for Jar, Jar Shop and Jar Post (expected
  6–9 KB less each).
- Strings and tables render correctly. Help (fn-h), menus and commands come
  from `capp_info`, and Today runs the commands headless.
- A stale-cache check through the DROM view.

## Out of scope

Overlays (their own spec, same plan), and constants for the kernel itself.
