# App data overlays — design

Status: **approved by alex 2026-10-10** ("cool do the overlay and the flash constants"), with the defaults in "For alex to decide": card-backed saves, four overlays, 0 live at start, first real user chosen later.
Date: 2026-10-10. Builds on `2026-10-09-xip-app-code-design.md` (branch
`worktree-xip-app-code`).

## The problem

Since the XIP change, an app's code costs no RAM, and the app on screen
keeps its data in the 28 KB arena. Data is now the only limit, and the apps
near it are not full of constants. They are full of zeroed state: buffers,
grids, tracks, undo history. Measured from `build/apps/*.o` on 2026-10-10:

| app | data | constants | zeroed state |
|---|---|---|---|
| midi | 28.4 KB | 2.6 KB | 25.8 KB |
| forklift | 28.2 KB | 3.8 KB | 24.4 KB |
| notes | 27.1 KB | 2.1 KB | 25.0 KB |
| calc | 24.4 KB | 2.9 KB | 21.5 KB |
| all 43 apps | 465 KB | 84 KB (18%) | |

Moving constants to flash would help Jar, whose apps are 30–47% constants,
and barely help the rest. The general answer has to be about the zeroed
state.

Most of that state belongs to one mode of the app: Midi's editor or its
player, Jar's shop or its post office, Forklift's editor or its warehouse.
Those modes are never on screen at once, so their state need never be in RAM
at once.

## The idea

An app marks the state that belongs to one mode as being in **overlay N**.
All of an app's overlays share **one region** at the end of its data block,
so the data block holds `common + the largest overlay`, not `common + all of
them`. One overlay is **live** at a time. `api->overlay(n, flags)` switches:
it saves the live overlay to the card (unless told to discard it), then
brings overlay `n` back from the card, or zeroes it if it has never been
used.

Only zeroed state (`.bss`) may be overlaid. That is what makes this small:
there is no starting image to carry or load, only what the app itself left
there. Code needs no overlays, since it is in flash. Constants stay in the
common part.

Example: Midi with its editor state in overlay 0 and its player state in
overlay 1 would need `common + max(editor, player)` instead of the sum.

## Design

### 1. Marking state (`capp.h`)

```c
#define CAPP_OVERLAY(n)  /* puts a zero-initialised variable in overlay n */
CAPP_OVERLAY(1) static ShopState shop;
```

The macro puts the variable in a named section the linker can tell apart
from ordinary `.bss`. Exact spelling is for the plan, with two constraints:

- The section must be **NOBITS**: no zeros in the file. GCC infers that
  from the name: a `.bss.`-prefixed name is nobits.
- `capp.ld`'s generic `*(.bss .bss.*)` inside `.data` must **not** swallow
  it. Either the overlay patterns are matched first, or the overlay name
  escapes the generic glob.

At most `CAPP_OVL_MAX` (4) overlays, numbered 0–3.

### 2. Linking (`apps/capp.ld`)

After `.data`, an `OVERLAY` statement places one output section per overlay
(`.ovl0` … `.ovl3`), all at the same address:
`ALIGN(16)` after the end of `.data`. GNU ld gives each the same VMA and
sizes the region to the largest. Every reference to an overlay variable is
an ordinary absolute address above `CAPP_DATA_ORIGIN`, so the relocations,
the arena and the flash cache need no change: the region is simply part of
the data block.

### 3. The build (`tools/build_apps.py`)

- **Data budget:** `common + max(overlay)` ≤ 28 KB, replacing `common` ≤
  28 KB. `sizes.txt` gains a column per overlay.
- **Refused:**
  - an overlay section that is not NOBITS (an initialised variable marked
    by mistake);
  - overlays that do not all start at the same address after `.data`;
  - `capp_info` or anything it points to inside an overlay. The icon scan
    reads it without running the app.

### 4. Loading (`kernel/app/elfload.c`)

Today the loader takes the first data-side section as the whole data
block. Instead:

- The block runs from `CAPP_DATA_ORIGIN` to the end of the last data-side
  section, overlays included.
- It is zeroed, and the PROGBITS `.data` is read into its start, as now.
  The overlay region starts as zeros.
- The region's offset and each overlay's size are recorded in `LoadedApp`
  (`ovl_off`, `ovl_size[4]`, `ovl_n`), from the `.ovlN` section headers.
- The info-only load used by the icon scan ignores overlays.

Nothing changes for an app without overlays: the block is exactly the one
section, as now.

### 5. Switching (`kernel/app/ovl.c`, portable, host-tested; device glue in capprun)

```c
/* API 37 */
int (*overlay)(int n, int flags);   /* 0 ok; -1 failed, and nothing changed */
int (*overlay_live)(void);          /* the live overlay, or -1 for none */
#define CAPP_OVL_KEEP     0   /* save the live overlay before switching */
#define CAPP_OVL_DISCARD  1   /* throw it away: next time it comes back zeroed */
```

- At start, **overlay 0 is live and zeroed**. An app with no overlays gets
  -1 from both calls.
- `overlay(n, KEEP)`:
  1. Write the live overlay's bytes (its own size, not the region's) to
     `/cache/ovl/<load>-<live>.bin`.
  2. Read `/cache/ovl/<load>-<n>.bin` into the region if it exists; zero the
     region otherwise.
  3. Mark `n` live.
- If the write fails (no card, a full card), return -1 with the live overlay
  untouched. If the read fails after a good write, zero the target and
  return -1. The app sees an empty mode and is told so. Nothing is ever half
  loaded silently.
- `overlay(n, DISCARD)` skips the write and deletes the live overlay's file,
  if any. It works with no card.
- `overlay(live, …)` is a no-op returning 0.
- `<load>` is a per-load id from capprun. Two instances of one app (on
  screen and headless) never share files.
- On unload (`capp_unload`), the load's files are deleted. At boot,
  `/cache/ovl` is emptied, since a power cut can strand files.
- Cost: one write of the outgoing overlay and one read of the incoming,
  about 50–150 ms for 20 KB on the card. A mode switch, not a frame. `log`
  gets nothing per switch, since the card is not for chatter. A failed
  switch logs one line.

### 6. The app's side of the bargain

- **A pointer into an overlay is valid only while that overlay is live.**
  The OS cannot trap a stray access cheaply on this chip (no MMU over RAM).
  Code that touches overlay 1 while overlay 2 is live corrupts overlay 2.
  This goes in `capp.h` as the one rule, and the first user shows the
  pattern: each mode's state reached only through that mode's functions.
- Headless commands (`run_command`) run with overlay 0 live, so a command
  should keep to the common part or overlay 0.

### 7. What does not change

The flash cache, its keys and its relocation, the arena (still one block,
now `common + region`), and apps without overlays: byte for byte the same
load. The API version moves to 37, so every `.capp` is rebuilt, as for any
API change.

## Testing

Host:
- `test_ovl.c` for `ovl.c`, over in-memory files:
  - keep and discard;
  - a first visit is zeroed;
  - a switch back restores exactly;
  - the outgoing overlay is saved at its own size when sizes differ;
  - a write failure leaves the live overlay untouched;
  - a read failure zeroes and reports;
  - switching to the live overlay is a no-op;
  - unload deletes the files;
  - two load ids never collide.
- `build_apps.py` checks, run on a fixture app with two overlays:
  - they overlap;
  - the budget counts the larger one;
  - an initialised variable in an overlay is refused;
  - `capp_info` in an overlay is refused.

Device:
- A small CLI app (`ovltest`, folder `-`) fills overlay 0, switches to 1,
  fills it, switches back and checks every byte, then the same with
  DISCARD. It prints pass or fail.
- Switch time for a 20 KB overlay.
- `mem` before and after shows the data block at `common + max`.
- The first real user is chosen after this lands. Jar's rewrite is the
  obvious one (shop and post as overlays); Midi or Forklift could follow.

## Out of scope

- Overlays with initialised data or constants. Constants in flash is the
  separate, smaller follow-up.
- Code overlays (code is in flash).
- Automatic switching, or detecting a stray access.
- Keeping an overlay's contents after the app closes. It is zeroed state,
  and it goes when the app goes, like `.bss`.

## For alex to decide

1. **Card-backed** saves (`/cache/ovl`), as above. The alternative is a
   second RAM block, which would defeat the purpose.
2. **Four overlays** per app, numbered from 0, with 0 live at start.
3. **The first real user:** the Jar rewrite, when you are ready for it.
