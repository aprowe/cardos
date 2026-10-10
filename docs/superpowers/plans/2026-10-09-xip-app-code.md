# XIP App Code Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A foreground app's code runs in place from a flash cache, and its data lives in a fixed 28 KB arena, so an app like Jar takes no heap at all while it runs.

**Architecture:** Three portable, host-tested modules plus device glue:
- `arena.c`: the fixed data block, and the placement policy.
- `capprel.c`: relocation arithmetic, whole-image or a window at a time.
- `xipcache.c`: a crash-safe ring of relocated code entries over an abstract flash.
- `xipflash.c` (device only): binds the ring to a new `appcode` partition mapped once at boot.

`elfload.c` gains a new load order: it reads the flags from the file, puts data in the arena, finds or writes the code in flash, then reads the data. Every failure falls back to today's RAM path.

**Tech Stack:** C (ESP-IDF 6.1 on device; MSVC host suite via `build.bat`), Python (`tools/build_apps.py`), `tools/cardctl.py` for the device.

**Spec:** `docs/superpowers/specs/2026-10-09-xip-app-code-design.md`. Read it first; this plan argues from it.

## Global Constraints

- Arena: **28 KB** (`ARENA_SIZE 28u * 1024u`), 16-byte aligned, one holder, the app started on screen only. Headless commands and the icon scan never take it.
- `appcode` partition: `data`, subtype `0x40`, offset `0x780000`, size `0x80000` (512 KB). `spiffs` keeps label and offset `0x751000`, size `0x2F000`.
- Sector `4096`; entry header `128` bytes; code at entry offset `128`; magic `0x43504958` ("XIPC").
- Opt-out flag `CAPP_CODE_IN_RAM 0x0040` (0x0020 is `CAPP_PAINT_DIRECT`). **No API version bump.**
- Budget: data ≤ 28 KB for every app; code ≤ 16 KB for `CAPP_CODE_IN_RAM` apps; code ≤ 96 KB (`CAPP_MAX_CODE`) for all.
- Flash is written only on a cache miss. A launch that hits writes nothing.
- Nothing new runs on `cardos-bg`. Flash writes happen on the shell's loop, inside `capp_load`.
- Code style: match the surrounding files. C89-style declarations at the top of blocks, comments that say *why*, `snake_case`, no new dependencies.
- Host tests use `test/tinytest.h` (`CHECK`, `CHECK_EQ`). Every `void test_*(void)` is picked up by `tools/gen_test_main.py`, which `build.bat` runs. Do not edit `test/test_main.c` by hand.
- Run the host suite with `cmd /c "<worktree>\build.bat"` from PowerShell (it needs the full path). Baseline: **21944 checks, 0 failures**.
- Device: `python -m platformio run` builds debug; `-t upload --upload-port COM3` flashes. Run `python tools/build_apps.py` before any firmware build. In Git Bash, `MSYS_NO_PATHCONV=1` before `tools/cardctl.py`.

## Review Focus

1. **A `.capp` replaced with the same size and mtime** (no clock, so FAT stamps 1980). The cache must not run old code with new data. The fs change hook kills entries for that path (`xip_forget`, Task 4 test, Task 5 hook). Path hashing is case-insensitive, because FAT is.
2. **Power pulled during a first launch.** The device must come back with no half-valid entry, and the next launch rewrites it. Task 4 tests a torn write at every step.
3. **An app updated while it runs from flash.** The new version's commit kills the old entry, but its code must stay intact until the running app closes. Task 4 test.
4. **New firmware moved the arena or the mapping.** Old entries must miss, not run, and must be killed when rewritten. Task 4 test (another arena means a miss; a rewrite of the same path kills).
5. **A device updated over the air**, which has the old partition table. Every app must load exactly as today. Task 2 truth table (`have_partition = 0`), and Task 5's `xipflash_ready()` returning 0.

---

## File Structure

| File | Responsibility | Host? |
|---|---|---|
| `kernel/app/arena.h/.c` (new) | The 28 KB block: claim, release, holder; `arena_code_in_flash` policy | yes |
| `kernel/app/capprel.h/.c` (new) | `R_XTENSA_32` arithmetic over a window; validation | yes |
| `kernel/app/xipcache.h/.c` (new) | The ring: open/scan, find, begin/write/commit, ref, forget, wipe, each, crc32 | yes |
| `kernel/app/xipflash.h/.c` (new) | `appcode` partition: find, mmap INST once, bind `XipFlash` to `esp_partition_*` | device |
| `kernel/app/elfload.h/.c` | New load order; `capp_load_ex(path, out, foreground)`; unload; prepare hook | device |
| `kernel/app/capp.h` | `CAPP_CODE_IN_RAM` | both |
| `kernel/app/capprun.c` | `ensure_loaded(s, foreground)`; fs change kills cache entries | device |
| `kernel/ui/launchui.c` | "preparing NAME…" on the row during a cache write | device |
| `kernel/sys/memreport.c` | arena and appcode lines in `mem` | device |
| `src/main.c` | `xipflash_init()` at boot; `xip` / `xip wipe` commands | device |
| `partitions.csv` | `spiffs` shrinks, `appcode` added | — |
| `host/CMakeLists.txt`, `src/CMakeLists.txt` | new sources | — |
| `tools/build_apps.py` | new budget rules | — |
| `test/test_arena.c`, `test/test_capprel.c`, `test/test_xipcache.c` (new) | host tests | yes |
| `test/fixtures/capp_{jar,forklift,cat}.capp` (new) | real-toolchain fixtures | — |
| `CLAUDE.md`, the spec | the new paragraph, the measurements | — |

---

### Task 1: Baseline measurements on master's firmware

Measure before anything changes. The device must be running master's firmware.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-09-xip-app-code-design.md` (the Measurements table, "master" rows)

**Interfaces:** none.

- [ ] **Step 1: Confirm the device runs master**

Run (Git Bash): `MSYS_NO_PATHCONV=1 python tools/cardctl.py sh "bootinfo"`.
If it is not master `12201ad` or later, build and flash master from the main checkout (`C:\Users\alexr\Projects\cardos`). That is a plain upload; the partition table does not change. Run `python tools/build_apps.py && python -m platformio run -t upload --upload-port COM3` there, then `update apps` on the device if the apps are older. Ask alex before flashing if the device is in use.

- [ ] **Step 2: Bring both radios up with background activity**

```
python tools/cardctl.py sh "wifi"            # joined
python tools/cardctl.py sh "print test"      # Bluetooth up, a print in progress
```
For churn, run `print test` three times and `open todo` / `key quit` three times (each Todo open syncs).

- [ ] **Step 3: Measure three states, `mem` and `mem map` in each**

1. Launcher idle (`key quit` until `state` says launcher, no app).
2. Jar open (`open jar`), while a `print test` runs.
3. Today gathering (`open today`, measured within 2 s, while its sections still say waiting).

Each: `python tools/cardctl.py sh "mem"` and `python tools/cardctl.py sh "mem map"`. Record heap free, largest 8-bit, exec free, largest exec, and low water.

- [ ] **Step 4: Write the master rows into the spec's Measurements table, then commit**

```bash
git add docs/superpowers/specs/2026-10-09-xip-app-code-design.md
git commit -m "XIP spec: baseline memory on master, radios up, under load"
```
(End every commit message with the `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>` line.)

---

### Task 2: The arena and the placement policy

**Files:**
- Create: `kernel/app/arena.h`, `kernel/app/arena.c`, `test/test_arena.c`
- Modify: `kernel/app/capp.h:77-79` (add the flag), `host/CMakeLists.txt` (after the `appidx.c` line), `src/CMakeLists.txt` (after `"../kernel/app/appidx.c"`)

**Interfaces:**
- Produces:
  - `#define ARENA_SIZE (28u * 1024u)`, `#define ARENA_ALIGN 16u`
  - `void *arena_claim(uint32_t size, uint32_t align, const char *who);`: NULL if held, `size == 0`, `size > ARENA_SIZE` or `align > ARENA_ALIGN`; zeroes `size` bytes.
  - `void arena_release(const void *p);`: a no-op unless `p` is the arena and it is held.
  - `int arena_held(void);`, `const char *arena_holder(void);`, `uintptr_t arena_addr(void);`, `int arena_owns(const void *p);`
  - `int arena_code_in_flash(int data_in_arena, int have_partition, uint16_t flags);`
  - `#define CAPP_CODE_IN_RAM 0x0040` in `capp.h`

- [ ] **Step 1: Add the flag to `capp.h`** (after `CAPP_NEEDS_PROXY`):

```c
/* Keep this app's code in executable RAM rather than running it from the
 * flash cache (docs/superpowers/specs/2026-10-09-xip-app-code-design.md).
 * For an inner loop that the 16 KB instruction cache would slow: measured,
 * not guessed. A flag, not a table change, so no API bump. */
#define CAPP_CODE_IN_RAM 0x0040
```

- [ ] **Step 2: Write the failing tests, `test/test_arena.c`**

```c
/* The app data arena: one fixed block for the data of the app on screen,
 * so code relocated against its address can live in flash. */
#include <string.h>
#include "tinytest.h"
#include "kernel/app/arena.h"
#include "kernel/app/capp.h"

void test_arena_one_holder_at_a_time(void) {
  void *a = arena_claim(20000, 8, "jar.capp"), *b;
  CHECK(a != NULL);
  CHECK(arena_owns(a));
  CHECK_EQ(arena_held(), 1);
  CHECK(strcmp(arena_holder(), "jar.capp") == 0);
  b = arena_claim(100, 4, "todo.capp");
  CHECK(b == NULL);
  arena_release(a);
  CHECK_EQ(arena_held(), 0);
  CHECK(strcmp(arena_holder(), "") == 0);
}

/* Today holds the arena; the apps it asks for sections load beside it on
 * the RAM path. A refused claim must leave Today's hold exactly as it was. */
void test_arena_held_so_a_command_load_falls_back(void) {
  void *today = arena_claim(8548, 4, "today.capp");
  CHECK(today != NULL);
  CHECK(arena_claim(15416, 4, "calendar.capp") == NULL);  /* -> heap, RAM code */
  CHECK(arena_claim(17852, 4, "todo.capp") == NULL);
  CHECK(strcmp(arena_holder(), "today.capp") == 0);
  CHECK_EQ(arena_code_in_flash(0, 1, 0), 0);                /* heap data: never flash */
  arena_release(today);
  CHECK(arena_claim(15416, 4, "calendar.capp") != NULL);    /* free again */
  arena_release((void *)(uintptr_t)arena_addr());
}

void test_arena_refuses_too_big_or_overaligned(void) {
  CHECK(arena_claim(ARENA_SIZE + 4, 4, "big") == NULL);
  CHECK(arena_claim(0, 4, "empty") == NULL);
  CHECK(arena_claim(100, 32, "odd") == NULL);
  CHECK_EQ(arena_held(), 0);
  CHECK(arena_claim(ARENA_SIZE, 16, "exact") != NULL);
  arena_release((void *)(uintptr_t)arena_addr());
}

void test_arena_release_by_a_stranger_is_ignored(void) {
  char other[16];
  void *a = arena_claim(64, 4, "a");
  arena_release(other);
  arena_release(NULL);
  CHECK_EQ(arena_held(), 1);
  arena_release(a);
  CHECK_EQ(arena_held(), 0);
}

void test_arena_claim_is_zeroed(void) {
  uint8_t *a = arena_claim(256, 4, "a");
  int i, zero = 1;
  memset(a, 0xA5, 256);
  arena_release(a);
  a = arena_claim(256, 4, "b");
  for (i = 0; i < 256; i++) if (a[i]) zero = 0;
  CHECK(zero);
  CHECK_EQ((uintptr_t)a % ARENA_ALIGN, 0);
  arena_release(a);
}

void test_arena_code_goes_to_flash_only_when_allowed(void) {
  CHECK_EQ(arena_code_in_flash(1, 1, 0), 1);
  CHECK_EQ(arena_code_in_flash(1, 1, CAPP_FULLSCREEN | CAPP_NEEDS_NET), 1);
  CHECK_EQ(arena_code_in_flash(1, 1, CAPP_CODE_IN_RAM), 0);
  CHECK_EQ(arena_code_in_flash(1, 0, 0), 0);   /* an OTA'd device: old table */
  CHECK_EQ(arena_code_in_flash(0, 1, 0), 0);   /* data on the heap */
}
```

- [ ] **Step 3: Add `kernel/app/arena.c` to `host/CMakeLists.txt`** (after the `appidx.c` line):

```cmake
  ${ROOT}/kernel/app/arena.c     # the app data arena; elfload.c claims it
```
Run: `cmd /c "<worktree>\build.bat"`. Expected: a link error for `arena_claim` (no `arena.c` yet). Create an empty `kernel/app/arena.c` if CMake refuses a missing file, and expect unresolved externals.

- [ ] **Step 4: Write `kernel/app/arena.h`**

```c
/* The app data arena.
 *
 * One block, reserved for the uptime, for the data of the app on screen.
 * It exists because an app's literals -- inside .code -- hold the absolute
 * addresses of its data, so code cached in flash is valid for one data
 * address only. A heap block lands somewhere new on every launch; this
 * does not. See docs/superpowers/specs/2026-10-09-xip-app-code-design.md.
 *
 * One holder: the app started on screen. Headless commands and the icon
 * scan load on the heap as before. */
#ifndef CARDOS_ARENA_H
#define CARDOS_ARENA_H

#include <stdint.h>

#define ARENA_SIZE  (28u * 1024u)   /* tools/build_apps.py DATA_BUDGET */
#define ARENA_ALIGN 16u

/* The arena, zeroed for `size` bytes, or NULL: held already, empty, too big,
 * or wanting more alignment than it has. `who` is for `mem`. */
void       *arena_claim(uint32_t size, uint32_t align, const char *who);
/* Give it back. Anything but the arena itself is ignored. */
void        arena_release(const void *p);
int         arena_held(void);
const char *arena_holder(void);     /* "" when free */
uintptr_t   arena_addr(void);
int         arena_owns(const void *p);

/* Where code goes once the flags are known: 1 the flash cache, 0 executable
 * RAM. Only data in the arena can have its code in flash. */
int arena_code_in_flash(int data_in_arena, int have_partition, uint16_t flags);

#endif /* CARDOS_ARENA_H */
```

- [ ] **Step 5: Write `kernel/app/arena.c`**

```c
/* The app data arena. See arena.h. */

#include "kernel/app/arena.h"
#include "kernel/app/capp.h"

#include <stdio.h>
#include <string.h>

/* Static rather than allocated at boot: same address on every boot of a
 * build, and no chance of a radio getting there first. */
#if defined(_MSC_VER)
__declspec(align(16)) static uint8_t s_arena[ARENA_SIZE];
#else
static uint8_t s_arena[ARENA_SIZE] __attribute__((aligned(16)));
#endif
static int  s_held;
static char s_who[24];

void *arena_claim(uint32_t size, uint32_t align, const char *who) {
  if (s_held || size == 0 || size > ARENA_SIZE || align > ARENA_ALIGN) return NULL;
  s_held = 1;
  snprintf(s_who, sizeof s_who, "%s", who ? who : "?");
  memset(s_arena, 0, size);
  return s_arena;
}

void arena_release(const void *p) {
  if (!s_held || p != (const void *)s_arena) return;
  s_held = 0;
  s_who[0] = 0;
}

int arena_held(void) { return s_held; }
const char *arena_holder(void) { return s_held ? s_who : ""; }
uintptr_t arena_addr(void) { return (uintptr_t)s_arena; }
int arena_owns(const void *p) { return p == (const void *)s_arena; }

int arena_code_in_flash(int data_in_arena, int have_partition, uint16_t flags) {
  return data_in_arena && have_partition && !(flags & CAPP_CODE_IN_RAM);
}
```

- [ ] **Step 6: Add to `src/CMakeLists.txt`** after `"../kernel/app/appidx.c"`:

```cmake
    "../kernel/app/arena.c"
```

- [ ] **Step 7: Run the host suite.** Expected: six `test_arena_*` ok, 0 failures.

- [ ] **Step 8: Commit**

```bash
git add kernel/app/arena.h kernel/app/arena.c kernel/app/capp.h test/test_arena.c host/CMakeLists.txt src/CMakeLists.txt
git commit -m "Arena: one fixed 28 KB block for the data of the app on screen"
```

---

### Task 3: Relocation arithmetic, portable, windowed

`elfload.c` behaves exactly as before; the arithmetic moves out so it can be windowed and host-tested.

**Files:**
- Create: `kernel/app/capprel.h`, `kernel/app/capprel.c`, `test/test_capprel.c`, `test/fixtures/capp_jar.capp`, `test/fixtures/capp_forklift.capp`, `test/fixtures/capp_cat.capp`
- Modify: `kernel/app/elfload.c` (the relocation loop at lines 307–359, the type defines at 26–29, `Elf32_Rela` at 51–56, `CAPP_DATA_ORIGIN` at 62), `host/CMakeLists.txt`, `src/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `#define CAPP_DATA_ORIGIN 0x10000000u`
  - `typedef struct { uint32_t r_offset, r_info; int32_t r_addend; } CappRela;` (the ELF32 RELA layout, 12 bytes)
  - `int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit, int into_code, const CappRela *r, uint32_t n, uint32_t code_base, uint32_t data_base);` returns 0, or -1 for a relocation the loader cannot apply. Every entry is validated against `limit` whether or not it falls in the window, so windowed and whole-image runs fail alike. `win_off` and `win_len` must be multiples of 4.
  - In `elfload.c` (static): `static CappResult relocate(int fd, const Elf32_Shdr *sh, int shnum, int sec, int into_code, uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit, uint32_t code_base, uint32_t data_base);`

- [ ] **Step 1: Make the fixtures**

```bash
python tools/build_apps.py
cp build/apps/jar.capp test/fixtures/capp_jar.capp
cp build/apps/forklift.capp test/fixtures/capp_forklift.capp
cp build/apps/cat.capp test/fixtures/capp_cat.capp
```

- [ ] **Step 2: Write the failing tests, `test/test_capprel.c`**

```c
/* Relocating an app's code a window at a time must give exactly the bytes
 * relocating it whole does -- the flash cache writes code through a 28 KB
 * window -- and every absolute reference must land in the half it names.
 * Fixtures are real .capps from tools/build_apps.py. */
#include <stdlib.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/app/capprel.h"

typedef struct {
  uint8_t  *file;
  long      size;
  uint32_t  code_off, code_size, data_off, data_size;
  CappRela *code_rel, *data_rel;
  uint32_t  ncode_rel, ndata_rel;
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

/* Just enough ELF: the two allocated sections (by link address, as the
 * loader does) and the RELA sections aimed at them. */
static int load_capp(const char *name, Capp *c) {
  char path[512];
  FILE *fp;
  uint32_t shoff, i;
  uint16_t shentsize, shnum;
  int code_sec = -1, data_sec = -1;
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
    if (addr >= CAPP_DATA_ORIGIN) { if (data_sec < 0) { data_sec = (int)i; c->data_off = u32(s + 16); c->data_size = size; } }
    else if (code_sec < 0) { code_sec = (int)i; c->code_off = u32(s + 16); c->code_size = size; }
  }
  for (i = 0; i < shnum; i++) {
    const uint8_t *s = c->file + shoff + i * shentsize;
    if (u32(s + 4) != 4) continue;                       /* SHT_RELA */
    if ((int)u32(s + 28) == code_sec) {
      c->code_rel = (CappRela *)(c->file + u32(s + 16)); c->ncode_rel = u32(s + 20) / 12;
    } else if ((int)u32(s + 28) == data_sec) {
      c->data_rel = (CappRela *)(c->file + u32(s + 16)); c->ndata_rel = u32(s + 20) / 12;
    }
  }
  return code_sec >= 0 && data_sec >= 0 ? 0 : -1;
}

#define CODE_BASE 0x42480080u
#define DATA_BASE 0x3FC9A000u

static const char *FIXTURES[] = { "capp_jar.capp", "capp_forklift.capp", "capp_cat.capp" };

void test_capprel_windows_match_whole_image(void) {
  static const uint32_t WINDOWS[] = { 4, 8, 12, 4096, 28672 };
  size_t f, w;
  for (f = 0; f < sizeof FIXTURES / sizeof *FIXTURES; f++) {
    Capp c;
    uint8_t *whole, *win;
    uint32_t o;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    CHECK_EQ(c.code_size % 4, 0);
    whole = malloc(c.code_size); win = malloc(c.code_size);
    memcpy(whole, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(whole, 0, c.code_size, c.code_size, 1, c.code_rel, c.ncode_rel,
                           CODE_BASE, DATA_BASE), 0);
    CHECK(memcmp(whole, c.file + c.code_off, c.code_size) != 0);   /* it did something */
    for (w = 0; w < sizeof WINDOWS / sizeof *WINDOWS; w++) {
      memcpy(win, c.file + c.code_off, c.code_size);
      for (o = 0; o < c.code_size; o += WINDOWS[w]) {
        uint32_t n = c.code_size - o < WINDOWS[w] ? c.code_size - o : WINDOWS[w];
        CHECK_EQ(capprel_apply(win + o, o, n, c.code_size, 1, c.code_rel, c.ncode_rel,
                               CODE_BASE, DATA_BASE), 0);
      }
      CHECK(memcmp(win, whole, c.code_size) == 0);
    }
    free(whole); free(win); free(c.file);
  }
}

/* The premise of the arena: code holds data addresses. */
void test_capprel_refs_land_in_the_right_half(void) {
  size_t f;
  for (f = 0; f < sizeof FIXTURES / sizeof *FIXTURES; f++) {
    Capp c;
    uint8_t *code;
    uint32_t j, to_data = 0;
    CHECK_EQ(load_capp(FIXTURES[f], &c), 0);
    code = malloc(c.code_size);
    memcpy(code, c.file + c.code_off, c.code_size);
    CHECK_EQ(capprel_apply(code, 0, c.code_size, c.code_size, 1, c.code_rel, c.ncode_rel,
                           CODE_BASE, DATA_BASE), 0);
    for (j = 0; j < c.ncode_rel; j++) {
      uint32_t v;
      if ((c.code_rel[j].r_info & 0xFF) != R_XTENSA_32) continue;
      v = u32(code + c.code_rel[j].r_offset);
      CHECK((v >= CODE_BASE && v <= CODE_BASE + c.code_size) ||
            (v >= DATA_BASE && v <= DATA_BASE + c.data_size));
      if (v >= DATA_BASE && v <= DATA_BASE + c.data_size) to_data++;
    }
    if (strcmp(FIXTURES[f], "capp_cat.capp") != 0) CHECK(to_data > 0);
    free(code); free(c.file);
  }
}

void test_capprel_refuses_what_it_cannot_apply(void) {
  uint8_t buf[64];
  CappRela r;
  memset(buf, 0, sizeof buf);
  r.r_addend = 0;
  r.r_offset = 8;  r.r_info = 5;                 /* a type the loader does not know */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_info = R_XTENSA_32; r.r_offset = 64;       /* past the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 62;                               /* runs off the end */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 6;                                /* unaligned */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = CAPP_DATA_ORIGIN - 2;             /* wraps to near 2^32 in data */
  CHECK_EQ(capprel_apply(buf, 0, 64, 64, 0, &r, 1, 0, 0), -1);
  r.r_offset = 60;                               /* outside the window, still checked */
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 1, &r, 1, 0, 0), 0);
  r.r_offset = 64;
  CHECK_EQ(capprel_apply(buf, 0, 32, 64, 1, &r, 1, 0, 0), -1);
  r.r_offset = 0;
  CHECK_EQ(capprel_apply(buf, 2, 32, 64, 1, &r, 1, 0, 0), -1);   /* window not aligned */
}

void test_capprel_leaves_pc_relative_alone(void) {
  uint8_t buf[16], before[16];
  CappRela r[3];
  int i;
  for (i = 0; i < 16; i++) buf[i] = before[i] = (uint8_t)(i * 13);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_SLOT0_OP; r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_NONE;     r[1].r_addend = 0;
  r[2].r_offset = 8; r[2].r_info = R_XTENSA_ASM_EXPAND; r[2].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 16, 16, 1, r, 3, 0x1000, 0x2000), 0);
  CHECK(memcmp(buf, before, 16) == 0);
}

void test_capprel_adds_the_right_base(void) {
  uint8_t buf[8];
  uint32_t a = 0x40, b = CAPP_DATA_ORIGIN + 0x10, v;
  CappRela r[2];
  memcpy(buf, &a, 4); memcpy(buf + 4, &b, 4);
  r[0].r_offset = 0; r[0].r_info = R_XTENSA_32; r[0].r_addend = 0;
  r[1].r_offset = 4; r[1].r_info = R_XTENSA_32; r[1].r_addend = 0;
  CHECK_EQ(capprel_apply(buf, 0, 8, 8, 1, r, 2, CODE_BASE, DATA_BASE), 0);
  memcpy(&v, buf, 4);     CHECK_EQ(v, CODE_BASE + 0x40);
  memcpy(&v, buf + 4, 4); CHECK_EQ(v, DATA_BASE + 0x10);
}
```

- [ ] **Step 3: Add `${ROOT}/kernel/app/capprel.c` to `host/CMakeLists.txt`** (comment: `# relocation arithmetic; elfload.c reads the tables`). Run the suite. Expected: link errors for `capprel_apply`.

- [ ] **Step 4: Write `kernel/app/capprel.h`**

```c
/* Relocation arithmetic for .capp images, portable so the host suite can
 * reach it. See elfload.h for why only R_XTENSA_32 is ever applied.
 *
 * Windowed because the flash cache relocates code through the 28 KB arena:
 * an app's code may not fit in one piece of RAM, which is the whole reason
 * for the cache. Each window is handed every relocation of its section;
 * those outside it are checked and skipped. */
#ifndef CARDOS_CAPPREL_H
#define CARDOS_CAPPREL_H

#include <stdint.h>

/* Where capp.ld links each half. Code at 0, data far away, so an absolute
 * value says which half it points into without any symbol lookup. */
#define CAPP_DATA_ORIGIN 0x10000000u

#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP   20

typedef struct {            /* Elf32_Rela */
  uint32_t r_offset, r_info;
  int32_t  r_addend;
} CappRela;

/* Apply `r` to the bytes [win_off, win_off + win_len) of a section that is
 * `limit` bytes long, held at `win`. into_code says which section: code
 * offsets are link addresses from 0, data offsets from CAPP_DATA_ORIGIN.
 * win_off and win_len are multiples of 4. Returns 0, or -1 for a type it
 * does not know or an offset that is out of range or unaligned -- checked
 * for every entry, in the window or not. */
int capprel_apply(uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                  int into_code, const CappRela *r, uint32_t n,
                  uint32_t code_base, uint32_t data_base);

#endif /* CARDOS_CAPPREL_H */
```

- [ ] **Step 5: Write `kernel/app/capprel.c`**

```c
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
```

- [ ] **Step 6: Run the suite.** Expected: five `test_capprel_*` ok.

- [ ] **Step 7: Make `elfload.c` use it.**
  - Delete the `R_XTENSA_*` defines, the `Elf32_Rela` typedef, `ELF32_R_TYPE` and `CAPP_DATA_ORIGIN` from `elfload.c`, and `#include "kernel/app/capprel.h"`.
  - Add above `capp_load`:

```c
/* Apply the relocations aimed at section `sec` to the window of it at
 * `win`. Read 32 at a time: one at a time was a seek and a read per entry,
 * and 384 bytes of stack is nothing next to that. */
static CappResult relocate(int fd, const Elf32_Shdr *sh, int shnum, int sec, int into_code,
                           uint8_t *win, uint32_t win_off, uint32_t win_len, uint32_t limit,
                           uint32_t code_base, uint32_t data_base) {
  CappRela buf[32];
  int i;
  for (i = 0; i < shnum; i++) {
    uint32_t n, j, k;
    if (sh[i].sh_type != SHT_RELA || (int)sh[i].sh_info != sec) continue;
    n = sh[i].sh_size / sizeof(CappRela);
    for (j = 0; j < n; j += k) {
      k = n - j < 32 ? n - j : 32;
      if (read_at(fd, sh[i].sh_offset + j * sizeof(CappRela), buf, k * sizeof(CappRela)) != 0)
        return CAPP_ERR_RELOC;
      if (capprel_apply(win, win_off, win_len, limit, into_code, buf, k,
                        code_base, data_base) != 0) {
        ESP_LOGE(TAG, "a relocation in section %d cannot be applied", sec);
        return CAPP_ERR_RELOC;
      }
    }
  }
  return CAPP_OK;
}
```
  - Replace the whole `/* Relocate. ... */ { ... }` block (lines 307–359) with:

```c
  /* Relocate. Code is patched through its data-window alias, because the
   * compiler is free to implement the read-modify-write with narrower
   * accesses than the instruction window permits. */
  {
    uint32_t code_base = (uint32_t)(uintptr_t)code;
    uint32_t data_base = (uint32_t)(uintptr_t)data;
    rc = relocate(fd, sh, eh.e_shnum, code_sec, 1, code_w, 0, code_size, code_size,
                  code_base, data_base);
    if (rc == CAPP_OK && data_sec >= 0)
      rc = relocate(fd, sh, eh.e_shnum, data_sec, 0, data, 0, data_size, data_size,
                    code_base, data_base);
    if (rc != CAPP_OK) goto done;
    rc = CAPP_ERR_OPEN;     /* what the code below expects until it succeeds */
  }
```
  - Add `"../kernel/app/capprel.c"` to `src/CMakeLists.txt` after `arena.c`.

- [ ] **Step 8: Build the firmware and check it on the device**

Run: `python tools/build_apps.py && python -m platformio run`. Expected: builds, no new warnings in `elfload.c`.
Flash it (plain upload, same table): `python -m platformio run -t upload --upload-port COM3`. Then `cardctl open jar`, `open forklift` and `open todo`, and check that `state` shows each running and `log` shows no load failure.

- [ ] **Step 9: Run the host suite** (0 failures), then commit:

```bash
git add kernel/app/capprel.h kernel/app/capprel.c kernel/app/elfload.c test/test_capprel.c test/fixtures/capp_*.capp host/CMakeLists.txt src/CMakeLists.txt
git commit -m "Relocation arithmetic out of elfload.c, windowed and host-tested"
```

---

### Task 4: The code cache ring

**Files:**
- Create: `kernel/app/xipcache.h`, `kernel/app/xipcache.c`, `test/test_xipcache.c`
- Modify: `host/CMakeLists.txt`, `src/CMakeLists.txt`

**Interfaces:**
- Produces (all in `xipcache.h`, below): `XipFlash`, `XipKey`, `XipHdr`, `XipCache`, the `XIP_*` constants and results, and `xip_open`, `xip_find`, `xip_begin`, `xip_write`, `xip_commit`, `xip_abandon`, `xip_ref`, `xip_unref`, `xip_forget`, `xip_wipe`, `xip_each`, `xip_crc32`, `xip_path_hash`.

- [ ] **Step 1: Write `kernel/app/xipcache.h`.** The tests need the types.

```c
/* The flash cache of relocated app code.
 *
 * A ring of 4 KB sectors over an abstract flash (device: the appcode
 * partition, xipflash.c; host: an array). An entry is a 128-byte header and
 * an app's code, relocated for one (map base, arena address) pair, in whole
 * sectors. New entries go at the head; the erase evicts whatever was there,
 * oldest-written first, so every sector is erased once a lap and nothing
 * needs an allocator. See
 * docs/superpowers/specs/2026-10-09-xip-app-code-design.md.
 *
 * Crash safety is in the order of writes: erase, code, header, and last the
 * commit word. Anything without a commit is free space. There is no index
 * in RAM: a lookup walks the headers, at most 128 small reads. */
#ifndef CARDOS_XIPCACHE_H
#define CARDOS_XIPCACHE_H

#include <stdint.h>

#define XIP_SECTOR    4096u
#define XIP_HDR       128u          /* code starts here in an entry */
#define XIP_MAGIC     0x43504958u   /* "XIPC" */
#define XIP_PATH_MAX  24
#define XIP_INUSE_MAX 4             /* entries mapped by loaded apps at once */

enum { XIP_OK = 0, XIP_MISS = 1,
       XIP_ERR_IO = -1, XIP_ERR_FULL = -2, XIP_ERR_BUSY = -3, XIP_ERR_STATE = -4 };

typedef struct {
  int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
  int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t n);  /* only clears bits */
  int (*erase)(void *ctx, uint32_t off, uint32_t n);                   /* whole sectors */
  void    *ctx;
  uint32_t size;                    /* a multiple of XIP_SECTOR */
} XipFlash;

/* What an entry is valid for. All of it must match. */
typedef struct {
  uint32_t path_hash, file_size, file_mtime;   /* the .capp, as appidx keys it */
  uint32_t api, code_size;
  uint32_t map_base, arena;                    /* what it was relocated against */
} XipKey;

typedef struct {                    /* on flash, XIP_HDR bytes */
  uint32_t magic, seq, sectors;
  XipKey   key;
  uint32_t crc;                     /* of the code as written */
  char     path[XIP_PATH_MAX];      /* the tail of the path, for listings */
  uint32_t reserved[13];            /* left erased */
  uint32_t live;                    /* 0xFFFFFFFF; 0 kills it, no erase needed */
  uint32_t commit;                  /* XIP_MAGIC, written last */
} XipHdr;

typedef struct { uint32_t off, len; uint16_t refs; } XipUse;

typedef struct {
  XipFlash f;
  uint32_t head, next_seq;
  uint32_t pend_off, pend_len;      /* begun, not committed; len 0 for none */
  XipUse   use[XIP_INUSE_MAX];
} XipCache;

/* Bind to a flash and find the head (the end of the highest seq). */
int  xip_open(XipCache *c, const XipFlash *f);
/* A live, committed entry matching k whose code still checks: XIP_OK and
 * its offset, else XIP_MISS. */
int  xip_find(XipCache *c, const XipKey *k, uint32_t *off);
/* Room for code_size bytes at the head (or at 0, if the head is too near
 * the end), erased. XIP_ERR_FULL if it could never fit, XIP_ERR_BUSY if it
 * would erase an entry in use. One at a time. */
int  xip_begin(XipCache *c, uint32_t code_size, uint32_t *off);
/* Code bytes at `at` into the begun entry. */
int  xip_write(XipCache *c, uint32_t at, const void *buf, uint32_t n);
/* Header, then the commit word; then every other live entry for the same
 * path is killed. */
int  xip_commit(XipCache *c, const XipKey *k, uint32_t crc, const char *path);
void xip_abandon(XipCache *c);
/* A loaded app maps the entry at off: never erase it until unref. */
int  xip_ref(XipCache *c, uint32_t off);
void xip_unref(XipCache *c, uint32_t off);
/* The .capp at this path changed: kill its live entries. Its code stays
 * where it is, so an app running from one carries on. */
int  xip_forget(XipCache *c, uint32_t path_hash);
/* Erase everything. XIP_ERR_BUSY while anything is in use. */
int  xip_wipe(XipCache *c);
/* Every committed entry, live or dead, in flash order. */
int  xip_each(XipCache *c, void (*fn)(const XipHdr *h, uint32_t off, int in_use, void *ctx),
              void *ctx);

uint32_t xip_crc32(uint32_t crc, const void *p, uint32_t n);   /* crc32(0, ...) to start */
/* FNV-1a of the path, letters folded: FAT does not care about case. */
uint32_t xip_path_hash(const char *path);

#endif /* CARDOS_XIPCACHE_H */
```

- [ ] **Step 2: Write the failing tests, `test/test_xipcache.c`**

```c
/* The flash cache of relocated app code, over a fake flash that behaves
 * like the real one: programming only clears bits, erases are whole
 * sectors, and a write can be made to fail as a pulled battery would. */
#include <string.h>
#include "tinytest.h"
#include "kernel/app/xipcache.h"

#define FSIZE (128u * XIP_SECTOR)
static uint8_t s_flash[FSIZE];
static int s_bad_writes;     /* a write that needed a 0 -> 1 */
static int s_fail_after;     /* writes left before the power goes; -1 never */

static int f_read(void *ctx, uint32_t off, void *buf, uint32_t n) {
  (void)ctx;
  if (off > FSIZE || n > FSIZE - off) return -1;
  memcpy(buf, s_flash + off, n);
  return 0;
}
static int f_write(void *ctx, uint32_t off, const void *buf, uint32_t n) {
  const uint8_t *b = buf;
  uint32_t i;
  (void)ctx;
  if (off > FSIZE || n > FSIZE - off) return -1;
  if (s_fail_after == 0) return -1;
  if (s_fail_after > 0) s_fail_after--;
  for (i = 0; i < n; i++) {
    if (b[i] & (uint8_t)~s_flash[off + i]) s_bad_writes++;
    s_flash[off + i] &= b[i];
  }
  return 0;
}
static int f_erase(void *ctx, uint32_t off, uint32_t n) {
  (void)ctx;
  if (off % XIP_SECTOR || n % XIP_SECTOR || off > FSIZE || n > FSIZE - off) return -1;
  memset(s_flash + off, 0xFF, n);
  return 0;
}

static void reset(XipCache *c) {
  XipFlash f = { f_read, f_write, f_erase, NULL, FSIZE };
  memset(s_flash, 0xFF, FSIZE);
  s_bad_writes = 0;
  s_fail_after = -1;
  CHECK_EQ(xip_open(c, &f), XIP_OK);
}
static void reboot(XipCache *c) {
  XipFlash f = { f_read, f_write, f_erase, NULL, FSIZE };
  s_fail_after = -1;
  CHECK_EQ(xip_open(c, &f), XIP_OK);
}

static XipKey key(const char *path, uint32_t size, uint32_t mtime) {
  XipKey k;
  memset(&k, 0, sizeof k);
  k.path_hash = xip_path_hash(path);
  k.file_size = size + 1000;
  k.file_mtime = mtime;
  k.api = 36;
  k.code_size = size;
  k.map_base = 0x42400000u;
  k.arena = 0x3FC9A000u;
  return k;
}

/* As the loader does it: begin, the code, commit. The entry offset, or a
 * negative XIP_ERR. */
static long install(XipCache *c, const char *path, uint32_t size, uint32_t mtime, uint8_t fill) {
  static uint8_t buf[96 * 1024];
  XipKey k = key(path, size, mtime);
  uint32_t off;
  int r;
  memset(buf, fill, size);
  if ((r = xip_begin(c, size, &off)) != XIP_OK) return r;
  if ((r = xip_write(c, 0, buf, size)) != XIP_OK) { xip_abandon(c); return r; }
  if ((r = xip_commit(c, &k, xip_crc32(0, buf, size), path)) != XIP_OK) return r;
  return (long)off;
}
static int found(XipCache *c, const char *path, uint32_t size, uint32_t mtime, uint32_t *off) {
  XipKey k = key(path, size, mtime);
  uint32_t o;
  int r = xip_find(c, &k, off ? off : &o);
  return r == XIP_OK;
}

#define JAR 24480u                  /* 7 sectors with the header */

void test_xip_crc32_is_the_usual_one(void) {
  CHECK_EQ(xip_crc32(0, "123456789", 9), 0xCBF43926u);
  CHECK_EQ(xip_crc32(xip_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

void test_xip_path_hash_folds_case(void) {
  CHECK_EQ(xip_path_hash("/apps/Games/Jar.capp"), xip_path_hash("/APPS/games/jar.CAPP"));
  CHECK(xip_path_hash("/apps/jar.capp") != xip_path_hash("/apps/jab.capp"));
}

void test_xip_empty_flash_finds_nothing(void) {
  XipCache c;
  reset(&c);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK_EQ(c.head, 0);
}

void test_xip_finds_what_it_wrote_after_a_reboot(void) {
  XipCache c;
  uint32_t off = 99;
  reset(&c);
  CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x5A), 0);
  CHECK(found(&c, "/apps/jar.capp", JAR, 1, &off));
  CHECK_EQ(off, 0);
  reboot(&c);
  CHECK(found(&c, "/apps/jar.capp", JAR, 1, &off));
  CHECK_EQ(off, 0);
  CHECK_EQ(s_flash[XIP_HDR + 100], 0x5A);
  CHECK_EQ(s_bad_writes, 0);
}

/* A new firmware moves the arena: the entry must not be used. */
void test_xip_another_key_misses(void) {
  XipCache c;
  XipKey k;
  uint32_t off;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 2, NULL));        /* changed file */
  CHECK(!found(&c, "/apps/jab.capp", JAR, 1, NULL));        /* another file */
  k = key("/apps/jar.capp", JAR, 1);
  k.arena += 16;
  CHECK_EQ(xip_find(&c, &k, &off), XIP_MISS);
  k = key("/apps/jar.capp", JAR, 1);
  k.map_base += XIP_SECTOR * 16;
  CHECK_EQ(xip_find(&c, &k, &off), XIP_MISS);
}

static int s_live, s_dead;
static void count(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  (void)off; (void)in_use; (void)ctx;
  if (h->live == 0xFFFFFFFFu) s_live++; else s_dead++;
}

void test_xip_rewrite_kills_the_old_entry(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  install(&c, "/apps/todo.capp", 15044, 1, 0x22);
  install(&c, "/apps/jar.capp", JAR, 2, 0x33);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(found(&c, "/apps/jar.capp", JAR, 2, NULL));
  CHECK(found(&c, "/apps/todo.capp", 15044, 1, NULL));
  s_live = s_dead = 0;
  xip_each(&c, count, NULL);
  CHECK_EQ(s_live, 2);
  CHECK_EQ(s_dead, 1);
  CHECK_EQ(s_bad_writes, 0);
}

void test_xip_a_bad_crc_is_a_miss(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x5A);
  s_flash[XIP_HDR + 5000] = 0x00;
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
}

/* The battery pulled after each write in turn: code, header, commit. */
void test_xip_power_cut_leaves_nothing_half_valid(void) {
  int n;
  for (n = 0; n < 3; n++) {
    XipCache c;
    reset(&c);
    s_fail_after = n;
    CHECK(install(&c, "/apps/jar.capp", JAR, 1, 0x5A) < 0);
    reboot(&c);
    CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
    CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x5A) >= 0, 1);
    CHECK(found(&c, "/apps/jar.capp", JAR, 1, NULL));
    CHECK_EQ(s_bad_writes, 0);
  }
}

void test_xip_head_survives_a_reboot(void) {
  XipCache c;
  reset(&c);
  install(&c, "/a.capp", JAR, 1, 1);
  install(&c, "/b.capp", JAR, 1, 2);
  install(&c, "/c.capp", JAR, 1, 3);
  reboot(&c);
  CHECK_EQ(install(&c, "/d.capp", JAR, 1, 4), 21 * (long)XIP_SECTOR);
}

/* 18 entries of 7 sectors fill 126 of 128; the 19th wraps to 0 and takes
 * only the first entry with it. */
void test_xip_wraps_and_evicts_the_oldest(void) {
  XipCache c;
  char name[16];
  int i;
  reset(&c);
  for (i = 0; i < 18; i++) {
    snprintf(name, sizeof name, "/%02d.capp", i);
    CHECK_EQ(install(&c, name, JAR, 1, (uint8_t)i), (long)i * 7 * (long)XIP_SECTOR);
  }
  CHECK_EQ(install(&c, "/new.capp", JAR, 1, 0x77), 0);
  CHECK(!found(&c, "/00.capp", JAR, 1, NULL));
  CHECK(found(&c, "/01.capp", JAR, 1, NULL));
  CHECK(found(&c, "/17.capp", JAR, 1, NULL));
  CHECK(found(&c, "/new.capp", JAR, 1, NULL));
}

void test_xip_never_erases_an_entry_in_use(void) {
  XipCache c;
  char name[16];
  uint32_t off;
  int i;
  reset(&c);
  for (i = 0; i < 18; i++) {
    snprintf(name, sizeof name, "/%02d.capp", i);
    install(&c, name, JAR, 1, (uint8_t)i);
  }
  CHECK_EQ(xip_ref(&c, 0), XIP_OK);                 /* /00 is running */
  CHECK_EQ(xip_begin(&c, JAR, &off), XIP_ERR_BUSY);
  CHECK(found(&c, "/00.capp", JAR, 1, NULL));
  CHECK_EQ(xip_wipe(&c), XIP_ERR_BUSY);
  xip_unref(&c, 0);
  CHECK_EQ(xip_begin(&c, JAR, &off), XIP_OK);
  CHECK_EQ(off, 0);
  xip_abandon(&c);
}

/* Jar updated on the card while it runs from flash: the new version kills
 * the old entry, but the code under the running app is not touched. */
void test_xip_a_killed_entry_in_use_keeps_its_code(void) {
  XipCache c;
  uint8_t before[JAR];
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  memcpy(before, s_flash + XIP_HDR, JAR);
  CHECK_EQ(xip_ref(&c, 0), XIP_OK);
  CHECK(install(&c, "/apps/jar.capp", JAR, 2, 0x22) > 0);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(memcmp(before, s_flash + XIP_HDR, JAR) == 0);
  xip_unref(&c, 0);
}

/* The card changed under the cache (same size, same 1980 mtime): forget. */
void test_xip_forget_kills_by_path(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  install(&c, "/apps/todo.capp", 15044, 1, 0x22);
  CHECK_EQ(xip_forget(&c, xip_path_hash("/APPS/JAR.CAPP")), XIP_OK);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK(found(&c, "/apps/todo.capp", 15044, 1, NULL));
}

void test_xip_too_big_is_full(void) {
  XipCache c;
  uint32_t off;
  reset(&c);
  CHECK_EQ(xip_begin(&c, FSIZE, &off), XIP_ERR_FULL);
  CHECK_EQ(xip_begin(&c, 0, &off), XIP_ERR_FULL);
}

void test_xip_one_write_at_a_time(void) {
  XipCache c;
  uint32_t off;
  XipKey k = key("/a.capp", 100, 1);
  reset(&c);
  CHECK_EQ(xip_write(&c, 0, "x", 1), XIP_ERR_STATE);              /* nothing begun */
  CHECK_EQ(xip_commit(&c, &k, 0, "/a.capp"), XIP_ERR_STATE);
  CHECK_EQ(xip_begin(&c, 100, &off), XIP_OK);
  CHECK_EQ(xip_begin(&c, 100, &off), XIP_ERR_STATE);
  CHECK_EQ(xip_write(&c, XIP_SECTOR, "x", 1), XIP_ERR_STATE);     /* past the entry */
  xip_abandon(&c);
}

void test_xip_wipe(void) {
  XipCache c;
  reset(&c);
  install(&c, "/apps/jar.capp", JAR, 1, 0x11);
  CHECK_EQ(xip_wipe(&c), XIP_OK);
  CHECK(!found(&c, "/apps/jar.capp", JAR, 1, NULL));
  CHECK_EQ(install(&c, "/apps/jar.capp", JAR, 1, 0x11), 0);
}

/* Many laps of mixed sizes: every live entry the walk reports must still
 * check, i.e. no erase ever cut an entry and left its header standing. */
static XipCache *s_soak;
static int s_soak_bad;
static void soak_check(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  uint32_t got;
  (void)in_use; (void)ctx;
  if (h->live != 0xFFFFFFFFu) return;
  if (xip_find(s_soak, &h->key, &got) != XIP_OK || got != off) s_soak_bad++;
}
void test_xip_soak_every_live_entry_checks(void) {
  XipCache c;
  char name[16];
  uint32_t seed = 12345;
  int i;
  reset(&c);
  for (i = 0; i < 400; i++) {
    uint32_t size;
    seed = seed * 1103515245u + 12345u;
    size = 4 + ((seed >> 8) % (40u * 1024u)) / 4 * 4;
    snprintf(name, sizeof name, "/%02u.capp", (unsigned)((seed >> 4) % 30));
    CHECK(install(&c, name, size, (uint32_t)i, (uint8_t)i) >= 0);
  }
  s_soak = &c;
  s_soak_bad = 0;
  xip_each(&c, soak_check, NULL);
  CHECK_EQ(s_soak_bad, 0);
  CHECK_EQ(s_bad_writes, 0);
}
```

- [ ] **Step 3: Add `${ROOT}/kernel/app/xipcache.c` to `host/CMakeLists.txt`** (comment: `# the flash code cache; xipflash.c is the partition around it`). Run. Expected: link errors for `xip_*`.

- [ ] **Step 4: Write `kernel/app/xipcache.c`**

```c
/* The flash cache of relocated app code. See xipcache.h. */

#include "kernel/app/xipcache.h"

#include <stddef.h>
#include <string.h>

typedef char xip_hdr_is_128_bytes[sizeof(XipHdr) == XIP_HDR ? 1 : -1];

#define LIVE 0xFFFFFFFFu

uint32_t xip_crc32(uint32_t crc, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  int i;
  crc = ~crc;
  while (n--) {
    crc ^= *b++;
    for (i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint32_t xip_path_hash(const char *path) {
  uint32_t h = 0x811C9DC5u;
  for (; path && *path; path++) {
    char ch = *path;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    h = (h ^ (uint8_t)ch) * 0x01000193u;
  }
  return h;
}

static int overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return a < b + blen && b < a + alen;
}

static int committed(const XipCache *c, const XipHdr *h, uint32_t off) {
  return h->magic == XIP_MAGIC && h->commit == XIP_MAGIC && h->sectors > 0 &&
         h->sectors <= (c->f.size - off) / XIP_SECTOR &&
         XIP_HDR + h->key.code_size <= h->sectors * XIP_SECTOR;
}

/* Every committed entry in flash order; fn returns nonzero to stop, and
 * that is what walk returns. An uncommitted sector is stepped over one at a
 * time, which is how a torn write or the tail of an evicted entry reads. */
static int walk(XipCache *c, int (*fn)(XipCache *, const XipHdr *, uint32_t, void *),
                void *ctx) {
  uint32_t off = 0;
  XipHdr h;
  int r;
  while (off < c->f.size) {
    if (c->f.read(c->f.ctx, off, &h, sizeof h) != 0) return XIP_ERR_IO;
    if (committed(c, &h, off)) {
      if ((r = fn(c, &h, off, ctx)) != 0) return r;
      off += h.sectors * XIP_SECTOR;
    } else {
      off += XIP_SECTOR;
    }
  }
  return 0;
}

static int kill(XipCache *c, uint32_t off) {
  uint32_t zero = 0;
  return c->f.write(c->f.ctx, off + (uint32_t)offsetof(XipHdr, live), &zero, 4) == 0
         ? 0 : XIP_ERR_IO;
}

static int in_use(const XipCache *c, uint32_t off) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && c->use[i].off == off) return 1;
  return 0;
}

struct newest { uint32_t seq, end; int any; };
static int find_newest(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct newest *n = (struct newest *)ctx;
  (void)c;
  if (!n->any || h->seq > n->seq) {
    n->any = 1;
    n->seq = h->seq;
    n->end = off + h->sectors * XIP_SECTOR;
  }
  return 0;
}

int xip_open(XipCache *c, const XipFlash *f) {
  struct newest n;
  memset(c, 0, sizeof *c);
  c->f = *f;
  memset(&n, 0, sizeof n);
  if (walk(c, find_newest, &n) < 0) return XIP_ERR_IO;
  c->head = n.any && n.end < c->f.size ? n.end : 0;
  c->next_seq = n.any ? n.seq + 1 : 1;
  return XIP_OK;
}

static int code_crc(XipCache *c, uint32_t off, uint32_t n, uint32_t *crc) {
  uint8_t buf[256];
  uint32_t at = 0, k, x = 0;
  while (at < n) {
    k = n - at < sizeof buf ? n - at : (uint32_t)sizeof buf;
    if (c->f.read(c->f.ctx, off + XIP_HDR + at, buf, k) != 0) return -1;
    x = xip_crc32(x, buf, k);
    at += k;
  }
  *crc = x;
  return 0;
}

struct lookup { const XipKey *k; uint32_t off; };
static int match(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct lookup *l = (struct lookup *)ctx;
  uint32_t crc;
  if (h->live != LIVE || memcmp(&h->key, l->k, sizeof *l->k) != 0) return 0;
  if (code_crc(c, off, h->key.code_size, &crc) != 0 || crc != h->crc) return 0;
  l->off = off;
  return 1;
}

int xip_find(XipCache *c, const XipKey *k, uint32_t *off) {
  struct lookup l;
  l.k = k;
  l.off = 0;
  if (walk(c, match, &l) != 1) return XIP_MISS;
  *off = l.off;
  return XIP_OK;
}

int xip_begin(XipCache *c, uint32_t code_size, uint32_t *off) {
  uint32_t len, start;
  int i;
  if (c->pend_len) return XIP_ERR_STATE;
  if (code_size == 0 || code_size > c->f.size) return XIP_ERR_FULL;
  len = (XIP_HDR + code_size + XIP_SECTOR - 1) / XIP_SECTOR * XIP_SECTOR;
  if (len > c->f.size) return XIP_ERR_FULL;
  /* The head is the end of the newest entry, or 0: every entry with a
   * header before it ends at or before it, so an erase from here never
   * leaves a header standing over a hole. The soak test holds that. */
  start = c->head + len > c->f.size ? 0 : c->head;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && overlap(c->use[i].off, c->use[i].len, start, len))
      return XIP_ERR_BUSY;
  if (c->f.erase(c->f.ctx, start, len) != 0) return XIP_ERR_IO;
  c->pend_off = start;
  c->pend_len = len;
  *off = start;
  return XIP_OK;
}

int xip_write(XipCache *c, uint32_t at, const void *buf, uint32_t n) {
  uint32_t room;
  if (!c->pend_len) return XIP_ERR_STATE;
  room = c->pend_len - XIP_HDR;
  if (at > room || n > room - at) return XIP_ERR_STATE;
  return c->f.write(c->f.ctx, c->pend_off + XIP_HDR + at, buf, n) == 0 ? XIP_OK : XIP_ERR_IO;
}

struct same { uint32_t hash, self; };
static int kill_same(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  const struct same *s = (const struct same *)ctx;
  if (off == s->self || h->live != LIVE || h->key.path_hash != s->hash) return 0;
  return kill(c, off);
}

int xip_commit(XipCache *c, const XipKey *k, uint32_t crc, const char *path) {
  XipHdr h;
  uint32_t magic = XIP_MAGIC;
  struct same s;
  size_t n;
  if (!c->pend_len || XIP_HDR + k->code_size > c->pend_len) return XIP_ERR_STATE;
  memset(&h, 0xFF, sizeof h);              /* what is not written stays erased */
  h.magic = XIP_MAGIC;
  h.seq = c->next_seq;
  h.sectors = c->pend_len / XIP_SECTOR;
  h.key = *k;
  h.crc = crc;
  memset(h.path, 0, sizeof h.path);
  if (path) {                              /* the tail: the name is at the end */
    n = strlen(path);
    if (n > XIP_PATH_MAX - 1) path += n - (XIP_PATH_MAX - 1);
    memcpy(h.path, path, strlen(path));
  }
  if (c->f.write(c->f.ctx, c->pend_off, &h, sizeof h) != 0) return XIP_ERR_IO;
  if (c->f.write(c->f.ctx, c->pend_off + (uint32_t)offsetof(XipHdr, commit), &magic, 4) != 0)
    return XIP_ERR_IO;
  s.hash = k->path_hash;
  s.self = c->pend_off;
  c->head = c->pend_off + c->pend_len;
  if (c->head >= c->f.size) c->head = 0;
  c->next_seq++;
  c->pend_len = 0;
  return walk(c, kill_same, &s) < 0 ? XIP_ERR_IO : XIP_OK;
}

void xip_abandon(XipCache *c) { c->pend_len = 0; }

int xip_ref(XipCache *c, uint32_t off) {
  XipHdr h;
  int i, slot = -1;
  for (i = 0; i < XIP_INUSE_MAX; i++) {
    if (c->use[i].refs && c->use[i].off == off) { c->use[i].refs++; return XIP_OK; }
    if (!c->use[i].refs && slot < 0) slot = i;
  }
  if (slot < 0) return XIP_ERR_BUSY;
  if (c->f.read(c->f.ctx, off, &h, sizeof h) != 0 || !committed(c, &h, off))
    return XIP_ERR_STATE;
  c->use[slot].off = off;
  c->use[slot].len = h.sectors * XIP_SECTOR;
  c->use[slot].refs = 1;
  return XIP_OK;
}

void xip_unref(XipCache *c, uint32_t off) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++)
    if (c->use[i].refs && c->use[i].off == off) { c->use[i].refs--; return; }
}

static int kill_hash(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  if (h->live != LIVE || h->key.path_hash != *(const uint32_t *)ctx) return 0;
  return kill(c, off);
}

int xip_forget(XipCache *c, uint32_t path_hash) {
  return walk(c, kill_hash, &path_hash) < 0 ? XIP_ERR_IO : XIP_OK;
}

int xip_wipe(XipCache *c) {
  int i;
  for (i = 0; i < XIP_INUSE_MAX; i++) if (c->use[i].refs) return XIP_ERR_BUSY;
  if (c->f.erase(c->f.ctx, 0, c->f.size) != 0) return XIP_ERR_IO;
  c->head = 0;
  c->next_seq = 1;
  c->pend_len = 0;
  return XIP_OK;
}

struct each { void (*fn)(const XipHdr *, uint32_t, int, void *); void *ctx; };
static int each_one(XipCache *c, const XipHdr *h, uint32_t off, void *ctx) {
  struct each *e = (struct each *)ctx;
  e->fn(h, off, in_use(c, off), e->ctx);
  return 0;
}

int xip_each(XipCache *c, void (*fn)(const XipHdr *h, uint32_t off, int in_use, void *ctx),
             void *ctx) {
  struct each e;
  e.fn = fn;
  e.ctx = ctx;
  return walk(c, each_one, &e) < 0 ? XIP_ERR_IO : XIP_OK;
}
```

- [ ] **Step 5: Run the suite.** Expected: all `test_xip_*` ok, `s_bad_writes` 0 everywhere. If the soak test finds a bad entry, the head invariant in `xip_begin` is wrong. Fix `xip_begin`, not the test.

- [ ] **Step 6: Add `"../kernel/app/xipcache.c"` to `src/CMakeLists.txt`**, run `python -m platformio run` (it must compile under `-Werror` for the device), and commit:

```bash
git add kernel/app/xipcache.h kernel/app/xipcache.c test/test_xipcache.c host/CMakeLists.txt src/CMakeLists.txt
git commit -m "XIP cache: a crash-safe ring of relocated app code over a flash"
```

---

### Task 5: The partition, its glue, and the commands

**Files:**
- Create: `kernel/app/xipflash.h`, `kernel/app/xipflash.c`
- Modify: `partitions.csv`, `src/CMakeLists.txt`, `src/main.c` (boot near `if (fs_mount() == 0) {`; the command dispatch near line 298; the completion list near line 464; the help text beside `mem`), `kernel/sys/memreport.c`, `kernel/app/capprun.c:440` (`apps_changed`)

**Interfaces:**
- Consumes: `xipcache.h` (Task 4), `arena.h` (Task 2).
- Produces:
  - `int xipflash_init(void);`: 0, or -1 when there is no `appcode` partition or it cannot be mapped.
  - `int xipflash_ready(void);`, `XipCache *xipflash_cache(void);` (NULL when not ready), `uint32_t xipflash_base(void);` (the INST address of offset 0).
  - `void xipflash_forget(const char *path);`: for the fs change hook; ignores paths not ending `.capp`.
  - `void xipflash_usage(uint32_t *live_sectors, uint32_t *total_sectors);`

- [ ] **Step 1: The partition table.** Replace the `spiffs` line in `partitions.csv` and add `appcode`:

```
spiffs,     data, spiffs,   0x751000, 0x2F000,
appcode,    data, 0x40,     0x780000, 0x80000,
```
Update the sizes block in the comment above it:
```
# Sizes, revised 2026-10-09 for app code in flash
# (docs/superpowers/specs/2026-10-09-xip-app-code-design.md):
#
#   factory  2.75 MB   the one that grows
#   ota_0    2.25 MB   a guest, or CardOS updating itself
#   ota_1    2.25 MB   the other half of the A/B pair
#   spiffs    188 KB   Arduino guests' LittleFS: it must exist and mount, and
#                      keeps its label and offset so every guest still finds it
#   appcode   512 KB   relocated app code, mapped and run in place; a cache,
#                      so erasing it is always safe. 64 KB-aligned, whole MMU
#                      pages. OTA cannot add it: a device needs one full USB
#                      flash, and until then app code loads into RAM as before.
```

- [ ] **Step 2: Write `kernel/app/xipflash.h`**

```c
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
```

- [ ] **Step 3: Write `kernel/app/xipflash.c`**

```c
/* The appcode partition. See xipflash.h. */

#include "kernel/app/xipflash.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "xip";

#define APPCODE_SUBTYPE 0x40          /* partitions.csv */

static const esp_partition_t       *s_part;
static esp_partition_mmap_handle_t  s_map;
static const void                  *s_base;
static XipCache                     s_cache;
static int                          s_ready;

static int p_read(void *ctx, uint32_t off, void *buf, uint32_t n) {
  (void)ctx;
  return esp_partition_read(s_part, off, buf, n) == ESP_OK ? 0 : -1;
}
/* IDF flushes the cache over what it wrote (flash_end_flush_cache in
 * esp_flash_api.c), so a rewritten entry is not served stale -- checked on
 * the device in Task 8, not assumed. */
static int p_write(void *ctx, uint32_t off, const void *buf, uint32_t n) {
  (void)ctx;
  return esp_partition_write(s_part, off, buf, n) == ESP_OK ? 0 : -1;
}
static int p_erase(void *ctx, uint32_t off, uint32_t n) {
  (void)ctx;
  return esp_partition_erase_range(s_part, off, n) == ESP_OK ? 0 : -1;
}

int xipflash_init(void) {
  XipFlash f;
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)APPCODE_SUBTYPE, "appcode");
  if (!s_part) {
    ESP_LOGW(TAG, "no appcode partition: app code loads into RAM (a full USB flash adds it)");
    return -1;
  }
  /* Once, for the uptime, without BLOCKS_WRITE -- that would refuse our own
   * writes for as long as it is mapped, which is forever. Boot maps in the
   * same order every time, so the address is the same across boots of one
   * build; an entry records it and is rewritten if it ever moves. */
  if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_INST,
                         &s_base, &s_map) != ESP_OK) {
    ESP_LOGE(TAG, "cannot map appcode: app code loads into RAM");
    return -1;
  }
  f.read = p_read;
  f.write = p_write;
  f.erase = p_erase;
  f.ctx = NULL;
  f.size = (uint32_t)s_part->size;
  if (xip_open(&s_cache, &f) != XIP_OK) {
    ESP_LOGE(TAG, "cannot read appcode: app code loads into RAM");
    return -1;
  }
  s_ready = 1;
  ESP_LOGI(TAG, "appcode %u KB mapped at %p, head %u", (unsigned)(f.size / 1024), s_base,
           (unsigned)s_cache.head);
  return 0;
}

int xipflash_ready(void) { return s_ready; }
XipCache *xipflash_cache(void) { return s_ready ? &s_cache : NULL; }
uint32_t xipflash_base(void) { return (uint32_t)(uintptr_t)s_base; }

void xipflash_forget(const char *path) {
  size_t n;
  if (!s_ready || !path) return;
  n = strlen(path);
  if (n < 5 || strcasecmp(path + n - 5, ".capp") != 0) return;
  xip_forget(&s_cache, xip_path_hash(path));
}

static void add_live(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  (void)off; (void)in_use;
  if (h->live == 0xFFFFFFFFu) *(uint32_t *)ctx += h->sectors;
}

void xipflash_usage(uint32_t *live_sectors, uint32_t *total_sectors) {
  *live_sectors = 0;
  *total_sectors = s_ready ? s_cache.f.size / XIP_SECTOR : 0;
  if (s_ready) xip_each(&s_cache, add_live, live_sectors);
}
```
(Add `#include <strings.h>` for `strcasecmp`. If picolibc lacks it, fold the five bytes by hand, as `apps_changed` does.)

- [ ] **Step 4: Boot.** In `src/main.c`, `#include "kernel/app/xipflash.h"` and call it just before `if (fs_mount() == 0) {`. It must run before anything can load an app:

```c
  xipflash_init();               /* before any app loads: the code cache */
```

- [ ] **Step 5: The fs hook.** In `kernel/app/capprun.c`, `#include "kernel/app/xipflash.h"`. Then make the first line of `apps_changed` (line 441, before the `/apps` filter, since a `.capp` can run from anywhere):

```c
  xipflash_forget(path);                  /* cached code for a changed .capp is stale */
```

- [ ] **Step 6: The commands.** In `src/main.c`, add `#include "kernel/app/arena.h"`, then:

```c
/* `xip`: the code cache, entry by entry. `xip wipe` empties it, which is
 * always safe -- the next launch of each app writes its entry again. */
static void xip_row(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  int stale = h->key.map_base != xipflash_base() || h->key.arena != (uint32_t)arena_addr();
  (void)ctx;
  con_printf("  %3u %-23.23s %3u KB seq %u%s%s%s\n", (unsigned)(off / XIP_SECTOR), h->path,
             (unsigned)(h->sectors * XIP_SECTOR / 1024), (unsigned)h->seq,
             h->live == 0xFFFFFFFFu ? "" : " dead", stale ? " stale" : "",
             in_use ? " in use" : "");
}

static void cmd_xip(const char *arg) {
  XipCache *c = xipflash_cache();
  if (!c) { con_write("no appcode partition: app code loads into RAM\n"); return; }
  if (arg && !strcmp(arg, "wipe")) {
    con_write(xip_wipe(c) == XIP_OK ? "appcode wiped\n"
                                    : "an app is running from it: close it first\n");
    return;
  }
  xip_each(c, xip_row, NULL);
}
```
Dispatch, beside `mem` (line 298):
```c
  else if (!strcmp(line, "xip"))    cmd_xip(arg);
```
Add `"xip"` to the completion list (line 464) in alphabetical order. Add a help line beside `mem`'s: `xip [wipe]  the app code cache in flash`.

- [ ] **Step 7: `mem`.** In `kernel/sys/memreport.c`, `#include "kernel/app/arena.h"` and `"kernel/app/xipflash.h"`. After the `low water` line add:

```c
  /* The arena is always 28 KB out of the heap; say who has it, because
   * "heap free" alone no longer says what an open app costs. */
  snprintf(b, sizeof b, "arena     %6u  %s", (unsigned)ARENA_SIZE,
           arena_held() ? arena_holder() : "free");
  out(b, ctx);
  if (xipflash_ready()) {
    uint32_t live, total;
    xipflash_usage(&live, &total);
    snprintf(b, sizeof b, "appcode   %u of %u sectors live", (unsigned)live, (unsigned)total);
  } else {
    snprintf(b, sizeof b, "appcode   none (code in RAM)");
  }
  out(b, ctx);
```

- [ ] **Step 8: Add `"../kernel/app/xipflash.c"` to `src/CMakeLists.txt`. Build:** `python tools/build_apps.py && python -m platformio run`. Expected: it builds. Do **not** flash yet: the table change needs the full flash in Task 8, once the loader is in.

- [ ] **Step 9: Host suite** (0 failures; nothing host-side changed but the build must still pass), then commit:

```bash
git add partitions.csv kernel/app/xipflash.h kernel/app/xipflash.c src/main.c src/CMakeLists.txt kernel/sys/memreport.c kernel/app/capprun.c
git commit -m "appcode partition (512 KB from spiffs), mapped at boot; xip command; mem lines"
```

---

### Task 6: The loader: data in the arena, code from flash

**Files:**
- Modify: `kernel/app/elfload.h`, `kernel/app/elfload.c`, `kernel/app/capprun.c` (`ensure_loaded` at 720, its callers at 962 and 1107), `kernel/ui/launchui.c` (a prepare note)

**Interfaces:**
- Consumes: `arena_claim/arena_release/arena_owns/arena_code_in_flash` (Task 2); `relocate` and `capprel.h` (Task 3); `xip_find/xip_begin/xip_write/xip_commit/xip_abandon/xip_ref/xip_unref/xip_crc32/xip_path_hash`, `XIP_HDR` (Task 4); `xipflash_ready/xipflash_cache/xipflash_base` (Task 5).
- Produces:
  - `CappResult capp_load_ex(const char *path, LoadedApp *out, int foreground);`, with `capp_load(path, out)` kept as `capp_load_ex(path, out, 0)`.
  - `LoadedApp` gains `int32_t xip_off;` (-1 when code is in RAM) and `uint8_t data_in_arena;`.
  - `void capp_on_prepare(void (*fn)(const char *path));`: called once before a cache write.

- [ ] **Step 1: `elfload.h`.** Add to `LoadedApp` after `code_cap`:

```c
  /* Where the halves went: code from the flash cache (xip_off is its entry,
   * code is the mapped address) or executable RAM (-1); data in the arena
   * or on the heap. See docs/superpowers/specs/2026-10-09-xip-app-code-design.md. */
  int32_t         xip_off;
  uint8_t         data_in_arena;
```
After the `capp_load` declaration add:

```c
/* As capp_load, for the app being started on screen when foreground is 1:
 * its data goes in the arena if it is free and fits, and its code then runs
 * from the flash cache unless it says CAPP_CODE_IN_RAM. Anything that
 * cannot be done that way is done as before. */
CappResult capp_load_ex(const char *path, LoadedApp *out, int foreground);

/* Called once before the cache writes an app's code (a few hundred
 * milliseconds, the screen frozen during each erase), so the launcher can
 * say "preparing". */
void capp_on_prepare(void (*fn)(const char *path));
```

- [ ] **Step 2: `elfload.c`: includes, the hook, `xip_place`.** Add includes for `arena.h`, `xipcache.h`, `xipflash.h`, then:

```c
static void (*s_on_prepare)(const char *path);
void capp_on_prepare(void (*fn)(const char *path)) { s_on_prepare = fn; }

/* The code into the flash cache: this file's entry if there is one, else
 * relocated a window at a time through `scratch` -- the arena, which holds
 * nothing yet; this is a launch that is only now claiming it -- and written
 * as a new entry. The whole point is that the code may not fit in one piece
 * of RAM, so it is never asked to. */
static CappResult xip_place(int fd, const char *path, const Elf32_Shdr *sh, int shnum,
                            int code_sec, uint32_t code_size, uint8_t *scratch,
                            uint32_t data_base, uint32_t *off_out) {
  XipCache *c = xipflash_cache();
  FsStat st;
  XipKey k;
  uint32_t off, at, n, crc = 0, code_base;
  CappResult rc;

  if (!c || fs_stat(path, &st) != 0) return CAPP_ERR_OPEN;
  memset(&k, 0, sizeof k);
  k.path_hash = xip_path_hash(path);
  k.file_size = st.size;
  k.file_mtime = st.mtime;
  k.api = CAPP_API_VERSION;
  k.code_size = code_size;
  k.map_base = xipflash_base();
  k.arena = data_base;
  if (xip_find(c, &k, &off) == XIP_OK) goto found;

  if (s_on_prepare) s_on_prepare(path);
  if (xip_begin(c, code_size, &off) != XIP_OK) {
    applogf("xip", "%s: no room in appcode, code to RAM", path);
    return CAPP_ERR_NO_MEMORY;
  }
  code_base = xipflash_base() + off + XIP_HDR;
  for (at = 0; at < code_size; at += n) {
    n = code_size - at < ARENA_SIZE ? code_size - at : ARENA_SIZE;
    if (read_at(fd, sh[code_sec].sh_offset + at, scratch, n) != 0) { rc = CAPP_ERR_NOT_ELF; goto fail; }
    rc = relocate(fd, sh, shnum, code_sec, 1, scratch, at, n, code_size, code_base, data_base);
    if (rc != CAPP_OK) goto fail;
    crc = xip_crc32(crc, scratch, n);
    if (xip_write(c, at, scratch, n) != XIP_OK) { rc = CAPP_ERR_NO_MEMORY; goto fail; }
  }
  if (xip_commit(c, &k, crc, path) != XIP_OK) { rc = CAPP_ERR_NO_MEMORY; goto fail; }
  applogf("xip", "%s: %u bytes of code to flash at %u", path, (unsigned)code_size, (unsigned)off);
found:
  if (xip_ref(c, off) != XIP_OK) return CAPP_ERR_NO_MEMORY;
  *off_out = off;
  return CAPP_OK;
fail:
  xip_abandon(c);
  applogf("xip", "%s: cache write failed (%s), code to RAM", path, capp_strerror(rc));
  return rc;
}
```

- [ ] **Step 3: `elfload.c`: rewrite `capp_load` as `capp_load_ex` in the new order.** Keep the header parse, section classification, truncation and size checks exactly as they are (down to the `CAPP_ERR_TOO_BIG` check). Then:

  (a) Move the symbol lookup up: lift the existing loop that finds `capp_main`/`capp_info` into `static CappResult find_symbols(int fd, const Elf32_Ehdr *eh, const Elf32_Shdr *sh, uint32_t *main_off, uint32_t *info_off)` returning `CAPP_ERR_NO_ENTRY` when either is missing, and call it here. Then:

```c
  if (main_off >= code_size || data_sec < 0 || sh[data_sec].sh_type != SHT_PROGBITS ||
      info_off < CAPP_DATA_ORIGIN ||
      info_off - CAPP_DATA_ORIGIN + sizeof(CappInfo) > data_size) {
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
```
  (`uint16_t head[2];` declared at the top.)

  (b) Data placement:

```c
  s_short_want = s_short_largest = 0;
  {
    const char *base = strrchr(path, '/');
    if (foreground &&
        (data = arena_claim(data_size, sh[data_sec].sh_addralign, base ? base + 1 : path)) != NULL)
      in_arena = 1;
  }
  if (!in_arena) {
    data = heap_caps_malloc(data_size, MALLOC_CAP_8BIT);
    if (!data) {
      /* (the existing data-shortfall ESP_LOGE / applogf block, unchanged) */
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
  }
  data_base = (uint32_t)(uintptr_t)data;
```

  (c) Code placement:

```c
  if (arena_code_in_flash(in_arena, xipflash_ready(), head[1]) &&
      sh[code_sec].sh_addralign <= XIP_HDR &&
      xip_place(fd, path, sh, eh.e_shnum, code_sec, code_size, data, data_base, &xip_off) == CAPP_OK) {
    in_flash = 1;
    code = (uint8_t *)(uintptr_t)(xipflash_base() + xip_off + XIP_HDR);
  } else {
    code = code_alloc(code_size, &code_cap);
    if (!code) {
      /* (the existing exec-shortfall ESP_LOGE / applogf block, unchanged) */
      rc = CAPP_ERR_NO_MEMORY;
      goto done;
    }
    code_w = writable(code);
    if (read_at(fd, sh[code_sec].sh_offset, code_w, code_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
    rc = relocate(fd, sh, eh.e_shnum, code_sec, 1, code_w, 0, code_size, code_size,
                  (uint32_t)(uintptr_t)code, data_base);
    if (rc != CAPP_OK) goto done;
  }
  code_base = (uint32_t)(uintptr_t)code;
```

  (d) Data last. The arena may hold scratch code from a first launch, so clear it first:

```c
  memset(data, 0, data_size);
  if (read_at(fd, sh[data_sec].sh_offset, data, data_size) != 0) { rc = CAPP_ERR_NOT_ELF; goto done; }
  rc = relocate(fd, sh, eh.e_shnum, data_sec, 0, data, 0, data_size, data_size, code_base, data_base);
  if (rc != CAPP_OK) goto done;

  out->info = (const CappInfo *)(data + (info_off - CAPP_DATA_ORIGIN));
  out->main = (int (*)(const CardApi *, int, char **))(code + main_off);
  out->code = code;
  out->data = data;
  out->code_size = code_size;
  out->code_cap = code_cap;
  out->data_size = data_size;
  out->xip_off = in_flash ? (int32_t)xip_off : -1;
  out->data_in_arena = (uint8_t)in_arena;
  rc = CAPP_OK;
```

  (e) Cleanup:

```c
done:
  fs_close(fd);
  free(sh);
  if (rc != CAPP_OK) {
    if (in_flash) xip_unref(xipflash_cache(), xip_off);
    else code_free(code, code_cap);
    if (in_arena) arena_release(data);
    else if (data) heap_caps_free(data);
  }
  if (rc == CAPP_OK)
    ESP_LOGI(TAG, "loaded %s (%s): %u code (%s), %u data (%s), exec free %u",
             path, out->info->name, (unsigned)out->code_size, in_flash ? "flash" : "RAM",
             (unsigned)out->data_size, in_arena ? "arena" : "heap",
             (unsigned)capp_exec_free());
  else
    ESP_LOGW(TAG, "load %s failed: %s", path, capp_strerror(rc));
  return rc;
}

CappResult capp_load(const char *path, LoadedApp *out) { return capp_load_ex(path, out, 0); }
```
  Declare at the top of the function: `uint32_t main_off = 0, info_off = 0, xip_off = 0, code_base, data_base;`, `int in_arena = 0, in_flash = 0;`, `uint16_t head[2];`. Delete the old symbol block and the old "handed over" tail; the cleanup above replaces it. `memset(out, 0, ...)` at the top stays, so set `out->xip_off = -1` right after it.

- [ ] **Step 4: `capp_unload`**

```c
void capp_unload(LoadedApp *la) {
  if (!la) return;
  if (la->xip_off >= 0) xip_unref(xipflash_cache(), (uint32_t)la->xip_off);
  else code_free(la->code, la->code_cap);
  if (la->data_in_arena) arena_release(la->data);
  else if (la->data) heap_caps_free(la->data);
  memset(la, 0, sizeof *la);
  la->xip_off = -1;
}
```

- [ ] **Step 5: `capprun.c`.** Change `static int ensure_loaded(Run *s)` to `static int ensure_loaded(Run *s, int foreground)`, and both `capp_load(s->entry->path, &s->la)` calls inside it to `capp_load_ex(s->entry->path, &s->la, foreground)`. The start path (line 962) passes `1`; the headless command path (line 1107) passes `0`. The icon scan's `capp_load` (line 614) is untouched and so never takes the arena. In `make_room`'s comment, add one line: "Data in the arena never needs this; only a RAM code block or a heap data block can be what failed."

- [ ] **Step 6: `launchui.c`: "preparing".** Add near `said_why`:

```c
/* A first launch from the flash cache writes the app's code (a few hundred
 * milliseconds, frozen during each erase): say so on the row first. */
static void preparing(const char *path) {
  const char *base = strrchr(path, '/');
  snprintf(s_note, sizeof s_note, "preparing %s...", base ? base + 1 : path);
  s_dirty = 1;
  flush();
}
```
and register it once in `launchui_init`: `capp_on_prepare(preparing);` (include `kernel/app/elfload.h` if not already). Clear `s_note` after a successful start, as the existing path already does for errors. If it does not, set `s_note[0] = 0; s_dirty = 1;` after `run_now` succeeds.

- [ ] **Step 7: Build:** `python tools/build_apps.py && python -m platformio run`. Expected: builds with no new warnings. Host suite: 0 failures.

- [ ] **Step 8: Commit**

```bash
git add kernel/app/elfload.h kernel/app/elfload.c kernel/app/capprun.c kernel/ui/launchui.c
git commit -m "Loader: the app on screen keeps its data in the arena and runs its code from flash"
```

---

### Task 7: The build budget

**Files:**
- Modify: `tools/build_apps.py:193-245`

**Interfaces:**
- Consumes: `read_info(elf)` → `(flags, name, icon)` (already in the file); `CAPP_CODE_IN_RAM = 0x0040` (Task 2).

- [ ] **Step 1: Replace the budget block** (from `# The size budget.` through the end of `check_budget`):

```python
# The size budget, since app code runs from flash
# (docs/superpowers/specs/2026-10-09-xip-app-code-design.md). An app's data
# goes in the 28 KB arena when it is the one on screen -- ARENA_SIZE in
# kernel/app/arena.h -- and its code into the flash cache, so neither needs
# a piece of a heap that breaks up over hours. An app that keeps its code in
# RAM (CAPP_CODE_IN_RAM, for an inner loop) still needs one block of
# executable RAM, and after a day the largest was 24 KB.
DATA_BUDGET = 28 * 1024          # ARENA_SIZE
RAM_CODE_BUDGET = 16 * 1024      # CAPP_CODE_IN_RAM apps
MAX_CODE = 96 * 1024             # CAPP_MAX_CODE in kernel/app/elfload.c
CAPP_CODE_IN_RAM = 0x0040

# Apps allowed over the budget for now, each with why. Take one off as soon
# as it is back under -- the build says when.
OVER_BUDGET = {
}


def check_budget(built):
    """Print code and data per app, write build/apps/sizes.txt, and refuse an
    app over budget that is not in OVER_BUDGET."""
    rows, bad = [], []
    for name, elf in built:
        code, dat = code_data(elf)
        flags = read_info(elf)[0]
        in_ram = bool(flags & CAPP_CODE_IN_RAM)
        over = []
        if dat > DATA_BUDGET:
            over.append("data %d > %d (the arena)" % (dat, DATA_BUDGET))
        if in_ram and code > RAM_CODE_BUDGET:
            over.append("code %d > %d for CAPP_CODE_IN_RAM" % (code, RAM_CODE_BUDGET))
        if code > MAX_CODE:
            over.append("code %d > %d" % (code, MAX_CODE))
        rows.append((name, code, dat, in_ram, over))
    with open(os.path.join(OUT, "sizes.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# app        code    data  where  (bytes; data <= %d, RAM code <= %d)\n"
                % (DATA_BUDGET, RAM_CODE_BUDGET))
        for name, code, dat, in_ram, over in rows:
            f.write("%-10s %7d %7d  %s%s\n" % (name, code, dat, "ram  " if in_ram else "flash",
                                              "  OVER" if over else ""))
    print("  %s" % os.path.relpath(os.path.join(OUT, "sizes.txt"), ROOT))
    for name, code, dat, in_ram, over in rows:
        if over and name in OVER_BUDGET:
            print("  warning: %s is over budget (%s), allowed: %s"
                  % (name, "; ".join(over), OVER_BUDGET[name]))
        elif over:
            bad.append("%s: %s" % (name, "; ".join(over)))
        elif name in OVER_BUDGET:
            print("  note: %s is under budget now -- take it out of OVER_BUDGET" % name)
    if bad:
        raise SystemExit(
            "over the app size budget (data must fit the 28 KB arena; "
            "see OVER_BUDGET in tools/build_apps.py):\n  " + "\n  ".join(bad))
```

- [ ] **Step 2: Run it:** `python tools/build_apps.py`. Expected: it succeeds; `build/apps/sizes.txt` lists Forklift and Jar as `flash` with no OVER. No warning about Forklift (it is no longer in `OVER_BUDGET`).

- [ ] **Step 3: Check that it refuses what it should.** Temporarily set `DATA_BUDGET = 20 * 1024`, run, and expect a `SystemExit` naming notes, midi and others. Revert.

- [ ] **Step 4: Commit**

```bash
git add tools/build_apps.py
git commit -m "Build budget: data fits the arena; only RAM-code apps have a code cap"
```

---

### Task 8: On the device: flash, verify, measure, decide the opt-outs, document

Ask alex before Step 1. It is the one-time full flash that changes the partition table.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-09-xip-app-code-design.md` (Measurements), `CLAUDE.md` (a paragraph), possibly `apps/kart.c`, `apps/pinball.c`, `apps/noodle.c`, `apps/calc.c` (the flag)

- [ ] **Step 1: Full flash with the new table**

```bash
python tools/build_apps.py
python -m platformio run -t upload --upload-port COM3
```
Then `python tools/cardctl.py sh "mem"`. Expect `arena 28672 free` and `appcode 0 of 128 sectors live`. `python tools/cardctl.py sh "env"` and `wifi`: if NVS was lost, the `/config` mirrors should have restored it on boot. Say in the report what came back and what did not.

- [ ] **Step 2: First and second launch**

`open jar`, then `log`: expect `xip ... bytes of code to flash`. `state` should show Jar running. `sh "mem"` should show `arena 28672 jar.capp` and heap free about the same as idle. `key quit`, `open jar` again: no new `xip` log line, which is a hit. Time both: run `python -c "import time,subprocess;t=time.time();subprocess.run(['python','tools/cardctl.py','open','jar']);print(time.time()-t)"` and compare with the same on master from Task 1 if taken; otherwise note it.

- [ ] **Step 3: The stale-cache check (spec §5)**

1. With Jar closed: `put` a rebuilt `jar.capp` with one visible change (e.g. its title string) to `/apps/...jar.capp`.
2. `open jar`. Expect a new `xip` write in `log`, the change on screen, and `xip` listing the old entry `dead`.
3. `xip wipe`, `open jar` again: a write, then it runs.

- [ ] **Step 4: Opening apps on top of each other.** In the desktop (`sh "desk"`), open two windows: the second has to load on the RAM path and run. In the launcher, an app that opens another (Notes → Edit) must work, with fn-` going back.

- [ ] **Step 5: Headless commands after churn (added at approval).** Churn as in Task 1: three `print test`, three Todo opens. Then `open today`. All sections must gather. `log` must show Calendar/Todo/Habits loaded with `code (RAM)` / `data (heap)` and no failure. If one fails, put its "needs N KB in one piece" line in the spec.

- [ ] **Step 6: Measurements (added at approval).** Repeat Task 1 Steps 2–3 exactly on this firmware, and fill the `xip` rows of the spec's Measurements table.

- [ ] **Step 7: Hot loops.** In `apps/pinball.c`, `apps/noodle.c` and `apps/kart.c` (and Calc's graph redraw), add a temporary counter in `tick`. Count the ticks that return 1 (repaints) over 5 s with `api->ticks_ms()`, and `api->log` the rate. Do not commit this. Run each app for 15 s from flash, then with `CAPP_CODE_IN_RAM` added to its `capp_info` flags (rebuild, `put` it). Record both rates. **Set the flag permanently only where flash is more than 10% slower.** Remove the counters. Put the numbers in the spec under Measurements.

- [ ] **Step 8: Power pull.** Ask alex to do this physically. `xip wipe`, `open forklift`, and pull the power while "preparing" shows. After boot, `xip` should show no forklift entry or a `dead` one, and `open forklift` should write again and run.

- [ ] **Step 9: CLAUDE.md.** Add a paragraph after "Boot does not load the apps any more":

```markdown
**App code runs from flash** (2026-10-09). The app on screen keeps its data
in a fixed 28 KB arena (`kernel/app/arena.c`) and its code in the `appcode`
partition (512 KB, from spiffs), relocated once on first launch and mapped
into the instruction bus (`kernel/app/xipcache.c` is the ring, host-tested;
`xipflash.c` the partition). The arena is fixed because an app's literals,
inside its code, hold its data's addresses: flash code is valid for one data
address. `xip` lists the cache, `xip wipe` empties it (always safe), `mem`
shows both. Headless commands, the icon scan and a second desktop window
load as before (heap data, RAM code). `CAPP_CODE_IN_RAM` keeps an inner
loop in RAM; the budget is now data <= 28 KB, and code <= 16 KB only for
those. A device updated over the air from the old table has no `appcode`
and loads everything as before; it takes one full USB flash. Design:
`docs/superpowers/specs/2026-10-09-xip-app-code-design.md`.
```
In the "Flash is the constraint" and budget paragraphs, change "44 KB total / 28 KB data" wording where it appears to point at this paragraph.

- [ ] **Step 10: Commit**

```bash
git add docs/superpowers/specs/2026-10-09-xip-app-code-design.md CLAUDE.md apps/*.c
git commit -m "XIP on the device: measured under load, opt-outs decided, documented"
```
The commit message body carries the measurement table's key numbers: idle heap, heap with Jar open, launch times first and later, and hot-loop rates.
