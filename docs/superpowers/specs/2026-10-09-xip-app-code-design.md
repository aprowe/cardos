# App code runs from flash (XIP) — design

Status: **approved by alex 2026-10-09** (relayed by session alexr-a2),
including the fixed 28 KB arena, with the two additions under Testing:
measuring under background load, and the headless-command fallback.
Date: 2026-10-09.

## The problem

An app needs two contiguous blocks: one of executable RAM for `.code` and one
of 8-bit heap for `.data`. After hours of radio up/down churn the largest free
piece is 24–39 KB, so `tools/build_apps.py` caps an app at 44 KB total and
28 KB of data. Jar (24.5 KB code, 19 KB data, `build/apps/sizes.txt`) did not
fit and was split into Jar / Jar Shop / Jar Post, losing in-memory state on
every hop.

The agreed direction is Option C of `docs/specs/NEXT-loadable-apps.md`,
layered on the relocating loader: relocate the code once, write it to a
reserved flash area, and map it into the instruction bus with
`esp_partition_mmap(..., ESP_PARTITION_MMAP_INST, ...)`. What was agreed
with alex:

1. Apps stay `.capp` files on the card; `update apps` and Build are
   unchanged; nothing is baked into firmware.
2. The flash area is a **cache**, keyed by the `.capp`. The first launch
   relocates and writes; later launches check the key and map. Evict the
   oldest when full. Wiping it is always safe.
3. The space comes from shrinking `spiffs`.
4. An opt-out flag keeps hot-loop apps' code in IRAM.
5. Flash is written only when an app changes.

All of that stands. One thing in the original framing does not, and it
changes the design, so it comes first.

## What reading the loader turned up: the data block has to stay put

The plan was "only `.data` takes heap." That cannot work as stated.

`capp.ld` keeps literals with the code, on purpose: `l32r` reads them
PC-relative, and the literal pool must sit within 256 KB *below* the
instruction. Every address of a global, a string, or a `CappUi` table that the
code uses is a literal, and therefore a word **inside `.code`** carrying an
`R_XTENSA_32` relocation against the data base. Today `elfload.c` patches
those words on every load, because the data block lands somewhere new every
time.

Once the code is in flash those words are frozen. So the cached code is valid
only for **one data address**. If `.data` comes from the heap, its address
differs on nearly every launch, and the code would have to be rewritten on
nearly every launch: slow, and exactly the wear point 5 rules out.

Nothing on this chip gets around that cheaply:

- The literal pool cannot move to RAM: `l32r` reaches 256 KB backwards, and
  DRAM (0x3FCx_xxxx) is nowhere near the flash instruction window
  (0x42xx_xxxx).
- The S3 core has no `CONST16`, and Xtensa GCC has no FDPIC or
  base-register data model.
- The MMU maps flash only; with no PSRAM, there is nothing to map RAM at a
  fixed virtual address.

**So the design gives the foreground app's data a fixed home: the app data
arena**, one 28 KB block reserved once, at a fixed address. The code is
relocated against the arena's address and the slot's mapped address, and
stays valid until one of them moves.

The arena is a cost and a gain, and alex should weigh both:

- **Cost:** 28 KB taken from the heap for good, whether an app is open or
  not. Idle free heap with both radios up drops from about 120 KB to about
  92 KB (to be measured).
- **Gain:** the foreground app takes **nothing** from the heap: code in
  flash, data in the arena. Today an app's code and data both come out of
  the heap while it runs (Jar would take 44 KB), so with an app open, free
  heap is higher than today. Data fragmentation also stops mattering for the
  foreground app, and that was half of why the budget exists. Even an app
  that keeps its code in RAM puts its data in the arena.

Rejected alternatives: re-relocating on every launch (wear, time); allocating
the arena on demand and hoping it lands at the same address (after radio
churn it will not); a larger arena (each KB is a KB the radios cannot have;
28 KB fits every app's data today, Midi's 28.3 KB included).

## Design

### 1. Partition: `appcode`, 512 KB, taken from `spiffs`

```
spiffs,   data, spiffs,  0x751000, 0x2F000,    # 188 KB (was 700)
appcode,  data, 0x40,    0x780000, 0x80000,    # 512 KB
```

- `spiffs` keeps its **label and offset**. The `partitions.csv` comments say
  guests find it by label in our table and only need it to exist and mount.
  188 KB of LittleFS is plenty for that, and keeping the offset means that
  even a guest that hardcoded it still finds it. A guest's stored LittleFS
  contents are lost once (any resize reformats), as in the 2026-09-12
  resize.
- `appcode` is 64 KB-aligned and a whole number of MMU pages, so mapping it
  wastes no page. Its custom data subtype means no guest touches it.
- The sum of all app code today is about 400 KB (`sizes.txt`), so 512 KB
  holds nearly every app at once. Eviction is for the edge case, not the
  normal one.
- It is a one-time **full USB flash** (`python -m platformio run -t upload`).
  NVS stays at 0x9000, so it may well survive; plan for losing it anyway,
  since `/config/*.txt` mirrors restore WiFi, Google, env and prefs.
- **OTA cannot deliver a partition table.** A device that gets this firmware
  through `update os` has no `appcode` partition, and it must keep working:
  the loader finds no partition and uses today's RAM path for everything
  (see §6).

### 2. The arena

`kernel/app/arena.c`: one 28 KB, 16-byte-aligned block. v1 is a static
`.bss` array, so its address is fixed for a given firmware build. It changes
when a rebuild moves it, and every cached slot is then re-relocated lazily on
its next launch (§4). If that turns out to happen annoyingly often after
`update os`, `SOC_RESERVE_MEMORY_REGION` (ESP-IDF's heap layout hook) can pin
it to an address no build changes. That is a later optimisation, not v1.

- One holder at a time: the app capprun starts on screen (`ensure_loaded`
  passes `CAPP_LOAD_FOREGROUND`). Headless command runs (Today asking
  Calendar), the icon scan, and a second window on the desktop use today's
  path (heap data, RAM code), as they do now.
- If the arena is taken, or the app's data is over 28 KB, the load falls back
  to today's path in full. Code relocated for the arena cannot run with data
  anywhere else.
- `capp_unload` gives it back. `mem` shows the arena's holder and size.

### 3. The code cache (`kernel/app/xipcache.c`, portable, host-tested)

The partition is mapped **once, at boot**, as a single INST mapping (8
pages), and stays mapped. Boot order is the same every time, so the address
is in practice the same across boots of one build. Each slot's code address
is `map_base + slot_offset`.

**Layout:** a ring of 4 KB sectors. An entry is a run of whole sectors:

```
offset 0    header, 128 bytes
  magic        'XIPC'
  seq          uint32, increases per write; the highest seq marks the ring head
  sectors      length of this entry
  key:  path FNV-1a, file size, file mtime      (the appidx key)
        api version, code size
        map base, arena address    (what it was relocated against)
  flash crc    CRC32 of the code as written, checked on every hit
  path         up to 24 chars, for listings only
  live         0xFFFFFFFF; programmed to 0 to kill the entry (1->0 needs no erase)
  commit       0xFFFFFFFF; programmed to the magic LAST
offset 128  code (sh_addralign up to 128 honoured; above that, RAM code)
```

**Boot scan** reads the headers with `esp_partition_read`, walking the ring:
a committed header jumps by `sectors`, a blank or uncommitted sector by one.
At most 128 sectors, so the scan costs a few milliseconds. There is no
separate index file, so there is no index to fall out of step with the
flash.

**No index in RAM.** A lookup walks the headers on flash (at most 128
reads of 128 bytes, a few milliseconds), so the only RAM state is the ring
head, the next seq and the few entries in use. A RAM table of 128 entries
would have cost 4–9 KB for the uptime.

**Lookup at launch:** stat the `.capp`. If a live, committed entry matches
the whole key, reloc pair included, and its flash CRC checks, map it with no
flash write. The raw code's CRC is deliberately not in the key: computing it
means reading the code from the card on every launch, which the cache exists
to avoid. Anything else (a new file, a new firmware, a bad CRC) writes a new
entry.

**Partial overlap cannot happen.** A write starts at the end of the newest
entry, or at 0. Every entry whose header comes before that point ends at or
before it, so an erase never leaves a header standing over a hole. Entries
the erase cuts lose their headers, and their tails read as free sectors. A
host soak test (hundreds of laps of mixed sizes, every live entry must still
check) holds the invariant, and the CRC check on every hit backs it up.

**A `.capp` that changes on the card is forgotten at once.** Size and mtime
are the key, and with no clock FAT stamps every write 1980-01-01. A rebuilt
app of the same size would then match its old entry, and old code would run
with new data. So the fs change hook (`fs_on_change`, which already deletes
`/cache/apps.idx`) also kills the cache entries for any changed path ending
in `.capp`. The path hash folds case, as FAT does.

**Write:** at the ring head. If `head + n` passes the end, wrap to 0 (the
tail sectors stay unused until the next lap). Every entry the new one
overlaps is evicted by the erase. The ring is FIFO by write age, not LRU: an
evicted app costs one rewrite (about 0.3 s) the next time it opens, and in
exchange every sector is erased once per lap, which is perfect wear
levelling with no allocator. The old entry for the same path is killed
(`live = 0`) once the new one commits.

**In-use entries are never erased.** RAM keeps a refcount per entry. If the
head would overlap a loaded entry, this launch takes the RAM path and logs
it. Nothing is loaded twice from XIP (the arena has one holder), so in
practice this only happens when one app fills most of the ring.

**Crash safety:** erase, write the code, write the header without `commit`,
then program `commit`. A power cut at any point leaves an uncommitted entry,
which is treated as free. Wiping the partition (`xip wipe`) is always safe.

**Wear:** 512 KB at 100k cycles per sector. Even 50 entry writes a day is
a lap every few days, which works out to centuries.

### 4. Loading (`elfload.c`)

The order changes: today it allocates code first. Data has to come first now,
because the flags are in `capp_info`, inside data.

1. Parse headers as today, and find `capp_main` and `capp_info` in the
   symbol table (moved up from the end).
2. Read only `capp_info`'s first four bytes (version, flags) straight from
   the file, and check the API version.
3. If the caller asked for the foreground and the arena is free and big
   enough: data goes into the arena; otherwise into the heap, as today.
4. **XIP** if: data is in the arena, the partition exists, and the flags lack
   `CAPP_CODE_IN_RAM`. Look up the cache. On a hit, `code = slot vaddr`. On a
   miss, relocate and write (below). If XIP fails at any point (ring busy,
   a write error), the code goes to executable RAM and **the data stays in
   the arena**. RAM code relocated against the arena is as good as any.
   **Otherwise** `code_alloc` from executable RAM, as today.
5. Read data into its block and relocate it against (data base, code base).
   Data is always loaded fresh from the `.capp` and relocated in RAM.

**Relocating code for flash, without a code-sized buffer:** the reason for
all this is that a 24 KB block may not exist. So the miss path uses the arena
itself as scratch, *before* data is read into it (the arena is reserved for
this launch and holds nothing yet). This is why step 2 reads the flags from
the file rather than from the loaded data. For each window of up to 28 KB of code:
read it, apply the `.code` relocations that fall in the window, erase, write.
Code of 28 KB or less is one window; `CAPP_MAX_CODE` (96 KB) is at most four.
The relocation arithmetic moves out of `elfload.c` into a portable
`capprel.c`, so whole-image and windowed relocation are the same code and the
host suite can compare them.

`LoadedApp` gains `xip_slot` (or -1). `capp_unload` drops the refcount
instead of freeing code. `capp_hold_code`, the spare block, is untouched: it
only ever holds RAM code.

The Today/command path loses nothing: those loads were RAM-path before and
still are.

### 5. Flash writes and the instruction cache

- **Code running during a write.** ESP-IDF already stalls the other core and
  disables the cache for every flash erase and write, because the kernel
  itself runs from flash. App code in `appcode` is flash code like the
  kernel's, so the same mechanism covers it. Nothing CardOS runs from an IRAM
  ISR calls into an app. OTA writes (`launcher.c`, `update os`) are covered
  the same way.
- **What is new:** never erase a slot that is mapped *and in use* (§3
  refcount), and never let the cache serve stale lines for a slot that was
  rewritten. IDF 6.1's `esp_flash` write path flushes the cache over the
  written range (`flash_end_flush_cache` in `esp_flash_api.c`). A device test
  confirms that: write a slot, read it back through the INST mapping, rewrite,
  read again.
- Map **without** `ESP_PARTITION_MMAP_BLOCKS_WRITE`, which would block our
  own writes for as long as the partition stays mapped (forever).
- `l32r` from IROM-mapped flash: this is how every line of the kernel's own
  flash code loads its literals (IDF's `.flash.text` holds `.literal`), so it
  is not a risk. The first device test, any app, proves it anyway.
- `.rodata` stays in data, so in the arena, for v1. Mapping it through the
  data bus (DROM) is a later step and would make the arena hold less.

### 6. Fallbacks, all to today's behaviour

| Situation | Result |
|---|---|
| No `appcode` partition (an OTA'd device, older table) | Data still in the arena for the app on screen, code in exec RAM; everything else as today |
| Arena held, or data > 28 KB | RAM path |
| `CAPP_CODE_IN_RAM` | Code in exec RAM, data in arena |
| Write failed, ring busy with in-use entries | Code in exec RAM, data stays in arena, line in `log` |
| Entry's reloc pair stale (new firmware) | Rewritten on this launch |

A big app on the RAM path fails as it does today, with "needs N KB in one
piece".

### 7. The opt-out flag

`CAPP_CODE_IN_RAM 0x0040` in `capp.h` (0x0020 is `CAPP_PAINT_DIRECT`). Adding a flag changes no table
layout, so **no API bump**. Old apps lack it and go to XIP, which is the
point.

Candidates: Kart, Pinball, Noodle, and Calc's graph plotting, decided by
measuring, not guessing. The I-cache is **16 KB** (`sdkconfig`:
`CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB`), shared with the kernel and the
radio stacks. Going to 32 KB costs 16 KB of SRAM and is a separate decision.

### 8. Build budget (`tools/build_apps.py`)

- **Data ≤ 28 KB** (the arena) stays a hard limit. It is the same number as
  today's `DATA_BUDGET`, now with a reason that does not depend on how the
  heap feels.
- **Total** limit goes away for XIP apps; code is bounded by `CAPP_MAX_CODE`
  (96 KB) and the ring.
- `CAPP_CODE_IN_RAM` apps: code ≤ 16 KB (it still needs one exec-RAM block;
  the hot-loop apps are 5–10 KB).
- Forklift (21 KB code, 27.5 KB data) fits and leaves `OVER_BUDGET`. Merging
  Jar back into one app is its own job, after this lands.
- A device without the partition cannot open an app past the old budget.
  The budget therefore changes in the same release as the table, and the
  full flash is part of rolling it out.

### 9. Tools

- `xip` in the console: lists the entries (name, KB, seq, in use, stale),
  plus `xip wipe`. `mem` gains the arena line and "appcode: N of 128 sectors
  live".
- Launching an app that needs a write shows "preparing Jar…" on the start
  row. A 24 KB write is about six 4 KB erases (≈45 ms each, cache off, the
  screen frozen meanwhile) plus programming: about 0.3–0.5 s, once.

## Testing

Host (`test/`, the existing suite):

- `test_xipcache.c` over an in-memory 512 KB "flash" that enforces erase
  semantics (programming can only clear bits): scan of empty, full, and
  wrapped rings; key hit, key miss, stale reloc; wrap and eviction; refusal
  to overlap an in-use entry; a torn write at every step (no commit means
  free); killing the old entry on rewrite; the head found from seq after a
  "reboot".
- `test_capprel.c`: on fixture `.capp`s from the real toolchain (Jar,
  Forklift, a small app), windowed relocation produces exactly the bytes
  whole-image relocation does, for window sizes that cut relocations at
  every boundary case; and data refs land on the arena base while code refs
  land on the slot vaddr.

Host, the fallback (added at approval):

- `test_capprun_arena` (or a case in an existing capprun test, whichever
  the fakes reach): while the arena is held, a second load (a headless
  command, `capprun_command`) is refused the arena and takes the RAM path,
  and the arena holder is unchanged afterwards. When the holder is released
  the arena is free again. Done at the arena module's level (`arena.c` is
  portable: claim, release, holder) if capprun cannot be driven on the host.

Device, measured (`tools/cardctl.py sh`), numbers into this spec and the
commit:

- **Under load, not idle** (added at approval): both radios up *and*
  background activity running (a print in progress, a Todo or Calendar
  sync), then `mem` and `mem map`: free heap, low water, largest 8-bit
  block, largest exec block, exec free. Taken on master before the change
  and on this branch after, in the same three states: launcher idle, Jar
  open, and Today gathering. The table goes under "Measurements" below.
- **Headless commands after churn** (added at approval): with Today in the
  foreground holding the arena, and after radio up/down churn (several
  prints and syncs), Today's sections still gather. Calendar, Todo and
  Habits load on the RAM path and answer, and `log` shows no load failure.
  If one fails, the "needs N KB in one piece" numbers go here.
- Launch time: first (write) and later (map) launches against today's load,
  for a small and a large app.
- Hot-loop cost: frame time for Noodle and Pinball, and Calc's graph redraw,
  XIP vs RAM. That decides the opt-out list.
- The stale-cache test in §5, and a power-pull during a first launch, which
  must come back with the entry treated as free.

## Measurements

Measured 2026-10-10 on the device (COM4). "master" is the firmware the
device was running before the flash: v0.10.0, debug, built Oct 9 08:46
from the main checkout, in ota_0 -- not exactly 12201ad, but before any of
this branch. "xip" is this branch at 8b35940 plus temporary timing logs.
The apps on the card were the same files for both (the main checkout's
build, API 44).

The printer never connected in either run (`connect failed (13)`; it was
off), so "print" means Bluetooth up for the ~15 s connect attempt, which is
the memory a print costs. Churn first in both: three `print test` and three
Todo opens. Low water is the minimum since boot, so read it across a row,
not down a column.

**One behaviour changes.** With the arena out of the heap, a print always
finds less than `PRINT_ROOM` and lets WiFi go first (`printq.c`, "making
room"); on master both radios stayed up through the print. WiFi then comes
back only when something asks for the network after the print. So the xip
"print" rows have Bluetooth up and WiFi down, and the master ones both up.

| State | Build | Radios | Free heap | Low water | Largest 8-bit | Largest exec | Exec free |
|---|---|---|---|---|---|---|---|
| launcher idle | master | WiFi | 101064 | 77904 | 57344 | 57344 | 69032 |
| launcher idle | xip | WiFi | 72420 | 62980 | 31744 | 25600 | 40388 |
| launcher idle, print | master | WiFi + BT | 64484 | 19664 | 31744 | 16384 | 32452 |
| launcher idle, print | xip | BT (WiFi let go) | 35648 | 25184 | 25600 | 7680 | 10020 |
| Jar open | master | BT off, WiFi let go earlier | 95676 | 19664 | 40960 | 40960 | 63644 |
| Jar open | xip | WiFi | 72064 | 62640 | 31744 | 25600 | 40032 |
| Jar open, print | master | WiFi + BT | 18004 | 9208 | 8704 | 7680 | 7932 |
| Jar open, print | xip | BT (WiFi let go) | 35044 | 19096 | 19456 | 7680 | 15504 |
| Today gathering, print | master | WiFi + BT | 45684 | 7968 | 31744 | 7680 | 13652 |
| Today gathering, print | xip | BT (WiFi let go) | 35248 | 18988 | 25600 | 7680 | 9620 |
| Today gathering, no print | xip | WiFi | 95424 | 18988 | 31744 | 30720 | 63392 |

What the rows say:

- **Idle costs the arena**: 28.6 KB less heap with nothing open (101 -> 72
  KB with WiFi up), as designed.
- **Opening Jar costs nothing**: 72420 -> 72064 (0.36 KB). On master Jar
  took about 45 KB (25.7 KB code + 19.2 KB data) and, with a print
  running, would not open at all ("code wants 25720, largest 22528"); it
  only ran beside a print when opened first, leaving 18 KB and a low water
  of 9 KB.
- **Today beside a print fails on both**: every section's RAM-code load
  wants more than the 7.7 KB largest exec block Bluetooth leaves (Calendar
  16.9 KB, Todo 15.0, Habits 13.3, Toggl 8.8). Not worse, not better.
  Without a print (Step 5) every section gathers on xip; Calendar's first
  try fails ("largest 15872"), `make_room` lets WiFi go, the retry loads.
- With Bluetooth up on xip, WiFi cannot rebuild until it goes: `wifi`
  needs 72 KB free (`WIFI_MIN_HEAP`) and there are 34. Todo's sync during
  a print says "offline: only 34 KB free, needs 72"; on master it said 30.
- TLS at the new idle heap: `get https://cardos.arowe.net/dash` (1599
  bytes) worked with 71 KB free, WiFi up, Bluetooth off.

### Launch times

Device side, from temporary `esp_timer` logs around `capp_load_ex` and
`capprun_start` (not committed). "start" includes `capp_main`.

| App (code) | First launch (write): load / start | Later (hit): load / start | `find_symbols` |
|---|---|---|---|
| Jar (25.7 KB) | 978 / 1113 ms | 716-731 / 860-876 ms | 662 ms |
| Calc (18.8 KB) | -- | 373 / 393 ms | -- |
| Clock (11.1 KB) | 480 / 576 ms | -- | -- |
| Edit (12.0 KB) | 294 / -- ms | 299 / 320-350 ms | ~260 ms |
| Files (7.5 KB) | 365 / 394 ms | 258 / 287 ms | -- |
| Pinball (4.8 KB) | 279 / 281 ms | 159 / 162 ms | 127 ms |

The flash write is 100-270 ms of a first launch. A hit's own cost (find
the entry, verify, reference) is about 30 ms for Jar. **Most of every
launch is `find_symbols`**, which reads the symbol table one 16-byte entry
and one name at a time, a seek each (pre-existing; capp_main and
capp_info are the last two symbols). Batching it as `relocate` batches its
reads would take about 0.6 s off every Jar launch and most of a 10 s icon
scan. From the PC, `cardctl open jar` round trips were 1.18 s on master and
1.19-1.29 s on xip (hit), 1.39 s on a miss; that includes Python and the
port and cannot separate the two.

### Hot loops (Step 7)

Temporary counters in `tick` (not committed): loop passes and repaints per
5 s, three windows each, 15 s per app; Calc's graph redraw timed per paint.
Same binaries but for the flag, both from the card.

| App | Code from flash | `CAPP_CODE_IN_RAM` | Difference |
|---|---|---|---|
| Kart (racing) | 34 / 34 / 34 frames per 5 s | 34 / 34 / 34 | none |
| Calc graph redraw (`y=sin(x)*x`) | 168-171 ms | 169 ms | none |
| Pinball (playing) | 717 / 588 / 790 passes (160 / 248 / 148 paints) | 746 / 585 / 820 (170 / 293 / 148) | within play-to-play variation, < 5% |
| Noodle (mic-driven) | 79 / 41 / 87 frames | 101 / 37 / 39 | tracks the room's noise, not the code |

**No app is slower from flash by more than 10%, so no app carries
`CAPP_CODE_IN_RAM`.** Kart is CPU-bound at about 7 frames a second either
way; its inner loop fits the instruction cache, which is the expected
result. Calc's code is 18.8 KB, over the 16 KB RAM-code budget, so it could
not take the flag anyway (measured with the budget raised temporarily).

### Other device checks

- `xip wipe` with 43 sectors live: 566 ms round trip; 250-300 ms with
  fewer. No watchdog line in the log.
- Map base 0x421b0000 and the arena the same across three reboots and
  several reflashes: Jar hit every time, no `dead` or stale entries.
- Shell stack high-water during launches: 4.0-4.6 KB free of 8 KB on a
  miss or a hit. Edit with no file opened the picker inside `capp_main`
  with 4.6 KB of `PmEntry` on the stack and overflowed it through
  `cardctl open` (fixed in 40abf70; 4.5 KB free after).
- Stale cache (§5): a `jar.capp` put with the same size and one changed
  byte (title "Jar FactorX"), mtime different because the clock is set:
  the put itself marked the old entry `dead` (fs hook), the next open
  wrote a new entry and loaded "Jar FactorX". `xip wipe`, open: written
  again, ran.

## Out of scope

- Merging Jar back into one app (after this lands).
- `.rodata` in DROM.
- Pinning the arena's address with `SOC_RESERVE_MEMORY_REGION`.
- Making the icon scan read only `capp_info` and skip the code. It is worth
  doing, but it is a separate change.

## For alex to decide

1. **The arena:** 28 KB of heap reserved for good, in exchange for the
   foreground app taking nothing from the heap. Without it, XIP cannot work
   on this chip (above).
2. **The split:** `appcode` 512 KB, `spiffs` 188 KB. Or 448 / 252 if guests'
   LittleFS matters more than holding every app at once.
3. **FIFO eviction** (by write age, perfect wear levelling) rather than LRU.
