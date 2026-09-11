# CardOS Memory Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the handle-based memory manager and swap allocator as portable C with a full host test suite, before any device code exists.

**Architecture:** Two portable modules. `kernel/mem` owns a single heap arena: relocatable handle-addressed blocks bump upward from the bottom, unmovable `MEM_FIXED` blocks allocate downward from the top, and the gap between them is the free space. Compaction slides unpinned movable blocks down to close gaps; when compaction is not enough, least-recently-used unlocked blocks are evicted to `kernel/swap`, which owns a bitmap page allocator over an extent-mapped backing store reached through a `SwapDev` function-pointer interface. On the host that interface is backed by RAM; on the device it will be backed by SD sectors. Neither module includes an ESP-IDF header.

**Tech Stack:** C11, CMake 3.20+, Ninja, MSVC 19.44 (host tests); ESP-IDF later for the device. No third-party test framework — a 40-line assert header lives in `test/tinytest.h`.

**Spec:** `docs/specs/2026-09-10-kernel-core-design.md`

## Global Constraints

- **Portable C11 only** in `kernel/mem` and `kernel/swap`. No ESP-IDF, no FreeRTOS, no `malloc`, no `stdio` outside tests. These files compile for both host and Xtensa.
- **Handle is `uint16_t`:** low 8 bits index, high 8 bits generation. Generation is never 0, so a live handle is never 0 and `0` stays the invalid handle.
- **256 handle descriptors**, 16 bytes each = 4 KB. The descriptor stores a `uint32_t` heap *offset*, not a pointer, so its layout is identical on a 64-bit host and a 32-bit device.
- **Maximum block size is 32 KB** (`MEM_MAX_BLOCK`). Larger requests fail. This is what makes a page-in provably satisfiable.
- **`kmem_lock` may return NULL** and every caller must check it.
- **Swap page size is 4096 bytes = 8 sectors of 512 bytes.**
- Measured hardware budget the design must fit (from the spec): 322 KB free heap after chip and display init, 65 KB full-screen canvas, no PSRAM.
- Every task ends with the whole suite green and a commit.

---

### Task 1: Host test harness and build

**Files:**
- Create: `CMakeLists.txt`
- Create: `test/tinytest.h`
- Create: `test/test_main.c`
- Create: `build.bat`
- Create: `.gitignore` (modify existing)

**Interfaces:**
- Consumes: nothing.
- Produces: `CHECK(cond)`, `CHECK_EQ(a,b)`, `CHECK_STR(a,b)`, `RUN(fn)`, `tt_report()` from `test/tinytest.h`; a `cardos_tests.exe` binary; `build.bat` which runs configure, build and the test binary in one step.

- [ ] **Step 1: Write `test/tinytest.h`**

```c
#ifndef TINYTEST_H
#define TINYTEST_H
#include <stdio.h>
extern int tt_checks, tt_fails; extern const char *tt_current;
#define CHECK(cond) do { tt_checks++; if (!(cond)) { tt_fails++; \
  printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, tt_current, #cond); } } while (0)
#define CHECK_EQ(a,b) do { tt_checks++; long long _a=(long long)(a), _b=(long long)(b); \
  if (_a!=_b) { tt_fails++; printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n", \
  __FILE__, __LINE__, tt_current, #a, #b, _a, _b); } } while (0)
#define RUN(fn) do { tt_current = #fn; int _before = tt_fails; fn(); \
  printf("%-46s %s\n", #fn, tt_fails == _before ? "ok" : "FAILED"); } while (0)
int tt_report(void);
#endif
```

- [ ] **Step 2: Write `test/test_main.c` with one deliberately failing test**

```c
#include "tinytest.h"
int tt_checks = 0, tt_fails = 0; const char *tt_current = "";
int tt_report(void) {
  printf("\n%d checks, %d failures\n", tt_checks, tt_fails);
  return tt_fails ? 1 : 0;
}
static void test_harness_reports_failure(void) { CHECK_EQ(1, 2); }
int main(void) { RUN(test_harness_reports_failure); return tt_report(); }
```

- [ ] **Step 3: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(cardos C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if (MSVC)
  add_compile_options(/W4 /wd4127 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()
add_executable(cardos_tests test/test_main.c)
target_include_directories(cardos_tests PRIVATE test kernel)
enable_testing()
add_test(NAME unit COMMAND cardos_tests)
```

- [ ] **Step 4: Write `build.bat`**

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake -G Ninja -S "%~dp0." -B "%~dp0build" >nul || exit /b 1
cmake --build "%~dp0build" || exit /b 1
"%~dp0build\cardos_tests.exe"
```

- [ ] **Step 5: Run it and confirm the harness reports the failure**

Run: `cmd /c build.bat`
Expected: `test_harness_reports_failure   FAILED`, `1 checks, 1 failures`, exit code 1.

- [ ] **Step 6: Flip the assertion to `CHECK_EQ(1, 1)` and re-run**

Expected: `ok`, `1 checks, 0 failures`, exit code 0. This proves the harness can both pass and fail — a harness that can only pass is worthless.

- [ ] **Step 7: Commit**

```bash
rtk git add CMakeLists.txt build.bat test/ .gitignore && rtk git commit -m "test: host test harness for the portable kernel core"
```

---

### Task 2: Handle table with generation counters

**Files:**
- Create: `kernel/mem/mem.h`
- Create: `kernel/mem/mem_internal.h`
- Create: `kernel/mem/mem.c`
- Create: `test/test_mem_handles.c`

**Interfaces:**
- Consumes: `test/tinytest.h`.
- Produces: `Handle`, `MEM_ZERO`, `MEM_FIXED`, `MEM_MAX_HANDLES`, `MEM_MAX_BLOCK`, `MemStats`, `kmem_init(void *heap, size_t bytes)`, `kmem_alloc(size_t, uint16_t) -> Handle`, `kmem_free(Handle)`, `kmem_size(Handle) -> size_t`, `kmem_valid(Handle) -> int`, `kmem_stats(MemStats *)`. Internals for tests via `mem_internal.h`: `MemDesc`, `mem_desc(Handle) -> MemDesc *`, `mem_index(Handle)`, `mem_generation(Handle)`.

Rationale for the encoding: with 256 descriptors an index needs exactly 8 bits, leaving 8 bits of generation inside the existing `uint16_t`. Generation increments on every free and skips 0. A handle whose generation does not match its descriptor is rejected by `kmem_valid`, so use-after-free is detected rather than silently reading another block.

- [ ] **Step 1: Write the failing test `test/test_mem_handles.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include "mem/mem_internal.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_alloc_returns_distinct_nonzero_handles(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(100, 0), b = kmem_alloc(100, 0);
  CHECK(a != 0); CHECK(b != 0); CHECK(a != b);
  CHECK_EQ(kmem_size(a), 100);
}

void test_handle_zero_is_always_invalid(void) {
  kmem_init(heap, sizeof heap);
  CHECK_EQ(kmem_valid(0), 0);
  CHECK_EQ(kmem_size(0), 0);
}

void test_freed_handle_is_detected_as_stale(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(100, 0);
  kmem_free(a);
  CHECK_EQ(kmem_valid(a), 0);          /* the whole point of the generation */
  Handle b = kmem_alloc(100, 0);
  CHECK_EQ(mem_index(b), mem_index(a));   /* slot reused ... */
  CHECK(b != a);                          /* ... but the handle differs */
  CHECK_EQ(kmem_valid(a), 0);
  CHECK_EQ(kmem_valid(b), 1);
}

void test_generation_never_becomes_zero(void) {
  kmem_init(heap, sizeof heap);
  for (int i = 0; i < 600; i++) {         /* wraps the 8-bit generation twice */
    Handle h = kmem_alloc(16, 0);
    CHECK(h != 0);
    CHECK(mem_generation(h) != 0);
    kmem_free(h);
  }
}

void test_handle_table_exhaustion_returns_zero(void) {
  kmem_init(heap, sizeof heap);
  int got = 0;
  for (int i = 0; i < MEM_MAX_HANDLES + 10; i++)
    if (kmem_alloc(16, 0)) got++;
  CHECK_EQ(got, MEM_MAX_HANDLES);
}

void test_oversized_allocation_is_refused(void) {
  kmem_init(heap, sizeof heap);
  CHECK_EQ(kmem_alloc(MEM_MAX_BLOCK + 1, 0), 0);
  CHECK_EQ(kmem_alloc(0, 0), 0);
}
```

- [ ] **Step 2: Run and verify it fails to compile** (`mem/mem.h` does not exist). Run: `cmd /c build.bat`.

- [ ] **Step 3: Write `kernel/mem/mem.h`** — the public API listed under Interfaces above, with `MEM_MAX_HANDLES 256`, `MEM_MAX_BLOCK (32u*1024u)`, `MEM_ZERO 0x0001`, `MEM_FIXED 0x0002`.

- [ ] **Step 4: Write `kernel/mem/mem_internal.h`** — `MemDesc { uint32_t off; uint32_t size; uint16_t swap_page; uint8_t lock; uint8_t flags; uint8_t gen; uint8_t lru_prev; uint8_t lru_next; uint8_t pad; }` plus the accessors. Assert `sizeof(MemDesc) == 16` with a `_Static_assert` so the budget claim is enforced by the compiler, not by a comment.

- [ ] **Step 5: Implement the table in `kernel/mem/mem.c`** — a static array of 256 descriptors, a free-slot scan, generation increment-and-skip-zero on free, and validation in every public entry point. Add `test/test_mem_handles.c` to `CMakeLists.txt` and register the tests in `test_main.c`.

- [ ] **Step 6: Run the tests** — `cmd /c build.bat`. Expected: all six pass.

- [ ] **Step 7: Commit**

```bash
rtk git add kernel/mem test/test_mem_handles.c test/test_main.c CMakeLists.txt && rtk git commit -m "feat(mem): handle table with generation counters"
```

---

### Task 3: Movable arena, lock discipline and MEM_ZERO

**Files:**
- Modify: `kernel/mem/mem.c`
- Create: `test/test_mem_alloc.c`

**Interfaces:**
- Consumes: Task 2's handle table.
- Produces: `kmem_lock(Handle) -> void *` (NULL on failure), `kmem_lock_ro(Handle) -> void *`, `kmem_unlock(Handle)`.

`kmem_lock` pins a block and returns a writable pointer, marking it dirty. `kmem_lock_ro` pins without marking dirty, so a clean block that already has a swap page can be evicted again without a write — this is what makes the console scrollback cheap. Lock counts nest.

- [ ] **Step 1: Write the failing test `test/test_mem_alloc.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_locked_pointer_is_writable_and_stable(void) {
  kmem_init(heap, sizeof heap);
  Handle h = kmem_alloc(256, 0);
  unsigned char *p = kmem_lock(h);
  CHECK(p != NULL);
  memset(p, 0xAB, 256);
  unsigned char *q = kmem_lock(h);        /* nested lock */
  CHECK_EQ(p == q, 1);
  CHECK_EQ(p[255], 0xAB);
  kmem_unlock(h); kmem_unlock(h);
}

void test_mem_zero_clears_the_block(void) {
  kmem_init(heap, sizeof heap);
  Handle h = kmem_alloc(64, MEM_ZERO);
  unsigned char *p = kmem_lock(h);
  for (int i = 0; i < 64; i++) CHECK_EQ(p[i], 0);
  kmem_unlock(h);
}

void test_blocks_do_not_overlap(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(1000, 0), b = kmem_alloc(1000, 0);
  unsigned char *pa = kmem_lock(a), *pb = kmem_lock(b);
  memset(pa, 1, 1000); memset(pb, 2, 1000);
  for (int i = 0; i < 1000; i++) { CHECK_EQ(pa[i], 1); CHECK_EQ(pb[i], 2); }
  kmem_unlock(a); kmem_unlock(b);
}

void test_lock_of_stale_handle_returns_null(void) {
  kmem_init(heap, sizeof heap);
  Handle h = kmem_alloc(64, 0);
  kmem_free(h);
  CHECK(kmem_lock(h) == NULL);
}

void test_alloc_fails_cleanly_when_heap_is_full(void) {
  kmem_init(heap, sizeof heap);
  int n = 0;
  while (kmem_alloc(MEM_MAX_BLOCK, 0)) n++;
  CHECK(n >= 1);
  MemStats s; kmem_stats(&s);
  CHECK(s.free_bytes < MEM_MAX_BLOCK);   /* it really did run out */
}
```

- [ ] **Step 2: Run and verify it fails.** Expected: `kmem_lock` undefined.

- [ ] **Step 3: Implement** the movable bump arena in `mem.c`: `movable_top` grows up from offset 0, allocation places a block at `movable_top` when `movable_top + size <= fixed_bottom`, `kmem_lock`/`kmem_lock_ro` return `heap_base + desc->off` after validating and incrementing `lock`, `kmem_unlock` decrements. Register the tests.

- [ ] **Step 4: Run the tests.** Expected: all five pass, plus Task 2's still green.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(mem): movable arena, nested locks, MEM_ZERO"
```

---

### Task 4: Fixed arena growing down from the top

**Files:**
- Modify: `kernel/mem/mem.c`
- Create: `test/test_mem_fixed.c`

**Interfaces:**
- Consumes: Tasks 2–3.
- Produces: `MEM_FIXED` behaviour; `MemStats.fixed_used`, `MemStats.movable_used`, `MemStats.free_bytes`.

This is the change to the approved spec from the review: fixed blocks allocate **downward from the top of the heap** with a coalescing free list, movable blocks upward from the bottom. The movable region therefore stays one unbroken run and compaction is always fully effective. In a single interleaved heap, task-stack churn would pin permanent holes that compaction could never close.

- [ ] **Step 1: Write the failing test `test/test_mem_fixed.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_fixed_blocks_sit_above_movable_blocks(void) {
  kmem_init(heap, sizeof heap);
  Handle m = kmem_alloc(1024, 0);
  Handle f = kmem_alloc(1024, MEM_FIXED);
  unsigned char *pm = kmem_lock(m), *pf = kmem_lock(f);
  CHECK(pf > pm);                         /* fixed lives at the top */
  kmem_unlock(m); kmem_unlock(f);
}

void test_fixed_block_never_moves_across_a_compaction(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(2048, 0);
  Handle f = kmem_alloc(1024, MEM_FIXED);
  unsigned char *pf = kmem_lock(f);
  memset(pf, 0x5A, 1024);
  kmem_unlock(f);                          /* unlocked, but FIXED */
  kmem_free(a);                            /* leaves a gap ... */
  kmem_compact();                          /* ... which compaction closes */
  unsigned char *pf2 = kmem_lock(f);
  CHECK_EQ(pf == pf2, 1);                 /* the fixed block did not budge */
  CHECK_EQ(pf2[1023], 0x5A);
  kmem_unlock(f);
}

void test_freed_fixed_blocks_are_coalesced_and_reused(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(1024, MEM_FIXED);
  Handle b = kmem_alloc(1024, MEM_FIXED);
  MemStats before; kmem_stats(&before);
  kmem_free(a); kmem_free(b);
  Handle c = kmem_alloc(2048, MEM_FIXED);  /* only fits if the two coalesced */
  CHECK(c != 0);
  MemStats after; kmem_stats(&after);
  CHECK_EQ(before.fixed_used, after.fixed_used);
}

void test_fixed_and_movable_arenas_cannot_collide(void) {
  kmem_init(heap, sizeof heap);
  while (kmem_alloc(MEM_MAX_BLOCK, MEM_FIXED)) { }
  MemStats s; kmem_stats(&s);
  CHECK(s.movable_used + s.fixed_used <= s.heap_size);
  CHECK_EQ(kmem_alloc(MEM_MAX_BLOCK, 0), 0);
}
```

- [ ] **Step 2: Run and verify it fails.** Expected: `kmem_compact` undefined and the placement assertions fail.

- [ ] **Step 3: Implement** the downward bump allocator plus a sorted free-range list (first fit, coalesce with both neighbours on free, and raise `fixed_bottom` when the coalesced range reaches it). Declare `void kmem_compact(void)` in `mem.h` — it is public because the shell's `mem` command triggers it.

- [ ] **Step 4: Run the tests.** Expected: all green.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(mem): fixed arena grows down from the top with a coalescing free list"
```

---

### Task 5: Compaction

**Files:**
- Modify: `kernel/mem/mem.c`
- Create: `test/test_mem_compact.c`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces: `kmem_compact(void)`; `MemStats.compactions`, `MemStats.largest_free`.

Compaction walks movable blocks in address order carrying a destination cursor. A block that is **locked** is pinned and cannot move (a caller holds its pointer), so the cursor jumps past it; every other movable block slides down to the cursor. This means compaction is partial when blocks are locked, which is correct and is what the lock discipline in the spec is buying.

- [ ] **Step 1: Write the failing test `test/test_mem_compact.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];
static void fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); memset(p, v, kmem_size(h)); kmem_unlock(h);
}
static int check_fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); if (!p) return 0;
  for (size_t i = 0; i < kmem_size(h); i++) if (p[i] != v) { kmem_unlock(h); return 0; }
  kmem_unlock(h); return 1;
}

void test_compaction_closes_a_gap_and_preserves_contents(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0), b = kmem_alloc(4096, 0), c = kmem_alloc(4096, 0);
  fill(a, 0xA1); fill(b, 0xB2); fill(c, 0xC3);
  kmem_free(b);                            /* gap in the middle */
  MemStats before; kmem_stats(&before);
  kmem_compact();
  MemStats after; kmem_stats(&after);
  CHECK(after.largest_free > before.largest_free);
  CHECK_EQ(after.compactions, before.compactions + 1);
  CHECK_EQ(check_fill(a, 0xA1), 1);
  CHECK_EQ(check_fill(c, 0xC3), 1);       /* c moved down, contents intact */
}

void test_compaction_does_not_move_a_locked_block(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0), b = kmem_alloc(4096, 0), c = kmem_alloc(4096, 0);
  fill(a, 0xA1); fill(c, 0xC3);
  unsigned char *pc = kmem_lock(c);        /* pinned across the compaction */
  memset(pc, 0xC3, 4096);
  kmem_free(b);
  kmem_compact();
  CHECK_EQ(kmem_lock(c) == pc, 1);         /* still exactly where it was */
  CHECK_EQ(pc[4095], 0xC3);
  kmem_unlock(c); kmem_unlock(c);
  CHECK_EQ(check_fill(a, 0xA1), 1);
}

void test_alloc_compacts_automatically_when_it_would_otherwise_fail(void) {
  kmem_init(heap, sizeof heap);
  Handle h[8]; for (int i = 0; i < 8; i++) { h[i] = kmem_alloc(7000, 0); CHECK(h[i] != 0); }
  for (int i = 0; i < 8; i += 2) kmem_free(h[i]);   /* alternating gaps */
  MemStats before; kmem_stats(&before);
  Handle big = kmem_alloc(20000, 0);       /* only fits after compaction */
  CHECK(big != 0);
  MemStats after; kmem_stats(&after);
  CHECK(after.compactions > before.compactions);
}

void test_compaction_is_a_noop_when_there_are_no_gaps(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0);
  fill(a, 0x11);
  unsigned char *before = kmem_lock(a); kmem_unlock(a);
  kmem_compact();
  CHECK_EQ(kmem_lock(a) == before, 1); kmem_unlock(a);
  CHECK_EQ(check_fill(a, 0x11), 1);
}
```

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Implement** compaction: collect resident movable descriptor indices, insertion-sort them by offset, sweep with a cursor moving pinned blocks past and `memmove`-ing the rest down. Then wire `kmem_alloc` to call `kmem_compact()` and retry once before giving up.

- [ ] **Step 4: Run the tests.** Expected: all green.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(mem): heap compaction that slides unpinned blocks around locked ones"
```

---

### Task 6: Swap device interface, extent mapping and the RAM fake

**Files:**
- Create: `kernel/swap/swapdev.h`
- Create: `kernel/swap/swap.h`
- Create: `kernel/swap/swap.c`
- Create: `test/fake_swapdev.h`
- Create: `test/fake_swapdev.c`
- Create: `test/test_swap_extent.c`

**Interfaces:**
- Consumes: nothing from `mem`.
- Produces: `SwapDev { int (*read)(void *ctx, uint32_t sector, void *buf, uint32_t count); int (*write)(void *ctx, uint32_t sector, const void *buf, uint32_t count); void *ctx; }`; `SwapExtent { uint32_t start_sector; uint32_t sector_count; }`; `swap_init(const SwapDev *, const SwapExtent *, int n, void *bitmap, size_t bitmap_bytes) -> int`; `swap_page_to_sector(uint16_t page) -> uint32_t`; `swap_pages_total(void) -> uint16_t`; `fake_swapdev_create/destroy/dev/read_counts`.

This is the review change to the approved spec: page → sector goes through an **extent table** rather than a single start sector, so nothing depends on `f_expand` being compiled into ESP-IDF's FatFs and a fragmented card still works. A one-extent table is exactly the contiguous case, so nothing is lost.

- [ ] **Step 1: Write the failing test `test/test_swap_extent.c`**

```c
#include "tinytest.h"
#include "swap/swap.h"
#include "fake_swapdev.h"

static unsigned char bitmap[128];

void test_single_extent_maps_pages_linearly(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);        /* 2048 sectors = 1 MB */
  SwapExtent ex[1] = { { 1000, 2048 } };
  CHECK_EQ(swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap), 0);
  CHECK_EQ(swap_pages_total(), 256);                 /* 2048 / 8 */
  CHECK_EQ(swap_page_to_sector(0), 1000);
  CHECK_EQ(swap_page_to_sector(1), 1008);
  CHECK_EQ(swap_page_to_sector(255), 1000 + 255 * 8);
  fake_swapdev_destroy(f);
}

void test_split_extents_map_across_the_boundary(void) {
  FakeSwapDev *f = fake_swapdev_create(4096);
  SwapExtent ex[2] = { { 100, 800 }, { 5000, 1248 } };  /* 800 + 1248 = 2048 */
  CHECK_EQ(swap_init(fake_swapdev_dev(f), ex, 2, bitmap, sizeof bitmap), 0);
  CHECK_EQ(swap_pages_total(), 256);
  CHECK_EQ(swap_page_to_sector(99), 100 + 99 * 8);   /* last page in extent 0 */
  CHECK_EQ(swap_page_to_sector(100), 5000);          /* first page in extent 1 */
  CHECK_EQ(swap_page_to_sector(101), 5008);
  fake_swapdev_destroy(f);
}

void test_extent_not_a_multiple_of_page_size_is_rejected(void) {
  FakeSwapDev *f = fake_swapdev_create(4096);
  SwapExtent ex[1] = { { 100, 803 } };               /* 803 is not divisible by 8 */
  CHECK(swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap) != 0);
  fake_swapdev_destroy(f);
}

void test_out_of_range_page_is_rejected(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 1000, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  CHECK_EQ(swap_page_to_sector(256), SWAP_BAD_SECTOR);
  fake_swapdev_destroy(f);
}
```

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Write `test/fake_swapdev.c`** — a `calloc`ed sector array with `read`/`write` callbacks that bounds-check every access and count operations, so a test can assert on I/O volume. Bounds-checking here is the point: it turns an off-by-one in swap into a loud test failure instead of silent corruption.

- [ ] **Step 4: Implement** `swap_init` (validate every extent is page-aligned in length, sum to the total, store the table) and `swap_page_to_sector` (walk extents accumulating pages, return `SWAP_BAD_SECTOR` for out of range).

- [ ] **Step 5: Run the tests.** Expected: all four pass.

- [ ] **Step 6: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(swap): SwapDev interface and extent-based page-to-sector mapping"
```

---

### Task 7: Swap bitmap allocator

**Files:**
- Modify: `kernel/swap/swap.c`
- Create: `test/test_swap_bitmap.c`

**Interfaces:**
- Consumes: Task 6.
- Produces: `swap_alloc_pages(uint16_t n) -> uint16_t` (`SWAP_INVALID_PAGE` on failure), `swap_free_pages(uint16_t first, uint16_t n)`, `swap_pages_free(void) -> uint16_t`.

A block occupies a contiguous run of pages, so this is a first-fit run allocator over a bitmap.

- [ ] **Step 1: Write the failing test `test/test_swap_bitmap.c`**

```c
#include "tinytest.h"
#include "swap/swap.h"
#include "fake_swapdev.h"

static unsigned char bitmap[128];
static FakeSwapDev *f;
static void setup(void) {
  if (f) fake_swapdev_destroy(f);
  f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 0, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
}

void test_pages_are_allocated_contiguously_and_distinctly(void) {
  setup();
  uint16_t a = swap_alloc_pages(3), b = swap_alloc_pages(2);
  CHECK_EQ(a, 0); CHECK_EQ(b, 3);
  CHECK_EQ(swap_pages_free(), 256 - 5);
}

void test_freed_run_is_reused(void) {
  setup();
  uint16_t a = swap_alloc_pages(4);
  swap_alloc_pages(1);
  swap_free_pages(a, 4);
  CHECK_EQ(swap_pages_free(), 256 - 1);
  uint16_t c = swap_alloc_pages(4);
  CHECK_EQ(c, a);                          /* first fit reclaims the hole */
}

void test_run_spanning_a_hole_is_not_allocated(void) {
  setup();
  uint16_t a = swap_alloc_pages(2);
  uint16_t b = swap_alloc_pages(2);
  uint16_t c = swap_alloc_pages(2);
  (void)b;
  swap_free_pages(a, 2); swap_free_pages(c, 2);   /* two 2-page holes, not adjacent */
  CHECK_EQ(swap_alloc_pages(4), 6);        /* must skip both holes, not straddle */
}

void test_exhaustion_returns_invalid_page(void) {
  setup();
  int n = 0;
  while (swap_alloc_pages(8) != SWAP_INVALID_PAGE) n++;
  CHECK_EQ(n, 32);                         /* 256 pages / 8 */
  CHECK_EQ(swap_pages_free(), 0);
  CHECK_EQ(swap_alloc_pages(1), SWAP_INVALID_PAGE);
}

void test_bitmap_too_small_is_rejected_at_init(void) {
  FakeSwapDev *g = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 0, 2048 } };
  unsigned char tiny[4];                   /* needs 256/8 = 32 bytes */
  CHECK(swap_init(fake_swapdev_dev(g), ex, 1, tiny, sizeof tiny) != 0);
  fake_swapdev_destroy(g);
}
```

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Implement** the bitmap: `swap_init` zeroes it (so stale pages from a previous boot can never be mistaken for live data) and rejects a bitmap smaller than `pages_total / 8`; `swap_alloc_pages` scans for the first run of `n` clear bits; `swap_free_pages` clears them.

- [ ] **Step 4: Run the tests.** Expected: all five pass.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(swap): first-fit bitmap page-run allocator"
```

---

### Task 8: Swap read and write round trip

**Files:**
- Modify: `kernel/swap/swap.c`
- Create: `test/test_swap_io.c`

**Interfaces:**
- Consumes: Tasks 6–7.
- Produces: `swap_write(uint16_t first_page, const void *src, size_t bytes) -> int`, `swap_read(uint16_t first_page, void *dst, size_t bytes) -> int`, `SwapStats`, `swap_stats(SwapStats *)`.

The subtle case is a block whose size is not a multiple of 512. Writing `ceil(bytes/512)` sectors straight from the caller's buffer would read past the end of it. The tail sector therefore goes through a 512-byte bounce buffer.

- [ ] **Step 1: Write the failing test `test/test_swap_io.c`**

```c
#include "tinytest.h"
#include "swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char bitmap[128];
static unsigned char src[9000], dst[9000];

void test_round_trip_of_a_page_aligned_block(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 64, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  for (int i = 0; i < 8192; i++) src[i] = (unsigned char)(i * 31 + 7);
  uint16_t p = swap_alloc_pages(2);
  CHECK_EQ(swap_write(p, src, 8192), 0);
  memset(dst, 0, sizeof dst);
  CHECK_EQ(swap_read(p, dst, 8192), 0);
  CHECK_EQ(memcmp(src, dst, 8192), 0);
  fake_swapdev_destroy(f);
}

void test_round_trip_of_a_ragged_block_does_not_read_past_the_buffer(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 64, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  for (int i = 0; i < 4700; i++) src[i] = (unsigned char)(i ^ 0x5A);
  uint16_t p = swap_alloc_pages(2);
  CHECK_EQ(swap_write(p, src, 4700), 0);   /* 4700 = 9 sectors + 92 bytes */
  memset(dst, 0xEE, sizeof dst);
  CHECK_EQ(swap_read(p, dst, 4700), 0);
  CHECK_EQ(memcmp(src, dst, 4700), 0);
  CHECK_EQ(dst[4700], 0xEE);               /* nothing written past the end */
  fake_swapdev_destroy(f);
}

void test_write_beyond_the_allocated_run_is_refused(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 64, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  CHECK(swap_write(255, src, 8192) != 0);  /* 2 pages starting at the last page */
  fake_swapdev_destroy(f);
}

void test_device_errors_are_propagated(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 64, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  fake_swapdev_fail_next_write(f);
  CHECK(swap_write(0, src, 4096) != 0);
  fake_swapdev_destroy(f);
}
```

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Implement** `swap_write`/`swap_read` walking sector by sector through `swap_page_to_sector` (which handles extent boundaries), using a static 512-byte bounce buffer for a ragged tail, bounds-checking against `pages_total`, and propagating any non-zero device return. Add `fake_swapdev_fail_next_write` to the fake.

- [ ] **Step 4: Run the tests.** Expected: all four pass.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(swap): sector-level page read/write with a bounce buffer for ragged tails"
```

---

### Task 9: Eviction, page-in and the LRU list

**Files:**
- Modify: `kernel/mem/mem.c`, `kernel/mem/mem.h`
- Create: `test/test_mem_evict.c`

**Interfaces:**
- Consumes: every earlier task.
- Produces: `mem_attach_swap(void)` wiring, `MemStats.evictions`, `MemStats.page_ins`, `MemStats.swapped_bytes`, `MemStats.resident_bytes`; `kmem_lock` returning NULL when a page-in cannot be satisfied.

The LRU list is an intrusive doubly-linked list threaded through the descriptors with `uint8_t` indices — no clock interface is needed, which drops one dependency the spec anticipated. A block joins the MRU end when unlocked and leaves when locked, freed or evicted. Only unlocked, resident, non-`MEM_FIXED` blocks are ever on it, so eviction never has to filter.

- [ ] **Step 1: Write the failing test `test/test_mem_evict.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include "swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char heap[64 * 1024];
static unsigned char bitmap[128];
static FakeSwapDev *f;

static void setup(void) {
  if (f) fake_swapdev_destroy(f);
  f = fake_swapdev_create(4096);
  SwapExtent ex[1] = { { 0, 4096 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  kmem_init(heap, sizeof heap);
}
static void fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); memset(p, v, kmem_size(h)); kmem_unlock(h);
}
static int check_fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); if (!p) return 0;
  for (size_t i = 0; i < kmem_size(h); i++) if (p[i] != v) { kmem_unlock(h); return 0; }
  kmem_unlock(h); return 1;
}

void test_allocation_pressure_evicts_and_page_in_restores_bytes(void) {
  setup();
  Handle h[12]; 
  for (int i = 0; i < 12; i++) {           /* 12 x 8 KB = 96 KB into a 64 KB heap */
    h[i] = kmem_alloc(8192, 0);
    CHECK(h[i] != 0);
    fill(h[i], (unsigned char)(0x40 + i));
  }
  MemStats s; kmem_stats(&s);
  CHECK(s.evictions > 0);
  for (int i = 0; i < 12; i++)             /* every byte survives the round trip */
    CHECK_EQ(check_fill(h[i], (unsigned char)(0x40 + i)), 1);
  kmem_stats(&s);
  CHECK(s.page_ins > 0);
}

void test_least_recently_used_block_is_evicted_first(void) {
  setup();
  Handle a = kmem_alloc(16384, 0), b = kmem_alloc(16384, 0);
  fill(a, 0xAA); fill(b, 0xBB);
  CHECK_EQ(check_fill(a, 0xAA), 1);        /* a is now the most recently used */
  Handle c = kmem_alloc(32768, 0);          /* forces one of them out */
  CHECK(c != 0);
  CHECK_EQ(kmem_resident(b), 0);            /* b was least recently used */
  CHECK_EQ(kmem_resident(a), 1);
}

void test_locked_blocks_are_never_evicted(void) {
  setup();
  Handle pinned = kmem_alloc(16384, 0);
  unsigned char *p = kmem_lock(pinned);
  memset(p, 0x77, 16384);
  Handle rest[8];
  for (int i = 0; i < 8; i++) rest[i] = kmem_alloc(8192, 0);
  CHECK_EQ(kmem_resident(pinned), 1);
  CHECK_EQ(p[16383], 0x77);                /* pointer still valid */
  CHECK_EQ(kmem_lock(pinned) == p, 1);
  kmem_unlock(pinned); kmem_unlock(pinned);
  (void)rest;
}

void test_fixed_blocks_are_never_evicted(void) {
  setup();
  Handle fx = kmem_alloc(8192, MEM_FIXED);
  fill(fx, 0x33);
  for (int i = 0; i < 8; i++) kmem_alloc(8192, 0);
  CHECK_EQ(kmem_resident(fx), 1);
  CHECK_EQ(check_fill(fx, 0x33), 1);
}

void test_clean_block_is_evicted_without_a_second_write(void) {
  setup();
  Handle a = kmem_alloc(16384, 0);
  fill(a, 0x9C);
  Handle b = kmem_alloc(32768, 0);          /* evicts a: one write */
  CHECK(b != 0);
  CHECK_EQ(kmem_resident(a), 0);
  unsigned char *p = kmem_lock_ro(a);       /* pages a back in, stays clean */
  CHECK(p != NULL);
  CHECK_EQ(p[16383], 0x9C);
  kmem_unlock(a);
  unsigned long writes_before = fake_swapdev_writes(f);
  kmem_free(b);
  Handle c = kmem_alloc(32768, 0);          /* evicts a again */
  CHECK(c != 0);
  CHECK_EQ(kmem_resident(a), 0);
  CHECK_EQ(fake_swapdev_writes(f), writes_before);   /* clean: no rewrite */
  CHECK_EQ(check_fill(a, 0x9C), 1);
}

void test_lock_returns_null_when_page_in_cannot_be_satisfied(void) {
  setup();
  Handle victim = kmem_alloc(MEM_MAX_BLOCK, 0);
  fill(victim, 0xD1);
  Handle hogs[2];
  for (int i = 0; i < 2; i++) { hogs[i] = kmem_alloc(MEM_MAX_BLOCK, 0); CHECK(hogs[i] != 0); }
  CHECK_EQ(kmem_resident(victim), 0);       /* pushed out */
  unsigned char *p0 = kmem_lock(hogs[0]);   /* pin the whole heap */
  unsigned char *p1 = kmem_lock(hogs[1]);
  CHECK(p0 != NULL); CHECK(p1 != NULL);
  CHECK(kmem_lock(victim) == NULL);         /* no room, and it says so */
  kmem_unlock(hogs[0]); kmem_unlock(hogs[1]);
  CHECK(kmem_lock(victim) != NULL);         /* room again once unpinned */
  kmem_unlock(victim);
}

void test_freeing_a_swapped_block_releases_its_swap_pages(void) {
  setup();
  Handle a = kmem_alloc(16384, 0);
  fill(a, 0x5E);
  Handle b = kmem_alloc(32768, 0);
  CHECK_EQ(kmem_resident(a), 0);
  uint16_t free_before = swap_pages_free();
  kmem_free(a);
  CHECK(swap_pages_free() > free_before);
  (void)b;
}
```

- [ ] **Step 2: Run and verify it fails.** Expected: `kmem_resident` and `fake_swapdev_writes` undefined.

- [ ] **Step 3: Implement**: the LRU list; `mem_evict_one()` picking the LRU tail, allocating a page run, writing it only when the dirty flag is set (reusing its existing run when clean and already backed), clearing `resident`; the `kmem_alloc` escalation of compact → evict → compact → retry; page-in inside `kmem_lock`/`kmem_lock_ro` allocating space by the same escalation and returning NULL if it still cannot fit; `kmem_free` releasing swap pages. Add `kmem_resident(Handle)` to `mem.h` and `fake_swapdev_writes` to the fake.

- [ ] **Step 4: Run the tests.** Expected: all seven pass, plus every earlier test still green.

- [ ] **Step 5: Commit**

```bash
rtk git add -A && rtk git commit -m "feat(mem): LRU eviction to swap, page-in, and a lock that can fail honestly"
```

---

### Task 10: Randomised torture test — the host-side `stress`

**Files:**
- Create: `test/test_mem_torture.c`

**Interfaces:**
- Consumes: every earlier task.
- Produces: nothing new — this is the safety net that decides whether the memory core is trustworthy.

Success criterion 2 in the spec is `stress 2` on the device. This is that test on the host, where it can run every commit instead of once a week. It drives a deterministic pseudo-random mix of allocate, free, lock, unlock and verify against a shadow model, so an eviction bug shows up as a specific byte mismatch with a reproducible seed rather than a hang on hardware with no debugger.

- [ ] **Step 1: Write the failing test `test/test_mem_torture.c`**

```c
#include "tinytest.h"
#include "mem/mem.h"
#include "swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char heap[64 * 1024];
static unsigned char bitmap[256];

static uint32_t rng_state;
static uint32_t rnd(void) {              /* xorshift32: same sequence everywhere */
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
  return rng_state;
}

#define SLOTS 48
typedef struct { Handle h; uint32_t size; unsigned char seed; int locked; } Slot;

/* Byte i of a block is (seed + i) & 0xff, so a shifted or truncated block is
   caught at the exact offset it went wrong. */
static void write_pattern(unsigned char *p, uint32_t n, unsigned char seed) {
  for (uint32_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i);
}
static int verify_pattern(const unsigned char *p, uint32_t n, unsigned char seed) {
  for (uint32_t i = 0; i < n; i++)
    if (p[i] != (unsigned char)(seed + i)) return 0;
  return 1;
}

void test_torture_every_byte_survives_thousands_of_operations(void) {
  FakeSwapDev *f = fake_swapdev_create(8192);        /* 4 MB, as on the card */
  SwapExtent ex[1] = { { 0, 8192 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  kmem_init(heap, sizeof heap);
  rng_state = 0xC0FFEEu;

  Slot s[SLOTS]; memset(s, 0, sizeof s);
  int mismatches = 0, allocs = 0;

  for (int step = 0; step < 20000; step++) {
    int i = (int)(rnd() % SLOTS);
    switch (rnd() % 4) {
    case 0:                                          /* allocate */
      if (s[i].h == 0) {
        uint32_t n = 64 + rnd() % 6000;
        Handle h = kmem_alloc(n, 0);
        if (h) {
          unsigned char *p = kmem_lock(h);
          if (p) { s[i].h = h; s[i].size = n; s[i].seed = (unsigned char)rnd();
                   write_pattern(p, n, s[i].seed); kmem_unlock(h); allocs++; }
          else kmem_free(h);
        }
      }
      break;
    case 1:                                          /* verify */
      if (s[i].h) {
        unsigned char *p = kmem_lock_ro(s[i].h);
        if (p) {
          if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
          kmem_unlock(s[i].h);
        }
      }
      break;
    case 2:                                          /* free */
      if (s[i].h && !s[i].locked) { kmem_free(s[i].h); memset(&s[i], 0, sizeof s[i]); }
      break;
    case 3:                                          /* hold a lock across other work */
      if (s[i].h && !s[i].locked) {
        unsigned char *p = kmem_lock(s[i].h);
        if (p) { s[i].locked = 1; if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++; }
      } else if (s[i].locked) { kmem_unlock(s[i].h); s[i].locked = 0; }
      break;
    }
  }
  for (int i = 0; i < SLOTS; i++) if (s[i].locked) kmem_unlock(s[i].h);

  CHECK_EQ(mismatches, 0);
  CHECK(allocs > 1000);                              /* it really did work hard */

  MemStats st; kmem_stats(&st);
  printf("    torture: %u allocs, %u compactions, %u evictions, %u page-ins\n",
         (unsigned)allocs, (unsigned)st.compactions,
         (unsigned)st.evictions, (unsigned)st.page_ins);
  CHECK(st.evictions > 100);                         /* pressure was real */
  CHECK(st.page_ins > 100);

  for (int i = 0; i < SLOTS; i++) if (s[i].h) kmem_free(s[i].h);
  MemStats end; kmem_stats(&end);
  CHECK_EQ(end.handles_used, 0);                     /* nothing leaked */
  CHECK_EQ(swap_pages_free(), swap_pages_total());   /* no swap leaked either */
  fake_swapdev_destroy(f);
}

void test_torture_with_fixed_blocks_interleaved(void) {
  FakeSwapDev *f = fake_swapdev_create(8192);
  SwapExtent ex[1] = { { 0, 8192 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  kmem_init(heap, sizeof heap);
  rng_state = 0x1234567u;

  Handle stacks[8]; memset(stacks, 0, sizeof stacks);
  Slot s[SLOTS]; memset(s, 0, sizeof s);
  int mismatches = 0;

  for (int step = 0; step < 8000; step++) {
    if (step % 37 == 0) {                            /* task stacks churning */
      int k = (int)(rnd() % 8);
      if (stacks[k]) { kmem_free(stacks[k]); stacks[k] = 0; }
      else stacks[k] = kmem_alloc(1024, MEM_FIXED | MEM_ZERO);
    }
    int i = (int)(rnd() % SLOTS);
    if (s[i].h == 0) {
      uint32_t n = 64 + rnd() % 4000;
      Handle h = kmem_alloc(n, 0);
      if (h) {
        unsigned char *p = kmem_lock(h);
        if (p) { s[i].h = h; s[i].size = n; s[i].seed = (unsigned char)rnd();
                 write_pattern(p, n, s[i].seed); kmem_unlock(h); }
        else kmem_free(h);
      }
    } else {
      unsigned char *p = kmem_lock_ro(s[i].h);
      if (p) {
        if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
        kmem_unlock(s[i].h);
      }
      if (rnd() % 3 == 0) { kmem_free(s[i].h); memset(&s[i], 0, sizeof s[i]); }
    }
  }
  CHECK_EQ(mismatches, 0);
  for (int i = 0; i < SLOTS; i++) if (s[i].h) kmem_free(s[i].h);
  for (int k = 0; k < 8; k++) if (stacks[k]) kmem_free(stacks[k]);
  MemStats end; kmem_stats(&end);
  CHECK_EQ(end.handles_used, 0);
  CHECK_EQ(end.fixed_used, 0);
  fake_swapdev_destroy(f);
}
```

- [ ] **Step 2: Run it.** If the earlier tasks are correct this may pass first time; if it fails, the seed is deterministic, so shrink the step count until the first mismatch reproduces and fix the underlying bug. Do not weaken the test to make it pass.

- [ ] **Step 3: Record the measured numbers** printed by the torture test in the commit message, per the working agreement in `CLAUDE.md` ("measure before optimising, and put the measurement in the commit message").

- [ ] **Step 4: Commit**

```bash
rtk git add -A && rtk git commit -m "test(mem): randomised torture test over alloc/free/lock/evict"
```

---

## Not in this plan

The scheduler, filesystem, console, keyboard and shell are separate plans. This one stops at the point where the memory core is provably correct on the host — which is the precondition for all of them. Two components the spec omits entirely and that the shell MVP needs, **a keyboard matrix driver and an ST7789 console**, need specs of their own before they get plans.

---

## As built

All ten tasks are implemented and committed. Where the finished code differs
from the plan above, the plan was wrong:

- **`movable_bump` became `movable_place` (first fit).** Placing only at the
  top of the movable region made the gaps between locked blocks unusable, and
  compaction cannot close those by definition. Measured over the torture run:
  allocation failures 3079 to 135, successful allocations 847 to 1724.
- **`MemStats.evict_writes` was added.** The plan tested the clean-eviction
  optimisation by counting device writes across a hand-built sequence, which
  was brittle. Counting evictions that actually wrote is directly meaningful
  and is what the shell's `swap` command needs anyway.
- **Paranoid relocation mode was added** (`CARDOS_MEM_PARANOID`, host builds
  only): every allocation deliberately relocates unpinned blocks, so a pointer
  illegally retained across an allocation fails immediately under a fixed seed.
  This is the cheap stand-in for the compile-time guarantee a Rust guard type
  would give, and it caught a live corruption bug within a minute of existing.
- **The test registry is generated** by `tools/gen_test_main.py` rather than
  hand-maintained, so a test cannot be written and then silently never run.
- **Three tests in the plan were wrong and were corrected:** a fake device
  sized smaller than its own extent; a first-fit expectation computed by hand
  as page 6 when page 4 is correct; and several eviction tests whose
  allocations fit the heap comfortably and so never forced an eviction at all.

Remaining before the device: the keyboard matrix driver and the ST7789
console, neither of which the spec currently mentions, plus the scheduler,
filesystem and shell.
