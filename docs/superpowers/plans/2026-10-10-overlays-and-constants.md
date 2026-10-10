# App Overlays and Constants in Flash Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The app on screen spends arena only on state that changes. Its constants run from flash beside its code, and the state of modes that are never on screen together shares one overlay region, kept on the card while another mode is live.

**Architecture:** Two changes to the loader that landed with XIP. The first adds a third link origin: `.rodata` at `0x20000000`, relocated three ways and written into the flash cache entry after the code. The cache maps `appcode` a second time, through the data bus, so constants can be read a byte at a time. On every path that is not XIP, the constants go in RAM after the data block. The second change adds an `OVERLAY` statement in `capp.ld` that lays `.ovl0`–`.ovl3` (NOBITS) at one address after `.data`. A portable `ovl.c` switches between them over an I/O callback table. `capprun.c` binds that table to the card, with per-load file names, and exposes it as API 46.

**Tech Stack:** C (ESP-IDF 6.1 on the device; the MSVC host suite via `build.bat`), Python (`tools/build_apps.py`, a new `tools/test_build_apps.py`), the Xtensa toolchain esp-15.2.0 (GCC 15.2, ld 2.45), `tools/cardctl.py` for the devices.

**Specs:** `docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md` and `docs/superpowers/specs/2026-10-10-app-data-overlays-design.md`, both building on `docs/superpowers/specs/2026-10-09-xip-app-code-design.md` (implemented on this branch). Read all three first. This plan argues from them and corrects them where the code says otherwise (below).

## Spec corrections

The specs were written before the master merge (d19353f) and before anyone tried the linker. Each correction below is planned around.

1. **The API version is 46, not 37.** `CAPP_API_VERSION` is 45 on this branch (master added `motion` … `paint_direct`). Overlays bump it to **46**. The constants move is an ABI change by itself: an API-45 loader would relocate a `0x2000xxxx` value against the data base. So the bump lands in Task 1, with the linker change, and the two `CardApi` fields join it in Task 6. Nothing is published between those tasks.
2. **"capp_info and everything it points to stay in `.data`" cannot be done without changing every app.** String literals pool into `.rodata.str1.1`, and tables like `ACTIONS[]`/`P_MIN[]` are ordinary `static const` objects. At link time nothing tells the ones `capp_info` reaches from the rest. Jar's `.rela.data` has exactly two entries, `capp_info.help` and `capp_info.commands`, and both point into `.rodata` once it moves. Measured across the 41 apps: 21 KB of the constants are string literals and 67 KB are tables. Keeping either in `.data` gives back most of the gain. **Correction:** only `capp_info` itself stays in `.data`. It is KEEP'd first, at `0x10000000`, because `read_info` and the loader's version/flags read need it there. The **info-only load (icon scan) loads the constants into RAM after the data block**, as every other non-XIP path does. Spec §2 already listed the info-only load among those paths, and its "skips constants entirely" sentence is dropped. The scan therefore costs exactly what it costs today: the same bytes, in a different place. **The build check becomes:** every pointer reachable from `capp_info` (help, commands, each command's id/label/menu/about/params, each param's name/about) lands in `.data`'s file bytes or in `.rodata`. It never lands in code, in an overlay, or nowhere. That check also covers the overlay spec's "capp_info or anything it points to inside an overlay".
3. **Overlay sections are named `.sbss.capp_ovlN`.** Verified with this toolchain:
   - `.bss.capp_ovl1` is NOBITS, but `capp.ld`'s `*(.bss .bss.*)` inside `.data` swallows it. ld assigns an input section to the first matching statement in script order, and the `OVERLAY` has to come after `.data`.
   - `.capp_ovl1.bss`, which escapes that glob, compiles **PROGBITS**: Xtensa GCC 15 gives it zeros in the file.
   - `.sbss.capp_ovl1` is NOBITS, and GCC refuses a nonzero initializer there (`error: only zero initializers are allowed in section '.sbss.capp_ovl1'`). Xtensa has no small-data model, so `-fdata-sections` never emits a `.sbss` name of its own, and nothing in `capp.ld` names `.sbss`. It reaches the `OVERLAY` untouched.
4. **An initialised overlay variable is refused by the compiler**, not first by the build. The build's "not NOBITS" check stays as a backstop and is tested on a synthetic section list. No source can produce that case any more.
5. **A failed read changes which overlay is live.** The API comment says "-1 failed, and nothing changed", but §5 says a read failure zeroes the target and returns -1. Planned: a **save** failure returns -1 with nothing changed. A **read** failure returns -1 with `n` live and zeroed. `overlay_live()` says which happened.
6. **Overlay files are deleted when the run lets go of its image** (`release_image` in `capprun.c`, just before `capp_unload`). The run owns the overlay state and the load id, and only a load whose code ran can have files. The icon scan's loads never run code. This is the same moment as the spec's "on unload (`capp_unload`)".
7. **The data mapping is both-or-neither.** If `appcode` cannot also be mapped through the data bus, there is no cache at all, which is today's no-partition path. Code in flash with constants in RAM would mean code relocated against a heap address, and no cache entry can be valid for that.
8. **If the cache write fails after the arena was claimed at the data block's size**, the constants go in a heap block of their own (`LoadedApp.rodata_heap`), not "appended to the data block". The arena was claimed for the flash path and holds only the block.
9. **The cache key gains `rodata_size` as well as `drom_base`.** Both are taken from the reserved words: `reserved` goes from 13 to 11 and the header stays 128 bytes. The entry's CRC chains the code and then the constants. The padding between them is never written, so it is left out of the CRC.
10. **`.capp` files are linked with `ld -n`** (no page alignment of segments). This is not in either spec. A third PT_LOAD segment would add up to 4 KB of padding to every `.capp` (about 170 KB over 42 apps in every `update apps`, and in the four EMBED blobs in firmware). With `-n` the sections pack: Jar's code moves from file offset 0x1000 to 0x94. The loader reads by `sh_offset`, so nothing else changes.
11. **Jar is one app** since master's 35d89ba (data 28532 bytes, 140 under the arena). The spec's "Jar, Jar Shop, Jar Post, 6–9 KB less each" becomes one measurement. Linked with the new script, Jar's data block is **15668** bytes and its constants **12864**.

## Global Constraints

- Arena: **28 KB** (`ARENA_SIZE 28u * 1024u`), 16-byte aligned, one holder. It is unchanged; it gains `arena_used()` for `mem`.
- **API 46** (`CAPP_API_VERSION 46`). The new `CardApi` fields go **at the end, after `paint_direct`**, in this order: `overlay`, `overlay_live`. The `cardapi.c` initializer follows the same order.
- `CAPP_OVL_MAX 4`, overlays numbered 0–3; `CAPP_OVL_KEEP 0`, `CAPP_OVL_DISCARD 1`. **Overlay 0 is live and zeroed at start**, and in a headless command. An app with no overlays gets -1 from both calls.
- Saves go to **`/cache/ovl/<load>-<n>.bin`**, where `<load>` is a per-load counter in `capprun.c` (`s_load_seq`, starting at 1 after each boot). Files are deleted when the run lets go of its image, and `/cache/ovl` is emptied at boot.
- `CAPP_DATA_ORIGIN 0x10000000`, **`CAPP_RODATA_ORIGIN 0x20000000`**. Constants start at `xip_ro_at(code_size) = (code_size + 15) & ~15` in an entry's body.
- **`capp_info` itself stays first in `.data`** (at `0x10000000`). What it points to may be in `.data` or `.rodata`, never in code or an overlay (Spec correction 2).
- `XIP_MAGIC 0x32504958` ("XIP2"). The header is **128 bytes**. "XIPC" entries read as free space, and no wipe is needed.
- **Nothing new on `cardos-bg`.** Flash is written only on a cache miss, on the shell's loop, inside `capp_load_ex`, under `xipflash_lock()`. Overlay switches run on whatever task called `api->overlay`, which is always the shell's loop.
- Code style matches the surrounding files: C89 declarations at the top of blocks, comments that say *why*, `snake_case`, no new dependencies.
- Host tests use `test/tinytest.h` (`CHECK`, `CHECK_EQ`). Every `void test_*(void)` is collected by `tools/gen_test_main.py`, which `build.bat` runs and which rewrites `test/test_main.c`. That file is tracked: commit the regenerated file with each task's tests. **Never edit it by hand.**
- The host suite, from PowerShell: `cmd /c "C:\Users\alexr\Projects\cardos\.claude\worktrees\xip-app-code\build.bat"`. It was 48893 checks / 0 failures before the master merge; Task 1 Step 1 records the number after it.
- Apps: `python tools/build_apps.py`. Firmware: `python -m platformio run` (debug). Server tests: `python -m server.tests.test_catalog` must keep passing (it reads commands through the new reader). The other server tests are unaffected.
- **Do not flash, put apps on a card, or publish between Task 1 and Task 5.** The loader cannot place constants until Task 5. Tasks 5 and 10 are the device steps.
- Devices: **two**, on COM3 and COM4 (USB `303A:1001` both, told apart in Task 10). `CARDOS_PORT=COMn` selects one for `tools/cardctl.py`, and `--upload-port COMn` for PlatformIO. In Git Bash, prefix `MSYS_NO_PATHCONV=1`.
- End every commit message with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **The icon scan follows `capp_info` into the constants.** `catalog_add` reads every command's id, about and params at scan time, through `capp_load_info`. If that load leaves the constants out, the catalog is garbage, or the scan faults at boot. Covered by Task 1's `check_info` (every pointer lands in `.data` or `.rodata`), Task 5's info-only path (constants in RAM after the block), and the Task 10 check that `/cache/commands.txt` matches `build/apps/commands.json` after the index is rebuilt.
2. **`.rela.rodata` not applied, or applied with the wrong base.** Constant tables hold pointers: the action tables, `const CappUi`, string tables, function pointers. Missing the third relocation pass leaves link addresses in flash, and the first menu or callback faults. Covered by Task 2: windowed equals whole for `.rodata` on every fixture, every relocated value lands in one of the three ranges, and Jar has `.rela.rodata` entries. The Task 10 device checks cover help and menus.
3. **Stale constants through the data view.** An entry rewritten at the same flash offset must not be read through stale D-cache lines, and an entry relocated for another data view must miss. Covered by Task 3 (constants are in the CRC; another `drom_base` or `rodata_size` misses) and Task 10 Step 8 (a changed string, rewritten at offset 0 after `xip wipe`, shows the new text).
4. **Overlay files colliding, leaking, or failing silently.** Two instances of one app (on screen and headless) must never share files. A save must be at the outgoing overlay's own size. A failed save must leave the live overlay intact, and a failed read must zero the target and say so. Files must go when the app goes and at boot. Covered by Task 7's `test_ovl_two_loads_never_share_files`, `..._saves_at_its_own_size`, `..._write_failure_...`, `..._read_failure_...`, and `..._drop_...`, and by Task 10 Steps 6–7 (`ls /cache/ovl` empty after `ovltest`, and a stray file gone after a reboot).
5. **Constants handed to DMA.** A sprite table in `.rodata` passed to `api->pixels` now lives in flash, and the panel's SPI DMA cannot read flash. Left to the SPI driver, every row would allocate a bounce buffer of its own. Task 5 bounces each row through one static RAM row in `api_pixels`, and Task 10 Step 5 opens every app and looks at it.

---

## File Structure

| File | Responsibility | Host? |
|---|---|---|
| `apps/capp.ld` | `.rodata` at `0x20000000` (Task 1); `OVERLAY` of `.ovl0`–`.ovl3` after `.data` (Task 6) | — |
| `tools/build_apps.py` | `ld -n`; `sections`/`layout`/`Image`; `check_layout`, `check_info`, `check_overlays`; the data-block budget; `sizes.txt` columns; `compile_link` | — |
| `tools/test_build_apps.py` (new) | the build's checks on real and fixture binaries | yes (Python) |
| `kernel/app/capp.h` | `CAPP_API_VERSION 46`; `CAPP_OVERLAY`, `CAPP_OVL_*`; `overlay`/`overlay_live` at the end of `CardApi` | both |
| `kernel/app/capprel.h/.c` | `CAPP_RODATA_ORIGIN`, `CappBases`, three-way `capprel_apply(…, origin, …)` | yes |
| `kernel/app/xipcache.h/.c` | key `rodata_size`/`drom_base`, `XIP2`, body = code + constants, chained CRC, `xip_ro_at`/`xip_body` | yes |
| `kernel/app/xipflash.h/.c` | second mapping (DATA), `xipflash_drom_base()` | device |
| `kernel/app/arena.h/.c` | `arena_used()` | yes |
| `kernel/app/elfload.h/.c` | `LoadedApp` constants and overlay fields; three-way placement; `xip_place` writes constants; `find_overlays` | device |
| `kernel/app/ovl.h/.c` (new) | switching overlays over `OvlIo`; paths; drop | yes |
| `kernel/app/capprun.h/.c` | `Ovl` in `Run`, load ids, card glue, `capprun_overlay[_live]`, `capprun_clear_overlays` | device |
| `kernel/app/cardapi.c` | the two API entries; `api_pixels` bounces rows from flash | device |
| `kernel/sys/memreport.c` | `arena USED/28672 holder` | device |
| `src/main.c` | `xip` stale check knows `drom_base`; `capprun_clear_overlays()` at boot | device |
| `apps/ovltest.c` (new), `apps/folders.txt` | the device test and build fixture; `ovltest -` | — |
| `test/test_capprel.c`, `test/test_xipcache.c`, `test/test_arena.c`, `test/test_ovl.c` (new) | host tests | yes |
| `test/fakeapi.c` | stubs for the two new calls | yes |
| `test/fixtures/capp_{jar,forklift,cat}.capp` (rebuilt), `capp_ovltest.capp` (new) | real-toolchain fixtures | — |
| `host/CMakeLists.txt`, `src/CMakeLists.txt` | `ovl.c` | — |
| `CLAUDE.md`, both specs | API paragraph, XIP paragraph, measurements | — |

---

### Task 1: The build: constants at their own origin, the layout checks, the budget

The linker change and the build's understanding of it go together. Without the reader change, `emit_catalog` follows `capp_info.commands` into `.rodata` and fails with an `IndexError`.

**Files:**
- Modify: `apps/capp.ld`, `tools/build_apps.py`, `kernel/app/capp.h:41` (the version)
- Create: `tools/test_build_apps.py`

**Interfaces:**
- Produces (Python, `tools/build_apps.py`):
  - `SHT_PROGBITS = 1`, `SHT_NOBITS = 8`, `SHF_WRITE = 1`, `SHF_ALLOC = 2`, `DATA_BASE = 0x10000000`, `RODATA_BASE = 0x20000000`, `OVL_MAX = 4`, `KNOWN_SECTIONS`
  - `sections(elf) -> (list of {"name","type","flags","addr","offset","size","align"}, bytes)`
  - `layout(elf) -> {"code","data","rodata","block","ovl": {n: size}, "ovl_addr": set}`
  - `align16(n)`
  - `class Image(elf)`: `.where(addr)`, `.u8(addr)`, `.u32(addr)`, `.cstr(addr)`
  - `INFO_HELP = 52`
  - `check_info(elf, name)`, `check_layout(elf, name)`. Each raises `SystemExit` on refusal.
  - `compile_link(src, out_dir=OUT) -> elf path`
  - `read_commands(elf)`, unchanged output
- Produces (C): `#define CAPP_API_VERSION 46`

- [ ] **Step 1: Baseline.** Run the host suite and write down the check count it prints (`N checks, 0 failures`). It is the baseline for every later "Expected". Then keep the old sizes for Task 10:

```bash
python tools/build_apps.py
cp build/apps/sizes.txt build/apps/sizes-before.txt
```
(`build/` is gitignored.)

- [ ] **Step 2: Write the failing test, `tools/test_build_apps.py`**

```python
"""tools/build_apps.py's layout checks, on real binaries.

Constants in flash (docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md):
every app's .rodata is linked at RODATA_BASE and is not writable, capp_info
stays at the start of .data, and everything capp_info points to is in .data
or .rodata -- the icon scan reads it without running the app. Overlays
(docs/superpowers/specs/2026-10-10-app-data-overlays-design.md) are checked
in the class below this one. Run tools/build_apps.py first.

    python tools/test_build_apps.py
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_apps

OUT = build_apps.OUT
JAR = os.path.join(OUT, "jar.capp")


def built():
    return sorted(os.path.join(OUT, f) for f in os.listdir(OUT) if f.endswith(".capp"))


@unittest.skipUnless(os.path.isfile(JAR), "run tools/build_apps.py first")
class Constants(unittest.TestCase):

    def test_every_app_passes_the_layout_check(self):
        for elf in built():
            build_apps.check_layout(elf, os.path.basename(elf))

    def test_constants_are_linked_at_their_own_origin(self):
        for elf in built():
            secs, _ = build_apps.sections(elf)
            for s in secs:
                if s["name"] == ".rodata":
                    self.assertEqual(s["addr"], build_apps.RODATA_BASE, elf)
                    self.assertFalse(s["flags"] & build_apps.SHF_WRITE, elf)

    def test_jar_keeps_its_constants_out_of_the_arena(self):
        lay = build_apps.layout(JAR)
        self.assertGreater(lay["rodata"], 8 * 1024)
        self.assertLessEqual(lay["block"], build_apps.DATA_BUDGET - 8 * 1024)

    def test_capp_info_help_is_a_constant_now(self):
        img = build_apps.Image(JAR)
        help_ = img.u32(build_apps.DATA_BASE + build_apps.INFO_HELP)
        self.assertGreaterEqual(help_, build_apps.RODATA_BASE)
        self.assertTrue(img.cstr(help_))


if __name__ == "__main__":
    unittest.main(verbosity=1)
```
(`shutil` and `tempfile` are for the overlay class in Task 6.)

- [ ] **Step 3: Run it.** `python tools/test_build_apps.py`. Expected: `AttributeError: module 'build_apps' has no attribute 'check_layout'` (or `sections`).

- [ ] **Step 4: `apps/capp.ld`.** Replace the header comment's second and third paragraphs, and the `SECTIONS` block, with:

```
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
```

```
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
```

- [ ] **Step 5: `kernel/app/capp.h`.** Change `#define CAPP_API_VERSION 45` to `#define CAPP_API_VERSION 46`. Every app is rebuilt against it below, and the loader refuses the old ones. An API-45 loader would misplace these apps' constants, so the version must say so.

- [ ] **Step 6: `tools/build_apps.py`: imports and the layout readers.** Add `import struct` after `import re`. Then replace the whole `code_data` function (from `def code_data(path):` to its `return code, dat`) with:

```python
# Where apps/capp.ld links each part, and what the loader makes of it
# (kernel/app/elfload.c): code at 0; the data block at DATA_BASE -- .data,
# then the overlay region all of an app's overlays share; the constants at
# RODATA_BASE, in flash beside the code when the app runs from the flash
# cache and in RAM after the data block otherwise. See
# docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md.
SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_WRITE, SHF_ALLOC = 1, 2
DATA_BASE = 0x10000000            # CAPP_DATA_ORIGIN in kernel/app/capprel.h
RODATA_BASE = 0x20000000          # CAPP_RODATA_ORIGIN
OVL_MAX = 4                       # CAPP_OVL_MAX in kernel/app/capp.h
KNOWN_SECTIONS = {".code", ".data", ".rodata"} | {".ovl%d" % n for n in range(OVL_MAX)}


def align16(n):
    return (n + 15) & ~15


def sections(elf):
    """Every section header of a built .capp, named, and the file's bytes."""
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    names = raw[shstrndx][4]
    out = []
    for s in raw:
        end = data.index(bytes([0]), names + s[0])
        out.append({"name": data[names + s[0]:end].decode("ascii", "replace"),
                    "type": s[1], "flags": s[2], "addr": s[3], "offset": s[4],
                    "size": s[5], "align": s[8]})
    return out, data


def layout(elf):
    """What the loader allocates. block is the data block: .data and then
    the largest overlay, alignment included -- the arena's share when the
    app runs from the flash cache, since its constants are in flash."""
    secs, _ = sections(elf)
    lay = {"code": 0, "data": 0, "rodata": 0, "block": 0, "ovl": {}, "ovl_addr": set()}
    for s in secs:
        if not s["flags"] & SHF_ALLOC or s["size"] == 0:
            continue
        if s["addr"] >= RODATA_BASE:
            lay["rodata"] += s["size"]
        elif s["addr"] >= DATA_BASE:
            lay["block"] = max(lay["block"], s["addr"] - DATA_BASE + s["size"])
            m = re.match(r"\.ovl([0-9])$", s["name"])
            if m:
                lay["ovl"][int(m.group(1))] = s["size"]
                lay["ovl_addr"].add(s["addr"])
            elif s["name"] == ".data":
                lay["data"] = s["size"]
        else:
            lay["code"] += s["size"]
    return lay
```

- [ ] **Step 7: The budget.** Replace from `# The size budget, since app code runs from flash` through the end of `check_budget` with:

```python
# The size budget, since app code runs from flash
# (docs/superpowers/specs/2026-10-09-xip-app-code-design.md) and its
# constants with it (docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md).
# What has to fit the 28 KB arena -- ARENA_SIZE in kernel/app/arena.h -- is
# the data block: .data and the largest overlay. An app that keeps its code
# in RAM (CAPP_CODE_IN_RAM, for an inner loop) keeps its constants in RAM
# too, after the block, and still needs one block of executable RAM; after a
# day the largest was 24 KB.
DATA_BUDGET = 28 * 1024          # ARENA_SIZE
RAM_CODE_BUDGET = 16 * 1024      # CAPP_CODE_IN_RAM apps
MAX_CODE = 96 * 1024             # CAPP_MAX_CODE in kernel/app/elfload.c
CAPP_CODE_IN_RAM = 0x0040

# Apps allowed over the budget for now, each with why. Take one off as soon
# as it is back under -- the build says when.
OVER_BUDGET = {
}


def check_budget(built):
    """Print the layout per app, write build/apps/sizes.txt, and refuse an
    app over budget that is not in OVER_BUDGET."""
    rows, bad = [], []
    for name, elf in built:
        lay = layout(elf)
        flags = read_info(elf)[0]
        in_ram = bool(flags & CAPP_CODE_IN_RAM)
        need = align16(lay["block"]) + lay["rodata"] if in_ram else lay["block"]
        over = []
        if need > DATA_BUDGET:
            over.append("data block %d > %d (the arena)" % (need, DATA_BUDGET))
        if in_ram and lay["code"] > RAM_CODE_BUDGET:
            over.append("code %d > %d for CAPP_CODE_IN_RAM" % (lay["code"], RAM_CODE_BUDGET))
        if lay["code"] > MAX_CODE:
            over.append("code %d > %d" % (lay["code"], MAX_CODE))
        rows.append((name, lay, in_ram, over))
    with open(os.path.join(OUT, "sizes.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# app        code    data   ovl0   ovl1   ovl2   ovl3  rodata   block  where"
                "  (bytes; block = data + largest overlay <= %d; RAM code <= %d)\n"
                % (DATA_BUDGET, RAM_CODE_BUDGET))
        for name, lay, in_ram, over in rows:
            ovl = ["%6d" % lay["ovl"][n] if n in lay["ovl"] else "     -" for n in range(OVL_MAX)]
            f.write("%-10s %7d %7d %s %7d %7d  %s%s\n"
                    % (name, lay["code"], lay["data"], " ".join(ovl), lay["rodata"],
                       lay["block"], "ram  " if in_ram else "flash", "  OVER" if over else ""))
    print("  %s" % os.path.relpath(os.path.join(OUT, "sizes.txt"), ROOT))
    for name, lay, in_ram, over in rows:
        if over and name in OVER_BUDGET:
            print("  warning: %s is over budget (%s), allowed: %s"
                  % (name, "; ".join(over), OVER_BUDGET[name]))
        elif over:
            bad.append("%s: %s" % (name, "; ".join(over)))
        elif name in OVER_BUDGET:
            print("  note: %s is under budget now -- take it out of OVER_BUDGET" % name)
    if bad:
        raise SystemExit(
            "over the app size budget (the data block must fit the 28 KB arena; "
            "see OVER_BUDGET in tools/build_apps.py):\n  " + "\n  ".join(bad))
```

- [ ] **Step 8: The reader, and the checks.** Replace from `# The commands, followed out of capp_info the same way` through the end of `read_commands` with the block below. It also deletes `_data_section` and the old `DATA_BASE = 0x10000000` line there, since Step 6 defined it.

```python
# The commands, followed out of capp_info the same way (CappInfo.commands and
# CappAction/CappParam in kernel/app/capp.h, 32-bit Xtensa layout). Pointers
# are link-time addresses, into .data or -- for anything const, which is
# nearly everything capp_info points to -- into .rodata.
INFO_HELP, INFO_COMMANDS, INFO_NCOMMANDS = 52, 56, 60
ACTION_SIZE, PARAM_SIZE = 28, 12
ARG_TYPES = {1: "text", 2: "int", 3: "bool", 4: "choice"}
CMD_YES, CMD_NET = 0x01, 0x02


class Image:
    """A built .capp's .data and .rodata bytes, read by link address -- as the
    kernel reads capp_info and its tables in the icon scan, with the
    constants in RAM beside the data."""

    def __init__(self, elf):
        secs, data = sections(elf)
        self.elf = elf
        self.parts = [(s["addr"], data[s["offset"]:s["offset"] + s["size"]])
                      for s in secs
                      if s["name"] in (".data", ".rodata") and s["type"] == SHT_PROGBITS]
        if not any(a == DATA_BASE for a, _ in self.parts):
            raise SystemExit("%s: no .data section, so no capp_info" % elf)

    def where(self, addr):
        """(bytes, offset) holding addr, or None outside .data and .rodata."""
        for base, b in self.parts:
            if base <= addr < base + len(b):
                return b, addr - base
        return None

    def _at(self, addr, n):
        w = self.where(addr)
        if w is None or w[1] + n > len(w[0]):
            raise SystemExit("%s: 0x%08x is not in .data or .rodata" % (self.elf, addr))
        return w

    def u8(self, addr):
        b, o = self._at(addr, 1)
        return b[o]

    def u32(self, addr):
        b, o = self._at(addr, 4)
        return struct.unpack_from("<I", b, o)[0]

    def cstr(self, addr):
        if not addr:
            return None
        b, o = self._at(addr, 1)
        return b[o:b.index(bytes([0]), o)].decode("utf-8", "replace")


def read_commands(elf):
    """The app's commands: [{"id", "about", "net", "params": [{"name",
    "type", "about"}]}], only entries marked CAPP_CMD_YES."""
    img = Image(elf)
    table = img.u32(DATA_BASE + INFO_COMMANDS)
    n = img.u8(DATA_BASE + INFO_NCOMMANDS)
    out = []
    for i in range(n if table else 0):
        a = table + i * ACTION_SIZE
        cmd = img.u8(a + 25)
        if not cmd & CMD_YES:
            continue
        params, np = img.u32(a + 20), img.u8(a + 24)
        plist = []
        for j in range(np if params else 0):
            p = params + j * PARAM_SIZE
            plist.append({"name": img.cstr(img.u32(p)),
                          "type": ARG_TYPES.get(img.u8(p + 4), "?"),
                          "about": img.cstr(img.u32(p + 8))})
        out.append({"id": img.cstr(img.u32(a)), "about": img.cstr(img.u32(a + 16)),
                    "net": bool(cmd & CMD_NET), "params": plist})
    return out


def check_info(elf, name):
    """Everything capp_info points to is in .data's bytes or in .rodata: the
    icon scan reads it without running the app, with the constants loaded
    into RAM and nothing else -- not the code, not an overlay."""
    img = Image(elf)

    def ok(what, v):
        if v and img.where(v) is None:
            raise SystemExit(
                "%s: capp_info's %s points at 0x%08x, outside .data and .rodata (in an "
                "overlay, or the code). The icon scan reads capp_info and what it points "
                "to without running the app, so all of it must be data or constants."
                % (name, what, v))
        return v

    ok("help", img.u32(DATA_BASE + INFO_HELP))
    table = ok("commands", img.u32(DATA_BASE + INFO_COMMANDS))
    for i in range(img.u8(DATA_BASE + INFO_NCOMMANDS) if table else 0):
        a = ok("command %d" % i, table + i * ACTION_SIZE)
        for what, off in (("id", 0), ("label", 4), ("menu", 8), ("about", 16)):
            ok("command %d %s" % (i, what), img.u32(a + off))
        params = ok("command %d params" % i, img.u32(a + 20))
        for j in range(img.u8(a + 24) if params else 0):
            p = ok("command %d param %d" % (i, j), params + j * PARAM_SIZE)
            ok("command %d param %d name" % (i, j), img.u32(p))
            ok("command %d param %d about" % (i, j), img.u32(p + 8))


def check_layout(elf, name):
    """Refuse a layout the loader does not expect: a section it would not
    place, constants that are writable, misplaced or over-aligned, capp_info
    anywhere but the start of .data, or capp_info reaching outside .data and
    .rodata."""
    secs, _ = sections(elf)
    alloc = [s for s in secs if s["flags"] & SHF_ALLOC and s["size"]]
    for s in alloc:
        if s["name"] not in KNOWN_SECTIONS:
            raise SystemExit(
                "%s: section %s is not one the loader places (.code, .data, .rodata, "
                ".ovl0-.ovl%d); overlays are numbered 0-%d"
                % (name, s["name"], OVL_MAX - 1, OVL_MAX - 1))
        if s["name"] == ".rodata":
            if s["addr"] != RODATA_BASE:
                raise SystemExit("%s: .rodata at 0x%08x, not 0x%08x" % (name, s["addr"], RODATA_BASE))
            if s["flags"] & SHF_WRITE:
                raise SystemExit(
                    "%s: .rodata is writable -- something not const was put in a .rodata "
                    "section. Constants run from flash, which an app cannot write." % name)
            if s["align"] > 16:
                raise SystemExit(
                    "%s: .rodata wants %d-byte alignment; the flash cache places constants "
                    "on a 16-byte boundary" % (name, s["align"]))
    syms = run([READELF, "-s", "--wide", elf])
    m = re.search(r"^\s*\d+:\s+([0-9a-f]{8})\s+\d+\s+OBJECT\s+GLOBAL\s+\S+\s+\S+\s+capp_info\s*$",
                  syms, re.M)
    if not m or int(m.group(1), 16) != DATA_BASE:
        raise SystemExit(
            "%s: capp_info is not the first thing in .data; apps/capp.ld KEEPs it there, "
            "and the loader and read_info read it from there" % name)
    check_info(elf, name)
```

- [ ] **Step 9: `build()` and the link.** Replace `def build(src):` through its `return name, elf` with:

```python
def compile_link(src, out_dir=OUT):
    """Compile and link one app; the .capp's path. Linked -n (no page
    alignment): the constants are a third segment, and page-aligned
    segments added up to 4 KB of padding to every file and every download,
    which the loader -- reading by section offset -- never needed."""
    name = os.path.splitext(os.path.basename(src))[0]
    obj = os.path.join(out_dir, name + ".o")
    elf = os.path.join(out_dir, name + ".capp")
    run([GCC] + CFLAGS + ["-isystem", gcc_include_dir(), "-c", src, "-o", obj])
    run([LD, "-q", "-n", "-T", os.path.join(APPS, "capp.ld"),
         "--gc-sections", "-o", elf, obj])
    return elf


def build(src):
    name = os.path.splitext(os.path.basename(src))[0]
    elf = compile_link(src)
    check_image(elf, name)
    check_placement(name, elf)
    before = loaded_bytes(elf)
    strip_relocs(elf)
    check_image(elf, name)
    if loaded_bytes(elf) != before:
        raise SystemExit("%s: stripping relocations changed the loaded image" % name)
    check_layout(elf, name)

    size = os.path.getsize(elf)
    lay = layout(elf)
    print("  %-14s %6d bytes  code %6d  data %6d  rodata %6d"
          % (name + ".capp", size, lay["code"], lay["block"], lay["rodata"]))
    return name, elf
```

- [ ] **Step 10: Run everything.**
  - `python tools/build_apps.py`. Expected: all 41 apps build, and `sizes.txt` has the new columns. Jar should show data 15668 and rodata 12864 (±100 if the source moved), and every file is smaller than in `sizes-before.txt`'s run (compare `ls -l build/apps/*.capp` before and after for Jar: about 4 KB less).
  - `python tools/test_build_apps.py`. Expected: 4 tests OK.
  - `python -m server.tests.test_catalog`. Expected: OK. Todo's commands now come out of `.rodata`.
  - The host suite. Expected: the Step 1 count, 0 failures. Nothing host-side changed yet, but the apps compiled on the host include `capp.h`.

- [ ] **Step 11: Commit**

```bash
git add apps/capp.ld tools/build_apps.py tools/test_build_apps.py kernel/app/capp.h
git commit -m "Apps: constants at 0x20000000, the build reads and checks them; API 46"
```
The body gives Jar's data block before and after (28532 -> ~15668) and its `.capp` size before and after.

---
### Task 2: Three-way relocation, and `.rela.rodata`

`capprel_apply` takes the section's link origin instead of `into_code`, and a `CappBases` instead of two bases. The fixtures are rebuilt with Task 1's script. `elfload.c` is changed only enough to compile and behave as before for code and data. It places the constants in Task 5.

**Files:**
- Modify: `kernel/app/capprel.h`, `kernel/app/capprel.c`, `test/test_capprel.c` (rewritten), `kernel/app/elfload.c` (`relocate` and its three callers)
- Replace: `test/fixtures/capp_jar.capp`, `capp_forklift.capp`, `capp_cat.capp`

**Interfaces:**
- Consumes: Task 1's linked layout (`.rodata` at `0x20000000`, `.rela.rodata` kept by `strip_relocs`, which keeps every RELA section's `R_XTENSA_32` entries).
- Produces:
  - `#define CAPP_RODATA_ORIGIN 0x20000000u`
  - `typedef struct { uint32_t code, data, rodata; } CappBases;`
  - `int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit, uint32_t origin, const CappRela *r, uint32_t n, const CappBases *b);`: `origin` is `0`, `CAPP_DATA_ORIGIN` or `CAPP_RODATA_ORIGIN`. A value below `CAPP_DATA_ORIGIN` gets `b->code` added, a value below `CAPP_RODATA_ORIGIN` is moved by `b->data`, and anything higher by `b->rodata`. Returns -1 for an `r_offset` below `origin`, or for `b == NULL`.
  - In `elfload.c` (static): `static CappResult relocate(int fd, const Elf32_Shdr *sh, int shnum, int sec, uint32_t origin, uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit, const CappBases *b);`

- [ ] **Step 1: Rebuild the fixtures**

```bash
python tools/build_apps.py
cp build/apps/jar.capp test/fixtures/capp_jar.capp
cp build/apps/forklift.capp test/fixtures/capp_forklift.capp
cp build/apps/cat.capp test/fixtures/capp_cat.capp
```
Linked by hand from the same objects on 2026-10-10, Jar had 283 `.rela.rodata` entries, Forklift 98, and Cat none. Cat's constants are strings only.

- [ ] **Step 2: Write the failing tests: replace `test/test_capprel.c` whole**

```c
/* Relocating an app a window at a time must give exactly the bytes
 * relocating it whole does -- the flash cache writes code and constants
 * through a 28 KB window -- and every absolute reference must land in the
 * part it names: code, data or constants. Fixtures are real .capps from
 * tools/build_apps.py. */
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/capprel.h"

typedef struct {
  uint8_t  *file;
  long      size;
  uint32_t  code_off, code_size, data_off, data_size, ro_off, ro_size;
  uint32_t  block;          /* the data block: .data and anything after it, overlays too */
  CappRela *code_rel, *data_rel, *ro_rel;
  uint32_t  ncode_rel, ndata_rel, nro_rel;
} Capp;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void fixture_path(char *out, size_t cap, const char *name) {
  const char *f = __FILE__, *cut = strrchr(f, '/');
  const char *bs = strrchr(f, '\\');
  if (bs && (!cut || bs > cut)) cut = bs;
  if (cut) snprintf(out, cap, "%.*sfixtures/%s", (int)(cut - f + 1), f, name);
  else snprintf(out, cap, "test/fixtures/%s", name);
}

/* Just enough ELF: the three parts, by link address as the loader classifies
 * them, and the RELA sections aimed at each. */
static int load_capp(const char *name, Capp *c) {
  char path[512];
  FILE *fp;
  uint32_t shoff, i;
  uint16_t shentsize, shnum;
  int code_sec = -1, data_sec = -1, ro_sec = -1;
  memset(c, 0, sizeof *c);
  fixture_path(path, sizeof path, name);
  if ((fp = fopen(path, "rb")) == NULL) return -1;
  fseek(fp, 0, SEEK_END); c->size = ftell(fp); fseek(fp, 0, SEEK_SET);
  c->file = malloc((size_t)c->size);
  if (fread(c->file, 1, (size_t)c->size, fp) != (size_t)c->size) { fclose(fp); return -1; }
  fclose(fp);
  shoff = u32(c->file + 0x20); shentsize = u16(c->file + 0x2E); shnum = u16(c->file + 0x30);
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    uint32_t type = u32(s + 4), flags = u32(s + 8), addr = u32(s + 12), size = u32(s + 20);
    if (!(flags & 2) || (type != 1 && type != 8) || size == 0) continue;
    if (addr >= CAPP_RODATA_ORIGIN) {
      if (ro_sec < 0 && type == 1) { ro_sec = (int)i; c->ro_off = u32(s + 16); c->ro_size = size; }
    } else if (addr >= CAPP_DATA_ORIGIN) {
      if (addr - CAPP_DATA_ORIGIN + size > c->block) c->block = addr - CAPP_DATA_ORIGIN + size;
      if (data_sec < 0 && type == 1) { data_sec = (int)i; c->data_off = u32(s + 16); c->data_size = size; }
    } else if (code_sec < 0) {
      code_sec = (int)i; c->code_off = u32(s + 16); c->code_size = size;
    }
  }
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    int to = (int)u32(s + 28);
    if (u32(s + 4) != 4) continue;                       /* SHT_RELA */
    if (to == code_sec) { c->code_rel = (CappRela *)(c->file + u32(s + 16)); c->ncode_rel = u32(s + 20) / 12; }
    else if (to == data_sec) { c->data_rel = (CappRela *)(c->file + u32(s + 16)); c->ndata_rel = u32(s + 20) / 12; }
    else if (ro_sec >= 0 && to == ro_sec) { c->ro_rel = (CappRela *)(c->file + u32(s + 16)); c->nro_rel = u32(s + 20) / 12; }
  }
  return code_sec >= 0 && data_sec >= 0 ? 0 : -1;
}

#define CODE_BASE 0x42480080u
#define DATA_BASE 0x3FC9A000u
#define RO_BASE   0x3C4800A0u

static const CappBases BASES = { CODE_BASE, DATA_BASE, RO_BASE };

static const char *FIXTURES[] = { "capp_jar.capp", "capp_forklift.capp", "capp_cat.capp" };
#define NFIX (sizeof FIXTURES / sizeof *FIXTURES)

/* One section relocated whole, then in windows of every awkward size: the
 * same bytes. */
static void check_windows(const uint8_t *src, uint32_t size, uint32_t origin,
                          const CappRela *rel, uint32_t nrel) {
  static const uint32_t WINDOWS[] = { 4, 8, 12, 4096, 28672 };
  uint8_t *whole = malloc(size), *win = malloc(size);
  size_t w;
  uint32_t o;
  memcpy(whole, src, size);
  CHECK_EQ(capprel_apply(whole, 0, size, size, origin, rel, nrel, &BASES), 0);
  if (nrel > 0) CHECK(memcmp(whole, src, size) != 0);      /* it did something */
  for (w = 0; w < sizeof WINDOWS / sizeof *WINDOWS; w++) {
    memcpy(win, src, size);
    for (o = 0; o < size; o += WINDOWS[w]) {
      uint32_t n = size - o < WINDOWS[w] ? size - o : WINDOWS[w];
      CHECK_EQ(capprel_apply(win + o, o, n, size, origin, rel, nrel, &BASES), 0);
    }
    CHECK(memcmp(win, whole, size) == 0);
  }
  free(whole); free(win);
}

void test_capprel_windows_match_whole_image(void) {
  size_t f;
  for (f = 0; f < NFIX; f++) {
    Capp c;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    CHECK_EQ(c.code_size % 4, 0);
    check_windows(c.file + c.code_off, c.code_size, 0, c.code_rel, c.ncode_rel);
    if (c.ro_size) {
      CHECK_EQ(c.ro_size % 4, 0);                          /* capp.ld's ALIGN(4) */
      check_windows(c.file + c.ro_off, c.ro_size, CAPP_RODATA_ORIGIN, c.ro_rel, c.nro_rel);
    }
    free(c.file);
  }
}

static int in(uint32_t v, uint32_t base, uint32_t size) { return v >= base && v <= base + size; }

/* The premise of the arena and of the flash cache: code and constants hold
 * addresses of all three parts, and each moves by its own base. */
void test_capprel_refs_land_in_the_right_part(void) {
  size_t f;
  for (f = 0; f < NFIX; f++) {
    Capp c;
    uint8_t *code, *ro = NULL;
    uint32_t j, to_data = 0, to_ro = 0;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    code = malloc(c.code_size);
    memcpy(code, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(code, 0, c.code_size, c.code_size, 0, c.code_rel, c.ncode_rel, &BASES), 0);
    if (c.ro_size) {
      ro = malloc(c.ro_size);
      memcpy(ro, c.file + c.ro_off, c.ro_size);
      CHECK_EQ(capprel_apply(ro, 0, c.ro_size, c.ro_size, CAPP_RODATA_ORIGIN, c.ro_rel, c.nro_rel,
                             &BASES), 0);
    }
    for (j = 0; j < c.ncode_rel + c.nro_rel; j++) {
      const CappRela *r = j < c.ncode_rel ? &c.code_rel[j] : &c.ro_rel[j - c.ncode_rel];
      uint32_t v;
      if ((r->r_info & 0xFF) != R_XTENSA_32) continue;
      v = j < c.ncode_rel ? u32(code + r->r_offset) : u32(ro + (r->r_offset - CAPP_RODATA_ORIGIN));
      CHECK(in(v, CODE_BASE, c.code_size) || in(v, DATA_BASE, c.block) || in(v, RO_BASE, c.ro_size));
      if (in(v, DATA_BASE, c.block)) to_data++;
      if (in(v, RO_BASE, c.ro_size)) to_ro++;
    }
    if (strcmp(FIXTURES[f], "capp_cat.capp") != 0) { CHECK(to_data > 0); CHECK(to_ro > 0); }
    free(code); free(ro); free(c.file);
  }
}

/* Constants hold pointers -- action tables, string tables, const CappUi --
 * so a third relocation section exists and must be applied. */
void test_capprel_constants_carry_relocations(void) {
  Capp c;
  CHECK_EQ(load_capp("capp_jar.capp", &c), 0);
  CHECK(c.ro_size > 8u * 1024u);
  CHECK(c.nro_rel > 0);
  free(c.file);
}

void test_capprel_refuses_what_it_cannot_apply(void) {
  uint8_t buf[64];
  CappRela r;
  memset(buf, 0, sizeof buf);
  r.r_addend = 0;
  r.r_offset = 8;  r.r_info = 5;                 /* a type the loader does not know */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_info = R_XTENSA_32; r.r_offset = 64;       /* past the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 62;                               /* runs off the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 6;                                /* unaligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = CAPP_DATA_ORIGIN - 2;             /* below the data origin */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, CAPP_DATA_ORIGIN, &r, 1, &BASES), -1);
  r.r_offset = CAPP_DATA_ORIGIN + 4;             /* a data offset aimed at the constants */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, CAPP_RODATA_ORIGIN, &r, 1, &BASES), -1);
  r.r_offset = 60;                               /* outside the window, still checked */
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 0, &r, 1, &BASES), 0);
  r.r_offset = 64;
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 0, &r, 1, &BASES), -1);
  r.r_offset = 0;
  CHECK_EQ(capprel_apply(buf, 2, 32, 64, 0, &r, 1, &BASES), -1);   /* window not aligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, NULL), -1);     /* no bases */
}

void test_capprel_leaves_pc_relative_alone(void) {
  static const CappBases B = { 0x1000, 0x2000, 0x3000 };
  uint8_t buf[16], before[16];
  CappRela r[3];
  int i;
  for (i = 0; i < 16; i++) buf[i] = before[i] = (uint8_t)(i * 13);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_SLOT0_OP;   r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_NONE;       r[1].r_addend = 0;
  r[2].r_offset = 8; r[2].r_info = R_XTENSA_ASM_EXPAND; r[2].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 0, r, 3, &B), 0);
  CHECK(memcmp(buf, before, 16) == 0);
}

void test_capprel_adds_the_right_base(void) {
  uint8_t buf[16];
  uint32_t vals[4] = { 0x40, CAPP_DATA_ORIGIN + 0x10, CAPP_RODATA_ORIGIN + 0x20,
                       CAPP_RODATA_ORIGIN - 4 };              /* the last data word */
  uint32_t v;
  CappRela r[4];
  int i;
  memcpy(buf, vals, sizeof vals);
  for (i = 0; i < 4; i++) { r[i].r_offset = (uint32_t)(i * 4); r[i].r_info = R_XTENSA_32; r[i].r_addend = 0; }
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 0, r, 4, &BASES), 0);
  memcpy(&v, buf, 4);      CHECK_EQ(v, CODE_BASE + 0x40);
  memcpy(&v, buf + 4, 4);  CHECK_EQ(v, DATA_BASE + 0x10);
  memcpy(&v, buf + 8, 4);  CHECK_EQ(v, RO_BASE + 0x20);
  memcpy(&v, buf + 12, 4); CHECK_EQ(v, DATA_BASE + (CAPP_RODATA_ORIGIN - 4 - CAPP_DATA_ORIGIN));
}

/* Relocations into the constants themselves: offsets carry 0x20000000. */
void test_capprel_relocates_inside_the_constants(void) {
  uint8_t buf[8];
  uint32_t a = CAPP_DATA_ORIGIN + 8, v;
  CappRela r;
  memset(buf, 0, sizeof buf);
  memcpy(buf + 4, &a, 4);
  r.r_offset = CAPP_RODATA_ORIGIN + 4; r.r_info = R_XTENSA_32; r.r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 8, 8, CAPP_RODATA_ORIGIN, &r, 1, &BASES), 0);
  memcpy(&v, buf + 4, 4);
  CHECK_EQ(v, DATA_BASE + 8);
}
```

- [ ] **Step 3: Run the suite.** Expected: compile errors in `test_capprel.c` (`CappBases`, `CAPP_RODATA_ORIGIN` unknown).

- [ ] **Step 4: `kernel/app/capprel.h`.** Replace everything from `/* Where capp.ld links each half.` to the end of the `capprel_apply` declaration with:

```c
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
```
Also change the file's top comment, "the flash cache relocates code through the 28 KB arena", to "the flash cache relocates code and constants through the 28 KB arena".

- [ ] **Step 5: `kernel/app/capprel.c`.** Replace the function:

```c
int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                  uint32_t origin, const CappRela *r, uint32_t n, const CappBases *b) {
  uint32_t j;
  if ((win_off & 3u) || (win_len & 3u) || !b) return -1;
  for (j = 0; j < n; j++) {
    uint32_t type = r[j].r_info & 0xFFu, off, v;
    if (type == R_XTENSA_NONE || type == R_XTENSA_SLOT0_OP || type == R_XTENSA_ASM_EXPAND)
      continue;               /* PC-relative, or nothing at all */
    if (type != R_XTENSA_32) return -1;
    /* r_offset is a link-time address, so it carries its section's origin.
     * Compared before subtracting: an r_offset just below the origin gave an
     * `off` near 2^32, and `off + 4` came round to something small that
     * passed. */
    if (r[j].r_offset < origin) return -1;
    off = r[j].r_offset - origin;
    if (off >= limit || limit - off < 4 || (off & 3u)) return -1;
    if (off < win_off || off - win_off >= win_len) continue;   /* another window's */
    /* memcpy, not a uint32_t store: the compiler may not narrow it into
     * accesses the instruction window refuses, and the host does not care. */
    memcpy(&v, win + (off - win_off), 4);
    if (v >= CAPP_RODATA_ORIGIN)    v = (v - CAPP_RODATA_ORIGIN) + b->rodata;
    else if (v >= CAPP_DATA_ORIGIN) v = (v - CAPP_DATA_ORIGIN) + b->data;
    else                            v = v + b->code;
    memcpy(win + (off - win_off), &v, 4);
  }
  return 0;
}
```

- [ ] **Step 6: Run the suite.** Expected: the seven `test_capprel_*` tests pass, with 0 failures overall. Then `elfload.c` no longer compiles on the device; the next step fixes it.

- [ ] **Step 7: `elfload.c`, enough to compile.** The constants are not placed until Task 5, so `rodata` is 0 here.
  - `relocate`: change the signature to `(int fd, const Elf32_Shdr *sh, int shnum, int sec, uint32_t origin, uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit, const CappBases *b)`, and the call inside it to `capprel_apply(win, win_off, win_len, limit, origin, buf, k, b)`.
  - `xip_place`: add `CappBases b;` to its declarations. After `code_base = xipflash_base() + off + XIP_HDR;` add `b.code = code_base; b.data = data_base; b.rodata = 0;`. The call becomes `rc = relocate(fd, sh, shnum, code_sec, 0, scratch, at, n, code_size, &b);`.
  - `capp_load_ex`, the RAM-code relocation. Replace the `rc = relocate(... code_w ...)` statement with:

```c
    {
      CappBases b;
      b.code = (uint32_t)(uintptr_t)code; b.data = data_base; b.rodata = 0;
      rc = relocate(fd, sh, eh.e_shnum, code_sec, 0, code_w, 0, code_size, code_size, &b);
    }
```
  - `capp_load_ex`, the data relocation after `data:`. Replace it with:

```c
  {
    CappBases b;
    b.code = code_base; b.data = data_base; b.rodata = 0;
    rc = relocate(fd, sh, eh.e_shnum, data_sec, CAPP_DATA_ORIGIN, data, 0, data_size, data_size, &b);
  }
```

- [ ] **Step 8: Build:** `python -m platformio run`. Expected: it builds with no new warnings. Do not flash (Global Constraints).

- [ ] **Step 9: Host suite** (0 failures), then commit:

```bash
git add kernel/app/capprel.h kernel/app/capprel.c kernel/app/elfload.c test/test_capprel.c test/test_main.c test/fixtures/capp_jar.capp test/fixtures/capp_forklift.capp test/fixtures/capp_cat.capp
git commit -m "Relocation: three parts, code, data and constants; .rela.rodata tested"
```

---

### Task 3: The cache entry holds the constants

**Files:**
- Modify: `kernel/app/xipcache.h`, `kernel/app/xipcache.c`, `test/test_xipcache.c` (append)

**Interfaces:**
- Produces:
  - `#define XIP_MAGIC 0x32504958u` ("XIP2")
  - `XipKey` = `{ path_hash, file_size, file_mtime; api, code_size; map_base, arena; rodata_size, drom_base; }` (nine `uint32_t`, 36 bytes)
  - `XipHdr` byte layout: `magic` 0, `seq` 4, `sectors` 8, `key` 12–47, `crc` 48, `path[24]` 52–75, `reserved[11]` 76–119, `live` 120, `commit` 124. `live` and `commit` stay where they were.
  - `uint32_t xip_ro_at(uint32_t code_size);` returns `(code_size + 15) & ~15`
  - `uint32_t xip_body(uint32_t code_size, uint32_t rodata_size);` returns `rodata_size ? xip_ro_at(code_size) + rodata_size : code_size`
  - `xip_begin(c, size, &off)`: `size` is the **body** (`xip_body(...)`); `xip_write(c, at, …)`: `at` is from the body's start
  - The entry's `crc` is `crc32` of the code, then chained over the constants. The padding is not covered.

- [ ] **Step 1: Write the failing tests, appended to `test/test_xipcache.c`**

```c
/* ---- constants beside the code (2026-10-10) ---- */

#define DROM 0x3C1B0000u
#define RO   12864u                  /* Jar's, linked 2026-10-10 */
#define CODE 24484u                  /* not 16-aligned: the constants start at 24496 */

static long install_ro(XipCache *c, const char *path, uint32_t code, uint32_t ro,
                       uint32_t mtime, uint8_t fill) {
  static uint8_t cb[96 * 1024], rb[32 * 1024];
  XipKey k = key(path, code, mtime);
  uint32_t off, crc;
  int r;
  k.rodata_size = ro;
  k.drom_base = DROM;
  memset(cb, fill, code);
  memset(rb, (uint8_t)(fill ^ 0xFF), ro);
  if ((r = xip_begin(c, xip_body(code, ro), &off)) != XIP_OK) return r;
  if ((r = xip_write(c, 0, cb, code)) != XIP_OK) { xip_abandon(c); return r; }
  if ((r = xip_write(c, xip_ro_at(code), rb, ro)) != XIP_OK) { xip_abandon(c); return r; }
  crc = xip_crc32(xip_crc32(0, cb, code), rb, ro);
  if ((r = xip_commit(c, &k, crc, path)) != XIP_OK) return r;
  return (long)off;
}
static int found_ro(XipCache *c, const char *path, uint32_t code, uint32_t ro,
                    uint32_t mtime, uint32_t drom) {
  XipKey k = key(path, code, mtime);
  uint32_t off;
  k.rodata_size = ro;
  k.drom_base = drom;
  return xip_find(c, &k, &off) == XIP_OK;
}

void test_xip_body_layout(void) {
  CHECK_EQ(xip_ro_at(CODE), 24496u);
  CHECK_EQ(xip_ro_at(24480), 24480u);
  CHECK_EQ(xip_body(CODE, 0), CODE);                 /* no constants: as before */
  CHECK_EQ(xip_body(CODE, RO), 24496u + RO);
}

void test_xip_entry_holds_code_and_constants(void) {
  XipCache c;
  uint32_t i;
  int pad_erased = 1;
  reset(&c);
  CHECK_EQ(install_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, 0x5A), 0);
  CHECK(found_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, DROM));
  CHECK_EQ(s_flash[XIP_HDR + CODE - 1], 0x5A);
  CHECK_EQ(s_flash[XIP_HDR + 24496], 0xA5);
  for (i = XIP_HDR + CODE; i < XIP_HDR + 24496; i++) if (s_flash[i] != 0xFF) pad_erased = 0;
  CHECK(pad_erased);
  /* 128 + 24496 + 12864 = 37488 bytes: ten sectors, so the next entry is at 10. */
  CHECK_EQ(install(&c, "/apps/todo.capp", 15044, 1, 0x11), 10 * (long)XIP_SECTOR);
  reboot(&c);
  CHECK(found_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, DROM));
  CHECK_EQ(s_bad_writes, 0);
}

void test_xip_constants_are_in_the_crc(void) {
  XipCache c;
  reset(&c);
  CHECK_EQ(install_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, 0x5A), 0);
  CHECK_EQ(xip_verify(&c, 0), XIP_OK);
  s_flash[XIP_HDR + 24496 + 1000] ^= 0x01;
  CHECK(!found_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, DROM));
  CHECK(xip_verify(&c, 0) != XIP_OK);
}

/* Relocated for one data view and one size of constants: anything else misses. */
void test_xip_key_names_the_data_view(void) {
  XipCache c;
  reset(&c);
  CHECK_EQ(install_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, 0x5A), 0);
  CHECK(!found_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, DROM + 0x10000u));
  CHECK(!found_ro(&c, "/apps/Games/jar.capp", CODE, RO - 4, 1, DROM));
  CHECK(!found_ro(&c, "/apps/Games/jar.capp", CODE, 0, 1, 0));
}

/* What the firmware before this left: "XIPC" entries, committed. They read
 * as free space -- the head starts at 0 and nothing matches -- and the next
 * write erases over them cleanly. No wipe needed. */
void test_xip_old_magic_reads_as_free(void) {
  XipCache c;
  XipHdr h;
  reset(&c);
  memset(&h, 0xFF, sizeof h);
  h.magic = 0x43504958u;
  h.commit = 0x43504958u;
  h.seq = 7;
  h.sectors = 7;
  h.key = key("/apps/Games/jar.capp", JAR, 1);
  memcpy(s_flash, &h, sizeof h);
  memcpy(s_flash + 7 * XIP_SECTOR, &h, sizeof h);
  reboot(&c);
  CHECK_EQ(c.head, 0);
  CHECK_EQ(c.next_seq, 1);
  CHECK(!found(&c, "/apps/Games/jar.capp", JAR, 1, NULL));
  CHECK_EQ(install_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, 0x5A), 0);
  CHECK(found_ro(&c, "/apps/Games/jar.capp", CODE, RO, 1, DROM));
  CHECK_EQ(s_bad_writes, 0);
}
```

- [ ] **Step 2: Run the suite.** Expected: compile errors (`xip_body`, `rodata_size`, `drom_base` unknown).

- [ ] **Step 3: `kernel/app/xipcache.h`.** Update the top comment's second sentence to: "An entry is a 128-byte header, an app's code and then its constants (16-aligned after the code), relocated for one (code map, data map, arena) triple, in whole sectors." Then:

```c
#define XIP_MAGIC     0x32504958u   /* "XIP2": code and constants. Entries the
                                     * firmware before wrote ("XIPC", code
                                     * only) read as free space. */
```

```c
/* What an entry is valid for. All of it must match. */
typedef struct {
  uint32_t path_hash, file_size, file_mtime;   /* the .capp, as appidx keys it */
  uint32_t api, code_size;
  uint32_t map_base, arena;                    /* what it was relocated against */
  uint32_t rodata_size, drom_base;             /* its constants, and the data view they are read through */
} XipKey;

typedef struct {                    /* on flash, XIP_HDR bytes */
  uint32_t magic, seq, sectors;
  XipKey   key;
  uint32_t crc;                     /* of the code, then the constants, as written */
  char     path[XIP_PATH_MAX];      /* the tail of the path, for listings */
  uint32_t reserved[11];            /* left erased */
  uint32_t live;                    /* 0xFFFFFFFF; 0 kills it, no erase needed */
  uint32_t commit;                  /* XIP_MAGIC, written last */
} XipHdr;
```
After `XipCache`, add:

```c
/* Where an entry's constants start, counted from its code: the next
 * 16-byte boundary. The padding is never written, so it stays erased. */
uint32_t xip_ro_at(uint32_t code_size);
/* An entry's body after the header: code, padding, constants -- or just
 * the code when there are none, which is exactly the old entry. */
uint32_t xip_body(uint32_t code_size, uint32_t rodata_size);
```
Change the `xip_begin` comment to "Room for a body of `size` bytes (xip_body) at the head…", and the `xip_write` comment to "Body bytes at `at` (from the end of the header) into the begun entry. The constants go at xip_ro_at(code_size)."

- [ ] **Step 4: `kernel/app/xipcache.c`.** Add after `xip_path_hash`:

```c
uint32_t xip_ro_at(uint32_t code_size) { return (code_size + 15u) & ~15u; }

uint32_t xip_body(uint32_t code_size, uint32_t rodata_size) {
  return rodata_size ? xip_ro_at(code_size) + rodata_size : code_size;
}
```
Replace `committed`:

```c
/* The sizes are bounded before they are added: a header read from flash
 * that was never ours can say anything. */
static int committed(const XipCache *c, const XipHdr *h, uint32_t off) {
  return h->magic == XIP_MAGIC && h->commit == XIP_MAGIC && h->sectors > 0 &&
         h->sectors <= (c->f.size - off) / XIP_SECTOR &&
         h->key.code_size <= c->f.size && h->key.rodata_size <= c->f.size &&
         XIP_HDR + xip_body(h->key.code_size, h->key.rodata_size) <= h->sectors * XIP_SECTOR;
}
```
Replace `code_crc` with:

```c
static int crc_range(XipCache *c, uint32_t at, uint32_t n, uint32_t *crc) {
  uint8_t buf[256];
  uint32_t k;
  while (n) {
    k = n < sizeof buf ? n : (uint32_t)sizeof buf;
    if (c->f.read(c->f.ctx, at, buf, k) != 0) return -1;
    *crc = xip_crc32(*crc, buf, k);
    at += k;
    n -= k;
  }
  return 0;
}

/* What an entry's crc covers: its code, then its constants, chained. */
static int body_crc(XipCache *c, uint32_t off, const XipKey *k, uint32_t *crc) {
  *crc = 0;
  if (crc_range(c, off + XIP_HDR, k->code_size, crc) != 0) return -1;
  if (k->rodata_size &&
      crc_range(c, off + XIP_HDR + xip_ro_at(k->code_size), k->rodata_size, crc) != 0)
    return -1;
  return 0;
}
```
In `xip_verify`, replace `code_crc(c, off, h.key.code_size, &crc)` with `body_crc(c, off, &h.key, &crc)`. In `match`, replace `code_crc(c, off, h->key.code_size, &crc)` with `body_crc(c, off, &h->key, &crc)`. In `xip_commit`, replace the first check with:

```c
  if (!c->pend_len || XIP_HDR + xip_body(k->code_size, k->rodata_size) > c->pend_len)
    return XIP_ERR_STATE;
```
In `xip_begin`, rename the parameter `code_size` to `size` (three uses). The arithmetic is unchanged.

- [ ] **Step 5: Run the suite.** Expected: all `test_xip_*` pass, including the five new ones, and `s_bad_writes` is 0 everywhere. The existing tests leave `rodata_size` 0, so their entries are byte for byte what they were, apart from the magic.

- [ ] **Step 6: Build** `python -m platformio run`. It must compile under `-Werror`. `elfload.c` still passes `code_size` to `xip_begin`, which is `xip_body(code_size, 0)`. Then commit:

```bash
git add kernel/app/xipcache.h kernel/app/xipcache.c test/test_xipcache.c test/test_main.c
git commit -m "Code cache: an entry holds the constants after the code; key names the data view; XIP2"
```

---

### Task 4: `appcode` through the data bus as well

**Files:**
- Modify: `kernel/app/xipflash.h`, `kernel/app/xipflash.c`, `src/main.c` (`xip_row`)

**Interfaces:**
- Produces: `uint32_t xipflash_drom_base(void);` returns the data-bus address of offset 0, or 0 when not ready. `xipflash_init()` maps both views or neither (Spec correction 7).

- [ ] **Step 1: `kernel/app/xipflash.h`.** After `xipflash_base`:

```c
uint32_t  xipflash_drom_base(void);   /* data-bus address of offset 0: the constants */
```
Change the top comment's first line to "The appcode partition, mapped once at boot into the instruction bus (code) and the data bus (constants), as the flash under xipcache.c."

- [ ] **Step 2: `kernel/app/xipflash.c`.** Add the statics:

```c
static esp_partition_mmap_handle_t  s_dmap;
static const void                  *s_drom;
```
After the INST `esp_partition_mmap` block, add:

```c
  /* And through the data bus, for the constants beside each app's code: the
   * instruction bus allows only aligned 32-bit loads, and a string is read
   * a byte at a time. IDF maps one flash range twice by sharing the pages
   * (ESP_MMU_MMAP_FLAG_PADDR_SHARED); on the S3 the two buses share one MMU
   * table anyway. Both views or neither: code in flash with its constants
   * in RAM would be code relocated against a heap address, which no cache
   * entry can be valid for. */
  if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA,
                         &s_drom, &s_dmap) != ESP_OK) {
    esp_partition_munmap(s_map);
    s_base = NULL;
    ESP_LOGE(TAG, "cannot map appcode for data: app code loads into RAM");
    return -1;
  }
```
In both later failure paths (no lock, `xip_open` failed), add `esp_partition_munmap(s_dmap); s_drom = NULL;` next to the existing `esp_partition_munmap(s_map);`. Change the success log to:

```c
  ESP_LOGI(TAG, "appcode %u KB mapped at %p (code) and %p (constants), head %u",
           (unsigned)(f.size / 1024), s_base, s_drom, (unsigned)s_cache.head);
```
Add after `xipflash_base`:

```c
uint32_t xipflash_drom_base(void) { return s_ready ? (uint32_t)(uintptr_t)s_drom : 0; }
```

- [ ] **Step 3: `src/main.c`, `xip_row`.** An entry relocated for another data view is stale too:

```c
  int stale = h->key.map_base != xipflash_base() || h->key.arena != (uint32_t)arena_addr() ||
              (h->key.rodata_size && h->key.drom_base != xipflash_drom_base()) ||
              h->key.api != CAPP_API_VERSION;
```
Add "or data view" to the comment's "relocated for another map or arena".

- [ ] **Step 4: Build** `python -m platformio run` (no flashing). Host suite: 0 failures. Commit:

```bash
git add kernel/app/xipflash.h kernel/app/xipflash.c src/main.c
git commit -m "appcode mapped through the data bus too, for constants; both views or none"
```

---
### Task 5: The loader places the constants

Constants go to flash beside the code on the XIP path. On every other path they go in RAM after the data block, the info-only load included. If the cache fails after the arena was claimed at the block's size, they go in a heap block of their own. `mem` shows how much of the arena the holder uses, and `api_pixels` bounces rows that are in flash.

**Files:**
- Modify: `kernel/app/elfload.h`, `kernel/app/elfload.c`, `kernel/app/arena.h`, `kernel/app/arena.c`, `test/test_arena.c` (append), `kernel/sys/memreport.c`, `kernel/app/cardapi.c` (`api_pixels`)

**Interfaces:**
- Consumes: `CappBases`, `CAPP_RODATA_ORIGIN` (Task 2); `xip_ro_at`, `xip_body`, `XipKey.rodata_size/drom_base` (Task 3); `xipflash_drom_base()` (Task 4).
- Produces:
  - `LoadedApp` gains `void *rodata; uint32_t rodata_size; void *rodata_heap;`
  - `uint32_t arena_used(void);` returns the size the holder claimed, or 0 when free
  - `static CappResult xip_place(int fd, const char *path, const Elf32_Shdr *sh, int shnum, int code_sec, uint32_t code_size, int ro_sec, uint32_t ro_size, uint8_t *scratch, CappBases *b, uint32_t *off_out);` fills `b->code` and `b->rodata` from the entry. `b->data` is input.
  - `static CappResult write_part(int fd, const Elf32_Shdr *sh, int shnum, int sec, uint32_t origin, uint32_t size, uint32_t at, uint8_t *scratch, const CappBases *b, uint32_t *crc);`

- [ ] **Step 1: Write the failing test, appended to `test/test_arena.c`**

```c
/* `mem` says how much of the arena the app on screen uses: the arena is
 * always 28 KB out of the heap, so the use is the only number that moves
 * when constants go to flash. */
void test_arena_used_is_what_the_holder_claimed(void) {
  void *a;
  CHECK_EQ(arena_used(), 0);
  a = arena_claim(15668, 4, "jar.capp");
  CHECK(a != NULL);
  CHECK_EQ(arena_used(), 15668);
  CHECK(arena_claim(100, 4, "todo.capp") == NULL);
  CHECK_EQ(arena_used(), 15668);
  arena_release(a);
  CHECK_EQ(arena_used(), 0);
}
```

- [ ] **Step 2: Run the suite.** Expected: an unresolved external for `arena_used`.

- [ ] **Step 3: `arena.h` and `arena.c`.** In `arena.h`, after `arena_holder`:

```c
uint32_t    arena_used(void);       /* what the holder claimed; 0 when free */
```
In `arena.c`, add `static uint32_t s_used;`. In `arena_claim`, after `s_held = 1;`, add `s_used = size;`. In `arena_release`, after `s_held = 0;`, add `s_used = 0;`. Add:

```c
uint32_t arena_used(void) { return s_held ? s_used : 0; }
```

- [ ] **Step 4: Run the suite.** Expected: `test_arena_used_is_what_the_holder_claimed` passes; 0 failures.

- [ ] **Step 5: `elfload.h`.** Change `data`'s comment to `/* the arena or the heap: .data, then the overlay region; constants too when they are in RAM */`. After `uint8_t data_in_arena;` add:

```c
  /* The constants (.rodata, linked at CAPP_RODATA_ORIGIN): in the flash
   * cache's data view beside the code when the code is there; otherwise in
   * RAM, after the data block, or in a block of their own (rodata_heap)
   * when the cache failed after the arena was claimed for the flash path.
   * See docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md. */
  void           *rodata;
  uint32_t        rodata_size;
  void           *rodata_heap;     /* freed by capp_unload; NULL when none */
```
Change `capp_load_info`'s comment's first sentence to: "Only what capp_info says -- name, icon, flags, commands -- for the icon scan: the data and the constants, relocated, and no code at all. The constants come too because capp_info's help and command tables are constants."

- [ ] **Step 6: `elfload.c`: `write_part`, `xip_bases`, `xip_place`.** Replace `xip_place` whole with:

```c
/* One section into the begun entry at body offset `at`, a window of the
 * arena at a time: read, relocate the window, fold it into the CRC, write.
 * The code and the constants are written the same way, which is the point
 * of capprel being windowed. */
static CappResult write_part(int fd, const Elf32_Shdr *sh, int shnum, int sec, uint32_t origin,
                             uint32_t size, uint32_t at, uint8_t *scratch,
                             const CappBases *b, uint32_t *crc) {
  XipCache *c = xipflash_cache();
  uint32_t done, n;
  CappResult rc;
  for (done = 0; done < size; done += n) {
    n = size - done < ARENA_SIZE ? size - done : ARENA_SIZE;
    if (read_at(fd, sh[sec].sh_offset + done, scratch, n) != 0) return CAPP_ERR_NOT_ELF;
    rc = relocate(fd, sh, shnum, sec, origin, scratch, done, n, size, b);
    if (rc != CAPP_OK) return rc;
    *crc = xip_crc32(*crc, scratch, n);
    if (xip_write(c, at + done, scratch, n) != XIP_OK) return CAPP_ERR_NO_MEMORY;
  }
  return CAPP_OK;
}

/* Where an entry's code and constants are, through the two views. */
static void xip_bases(uint32_t off, uint32_t code_size, uint32_t ro_size, CappBases *b) {
  b->code = xipflash_base() + off + XIP_HDR;
  b->rodata = ro_size ? xipflash_drom_base() + off + XIP_HDR + xip_ro_at(code_size) : 0;
}

/* The code and the constants into the flash cache: this file's entry if
 * there is one, else relocated a window at a time through `scratch` -- the
 * arena, which holds nothing yet; this is a launch that is only now
 * claiming it -- and written as a new entry, the constants 16-aligned after
 * the code. The whole point is that neither may fit in one piece of RAM, so
 * neither is asked to. On success the entry is referenced, and capp_unload
 * (or the load's own failure path) lets it go; b->code and b->rodata say
 * where they are.
 *
 * One lock span from the lookup to the reference: a forget from another
 * task must not program a word into the sectors this is writing, nor kill
 * or let the ring erase the entry between finding it and holding it. */
static CappResult xip_place(int fd, const char *path, const Elf32_Shdr *sh, int shnum,
                            int code_sec, uint32_t code_size, int ro_sec, uint32_t ro_size,
                            uint8_t *scratch, CappBases *b, uint32_t *off_out) {
  XipCache *c = xipflash_cache();
  FsStat st;
  XipKey k;
  uint32_t off, crc = 0;
  CappResult rc;

  if (!c || fs_stat(path, &st) != 0) return CAPP_ERR_OPEN;
  memset(&k, 0, sizeof k);
  k.path_hash = xipflash_path_hash(path);
  k.file_size = st.size;
  k.file_mtime = st.mtime;
  k.api = CAPP_API_VERSION;
  k.code_size = code_size;
  k.map_base = xipflash_base();
  k.arena = b->data;
  k.rodata_size = ro_size;
  k.drom_base = ro_size ? xipflash_drom_base() : 0;
  xipflash_lock();
  if (xip_find(c, &k, &off) == XIP_OK) goto found;

  if (s_on_prepare) s_on_prepare(path);
  if (xip_begin(c, xip_body(code_size, ro_size), &off) != XIP_OK) {
    xipflash_unlock();
    applogf("xip", "%s: no room in appcode, code to RAM", path);
    return CAPP_ERR_NO_MEMORY;
  }
  xip_bases(off, code_size, ro_size, b);
  rc = write_part(fd, sh, shnum, code_sec, 0, code_size, 0, scratch, b, &crc);
  if (rc == CAPP_OK && ro_size)
    rc = write_part(fd, sh, shnum, ro_sec, CAPP_RODATA_ORIGIN, ro_size, xip_ro_at(code_size),
                    scratch, b, &crc);
  if (rc != CAPP_OK) goto fail;
  /* Abandoned on failure too: a commit that fails leaves the write pending,
   * and the ring refuses every later begin until it is let go. */
  if (xip_commit(c, &k, crc, path) != XIP_OK) { rc = CAPP_ERR_NO_MEMORY; goto fail; }
  /* Read back before anything runs it. A word that did not program as
   * written would otherwise be found by the next launch's xip_find, after
   * this one had executed it. */
  if (xip_verify(c, off) != XIP_OK) {
    xip_kill(c, off);
    xipflash_unlock();
    applogf("xip", "%s: entry at %u did not read back, code to RAM", path, (unsigned)off);
    return CAPP_ERR_NO_MEMORY;
  }
  applogf("xip", "%s: %u code + %u constants to flash at %u", path, (unsigned)code_size,
          (unsigned)ro_size, (unsigned)off);
found:
  if (xip_ref(c, off) != XIP_OK) {
    xipflash_unlock();
    applogf("xip", "%s: cannot hold the entry at %u, code to RAM", path, (unsigned)off);
    return CAPP_ERR_NO_MEMORY;
  }
  xipflash_unlock();
  xip_bases(off, code_size, ro_size, b);
  *off_out = off;
  return CAPP_OK;
fail:
  xip_abandon(c);
  xipflash_unlock();
  applogf("xip", "%s: cache write failed (%s), code to RAM", path, capp_strerror(rc));
  return rc;
}
```

- [ ] **Step 7: `elfload.c`: `capp_load_ex` whole.** Replace the function with the version below. It keeps the header parse, truncation and symbol logic and changes only the classification, the placement and the third part.

```c
CappResult capp_load_ex(const char *path, LoadedApp *out, int foreground) {
  Elf32_Ehdr eh;
  Elf32_Shdr *sh = NULL;
  uint8_t *code = NULL, *code_w = NULL, *data = NULL, *ro = NULL, *ro_heap = NULL;
  uint32_t code_size = 0, data_size = 0, prog_size = 0, ro_size = 0, code_cap = 0;
  uint32_t ro_at, ram_size, claim;
  uint32_t main_off = 0, info_off = 0, xip_off = 0;
  CappBases b;
  uint16_t head[2];
  int code_sec = -1, data_sec = -1, ro_sec = -1;
  int in_arena = 0, in_flash = 0, try_flash;
  int fd, i, fsize;
  CappResult rc = CAPP_ERR_OPEN;

  memset(out, 0, sizeof *out);
  memset(&b, 0, sizeof b);
  out->xip_off = -1;

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return CAPP_ERR_OPEN;

  /* Measured up front so a short file is reported as short. A truncated app
   * otherwise fails wherever it happens to run out -- as "not a CardOS app",
   * which sends you looking at the toolchain instead of at the card. */
  fsize = fs_seek(fd, 0, FS_SEEK_END);

  if (read_at(fd, 0, &eh, sizeof eh) != 0) goto done;
  if (memcmp(eh.e_ident, "\x7F" "ELF", 4) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  if (eh.e_machine != EM_XTENSA) { rc = CAPP_ERR_WRONG_MACHINE; goto done; }
  if (eh.e_shnum == 0 || eh.e_shnum > 64) { rc = CAPP_ERR_NOT_ELF; goto done; }
  if (fsize > 0 &&
      (uint32_t)fsize < eh.e_shoff + (uint32_t)eh.e_shnum * sizeof(Elf32_Shdr)) {
    rc = CAPP_ERR_TRUNCATED;
    goto done;
  }

  sh = (Elf32_Shdr *)malloc((size_t)eh.e_shnum * sizeof *sh);
  if (!sh) { rc = CAPP_ERR_NO_MEMORY; goto done; }
  if (read_at(fd, eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof *sh) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }

  /* Classify the allocatable sections by where they were linked rather than
   * by name: the link address is what the relocations are expressed in.
   * Code below CAPP_DATA_ORIGIN. The data block from CAPP_DATA_ORIGIN to the
   * end of the last section below CAPP_RODATA_ORIGIN: .data first, read from
   * the file, and anything after it -- the overlay region -- zeroed. The
   * constants from CAPP_RODATA_ORIGIN. Sizes are bounded before they are
   * added, since a header can say anything. */
  for (i = 0; i < eh.e_shnum; i++) {
    uint32_t a = sh[i].sh_addr;
    if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
    if (sh[i].sh_type != SHT_PROGBITS && sh[i].sh_type != SHT_NOBITS) continue;
    if (sh[i].sh_size == 0) continue;
    if (sh[i].sh_size > CAPP_MAX_CODE) { rc = CAPP_ERR_TOO_BIG; goto done; }
    if (a >= CAPP_RODATA_ORIGIN) {
      if (ro_sec < 0 && a == CAPP_RODATA_ORIGIN && sh[i].sh_type == SHT_PROGBITS) {
        ro_sec = i;
        ro_size = sh[i].sh_size;
      }
    } else if (a >= CAPP_DATA_ORIGIN) {
      if (a - CAPP_DATA_ORIGIN > CAPP_MAX_DATA) { rc = CAPP_ERR_TOO_BIG; goto done; }
      if (a - CAPP_DATA_ORIGIN + sh[i].sh_size > data_size)
        data_size = a - CAPP_DATA_ORIGIN + sh[i].sh_size;
      if (data_sec < 0 && a == CAPP_DATA_ORIGIN) { data_sec = i; prog_size = sh[i].sh_size; }
    } else if (code_sec < 0) {
      code_sec = i;
      code_size = sh[i].sh_size;
    }
  }
  if (code_sec < 0) { rc = CAPP_ERR_NO_IMAGE; goto done; }
  if (fsize > 0 &&
      ((uint32_t)fsize < sh[code_sec].sh_offset + code_size ||
       (data_sec >= 0 && sh[data_sec].sh_type == SHT_PROGBITS &&
        (uint32_t)fsize < sh[data_sec].sh_offset + prog_size) ||
       (ro_sec >= 0 && (uint32_t)fsize < sh[ro_sec].sh_offset + ro_size))) {
    rc = CAPP_ERR_TRUNCATED;
    goto done;
  }
  if (code_size > CAPP_MAX_CODE || data_size > CAPP_MAX_DATA || ro_size > CAPP_MAX_DATA) {
    rc = CAPP_ERR_TOO_BIG;
    goto done;
  }

  /* The symbols before anything is placed: capp_info holds the flags. */
  rc = find_symbols(fd, &eh, sh, &main_off, &info_off);
  if (rc != CAPP_OK) goto done;
  if (main_off >= code_size || data_sec < 0 || sh[data_sec].sh_type != SHT_PROGBITS ||
      info_off < CAPP_DATA_ORIGIN ||
      info_off - CAPP_DATA_ORIGIN + sizeof(CappInfo) > prog_size) {
    rc = CAPP_ERR_NO_ENTRY;
    goto done;
  }
  /* Version and flags from the file, before anything is placed: the flags
   * decide where the code goes, and a first flash launch needs the arena
   * empty as scratch, so the data cannot be read first. */
  if (read_at(fd, sh[data_sec].sh_offset + (info_off - CAPP_DATA_ORIGIN), head, sizeof head) != 0) {
    rc = CAPP_ERR_NOT_ELF;
    goto done;
  }
  if (head[0] != CAPP_API_VERSION) { rc = CAPP_ERR_API; goto done; }

  /* Where the constants go when they are not in flash: after the data
   * block, 16-aligned, in the same allocation. */
  ro_at = (data_size + 15u) & ~15u;
  ram_size = ro_sec >= 0 ? ro_at + ro_size : data_size;

  /* The flash path, if it can be had: the app being started on screen, the
   * cache there, the app not asking for RAM code, alignments an entry can
   * keep. Then the constants go to flash with the code, and the arena holds
   * only the data block -- which is what this is all for. */
  try_flash = foreground == 1 && arena_code_in_flash(1, xipflash_ready(), head[1]) &&
              sh[code_sec].sh_addralign <= XIP_HDR &&
              (ro_sec < 0 || sh[ro_sec].sh_addralign <= 16);
  claim = try_flash ? data_size : ram_size;

  /* Data: the arena for the app being started on screen (and, inside this
   * call only, the icon scan), when it is free and the claim fits; the heap
   * otherwise, as before. */
  s_short_want = s_short_largest = 0;
  {
    const char *base = strrchr(path, '/');
    if (foreground &&
        (data = arena_claim(claim, sh[data_sec].sh_addralign, base ? base + 1 : path)) != NULL)
      in_arena = 1;
  }
  if (!in_arena) {
    /* Cached code is relocated for one data address, and only the arena
     * keeps one: off the arena there is no flash path. */
    try_flash = 0;
    claim = ram_size;
    data = heap_caps_malloc(claim, MALLOC_CAP_8BIT);
    if (!data) {
      /* The block that fails after hours of use: one piece of the 8-bit
       * heap, while the total says there is plenty. Said where it can be
       * read later, because nothing else records it. */
      s_short_want = claim;
      s_short_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      ESP_LOGE(TAG, "want %u bytes of data, largest block %u",
               (unsigned)claim, (unsigned)s_short_largest);
      applogf("load", "%s: data wants %u, free %u, largest %u", path, (unsigned)claim,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)s_short_largest);
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
  }
  b.data = (uint32_t)(uintptr_t)data;

  /* The descriptor only: no code, and the data's pointers into it aimed at
   * nowhere in particular, since none of them is followed. The constants
   * do come, after the block: capp_info's help and commands are constants. */
  if (foreground == LOAD_INFO) {
    if (ro_sec >= 0) ro = data + ro_at;
    b.rodata = (uint32_t)(uintptr_t)ro;
    goto data;
  }

  if (try_flash &&
      xip_place(fd, path, sh, eh.e_shnum, code_sec, code_size, ro_sec, ro_size, data, &b,
                &xip_off) == CAPP_OK) {
    in_flash = 1;
    code = (uint8_t *)(uintptr_t)b.code;
    ro = ro_sec >= 0 ? (uint8_t *)(uintptr_t)b.rodata : NULL;
  } else {
    /* The constants in RAM: after the block when it was allocated with room
     * for them, or a block of their own when the arena was claimed at the
     * block's size for a flash path that then failed. RAM code relocated
     * against the arena is as good as any, so the data stays put. */
    if (ro_sec >= 0) {
      if (claim >= ram_size) {
        ro = data + ro_at;
      } else if ((ro = ro_heap = heap_caps_malloc(ro_size, MALLOC_CAP_8BIT)) == NULL) {
        s_short_want = ro_size;
        s_short_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        applogf("load", "%s: constants want %u, largest %u", path, (unsigned)ro_size,
                (unsigned)s_short_largest);
        rc = CAPP_ERR_NO_MEMORY;
        goto done;
      }
    }
    b.rodata = (uint32_t)(uintptr_t)ro;
    code = code_alloc(code_size, &code_cap);
    if (!code) {
      s_short_want = code_size;
      s_short_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
      ESP_LOGE(TAG, "want %u bytes of exec RAM, %u free (largest block %u)",
               (unsigned)code_size, (unsigned)capp_exec_free(), (unsigned)s_short_largest);
      applogf("load", "%s: code wants %u, exec free %u, largest %u", path,
              (unsigned)code_size, (unsigned)capp_exec_free(), (unsigned)s_short_largest);
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
    /* Patched through its data-window alias, because the compiler is free
     * to implement the read-modify-write with narrower accesses than the
     * instruction window permits. */
    code_w = writable(code);
    if (read_at(fd, sh[code_sec].sh_offset, code_w, code_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
    b.code = (uint32_t)(uintptr_t)code;
    rc = relocate(fd, sh, eh.e_shnum, code_sec, 0, code_w, 0, code_size, code_size, &b);
    if (rc != CAPP_OK) goto done;
  }

data:
  /* Data last. The arena may hold scratch from a first launch, so the block
   * is cleared first; what is past .data -- the overlay region -- stays
   * zeros. */
  memset(data, 0, data_size);
  if (read_at(fd, sh[data_sec].sh_offset, data, prog_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  rc = relocate(fd, sh, eh.e_shnum, data_sec, CAPP_DATA_ORIGIN, data, 0, prog_size, prog_size, &b);
  if (rc != CAPP_OK) goto done;
  /* The constants, when they are in RAM, read and relocated like the data.
   * In flash they were written relocated. */
  if (ro_sec >= 0 && !in_flash) {
    if (read_at(fd, sh[ro_sec].sh_offset, ro, ro_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
    rc = relocate(fd, sh, eh.e_shnum, ro_sec, CAPP_RODATA_ORIGIN, ro, 0, ro_size, ro_size, &b);
    if (rc != CAPP_OK) goto done;
  }

  out->info = (const CappInfo *)(data + (info_off - CAPP_DATA_ORIGIN));
  out->main = code ? (int (*)(const CardApi *, int, char **))(code + main_off) : NULL;
  out->code = code;
  out->data = data;
  out->code_size = code_size;
  out->code_cap = code_cap;
  out->data_size = data_size;
  out->rodata = ro;
  out->rodata_size = ro_size;
  out->rodata_heap = ro_heap;
  out->xip_off = in_flash ? (int32_t)xip_off : -1;
  out->data_in_arena = (uint8_t)in_arena;
  rc = CAPP_OK;

done:
  fs_close(fd);
  free(sh);
  /* A failure gives back exactly what it took: the cache entry's reference
   * or the code block, the constants' own block, the arena or the heap
   * block. */
  if (rc != CAPP_OK) {
    if (in_flash) xip_release(xip_off);
    else code_free(code, code_cap);
    if (ro_heap) heap_caps_free(ro_heap);
    if (in_arena) arena_release(data);
    else if (data) heap_caps_free(data);
  }
  if (rc == CAPP_OK)
    ESP_LOGI(TAG, "loaded %s (%s): %u code (%s), %u data (%s), %u constants (%s), exec free %u",
             path, out->info->name, (unsigned)out->code_size,
             in_flash ? "flash" : code ? "RAM" : "not loaded",
             (unsigned)out->data_size, in_arena ? "arena" : "heap",
             (unsigned)ro_size, !ro_size ? "none" : in_flash ? "flash" : ro_heap ? "own block" : "RAM",
             (unsigned)capp_exec_free());
  else
    ESP_LOGW(TAG, "load %s failed: %s", path, capp_strerror(rc));
  return rc;
}
```

- [ ] **Step 8: `capp_unload`.** After the code line and before the data lines, add:

```c
  if (la->rodata_heap) heap_caps_free(la->rodata_heap);
```
The constants after the block go with the block, and constants in flash go with the entry's reference.

- [ ] **Step 9: `mem`.** In `kernel/sys/memreport.c`, replace the arena line with:

```c
  /* The arena is always 28 KB out of the heap; say who has it and how much
   * of it they use, because "heap free" alone no longer says what an open
   * app costs, and the use is what constants in flash and overlays shrink. */
  snprintf(b, sizeof b, "arena %5u/%5u %.13s", (unsigned)arena_used(),
           (unsigned)ARENA_SIZE, arena_held() ? arena_holder() : "free");
```

- [ ] **Step 10: `api_pixels` (`kernel/app/cardapi.c`).** Replace the function:

```c
/* Raw blit, clipped like everything else -- a program must not be able to draw
 * over another window's chrome.
 *
 * A row from the app's constants is in flash now (an icon or sprite table in
 * .rodata), and the panel's SPI DMA cannot read flash: left to the driver,
 * every row would allocate a bounce buffer of its own. So such a row is
 * copied into one RAM row first. display_blit is synchronous and apps run on
 * the shell's task only, so one static row is enough. */
static void api_pixels(CRect r, const uint16_t *px) {
  static uint16_t bounce[DISPLAY_W];
  Rect want = to_rect(r);
  Rect vis = rect_intersect(want, draw_clip());
  int16_t y;
  if (rect_is_empty(vis) || !px) return;
  for (y = vis.y; y < vis.y + vis.h; y++) {
    const uint16_t *row = px + (size_t)(y - want.y) * (size_t)want.w
                             + (size_t)(vis.x - want.x);
    if (!esp_ptr_internal(row) && vis.w <= DISPLAY_W) {
      memcpy(bounce, row, (size_t)vis.w * sizeof *row);
      row = bounce;
    }
    display_blit(vis.x, y, vis.w, 1, row);
  }
}
```
(`esp_memory_utils.h` is already included; `DISPLAY_W` comes through `kernel/ui/draw.h`.)

- [ ] **Step 11: Build.** `python tools/build_apps.py && python -m platformio run`. Expected: it builds with no new warnings. Host suite: 0 failures.

- [ ] **Step 12: Smoke test on one device.** Ask session cardos-78 for a device and wait for "free" (see Task 10 Step 1). Then, on that port (here `COM4`; use the one released):

```bash
python -m platformio run -t upload --upload-port COM4
export MSYS_NO_PATHCONV=1 CARDOS_PORT=COM4
python tools/cardctl.py sh "xip wipe"
python tools/cardctl.py put build/apps/jar.capp /apps/Games/jar.capp
python tools/cardctl.py put build/apps/todo.capp /apps/Plan/todo.capp
python tools/cardctl.py open jar
python tools/cardctl.py sh "mem" "log 5"
```
Expected:
  - `log` shows `xip …jar.capp: N code + 12864 constants to flash at 0`.
  - `mem` shows `arena 15668/28672 jar.capp` (±100).
  - `cardctl shot jar.png` shows the jar drawn correctly.
  - `key hex:e7` (fn-h, `KEY_HELP` in `kernel/drv/keyboard.h`) in Jar shows its help, which comes from `capp_info.help`, now a constant in flash.

Then `key quit`, `open jar` again: no new `xip` line, which is a hit. Then `open todo` and `sh "do todo list"`: answered. That exercises a headless load with constants in RAM after the block. The other apps on the card are API 45 and refuse to load until Task 10 puts them. That is expected. Hand the device back if cardos-78 asked for it.

- [ ] **Step 13: Commit**

```bash
git add kernel/app/elfload.h kernel/app/elfload.c kernel/app/arena.h kernel/app/arena.c test/test_arena.c test/test_main.c kernel/sys/memreport.c kernel/app/cardapi.c
git commit -m "Loader: constants to flash beside the code, to RAM everywhere else; mem shows arena use"
```
The body gives Jar's arena use before (28532) and after (from Step 12).

---

### Task 6: Overlays in the link, the build's checks, and `ovltest`

**Files:**
- Modify: `kernel/app/capp.h` (macro, constants, two `CardApi` fields), `apps/capp.ld` (`OVERLAY`), `tools/build_apps.py` (`check_overlays`), `tools/test_build_apps.py` (a class), `apps/folders.txt`, `test/fakeapi.c`, `test/test_capprel.c` (one fixture more)
- Create: `apps/ovltest.c`, `test/fixtures/capp_ovltest.capp`

**Interfaces:**
- Produces (C, `capp.h`):
  - `#define CAPP_OVL_MAX 4`, `#define CAPP_OVL_KEEP 0`, `#define CAPP_OVL_DISCARD 1`
  - `#define CAPP_OVERLAY(n)`, which puts a zero-initialised variable in section `.sbss.capp_ovl<n>` (GCC) and expands to nothing on the host compiler
  - At the end of `CardApi`, after `paint_direct`: `int (*overlay)(int n, int flags);` then `int (*overlay_live)(void);`
- Produces (Python): `check_overlays(secs, name)`, raising `SystemExit` (called from `check_layout`)
- Produces (linked layout): `.ovl0`–`.ovl3`, NOBITS, one VMA = `ALIGN(16)` after `.data`; empty ones dropped
- Note: `cardapi.c`'s positional initializer stays two entries short until Task 9, so the fields are NULL on the device. No app calls them before then, and `ovltest` only runs in Task 10.

- [ ] **Step 1: `kernel/app/capp.h`, the macro.** After the `CAPP_PAINT_DIRECT` block (before `/* What caps_ok() returns`), add:

```c
/* Data overlays (API 46): docs/superpowers/specs/2026-10-10-app-data-overlays-design.md.
 *
 * State that belongs to one mode of an app -- an editor's buffers, a
 * player's tracks, a shop -- can go in overlay 0..3. All of an app's
 * overlays share one region at the end of its data block, so they cost the
 * largest of them rather than their sum. One is live at a time;
 * api->overlay switches, keeping the outgoing one on the card. Overlay 0 is
 * live, and zeroed, when the app starts. Zero-initialised variables only:
 * no initialiser (the compiler refuses one).
 *
 *     CAPP_OVERLAY(1) static ShopState shop;
 *
 * THE RULE: a pointer into an overlay is good only while that overlay is
 * live. Nothing traps a stray access -- there is no MMU over RAM -- and
 * touching overlay 1 while 2 is live writes into 2. Reach each mode's state
 * only through that mode's functions. Headless commands run with overlay 0
 * live, so a command keeps to the common part or overlay 0.
 *
 * n is a literal 0..3, or a macro for one. The section is .sbss.capp_ovlN
 * because GCC makes any .sbss.* section NOBITS -- no zeros in the file --
 * and refuses a nonzero initialiser there; a .bss.* name would be swallowed
 * by the *(.bss .bss.*) in apps/capp.ld's .data before its OVERLAY saw it.
 * On the host compiler it is an ordinary static. */
#define CAPP_OVL_MAX     4
#define CAPP_OVL_KEEP    0     /* save the live overlay before switching */
#define CAPP_OVL_DISCARD 1     /* throw it away: next time it comes back zeroed */
#define CAPP_OVL_STR_(n) #n
#define CAPP_OVL_STR(n)  CAPP_OVL_STR_(n)
#if defined(__GNUC__)
#define CAPP_OVERLAY(n) __attribute__((section(".sbss.capp_ovl" CAPP_OVL_STR(n))))
#else
#define CAPP_OVERLAY(n)
#endif
```

- [ ] **Step 2: `capp.h`, the table.** After `void (*paint_direct)(int on);`, inside `CardApi`:

```c

  /* ---- data overlays (API 46) ----
   *
   * See CAPP_OVERLAY. overlay(n, flags) makes overlay n live. With
   * CAPP_OVL_KEEP the live one is written to the card first (/cache/ovl,
   * deleted when the app closes); with CAPP_OVL_DISCARD it is dropped and
   * comes back zeroed. A first visit is zeroed. Switching to the live one
   * does nothing and returns 0. Returns 0, or -1:
   *   - the save failed (no card, card full): nothing changed, the old
   *     overlay is still live;
   *   - the read failed: n is live and zeroed -- that mode is empty, and
   *     this is how the app hears it;
   *   - n is out of range, or the app has no overlays.
   * overlay_live() says which is live, -1 for an app without overlays. A
   * switch is a write and a read on the card, 50-150 ms for 20 KB: a mode
   * change, never a frame. */
  int (*overlay)(int n, int flags);
  int (*overlay_live)(void);
```

- [ ] **Step 3: `test/fakeapi.c`.** Next to `d_paint_direct`:

```c
static int d_overlay(int n, int flags) { (void)n; (void)flags; return -1; }   /* no overlays */
static int d_overlay_live(void) { return -1; }
```
and in `fakeapi_init`, after `a->paint_direct = d_paint_direct;`:

```c
  a->overlay = d_overlay;
  a->overlay_live = d_overlay_live;
```

- [ ] **Step 4: `apps/capp.ld`.** Between the `.data` block and `. = 0x20000000;`, insert:

```
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
```

- [ ] **Step 5: `apps/ovltest.c`**

```c
/* ovltest -- data overlays, checked on the device.
 *
 * Fills overlay 0, switches to 1 and finds it zeroed, fills it, switches
 * back and checks every byte of 0, switches again and checks 1, then
 * switches with DISCARD and finds 1 zeroed the next time. Says pass, with
 * the two switch times, or the step that failed.
 *
 * Also a fixture: tools/test_build_apps.py checks the build's overlay rules
 * on it, and test/test_capprel.c relocates its constants (STEP is a table
 * of pointers among the constants, so it has .rela.rodata).
 * docs/superpowers/specs/2026-10-10-app-data-overlays-design.md */

#include "kernel/app/capp.h"

#define BIG   20480     /* overlay 0: the size the spec times */
#define SMALL 6000      /* overlay 1: smaller, so a save at the wrong size shows */

static const CardApi *api;

CAPP_OVERLAY(0) static uint8_t big[BIG];
CAPP_OVERLAY(1) static uint8_t small[SMALL];

static const char *const STEP[] = {
  "start", "fill 0", "0 -> 1", "1 zeroed", "fill 1", "1 -> 0", "0 back",
  "0 -> 1 again", "1 back", "1 -> 0 discard", "0 still", "0 -> 1 after discard",
  "1 zeroed again", "out of range", "same overlay", "live",
};

static uint8_t pat(uint32_t i, int k) { return (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8)); }

static void fill(uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) p[i] = pat(i, k);
}

static int same(const uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i] != pat(i, k)) return 0;
  return 1;
}

static int zero(const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

static int fail(int step) {
  char line[64];
  api->fmt(line, sizeof line, "ovltest: FAIL at %s (live %d)", STEP[step], api->overlay_live());
  api->out_line(line);
  return 1;
}

/* One switch, timed; 0 when it worked. */
static int sw(int n, int flags, uint32_t *ms) {
  uint32_t t = api->ticks_ms();
  int r = api->overlay(n, flags);
  if (ms) *ms = api->ticks_ms() - t;
  return r;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_CLI,
  "ovltest",
  { 0 },
  NULL,
};

int capp_main(const CardApi *a, int argc, char **argv) {
  uint32_t t01 = 0, t10 = 0;
  char line[96];
  (void)argc; (void)argv;
  api = a;
  if (api->overlay_live() != 0) return fail(0);
  fill(big, BIG, 0);
  if (sw(1, CAPP_OVL_KEEP, &t01) != 0) return fail(2);
  if (!zero(small, SMALL)) return fail(3);
  fill(small, SMALL, 1);
  if (sw(0, CAPP_OVL_KEEP, &t10) != 0) return fail(5);
  if (!same(big, BIG, 0)) return fail(6);
  if (sw(1, CAPP_OVL_KEEP, NULL) != 0) return fail(7);
  if (!same(small, SMALL, 1)) return fail(8);
  if (sw(0, CAPP_OVL_DISCARD, NULL) != 0) return fail(9);
  if (!same(big, BIG, 0)) return fail(10);
  if (sw(1, CAPP_OVL_KEEP, NULL) != 0) return fail(11);
  if (!zero(small, SMALL)) return fail(12);
  if (api->overlay(4, CAPP_OVL_KEEP) != -1 || api->overlay(-1, CAPP_OVL_KEEP) != -1) return fail(13);
  if (api->overlay(1, CAPP_OVL_KEEP) != 0) return fail(14);
  if (api->overlay_live() != 1) return fail(15);
  api->fmt(line, sizeof line, "ovltest: pass; 0->1 %u ms, 1->0 %u ms (%u and %u bytes)",
           (unsigned)t01, (unsigned)t10, (unsigned)BIG, (unsigned)SMALL);
  api->out_line(line);
  return 0;
}
```
In `apps/folders.txt`, add after the `grep      -` line:

```
ovltest   -
```

- [ ] **Step 6: Write the failing tests, appended to `tools/test_build_apps.py`** (before `if __name__`)

```python
OVL = os.path.join(OUT, "ovltest.capp")

INIT = """#include "kernel/app/capp.h"
CAPP_OVERLAY(1) static int counter = 5;
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlinit" };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  return counter++;
}
"""

INFO = """#include "kernel/app/capp.h"
CAPP_OVERLAY(1) static char helpbuf[64];
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlinfo", { 0 }, helpbuf };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  helpbuf[0] = 'x';
  return helpbuf[1];
}
"""

FIVE = """#include "kernel/app/capp.h"
CAPP_OVERLAY(4) static char spare[64];
const CappInfo capp_info = { CAPP_API_VERSION, CAPP_CLI, "ovlfive" };
int capp_main(const CardApi *api, int argc, char **argv) {
  (void)api; (void)argc; (void)argv;
  spare[0] = 'x';
  return spare[1];
}
"""


@unittest.skipUnless(os.path.isfile(OVL), "run tools/build_apps.py first")
class Overlays(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fixture(self, name, text):
        src = os.path.join(self.tmp, name + ".c")
        with open(src, "w", encoding="utf-8") as f:
            f.write(text)
        return src

    def test_the_overlays_overlap(self):
        secs, _ = build_apps.sections(OVL)
        ovl = {s["name"]: s for s in secs if s["name"].startswith(".ovl")}
        self.assertEqual(sorted(ovl), [".ovl0", ".ovl1"])
        self.assertEqual(ovl[".ovl0"]["addr"], ovl[".ovl1"]["addr"])
        for s in ovl.values():
            self.assertEqual(s["type"], build_apps.SHT_NOBITS)
        data = [s for s in secs if s["name"] == ".data"][0]
        self.assertGreaterEqual(ovl[".ovl0"]["addr"], data["addr"] + data["size"])
        self.assertEqual(ovl[".ovl0"]["addr"] % 16, 0)

    def test_the_budget_counts_the_larger_overlay(self):
        lay = build_apps.layout(OVL)
        self.assertEqual(lay["ovl"], {0: 20480, 1: 6000})
        start = min(lay["ovl_addr"]) - build_apps.DATA_BASE
        self.assertEqual(lay["block"], start + 20480)
        self.assertLess(lay["block"], lay["data"] + 20480 + 6000)

    def test_an_initialised_overlay_variable_is_refused(self):
        with self.assertRaises(SystemExit):
            build_apps.compile_link(self.fixture("ovlinit", INIT), self.tmp)

    def test_capp_info_pointing_into_an_overlay_is_refused(self):
        elf = build_apps.compile_link(self.fixture("ovlinfo", INFO), self.tmp)
        with self.assertRaises(SystemExit):
            build_apps.check_layout(elf, "ovlinfo")

    def test_an_overlay_past_three_is_refused(self):
        elf = build_apps.compile_link(self.fixture("ovlfive", FIVE), self.tmp)
        with self.assertRaises(SystemExit):
            build_apps.check_layout(elf, "ovlfive")

    def test_an_overlay_with_bytes_in_the_file_is_refused(self):
        d = build_apps.DATA_BASE
        secs = [{"name": ".data", "type": build_apps.SHT_PROGBITS, "addr": d, "size": 0x40},
                {"name": ".ovl1", "type": build_apps.SHT_PROGBITS, "addr": d + 0x40, "size": 100}]
        with self.assertRaises(SystemExit):
            build_apps.check_overlays(secs, "fake")

    def test_overlays_at_two_addresses_are_refused(self):
        d = build_apps.DATA_BASE
        secs = [{"name": ".data", "type": build_apps.SHT_PROGBITS, "addr": d, "size": 0x40},
                {"name": ".ovl0", "type": build_apps.SHT_NOBITS, "addr": d + 0x40, "size": 100},
                {"name": ".ovl1", "type": build_apps.SHT_NOBITS, "addr": d + 0x50, "size": 100}]
        with self.assertRaises(SystemExit):
            build_apps.check_overlays(secs, "fake")
```

- [ ] **Step 7: Run.** `python tools/build_apps.py` (it builds `ovltest`), then `python tools/test_build_apps.py`. Expected: `AttributeError: … check_overlays` in the two synthetic tests. The others pass already: the compiler refuses `INIT`, `check_info` refuses `INFO`, and the section-name check refuses `FIVE`. Those three are regression tests for rules that already hold.

- [ ] **Step 8: `check_overlays`.** Add before `check_layout` in `tools/build_apps.py`:

```python
def check_overlays(secs, name):
    """The overlay sections as apps/capp.ld's OVERLAY lays them out: zeroed
    state only (NOBITS -- the compiler refuses an initialiser, this is the
    backstop), all at one 16-aligned address after .data, which is what lets
    them share the region."""
    ovl = [s for s in secs if re.match(r"\.ovl[0-9]$", s["name"]) and s["size"]]
    if not ovl:
        return
    for s in ovl:
        if s["type"] != SHT_NOBITS:
            raise SystemExit(
                "%s: %s holds initialised bytes; an overlay may hold only zeroed state "
                "(a CAPP_OVERLAY variable takes no initialiser)" % (name, s["name"]))
    data = [s for s in secs if s["name"] == ".data"]
    end = data[0]["addr"] + data[0]["size"] if data else DATA_BASE
    addrs = {s["addr"] for s in ovl}
    if len(addrs) != 1 or min(addrs) < end or min(addrs) % 16:
        raise SystemExit(
            "%s: overlays must all start at one 16-aligned address after .data; found %s"
            % (name, ", ".join("%s at 0x%08x" % (s["name"], s["addr"]) for s in ovl)))
```
In `check_layout`, after the `for s in alloc:` loop, add `check_overlays(secs, name)`.

- [ ] **Step 9: Run.** `python tools/build_apps.py`. Expected: 42 apps. `sizes.txt` has `ovltest` with `ovl0 20480  ovl1 6000` and `block` = (`.data` rounded to 16) + 20480, and no other app has an overlay column filled. Then `python tools/test_build_apps.py`: 11 tests OK. Then `python -m server.tests.test_catalog`: OK.

- [ ] **Step 10: The capprel fixture.** `cp build/apps/ovltest.capp test/fixtures/capp_ovltest.capp`. In `test/test_capprel.c`, change `FIXTURES` to:

```c
static const char *FIXTURES[] = { "capp_jar.capp", "capp_forklift.capp", "capp_cat.capp",
                                  "capp_ovltest.capp" };
```
`test_capprel_refs_land_in_the_right_part` needs no change: ovltest has data refs (into the overlay region, inside `c.block`) and constant refs (`STEP`), so it meets the non-cat expectations. Run the suite. Expected: 0 failures. Code refs to `big` and `small` land in `DATA_BASE + block`, which is why `load_capp` measures `block` and not `.data` alone.

- [ ] **Step 11: Build the firmware** (`python -m platformio run`, which must compile with the shorter `cardapi.c` initializer), then commit:

```bash
git add kernel/app/capp.h apps/capp.ld apps/ovltest.c apps/folders.txt tools/build_apps.py tools/test_build_apps.py test/fakeapi.c test/test_capprel.c test/fixtures/capp_ovltest.capp test/test_main.c
git commit -m "Overlays in the link: CAPP_OVERLAY, capp.ld OVERLAY, build checks, ovltest"
```

---

### Task 7: Switching overlays (`ovl.c`, portable)

**Files:**
- Create: `kernel/app/ovl.h`, `kernel/app/ovl.c`, `test/test_ovl.c`
- Modify: `host/CMakeLists.txt` (after the `xipcache.c` line), `src/CMakeLists.txt` (after `"../kernel/app/xipcache.c"`)

**Interfaces:**
- Consumes: `CAPP_OVL_MAX`, `CAPP_OVL_DISCARD` (Task 6).
- Produces:
  - `#define OVL_DIR "/cache/ovl"`, `#define OVL_PATH_MAX 40`
  - `typedef struct { int (*save)(void *ctx, const char *path, const void *buf, uint32_t n); int (*load)(void *ctx, const char *path, void *buf, uint32_t n); void (*remove)(void *ctx, const char *path); void *ctx; } OvlIo;` (`save` returns 0 or -1; `load` returns 0, or -1 when the file is missing, the wrong size or unreadable)
  - `typedef struct { uint8_t *region; uint32_t size[CAPP_OVL_MAX]; int n; int live; uint32_t load; unsigned saved; } Ovl;`
  - `void ovl_init(Ovl *o, uint8_t *region, const uint32_t size[CAPP_OVL_MAX], int n, uint32_t load);`
  - `int ovl_switch(Ovl *o, const OvlIo *io, int n, int flags);` (semantics as `CardApi.overlay`)
  - `int ovl_live(const Ovl *o);`
  - `void ovl_drop(Ovl *o, const OvlIo *io);`
  - `void ovl_path(char *out, size_t cap, uint32_t load, int n);` gives `"/cache/ovl/<load>-<n>.bin"`

- [ ] **Step 1: Write `kernel/app/ovl.h`.** The tests need the types.

```c
/* App data overlays: which one is live, and where the others are kept.
 * Portable so the host suite can reach it; capprun.c binds it to the card
 * and to the app running. See
 * docs/superpowers/specs/2026-10-10-app-data-overlays-design.md.
 *
 * An overlay is zeroed state only, so there is no starting image to load:
 * what is kept is what the app left there, and a first visit is zeros.
 * `saved` remembers which overlays have a file, so a first visit and a
 * DISCARD touch the card not at all -- which is also why DISCARD works
 * with no card. */
#ifndef CARDOS_OVL_H
#define CARDOS_OVL_H

#include <stddef.h>
#include <stdint.h>
#include "kernel/app/capp.h"

#define OVL_DIR      "/cache/ovl"
#define OVL_PATH_MAX 40

typedef struct {
  /* n bytes to path, replacing it: 0, or -1 (no card, card full). */
  int  (*save)(void *ctx, const char *path, const void *buf, uint32_t n);
  /* exactly n bytes from path: 0, or -1 (missing, another size, unreadable). */
  int  (*load)(void *ctx, const char *path, void *buf, uint32_t n);
  void (*remove)(void *ctx, const char *path);
  void *ctx;
} OvlIo;

typedef struct {
  uint8_t  *region;                 /* every overlay lives here while live */
  uint32_t  size[CAPP_OVL_MAX];     /* each one's own size; 0 if not declared */
  int       n;                      /* highest declared + 1; 0 for none */
  int       live;                   /* -1 for none */
  uint32_t  load;                   /* this load's id, in the file names */
  unsigned  saved;                  /* bit k: overlay k has a file */
} Ovl;

/* Overlay 0 live -- the loader zeroed the block, so it is zeroed -- or none
 * when n is 0. */
void ovl_init(Ovl *o, uint8_t *region, const uint32_t size[CAPP_OVL_MAX], int n, uint32_t load);
/* See CardApi.overlay: 0; -1 with nothing changed when the save failed;
 * -1 with n live and zeroed when the read failed; -1 for a bad n. */
int  ovl_switch(Ovl *o, const OvlIo *io, int n, int flags);
int  ovl_live(const Ovl *o);
/* The app is going: its files go too, and nothing is live. */
void ovl_drop(Ovl *o, const OvlIo *io);
/* "/cache/ovl/<load>-<n>.bin": the dash keeps load 1, overlay 12 apart from
 * load 11, overlay 2. */
void ovl_path(char *out, size_t cap, uint32_t load, int n);

#endif /* CARDOS_OVL_H */
```

- [ ] **Step 2: Write the failing tests, `test/test_ovl.c`**

```c
/* Switching data overlays, over files held in memory: keep and discard, a
 * first visit zeroed, a switch back exact, saves at the outgoing overlay's
 * own size, failures that change nothing or say so, and files that go with
 * the app and never collide between two loads. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/ovl.h"

#define NFILES 8
typedef struct { int used; char path[OVL_PATH_MAX]; uint32_t n; uint8_t data[8192]; } MemFile;
static MemFile s_files[NFILES];
static int s_save_fail, s_load_fail, s_saves, s_loads;

static MemFile *find(const char *path) {
  int i;
  for (i = 0; i < NFILES; i++) if (s_files[i].used && !strcmp(s_files[i].path, path)) return &s_files[i];
  return NULL;
}
static int m_save(void *ctx, const char *path, const void *buf, uint32_t n) {
  MemFile *f = find(path);
  int i;
  (void)ctx;
  if (s_save_fail || n > sizeof f->data) return -1;
  for (i = 0; !f && i < NFILES; i++) if (!s_files[i].used) f = &s_files[i];
  if (!f) return -1;
  f->used = 1;
  snprintf(f->path, sizeof f->path, "%s", path);
  memcpy(f->data, buf, n);
  f->n = n;
  s_saves++;
  return 0;
}
static int m_load(void *ctx, const char *path, void *buf, uint32_t n) {
  MemFile *f = find(path);
  (void)ctx;
  if (s_load_fail || !f || f->n != n) return -1;
  memcpy(buf, f->data, n);
  s_loads++;
  return 0;
}
static void m_remove(void *ctx, const char *path) {
  MemFile *f = find(path);
  (void)ctx;
  if (f) f->used = 0;
}
static const OvlIo IO = { m_save, m_load, m_remove, NULL };

static int nfiles(void) {
  int i, k = 0;
  for (i = 0; i < NFILES; i++) k += s_files[i].used;
  return k;
}
static uint32_t file_size(uint32_t load, int n) {
  char p[OVL_PATH_MAX];
  MemFile *f;
  ovl_path(p, sizeof p, load, n);
  f = find(p);
  return f ? f->n : 0;
}

static uint8_t s_region[4096], s_region2[4096];
static const uint32_t SIZES[CAPP_OVL_MAX] = { 4096, 1000, 0, 0 };

static void fresh(Ovl *o, uint8_t *region, uint32_t load) {
  memset(region, 0, 4096);
  ovl_init(o, region, SIZES, 2, load);
}
static void reset(void) {
  memset(s_files, 0, sizeof s_files);
  s_save_fail = s_load_fail = s_saves = s_loads = 0;
}
static void fill(uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) p[i] = (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8));
}
static int same(const uint8_t *p, uint32_t n, int k) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i] != (uint8_t)(i * 7u + (uint32_t)k * 13u + (i >> 8))) return 0;
  return 1;
}
static int zero(const uint8_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

void test_ovl_starts_with_0_live_and_none_without_overlays(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_live(&o), 0);
  ovl_init(&o, s_region, SIZES, 0, 2);
  CHECK_EQ(ovl_live(&o), -1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), -1);
  CHECK_EQ(nfiles(), 0);
}

void test_ovl_a_first_visit_is_zeroed(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  memset(s_region, 0xAA, sizeof s_region);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_live(&o), 1);
  CHECK(zero(s_region, 1000));
  CHECK_EQ(s_loads, 0);                          /* nothing to read: no card asked */
}

void test_ovl_a_switch_back_restores_exactly(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 1000, 1));
}

void test_ovl_saves_at_its_own_size(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(file_size(1, 0), 4096);
  CHECK_EQ(file_size(1, 1), 1000);
}

void test_ovl_discard_drops_the_live_one(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);      /* 1 has a file now */
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region, 1000, 1));
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(file_size(1, 1), 0);                            /* its file is gone */
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK(zero(s_region, 1000));
}

void test_ovl_discard_needs_no_card(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  s_save_fail = 1;
  s_load_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(ovl_live(&o), 1);
}

void test_ovl_a_write_failure_leaves_the_live_one_untouched(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  s_save_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(nfiles(), 0);
}

void test_ovl_a_read_failure_zeroes_and_reports(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  fill(s_region, 1000, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  fill(s_region, 4096, 0);
  s_load_fail = 1;
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 1);
  CHECK(zero(s_region, 1000));
}

void test_ovl_switching_to_the_live_one_is_a_noop(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  fill(s_region, 4096, 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_DISCARD), 0);
  CHECK_EQ(s_saves, 0);
  CHECK(same(s_region, 4096, 0));
  CHECK_EQ(ovl_switch(&o, &IO, 2, CAPP_OVL_KEEP), -1);     /* past the declared ones */
  CHECK_EQ(ovl_switch(&o, &IO, -1, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_switch(&o, &IO, CAPP_OVL_MAX, CAPP_OVL_KEEP), -1);
  CHECK_EQ(ovl_live(&o), 0);
}

void test_ovl_drop_deletes_the_files(void) {
  Ovl o;
  reset();
  fresh(&o, s_region, 1);
  CHECK_EQ(ovl_switch(&o, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&o, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK_EQ(nfiles(), 2);
  ovl_drop(&o, &IO);
  CHECK_EQ(nfiles(), 0);
  CHECK_EQ(ovl_live(&o), -1);
}

/* The same app on screen and headless at once: two loads, two sets of files. */
void test_ovl_two_loads_never_share_files(void) {
  Ovl a, b;
  char p1[OVL_PATH_MAX], p2[OVL_PATH_MAX];
  reset();
  fresh(&a, s_region, 1);
  fresh(&b, s_region2, 12);
  fill(s_region, 4096, 3);
  fill(s_region2, 4096, 4);
  CHECK_EQ(ovl_switch(&a, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(ovl_switch(&b, &IO, 1, CAPP_OVL_KEEP), 0);
  CHECK_EQ(nfiles(), 2);
  ovl_drop(&a, &IO);
  CHECK_EQ(nfiles(), 1);
  CHECK_EQ(ovl_switch(&b, &IO, 0, CAPP_OVL_KEEP), 0);
  CHECK(same(s_region2, 4096, 4));
  ovl_path(p1, sizeof p1, 1, 1);
  ovl_path(p2, sizeof p2, 11, 1);
  CHECK(strcmp(p1, p2) != 0);
  CHECK(strcmp(p1, "/cache/ovl/1-1.bin") == 0);
}
```

- [ ] **Step 3: Add `${ROOT}/kernel/app/ovl.c` to `host/CMakeLists.txt`** after the `xipcache.c` line, with the comment `# data overlays; capprun.c binds them to the card`. Run the suite. Expected: unresolved externals for `ovl_*`. Create an empty `kernel/app/ovl.c` if CMake refuses a missing file.

- [ ] **Step 4: Write `kernel/app/ovl.c`**

```c
/* App data overlays. See ovl.h. */

#include "kernel/app/ovl.h"

#include <stdio.h>
#include <string.h>

void ovl_path(char *out, size_t cap, uint32_t load, int n) {
  snprintf(out, cap, OVL_DIR "/%lu-%d.bin", (unsigned long)load, n);
}

void ovl_init(Ovl *o, uint8_t *region, const uint32_t size[CAPP_OVL_MAX], int n, uint32_t load) {
  int k;
  memset(o, 0, sizeof *o);
  o->region = region;
  o->n = n < 0 ? 0 : n > CAPP_OVL_MAX ? CAPP_OVL_MAX : n;
  for (k = 0; k < o->n; k++) o->size[k] = size ? size[k] : 0;
  o->live = o->n > 0 ? 0 : -1;
  o->load = load;
}

int ovl_live(const Ovl *o) { return o ? o->live : -1; }

int ovl_switch(Ovl *o, const OvlIo *io, int n, int flags) {
  char path[OVL_PATH_MAX];
  unsigned bit;
  if (!o || !io || o->n == 0 || n < 0 || n >= o->n) return -1;
  if (n == o->live) return 0;
  /* The outgoing one first, at its own size, not the region's. If it cannot
   * be kept, nothing has changed, and the app is told. */
  if (o->live >= 0) {
    bit = 1u << o->live;
    ovl_path(path, sizeof path, o->load, o->live);
    if (flags & CAPP_OVL_DISCARD) {
      if (o->saved & bit) io->remove(io->ctx, path);
      o->saved &= ~bit;
    } else if (o->size[o->live]) {
      if (io->save(io->ctx, path, o->region, o->size[o->live]) != 0) return -1;
      o->saved |= bit;
    }
  }
  /* Then the incoming one: back from its file, or zeros on a first visit.
   * A file that will not read is zeros as well, and -1: the mode is empty,
   * and the app hears so rather than finding out. */
  o->live = n;
  bit = 1u << n;
  if (o->size[n] == 0) return 0;
  if (o->saved & bit) {
    ovl_path(path, sizeof path, o->load, n);
    if (io->load(io->ctx, path, o->region, o->size[n]) == 0) return 0;
    memset(o->region, 0, o->size[n]);
    return -1;
  }
  memset(o->region, 0, o->size[n]);
  return 0;
}

void ovl_drop(Ovl *o, const OvlIo *io) {
  char path[OVL_PATH_MAX];
  int k;
  if (!o) return;
  for (k = 0; k < CAPP_OVL_MAX; k++) {
    if (!(o->saved & (1u << k))) continue;
    ovl_path(path, sizeof path, o->load, k);
    if (io) io->remove(io->ctx, path);
  }
  o->saved = 0;
  o->live = -1;
}
```

- [ ] **Step 5: Run the suite.** Expected: the eleven `test_ovl_*` tests pass; 0 failures.

- [ ] **Step 6: Add `"../kernel/app/ovl.c"` to `src/CMakeLists.txt`** after `"../kernel/app/xipcache.c"`. Build with `python -m platformio run`. Commit:

```bash
git add kernel/app/ovl.h kernel/app/ovl.c test/test_ovl.c test/test_main.c host/CMakeLists.txt src/CMakeLists.txt
git commit -m "Overlays: switching, portable and host-tested over in-memory files"
```

---

### Task 8: The loader records the overlay region

**Files:**
- Modify: `kernel/app/elfload.h`, `kernel/app/elfload.c`

**Interfaces:**
- Consumes: `CAPP_OVL_MAX` (Task 6); the linked `.ovlN` sections (Task 6).
- Produces: `LoadedApp` gains `uint32_t ovl_off; uint32_t ovl_size[CAPP_OVL_MAX]; uint8_t ovl_n;`, filled on every load except the info-only one. `ovl_n` is the highest overlay number + 1, and 0 for none. The region is `(uint8_t *)data + ovl_off`.

- [ ] **Step 1: `elfload.h`.** After `rodata_heap`:

```c
  /* The overlay region (docs/superpowers/specs/2026-10-10-app-data-overlays-design.md):
   * at data + ovl_off, inside the block and zeroed by the load; overlay n is
   * its first ovl_size[n] bytes while live. ovl_n is the highest overlay + 1,
   * 0 for none. Not filled by capp_load_info, which never runs the app. */
  uint32_t        ovl_off;
  uint32_t        ovl_size[CAPP_OVL_MAX];
  uint8_t         ovl_n;
```

- [ ] **Step 2: `elfload.c`, `find_overlays`.** Add above `capp_load_ex`:

```c
/* The overlay sections -- .ovl0 to .ovl3, NOBITS, all at one address after
 * .data (apps/capp.ld's OVERLAY) -- found by name, because the linker drops
 * an empty one and the section index says nothing about which overlay it
 * is. A layout the build would have refused is not loaded either. */
static CappResult find_overlays(int fd, const Elf32_Ehdr *eh, const Elf32_Shdr *sh,
                                uint32_t prog_size, LoadedApp *out) {
  uint32_t names, region = 0;
  char name[8];
  int i, k;
  if (eh->e_shstrndx >= eh->e_shnum) return CAPP_OK;      /* no names, no overlays */
  names = sh[eh->e_shstrndx].sh_offset;
  for (i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_NOBITS || !(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
    if (sh[i].sh_addr < CAPP_DATA_ORIGIN || sh[i].sh_addr >= CAPP_RODATA_ORIGIN) continue;
    memset(name, 0, sizeof name);
    if (read_at(fd, names + sh[i].sh_name, name, 6) != 0) return CAPP_ERR_NOT_ELF;
    if (memcmp(name, ".ovl", 4) != 0 || name[4] < '0' || name[4] >= '0' + CAPP_OVL_MAX ||
        name[5] != 0)
      continue;
    if (region && sh[i].sh_addr != region) return CAPP_ERR_NOT_ELF;   /* one region */
    region = sh[i].sh_addr;
    k = name[4] - '0';
    out->ovl_size[k] = sh[i].sh_size;
    if (k + 1 > out->ovl_n) out->ovl_n = (uint8_t)(k + 1);
  }
  if (region && region - CAPP_DATA_ORIGIN < prog_size) return CAPP_ERR_NOT_ELF;   /* over .data */
  out->ovl_off = region ? region - CAPP_DATA_ORIGIN : 0;
  return CAPP_OK;
}
```

- [ ] **Step 3: Call it.** In `capp_load_ex`, right after the `if (head[0] != CAPP_API_VERSION)` line, add:

```c
  /* The overlay region, for capprun to switch in; the icon scan never runs
   * the app, so it has no use for it. */
  if (foreground != LOAD_INFO && (rc = find_overlays(fd, &eh, sh, prog_size, out)) != CAPP_OK)
    goto done;
```
`out` was zeroed at the top, so an app without overlays leaves `ovl_n` 0.

- [ ] **Step 4: Build** (`python -m platformio run`) and run the host suite (0 failures). Commit:

```bash
git add kernel/app/elfload.h kernel/app/elfload.c
git commit -m "Loader: records the overlay region; an app without overlays loads as before"
```

---

### Task 9: API 46: the overlays, wired

**Files:**
- Modify: `kernel/app/capprun.h`, `kernel/app/capprun.c`, `kernel/app/cardapi.c`, `src/main.c`

**Interfaces:**
- Consumes: `Ovl`, `OvlIo`, `ovl_*`, `OVL_DIR` (Task 7); `LoadedApp.ovl_*` (Task 8).
- Produces:
  - `int capprun_overlay(int n, int flags);` (`api->overlay`)
  - `int capprun_overlay_live(void);` (`api->overlay_live`)
  - `void capprun_clear_overlays(void);` (boot)
  - `Run` gains `Ovl ovl;`; `static uint32_t s_load_seq;`; `static void run_loaded(Run *s);`

- [ ] **Step 1: `capprun.h`.** After `capprun_set_paint_direct`:

```c
/* api->overlay and api->overlay_live (API 46), for the app executing now;
 * kernel/app/ovl.c does the switching, this binds it to the card. */
int  capprun_overlay(int n, int flags);
int  capprun_overlay_live(void);
/* Empty /cache/ovl: once at boot, after the card mounts. A power cut can
 * strand files, and no load from before it can be running. */
void capprun_clear_overlays(void);
```

- [ ] **Step 2: `capprun.c`, the state.** Add `#include "kernel/app/ovl.h"`. At the end of `struct Run`, after `AppDef def;`:

```c

  /* Its overlays: which is live, and the card files the others are kept in
   * (kernel/app/ovl.c). A new load id on every load names those files, so an
   * instance on screen and a headless one never share them. */
  Ovl       ovl;
```
After `static Run *s_active;` and its comment, add:

```c
/* The id of the latest load, for overlay file names. From 1 after each
 * boot; boot empties /cache/ovl, so an id from before cannot collide. */
static uint32_t s_load_seq;
```

- [ ] **Step 3: `capprun.c`, the card glue.** Add after `to_crect`:

```c
/* The card under ovl.c. A save replaces the file whole, with no .tmp dance
 * (apps/safefile.h): a file is read back only for an overlay that is not
 * live, and that file was last written by a save that succeeded -- a torn
 * write is a failed switch, the overlay stays live, and its next save
 * replaces the file. */
static int ovl_fs_save(void *ctx, const char *path, const void *buf, uint32_t n) {
  int fd, ok;
  (void)ctx;
  fs_mkdir(OVL_DIR);                        /* already there is an error, and harmless */
  fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return -1;
  ok = fs_write(fd, buf, n) == (int)n;
  fs_close(fd);
  if (!ok) fs_remove(path);
  return ok ? 0 : -1;
}

static int ovl_fs_load(void *ctx, const char *path, void *buf, uint32_t n) {
  FsStat st;
  int fd, ok;
  (void)ctx;
  if (fs_stat(path, &st) != 0 || st.is_dir || st.size != n) return -1;
  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return -1;
  ok = fs_read(fd, buf, n) == (int)n;
  fs_close(fd);
  return ok ? 0 : -1;
}

static void ovl_fs_remove(void *ctx, const char *path) {
  (void)ctx;
  fs_remove(path);
}

static const OvlIo OVL_FS = { ovl_fs_save, ovl_fs_load, ovl_fs_remove, NULL };

/* An image just loaded into a run: a new load id, overlay 0 live. */
static void run_loaded(Run *s) {
  s->loaded = 1;
  ovl_init(&s->ovl, (uint8_t *)s->la.data + s->la.ovl_off, s->la.ovl_size, s->la.ovl_n,
           ++s_load_seq);
}
```

- [ ] **Step 4: `capprun.c`, where images come and go.**
  - In `load_entry`, replace `r->la = la;` / `r->loaded = 1;` with `r->la = la;` / `run_loaded(r);`.
  - In `ensure_loaded`, replace the final `s->loaded = 1;` with `run_loaded(s);`.
  - In `release_image`, replace `if (s->loaded) capp_unload(&s->la);` with:

```c
  if (s->loaded) {
    ovl_drop(&s->ovl, &OVL_FS);      /* this load's overlay files, while we know them */
    capp_unload(&s->la);
  }
```

- [ ] **Step 5: `capprun.c`, the calls.** After `capprun_set_paint_direct`:

```c
/* api->overlay. One log line for a switch that failed, none for one that
 * worked: the card's log is for what went wrong, not every mode change. */
int capprun_overlay(int n, int flags) {
  Run *s = s_running ? s_running : s_active;
  int r;
  if (!s || !s->loaded) return -1;
  r = ovl_switch(&s->ovl, &OVL_FS, n, flags);
  if (r != 0 && n >= 0 && n < s->ovl.n)
    applogf("ovl", "%s: overlay %d (%s) failed, %d live", s->name, n,
            (flags & CAPP_OVL_DISCARD) ? "discard" : "keep", ovl_live(&s->ovl));
  return r;
}

int capprun_overlay_live(void) {
  Run *s = s_running ? s_running : s_active;
  return s && s->loaded ? ovl_live(&s->ovl) : -1;
}

/* One file per listing: FAT is not asked to go on listing a directory it is
 * deleting from. A file that will not go ends it, rather than looping. */
void capprun_clear_overlays(void) {
  FsDir d;
  FsEntry e;
  char path[FS_PATH_MAX];
  int k, found;
  for (k = 0; k < 256; k++) {
    found = 0;
    if (fs_opendir(OVL_DIR, &d) != 0) return;
    while (fs_readdir(&d, &e) == 1) if (!e.is_dir) { found = 1; break; }
    fs_closedir(&d);
    if (!found) return;
    snprintf(path, sizeof path, "%s/%s", OVL_DIR, e.name);
    if (fs_remove(path) != 0) return;
  }
}
```

- [ ] **Step 6: `cardapi.c`.** After `capprun_set_paint_direct,` in `API`:

```c
  capprun_overlay,
  capprun_overlay_live,
```

- [ ] **Step 7: `src/main.c`.** After `capprun_watch_apps();        /* before anything below can touch /apps */`:

```c
    capprun_clear_overlays();    /* a power cut can strand overlay files */
```

- [ ] **Step 8: Build:** `python tools/build_apps.py && python -m platformio run`. Expected: it builds with no new warnings. Host suite: 0 failures (nothing host-side changed). Commit:

```bash
git add kernel/app/capprun.h kernel/app/capprun.c kernel/app/cardapi.c src/main.c
git commit -m "API 46: overlay and overlay_live, per-load files on the card, emptied at boot"
```

---
### Task 10: On both devices: flash, put the apps, verify, measure, document

Both devices belong to session cardos-78 until it releases them. COM3 is the original Cardputer, which cardos-78 is full-flashing with master. COM4 is the ADV (v0.15.0, with `appcode`). This firmware uses the same partition table, so a **plain upload** is enough and NVS survives. The apps are not fetched with `update apps`, because the store still serves API-45 builds until Task 11 lands. They are put on each card from this build with `cardctl`. The four EMBED apps (dashlink, files, edit, update) are rewritten by the firmware itself at first boot, because `CAPP_BLOB_STAMP` changed (`seed_capps` in `kernel/ui/icons.c`).

**Files:**
- Modify: `CLAUDE.md`, `docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md`, `docs/superpowers/specs/2026-10-10-app-data-overlays-design.md` (a Measurements section in each)

- [ ] **Step 1: Ask for the devices.** Message session cardos-78 (SendMessage, or the coord board if it is on one): "xip-app-code needs COM3 and COM4 for an upload and about an hour of tests; tell me when they are free." **Wait for "free".** Do not touch a port before then. If only one is released, do Steps 2–9 on it and leave Step 10 (between devices) until the second is.

- [ ] **Step 2: Tell them apart.**

```bash
python -c "import serial.tools.list_ports as l; [print(p.device, hex(p.vid or 0), hex(p.pid or 0), p.serial_number) for p in l.comports()]"
MSYS_NO_PATHCONV=1 CARDOS_PORT=COM3 python tools/cardctl.py sh "bootinfo"
MSYS_NO_PATHCONV=1 CARDOS_PORT=COM4 python tools/cardctl.py sh "bootinfo"
```
Both are `303a:1001`. Record each one's serial number, version and flavor here. Every step below says `$P` for the port, and is done on **both**. Run `export MSYS_NO_PATHCONV=1 CARDOS_PORT=$P` first.

- [ ] **Step 3: Before.** On each, on the firmware it has now: `python tools/cardctl.py open jar`, then `sh "mem"`, then `key quit`. Note heap free and low water. Jar's arena use before is the `data` column of `build/apps/sizes-before.txt` (28532), since that firmware's `mem` does not print the use.

- [ ] **Step 4: Flash and put the apps.**

```bash
python tools/build_apps.py
python -m platformio run -t upload --upload-port $P
python tools/cardctl.py sh "bootinfo"
python - <<'EOF'
import os, subprocess, sys
sys.path.insert(0, "tools")
import build_apps
for f in sorted(os.listdir("build/apps")):
    if f.endswith(".capp"):
        name = f[:-len(".capp")]
        subprocess.run([sys.executable, "tools/cardctl.py", "put", "build/apps/" + f,
                        "/apps/" + build_apps.seed_name(name)], check=True)
EOF
python tools/cardctl.py rm /apps/Games/jarshop.capp
python tools/cardctl.py rm /apps/Games/jarpost.capp
python tools/cardctl.py sh "xip wipe" "reboot"
```
The two `rm` lines may say "not there", which is fine: they are the retired companions. `reboot` drops the link, so reconnect. After it, `sh "log 20"` must show no "built for a different API version", and `ls /cache/ovl` says not there or empty.

- [ ] **Step 5: Every app from a fresh cache.** Open each non-CLI app in turn: for every `name` in `build/apps/sizes.txt` whose `capp_info` is not CLI, run `python tools/cardctl.py open NAME`, then `state`, then `shot NAME.png`, then `key quit`. Then:

```bash
python tools/cardctl.py sh "log 60" "xip" "mem"
```
Expected:
  - every app opens, with no load failure in `log`;
  - `xip` lists an entry per app with no `stale` and no `dead`;
  - the screenshots show text and icons where they belong. That is Review Focus 5: sprites and icons from constants reach the panel through `api_pixels`' bounce row.

Open Jar a second time: no new `xip` line, which is a hit.

- [ ] **Step 6: `ovltest`.**

```bash
python tools/cardctl.py sh "ovltest"
python tools/cardctl.py ls /cache/ovl
```
Expected: `ovltest: pass; 0->1 N ms, 1->0 M ms (20480 and 6000 bytes)`, and `/cache/ovl` empty or absent: the files went with the app. Record N and M. The spec expects 50–150 ms for 20 KB. Run it three times and record all three pairs. If it fails, the step name says where. Fix it in Task 7's or Task 9's code, not in the test.

- [ ] **Step 7: Overlay files at boot.**

```bash
printf stray > build/stray.bin
python tools/cardctl.py mkdir /cache/ovl
python tools/cardctl.py put build/stray.bin /cache/ovl/99-0.bin
python tools/cardctl.py sh "reboot"
python tools/cardctl.py ls /cache/ovl
```
Expected: `99-0.bin` is gone.

- [ ] **Step 8: Stale constants through the data view (Review Focus 3).**
  1. `open jar`, `key quit`. The constants have been read through the data view and are cached.
  2. Change the first line of Jar's help text (`capp_info.help` in `apps/jar.c`, a string literal and so a constant). Run `python tools/build_apps.py`, then `cardctl put build/apps/jar.capp /apps/Games/jar.capp`.
  3. `sh "xip wipe"`, then `open jar`. The entry is rewritten at offset 0, the same flash the old constants were read from. Press `key hex:e7` (fn-h, `KEY_HELP`). Then `shot help.png`, which must show the **new** line.
  4. Revert the change, rebuild, `put` the original back, and do one more `open jar` (a new entry).

- [ ] **Step 9: What `capp_info` reaches, and Jar Factory (Review Focus 1).**
  - `rm /cache/apps.idx`, then `reboot` (the scan runs through `capp_load_info`). Then `cat /cache/commands.txt` and compare it with `build/apps/commands.json`: the same commands, ids and params. The icon scan read them out of constants loaded into RAM.
  - In Jar: `key hex:e7` shows the help, and `key hex:e1` (fn-b) shows the toolbar with its menus. `sh "mem"` while Jar is open shows `arena 15668/28672 jar.capp` (±100): **28532 before**.
  - `open today`. Every section gathers (headless commands with constants in RAM after the block). `log` shows no load failure.
  - Jar Factory, per the coordinator:
    - it opens;
    - the shop screen (`s`) and the post screen (`f`, `m`) open and return without a reload: no new `loaded` line in `log`;
    - quit and reopen keeps the save (coins and placed items the same);
    - with the server reachable, restock (`r`) and Tibbs's finds (`f` in his screen) work.

- [ ] **Step 10: Between the two devices.** Jar Factory's friends list shows the other device. Send a gift from COM3's jar to COM4's: it floats down on COM4 within the poll interval (three minutes, or at once after reopening), opens, and the thank-you comes back as hearts on COM3. Then **Chat** between the devices: a message sent on each appears on the other.

- [ ] **Step 11: Measurements into the specs.** Add a `## Measurements` section to each spec:
  - **constants**: Jar's arena use before and after (28532 -> measured); its `.capp` size before and after (`-n`); its first launch (write) and later launch (hit) times from `log` stamps; the heap free and low water from Step 3 against the same with this firmware.
  - **overlays**: `ovltest`'s switch times (three runs, both devices); `ovltest`'s data block, which must be `.data` rounded to 16 plus 20480, not plus 26480. The figure is the `block` column of `sizes.txt`, checked against the `loaded …ovltest…: … N data` line the loader prints on the serial port (`python -m platformio device monitor -p $P` while `sh "ovltest"` runs, since a CLI app is gone before `mem` could see it).

  Each spec also gets a line under its status: "Planned with corrections: docs/superpowers/plans/2026-10-10-overlays-and-constants.md, Spec corrections."

- [ ] **Step 12: `CLAUDE.md`.**
  - In the API paragraph (`**API version 36**…`), change "… `sig_verify` (44) and `paint_direct` (45)." to "… `sig_verify` (44), `paint_direct` (45), and `overlay`/`overlay_live` (46), which came with constants moving to their own link origin."
  - After the `**App code runs from flash**` paragraph, add:

```markdown
**Constants run from flash too, and modes can share their state**
(2026-10-10). An app's `.rodata` is linked at `0x20000000` and written
into its flash cache entry after the code, read through a second mapping
of `appcode` on the data bus; on every other path (headless commands, RAM
code, the icon scan) it goes in RAM after the data. Only `capp_info`
itself stays in `.data`; what it points to is constants, which the icon
scan loads too. Jar's arena use went from 28532 to about 15700 bytes.
**Overlays:** `CAPP_OVERLAY(n) static T x;` puts zeroed state in overlay
0-3; all of an app's overlays share one region, so the arena holds `.data`
plus the largest. `api->overlay(n, CAPP_OVL_KEEP | CAPP_OVL_DISCARD)`
switches, keeping the outgoing one in `/cache/ovl/<load>-<n>.bin` (gone
when the app closes, and at boot); overlay 0 is live at start. A pointer
into an overlay is good only while it is live -- nothing traps a stray
one. `ovltest` (a CLI app) checks it on a device. The build refuses an
initialised overlay variable (the compiler does), `capp_info` reaching an
overlay or the code, and a writable `.rodata`; the budget is `.data` +
largest overlay <= 28 KB. Apps are linked `ld -n`. Design:
`docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md`,
`docs/superpowers/specs/2026-10-10-app-data-overlays-design.md`.
```
  - In the `**App code runs from flash**` paragraph, change "The budget is now data <= 28 KB" to "The budget is now the data block (`.data` and the largest overlay) <= 28 KB".

- [ ] **Step 13: Hand the devices back** to cardos-78 ("done with COM3 and COM4; both run xip-app-code's firmware at <sha>, apps from that build"). Then commit:

```bash
git add CLAUDE.md docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md docs/superpowers/specs/2026-10-10-app-data-overlays-design.md
git commit -m "Constants and overlays on the devices: measured, documented"
```
The body carries the key numbers: Jar's arena use before and after, `ovltest`'s switch times, and the Jar launch times.

---

### Task 11: Land on main

Publishing belongs to session cardos-78. This plan does not run `tools/deploy_droplet.sh`. **The GitHub branch is `main`.** `origin/master` is a stale leftover; never push to it. Local `master` is checked out in the main checkout (`C:\Users\alexr\Projects\cardos`), which holds another session's uncommitted work. **Do not move local `master` and do not touch that checkout.** It picks the change up with its own pull.

**Files:** none (merges only).

- [ ] **Step 1: Take what has landed since.**

```bash
git fetch origin
git merge origin/main
git merge master
```
Each merge may be "Already up to date". If one brings conflicts, resolve them, keeping both sides' intent. If `CAPP_API_VERSION` moved on main, renumber this work's bump to main's value + 1 and fix "API 46" in `capp.h`'s comment, `capprun.h`, `CLAUDE.md` and the specs. Then rerun `python tools/build_apps.py`.

- [ ] **Step 2: Everything green.**
  - The host suite: 0 failures.
  - `python tools/build_apps.py`.
  - `python tools/test_build_apps.py`.
  - `python -m server.tests.test_catalog`, `python -m server.tests.test_update`, `python -m server.tests.test_build`.
  - `python -m platformio run`.

  If Step 1 merged anything that touches `kernel/` or `apps/`, repeat Task 10 Steps 4–6 on one device before going on.

- [ ] **Step 3: Push, fast-forward only.**

```bash
git push origin HEAD:main
```
A non-fast-forward refusal means main moved during Step 2. Go back to Step 1 once. If it is refused again, **stop and report**; do not force.

- [ ] **Step 4: Say it landed.** Message session cardos-78: "xip-app-code landed on origin/main at <sha>: constants in flash and data overlays, API 46. Every .capp must be rebuilt (the store's are API 45), and the firmware too. Ready for you to deploy. Both devices run it already, with apps from this build."
