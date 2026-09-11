# CardOS kernel core — design

Status: approved 2026-09-10. First of five sub-projects (see Roadmap).

CardOS is a tiny custom operating system for the M5Stack Cardputer. It is
standalone: no PC, no network dependency, no Lua, no Arduino. Apps are written
in a custom language and compiled **on the device** to native Xtensa machine
code. This document specifies the **kernel core** only — the memory manager,
swap, scheduler, filesystem and shell.

## Decisions already made

| Decision | Choice | Why |
|---|---|---|
| OS depth | CardOS on ESP-IDF | The BT/WiFi radios need Espressif's binary stack, which is IDF-only. FreeRTOS and the radio drivers are a HAL underneath; everything the user sees is ours. |
| Execution | Template codegen → native Xtensa LX7 | `heap_caps_malloc(n, MALLOC_CAP_EXEC)` gives executable RAM; internal SRAM has no I-cache to invalidate. Real machine code, ~15–25 KB of compiler, no interpreter. (Sub-project 3.) |
| Tasks | Cooperative, per-task stacks, hand-written context switch | ~1 KB/task vs FreeRTOS's ~4 KB, and the switch is ours. |
| Filesystem | FAT32 + contiguous swap file | Card stays readable on any PC; swap bypasses the FS on the hot path. |
| MVP | A working shell | Exercises FS, tasks and swap against something you can poke at. |

## The memory problem

The Cardputer is an ESP32-S3FN8: **512 KB SRAM, no PSRAM**, 8 MB flash.
Measured on real hardware (from the sibling `cardputer` project):

| | |
|---|---|
| Free heap after chip + display init | **322 KB** |
| Full-screen 240×135 16-bit canvas | **65 KB** |
| WiFi stack, once started | **53 KB** |
| Lua 5.4 + full stdlib, for scale | ~63 KB |

**There is no MMU for internal RAM.** No page faults, so transparent demand
paging of ordinary pointers is impossible. CardOS gets past physical RAM two
ways instead:

1. **Handle-based swap** for read/write memory (software, this spec).
2. **Hardware paging for read-only data** via `esp_partition_mmap` — the flash
   MMU and cache demand-page flash into the address space for free. Compiled
   code images, string tables and constants live in flash at zero RAM cost.
   With 8 MB of flash against 512 KB of RAM this is the single biggest win
   available. A thin `rom_map()` API is specified here; its main consumer is
   sub-project 3.

## Components

### 1. Memory manager (`kernel/mem`)

Apps never hold raw pointers across a yield. They hold handles.

```c
typedef uint16_t Handle;                  /* 0 = invalid */

Handle  kmem_alloc(size_t bytes, uint16_t flags);   /* MEM_ZERO, MEM_FIXED */
void   *kmem_lock(Handle h);               /* pins; pages in if swapped */
void    kmem_unlock(Handle h);             /* now relocatable and evictable */
size_t  kmem_size(Handle h);
void    kmem_free(Handle h);
void    kmem_stats(MemStats *out);
```

**Handle table.** A fixed array of 256 descriptors (~12 bytes each, ~3 KB
total). Each descriptor: block pointer (or null when swapped), size, swap page
index, lock count, flags, LRU links.

**Two properties, both from the indirection.** An unlocked block is
*relocatable*, so the manager compacts the heap rather than fragmenting to
death; and it is *evictable*, so it can go to swap.

**Eviction.** When `kmem_alloc` or a page-in cannot be satisfied:
1. Try compaction (move unlocked resident blocks down, coalesce free space).
2. Still short: evict least-recently-used unlocked blocks to swap until the
   request fits.
3. Still short: return 0. Callers must handle it.

`MEM_FIXED` blocks are never moved or evicted — for task stacks, DMA buffers
and anything an interrupt can touch.

**Lock discipline.** `kmem_lock` nests (lock count, not a boolean). A block
locked across a yield simply stays resident; that is legal but costs RAM. The
shell's own code is the reference example of correct discipline.

### 2. Swap (`kernel/swap`)

`/cardos/swap.img` on the SD card, default 4 MB, created at first boot.

- Allocated **contiguous** with FatFs `f_expand()`, so the file occupies one
  unbroken run of sectors.
- The run's start sector is resolved once at mount.
- All subsequent traffic uses **raw `disk_read`/`disk_write` on sectors**,
  bypassing the filesystem entirely on the hot path.
- Divided into 4 KB pages with a bitmap allocator; a block occupies a
  contiguous run of pages.
- Swap contents do not survive a reboot and are not meant to; the image is
  reused in place, not rewritten.

### 3. Scheduler (`kernel/task`)

Cooperative round-robin. No preemption, no priorities in v1.

```c
Tid  task_spawn(const char *name, void (*entry)(void *), void *arg, size_t stack);
void task_yield(void);
void task_sleep(uint32_t ms);
void task_exit(void);
void task_kill(Tid t);
int  task_list(TaskInfo *out, int max);
```

- Per-task stack, default 1 KB, allocated `MEM_FIXED` (a moving stack would
  invalidate every saved frame pointer).
- Context switch is hand-written Xtensa assembly in the **`call0` ABI** —
  which sidesteps register windows entirely — saving `a0`, `a1`, `a12`–`a15`
  and the return address, then swapping stack pointers. Roughly 40 lines.
- States: RUNNING, READY, SLEEPING, BLOCKED, DEAD. A reaper frees the stacks
  of dead tasks.
- Later (sub-project 3) CardLang's compiler emits yield checks in loop
  back-edges and calls, so a compiled app cannot wedge the system. Until then,
  C tasks must yield honestly.

### 4. Filesystem (`kernel/fs`)

FAT32 through IDF's `esp_vfs_fat_sdspi_mount` (the Cardputer's card is on SPI:
CS 12, MOSI 14, MISO 39, SCK 40), behind a thin CardOS API so the
implementation can be replaced without touching callers:

```c
int  fs_open(const char *path, int flags);
int  fs_read(int fd, void *buf, size_t n);
int  fs_write(int fd, const void *buf, size_t n);
int  fs_seek(int fd, int32_t off, int whence);
void fs_close(int fd);
int  fs_stat(const char *path, FsStat *out);
int  fs_list(const char *dir, FsEntry *out, int max);
int  fs_mkdir(const char *path);
int  fs_remove(const char *path);
int  fs_rename(const char *from, const char *to);
```

Layout: `/cardos/swap.img`, `/cardos/apps/`, `/cardos/src/`, `/home/`.

### 5. Console and shell (`kernel/console`, `shell/`)

A text console straight on the ST7789 (240×135, 6×8 font → **40×16
characters**). No window system yet; that is sub-project 2.

Scrollback lives in a **swappable handle** — the OS eating its own dog food,
and the easiest way to see swap working.

Commands for the MVP:

| | |
|---|---|
| Files | `ls` `cd` `pwd` `cat` `cp` `mv` `rm` `mkdir` |
| Tasks | `ps` `spawn` `kill` |
| Memory | `mem` (heap, handles, compaction), `swap` (pages, hit/miss) |
| Diagnostics | `test [mem\|swap\|task\|fs]`, `stress <MB>`, `clear`, `help`, `reboot` |

`stress` deliberately over-allocates so eviction and page-in can be watched
live. It is a demo and a test at once.

## How this is verified

The parts with real logic — handle table, compaction, eviction policy, swap
bitmap allocator, scheduler ready-queue — are written as **portable C** with
the backing store and clock behind interfaces, so they compile and run in a
**host test suite on the PC** under real unit tests, TDD'd before they ever
reach the device.

Only three things genuinely need hardware: the Xtensa context switch, the SD
sector layer, and the display console. Those get on-device selftests reachable
from `test`, and the host suite uses a RAM-backed fake for the swap device.

Test-first is not optional here: an eviction bug that only appears under
memory pressure on a device with no debugger is exactly the kind of thing that
eats a week.

## Success criteria

1. Boots to a shell on the Cardputer, from ESP-IDF, with no Arduino layer.
2. `stress 2` allocates well past physical RAM, evicts to swap, pages back in,
   and every byte read back matches what was written.
3. `ps` shows several cooperative tasks running concurrently; `spawn` and
   `kill` work; no task can corrupt another's stack.
4. Files survive a reboot and the card mounts on a PC.
5. Host test suite passes, covering the handle table, compaction, eviction,
   swap allocator and scheduler queue.
6. Free heap after boot is reported by `mem` and is understood — no unexplained
   consumption.

## Roadmap (each gets its own spec)

1. **Kernel core** — this document
2. **Window system + desktop shell** — compositor, windows, event dispatch
3. **CardLang** — language, on-device compiler, Xtensa template codegen
4. **Editor + toolchain** — write, compile and run an app with no PC
5. **Bluetooth HID** — mouse pairing and pointer input

## Deliberately not in scope

Preemption, memory protection between apps, priorities, networking, and any
window system. All can layer on later; none is needed to prove the core.
