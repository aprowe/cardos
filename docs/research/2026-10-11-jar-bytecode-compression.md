# Jar scripts: complicated behaviour in very little memory

2026-10-11. Research only: nothing in `server/`, `apps/` or `test/` changed.
The prototype is `tools/bytecode_lab/`; every number below comes from running
it, and `tools/bytecode_lab/results/` holds the raw output.

## The short answer

1. **Re-encode the bytecode (format 2, "dense").** Keep the same stack
   machine, the same language and the same compiler front end, and change only
   the bytes. That halves every script: **14,071 → 7,032 bytes on a
   243-script corpus (50.0%)**. All 243 scripts behave identically to today's
   machine over three simulated days each. The machine also executes **25%
   fewer instructions**. It runs in place, so the device needs no decoder RAM.
   It is about 100 extra lines of C, and an action table (256 bytes) goes in
   flash.
2. **Entropy coding or LZ on top buys only about 5–9 more points** (41–46%
   of today's size). Each needs a trained table or dictionary that the
   firmware and the server share, and LZ needs a decompress buffer. Not worth
   it yet.
3. **The next real step is a behaviour table (format 3), not a better
   compressor.** A small state machine stored as data points to motion
   primitives in the firmware: a physics body, an arc to a target, perch
   points. With it, the three sandbox behaviours (pill bug, tree, bird) take
   **84 bytes in total**. Today's language needs 278 bytes for a crude
   imitation, and that imitation still has no momentum, no arcs and no perches.
   The table cannot loop, so it needs no step counter.

**Recommended order:** format 2 first, because it is cheap, provable and
compatible. Then format 3 for motion, as its own record section beside the
handlers. Skip general-purpose compression.

## What the 256 bytes actually hold today

### The corpus

There is no local copy of the server's pool, and reading the droplet was not
permitted, so I built the corpus three ways:

| source | scripts | how |
|---|---:|---|
| local | 43 | every string literal in the repo's tests, fixture maker, Jar Lab and the guide's example that compiles as a script (`corpus.py`, found by walking the Python AST) |
| claude-short | 160 | Claude asked with the real `LANGUAGE_GUIDE_V2` and the generation prompt's "at most 12 lines" rule, in 4 batches of 40 varied items (`gen_corpus.py`) |
| claude-rich | 40 | the same, but asked for the most elaborate behaviour the language allows |

All were compiled by the real compiler (`server/jarvm.py`, language v2).

### Sizes in today's format 1

| source | mean | median | p90 | max | header share | mean fill of 256 | handlers |
|---|---:|---:|---:|---:|---:|---:|---:|
| local | 33.2 | 17 | 66 | 159 | 25.0% | 13% | 2.1 |
| claude-short | 46.6 | 45 | 59 | 80 | 27.2% | 18% | 3.6 |
| claude-rich | 129.6 | 129 | 147 | 179 | 16.5% | 51% | 6.5 |
| all | 57.9 | 47 | 129 | 179 | 23.0% | 23% | 3.8 |

Even when asked for elaborate behaviour, Claude never came near 256 bytes; the
largest script was 179. **The 256-byte record cap is not what binds. The
shared spool is.** `JS_SPOOL` is 768 bytes for all 24 placed items, about 32
bytes per item. At today's 46.6-byte mean, the spool holds **16** typical
scripts. In format 2 it holds **33**, more than the jar can place.

### Where the bytes go (code only, 10,831 bytes)

| op | count | bytes | share |
|---|---:|---:|---:|
| ACTK (action + constant argument) | 1,854 | 5,562 | **51.4%** |
| PUSH8 | 799 | 1,598 | 14.8% |
| END | 924 | 924 | 8.5% |
| JZ | 294 | 588 | 5.4% |
| STORE / SENSE / LOAD | 569 | 1,138 | 10.5% |
| everything else | | 1,021 | 9.4% |

Operand distributions:

- **Actions:** `say 0`, `frame 1`, `say 1` and `frame 0` alone are 684 of the
  1,854 action calls. 95 distinct (action, argument) pairs appear, and **97.7%
  of them fall in a 94-entry table** built from the enumerations in
  `jarvm.def` and a few round numbers, not tuned to the corpus.
- **Constants:** 779 of 802 pushes are between −8 and 23. Most are 0 and 1.
- **Memory:** slot 0 is 292 of 375 slot uses.
- **Senses:** `neartraits` is 74 of 194 sense reads.
- **Jumps:** all jumps are forward and short. 112 of 353 jump exactly 5 bytes.
- **Header:** 3 bytes per handler, which is 23% of all bytes. Most filters
  are 0 or a small name (night = 4, rainy = 4), so they fit in 3 bits.

The most common sequences are ACTK→END (454), ACTK→ACTK (438), LOAD→PUSH
(102), SENSE→PUSH→BIT→JZ (40, the trait test), PUSH→RAND→PUSH→EQ→JZ (the
"once in N" chance) and LOAD k→PUSH→ADD→STORE k (`mem[k] += c`).

## Candidate encodings (part 2)

### Format 2, "dense" (`dense.py`)

The byte code changes; the stack machine stays the same.

- **Header entry:** 1 byte for the event (5 bits) and an inline filter
  0..6 (3 bits; 7 means a filter byte follows), plus 1 offset byte.
- **Implicit END:** a handler ends where the next one begins, so the compiler
  drops each handler's final END.
- **ACT1:** one byte, 0x80–0xFF, indexes a flash table of (action, argument)
  pairs. ACTK remains for anything else.
- **PUSHS:** one byte pushes a constant from −4 to 27.
- **One-byte LOAD k, STORE k and SENSE k.**
- **Fused compare-and-branch:** `JN<cmp>`, `JNBIT` and `JNZ`, each taking a
  relative offset.
- **ADDM k, c:** `mem[k] += c` in 2 bytes instead of 5–6.
- **Relative jumps:** offsets count from the end of the instruction, so code
  is relocatable. That matters for any dictionary or library later.

Format 1's opcodes 0–26 keep their meanings, so the machine's switch statement
only grows.

### What each part saves (`ablation.py`)

| feature | alone, on top of format 1 | cost of removing it from format 2 |
|---|---:|---:|
| ACT1: action + argument in one byte | **−25.8%** | +25.8% |
| implicit END at the end of a handler | −6.5% | +6.5% |
| compact entries | −5.7% | +5.7% |
| PUSHS: small constant in the opcode | −5.6% | +5.1% |
| LOAD/STORE/SENSE k in the opcode | −4.0% | +3.2% |
| ADDM | −2.1% | +0.8% |
| compare-and-branch | −1.7% | +1.7% |
| **all of them (format 2)** | **−50.0%** | |

More superinstructions mined from what format 2 leaves behind would save
another 4.8% of format 2:

- a fused "once in N" chance (`PUSH N RAND PUSH k JN..`, 75 sites);
- `JNHAS sense,trait` (66 sites);
- a frame/say variant counted from 1 (59 sites).

That would bring format 2 to about 47.6% of today's size. Optional.

**Register machine?** No. The scripts have almost no expressions to keep in
registers; 51% of the bytes are action calls. A register encoding would spend
operand bits on naming registers and save nothing.

**Bit-packed fields** are measured below, under compression, as the
bit-packed Huffman variant of format 2.

### Proving it

`verify.py` runs `jarvm.simulate_day` on the format-1 bytes with the real
machine, and on the format-2 bytes with `dense.run2`. Over three seeds it
compares every handler run: event, result, every action in order, position,
and memory at the end. **243 of 243 match.**

The verifier is not trivially passing. With one corrupted entry in the ACT1
table, 167 of 243 scripts differ.

## Compression (part 3, `compress.py`)

Every trained model is leave-one-out, or 5-fold for the LZ dictionaries: a
script is never coded with a table built from itself.

| format | bytes | % of today | executes in place? | device cost |
|---|---:|---:|---|---|
| format 1 (today) | 14,071 | 100% | yes | — |
| format 1 + compact header + implicit END | 12,356 | 87.8% | yes | — |
| **format 2 (dense)** | **7,032** | **50.0%** | **yes** | ~100 lines of C, 256 B table in flash, 0 RAM |
| static Huffman over format-1 bytes | 7,836 | 55.7% | no | |
| static Huffman over format-2 bytes | 5,906 | 42.0% | no (decode to RAM) | ~300 B flash table, 256 B buffer |
| format 2, bit-packed: Huffman opcodes, raw operands, 11-bit jump targets | 6,255 | 44.5% | yes, with a bit cursor | ~300 B flash table; ~3× slower decode per instruction |
| deflate (zlib, raw) on format 2 | 7,614 | 54.1% | no | tinfl state ~11 KB: too much |
| deflate on format 2 + 32 KB preset dictionary | 5,769 | 41.0% | no | dictionary 32 KB in flash plus inflate state: too much |
| LZ, shared flash dictionary 256 B | 6,760 | 48.0% | no | dictionary in flash, 256 B output buffer |
| LZ, shared flash dictionary 512 B | 6,529 | 46.4% | no | as above |
| LZ, shared flash dictionary 1 KB | 6,317 | 44.9% | no | as above |
| LZ, no dictionary | 7,912 | 56.2% | no | |

What this shows:

- **Most of what Huffman finds is the redundancy format 2 already removes.**
  Huffman over today's bytes gets 55.7%; format 2 alone gets 50%.
- **Format 2 is the floor for anything that runs in place.** Bit-packed
  Huffman (44.5%) does run in place, but jumps must name bit offsets, decoding
  is serial, and the gain over format 2 is 11%.
- **LZ and deflate cannot run in place,** because they use back-references.
  So the spool, the RAM that matters, would hold decompressed bytes and save
  nothing. They help only on the wire and in the record, which are not the
  binding limit.
- **A "standard library of idioms" as an LZ dictionary** is worth 2–5 points.
  The corpus's handlers are already 2–4 one-byte actions, and there is little
  left to share.

## Moving complexity into the VM (part 4)

The three sandbox behaviours, each written in three designs (`examples.py`):

- **today:** the language as it is. It cannot express momentum, arcs or
  perches, so these are the closest it can come.
- **extended:** today's language plus motion primitives. `lab_ext.def` adds
  `body`, `push`, `swoop`, `wobble` and `anim`, and the events `stopped`,
  `landed`, `perched` and `hit`. The real compiler compiles it over the
  extended table (`labvm.py`).
- **table (format 3):** a behaviour table (`bt.py`).

| example | today f1 | today f2 | extended f1 | extended f2 | **table** | what "today" loses |
|---|---:|---:|---:|---:|---:|---|
| pill bug | 100 | 49 | 115 | 64 | **32** | no momentum or bounce: "rolling" is a walk to a zone with the frames swapped |
| tree | 73 | 37 | 61 | 32 | **22** | shakes on a signal number the bird must agree on, not on a landing |
| bird | 105 | 56 | 104 | 68 | **30** | flies about and goes home; cannot pick a perch or arc to it |
| all three | 278 | 142 | 280 | 164 | **84** | |

### Why imperative scripts stay large even with motion primitives

The language has no way to say "then, later". A sequence over time, such as
roll, stop, wait 2 s, uncurl, wait 1 s, crawl, has to be a state machine
written by hand in `mem` slots driven from `on tick`. That is where the bytes
go. Adding motion verbs does not shrink the extended scripts below today's
sizes: they do more with the same bytes, but the timing code remains.

The behaviour table makes states, timers and "the motion finished" first-class.

### Format 3, the behaviour table

```
byte 0  format 3
byte 1  states-1 (3 bits) | perch points (2 bits) | global transitions (3 bits)
byte 2  body preset (4 bits: gravity, friction, bounce, top speed, arc apex from a flash table)
perch   2 bytes each: dx, dy
global  3 bytes each: [trigger:5 | next:3] [arg] [one ACT1 action]    (poke, bumped... in any state)
state   [motion:3 | param:5] [first frame:2 | frames-1:2 | ticks a frame:4] [n actions:4 | n transitions:4]
        actions: ACT1 bytes (format 2's table)
        transitions: [trigger:5 | next:3] [arg]
```

**Triggers:** any event (`tick` … `hit`), `after N` ticks, `done` (the
motion finished), or `chance N/256` per tick.

**Motions:** still, crawl, roll (an impulse), arc (to the nest, a random
perch, or a friend), perched (looking around), sway, hang, float.

**The pill bug, as a table** (32 bytes):

- **Globals:** poke → roll; bumped → roll; hit → `sound plop`.
- **States:**
  - crawl: frames 0–1;
  - roll: impulse 160 px/s, ball frame, `sound boing`; done → rest;
  - rest: after 8 ticks → uncurl;
  - uncurl: `say 1`; after 4 ticks → crawl.

### The tables run

`btsim.py` is a toy device: a 240 px jar, fixed-point physics in 1/16 px, a
quadratic-Bezier arc, and perch points that fire `perched` on their owner.
From `results/btsim.txt`:

```
  3.00s  pillbug  x= 62  -> state 1 (poke) roll  does sound 3
  4.25s  pillbug  x=232  bounces off the glass
  6.65s  pillbug  x=149  -> state 2 (done) still
  8.60s  pillbug  x=149  -> state 3 (after 8) still  does say 0
  9.60s  pillbug  x=149  -> state 0 (after 4) crawl
 25.60s  bird     x=215  -> state 1 (chance) arc  does sound 6
 26.85s  bird     x=157  lands on tree's perch 1
 26.85s  tree     x=150  -> state 1 (perched) sway  does emit 4
```

**Hybrid.** Format 3 does not replace format 2. Handlers keep the logic
(traits, berries, signals, memory). Tables keep motion and timing. Add one
trigger, "run handler k", and one action, "go to state s", and each can drive
the other.

**A flash library of macros** (`swoop_to`, `roll_physics`, `look_around`) is
the same idea as body presets and motion modes. As callable bytecode it fails
for the same reason the extended scripts stay large: a routine that unfolds
over time needs state of its own. Put the library in the firmware as motion
primitives and presets, which is what format 3 does.

## Runtime state, determinism and safety (part 5)

### Per-item RAM

`JPlaced` is already about 208 bytes × 24 items, about 5 KB. It already has
`x`/`tx` (32-bit sub-pixels), `vx`, `yoff`, `vy`, `flip`, `cur`, and the
part's `px`, `py`, `ptx`, `pty`, `pt`. Those cover a physics body and an arc's
endpoints.

Format 3 needs **4 to 6 new bytes per item:**

- `state` (u8)
- `stimer` (u16)
- `perch` (u8: whose, and which)
- `arc_n` (u8)
- optionally an arc start (2 × int16) when the whole item flies rather than
  its part

That is **≤ 240 bytes for 24 items**. Jar's data block is 17,256 of 28,672
bytes (`build/apps/sizes.txt`), so it fits easily.

Format 2 adds **no** RAM, and halves the spool's fill. That could become RAM
back: `JS_SPOOL` could drop from 768 to 512 bytes and still hold more scripts
than today.

### Boundedness and determinism

- **Format 2** keeps the 64-step limit and the 8-deep stack. It counts fused
  instructions as one step, so a script runs strictly fewer steps than its
  format-1 twin: 336,789 against 450,297 over the corpus's simulated days.
- **Format 3** has no loops to bound. At most one transition fires per item
  per tick, `after 0` is refused, and every trigger waits for time or an
  event.
- **Physics is integer** (1/16 px, friction and bounce in /256) and randomness
  is the jar's xorshift. Server and device compute the same path.

### The server's checks

`simulate_day` extends naturally:

- the compiled format-2 bytes run through `dense.run2`, which is
  `verify.py`'s machine;
- tables get `bt.check`: next-state bounds, `after 0`, the action table, and
  unreachable states;
- a simulated day for tables (a port of `btsim.py`) catches "never does
  anything visible".

### Signing

Signing is unaffected: script bytes stay opaque inside the record, and the
signature still covers everything except the signature and the memory slots.

## Compatibility (part 6)

- **The format byte is already there.** `jv_code0` refuses anything but
  format 1, so firmware today ignores a format-2 or format-3 script and falls
  back to the item's recipe and habits. That is the spec's "newer format:
  recipe" rule. Old records keep working everywhere.
- **Step A: no wire change.** The device transcodes format 1 → format 2 when
  it places an item (`js_place`), and executes format 2 from the spool. The
  transcoder is a pure function of format 1, the peephole in
  `dense.lower` + `layout`, about 150 lines of portable C. The signature still
  covers the format-1 bytes. Server change: none. Test change: a fixture set
  of (format-1 bytes, expected format-2 bytes) generated by
  `tools/bytecode_lab/dense.py` and checked by `test/test_jarvm.c`, plus
  `jv_run` for format 2 replaying the existing trace fixtures.
- **Step B: format 2 on the wire,** only if record space ever binds. This
  needs the device to report its VM version on `/jar/day` (`vm=2`) and the
  server to sign a format-2 copy. Not needed now: the record has 96 bytes
  spare and scripts top out at 179.
- **Step C: format 3 tables.** These go in their own record section, after
  the script, with a length in the 9 spare bytes of the 224-byte header (bytes
  215–223). Perch points could live there too: 2 bytes each. Old firmware
  ignores a longer header (it reads byte 1), so tables are invisible to it.
  The firmware gains the motion engine, which is what the sandbox is
  prototyping, and its parameters become the body presets. The server
  compiler gains a table writer from whatever the sandbox's surface syntax
  becomes.

## Ranked recommendation

| # | do | saves | device cost | risk |
|---:|---|---|---|---|
| 1 | **Format 2 by transcoding on placement (step A)** | spool 50%; 25% fewer VM steps | ~150 lines of C, 256 B flash table, 0 RAM | low: proven on 243 scripts, fixtures pin it |
| 2 | **Format 3 behaviour tables for motion** (with the sandbox's engine) | the sandbox's behaviours in 22–32 B each, against 73–105 B for weaker versions today | motion engine in flash, ≤ 6 B RAM per item | medium: new engine, but data-only and loop-free |
| 3 | The extra superinstructions (chance, has, from-1) | another 4.8% of format 2 | a few switch cases | low; only if the spool tightens again |
| 4 | Huffman, LZ or deflate | 5–9 more points | tables and buffers; LZ cannot run in place | not worth it |

### Concrete next step

Port `dense.lower` + `dense.layout` + `dense.run2` to C in `apps/jarvm.h`, as
`jv_transcode(f1, n, out)` and format-2 cases in `jv_run`/`jv_check`.

Then:

- make `tools/make_jarvm_fixtures.py` write the format-2 bytes for each
  fixture;
- have `test/test_jarvm.c` require that the transcoder's output matches them
  byte for byte, and that the format-2 run replays the same traces;
- call `jv_transcode` in `js_place` and `js_patch` before the bytes go into
  the spool.

Measure `sused` on a device before and after. Expect about half.

## Files

`tools/bytecode_lab/`:

- `corpus.py`, `gen_corpus.py`, `corpus/*.json`: the corpus
- `isa.py`: format 1 decoded
- `measure.py`: part 1
- `dense.py`: format 2, its encoder and its machine
- `verify.py`: equivalence over simulated days
- `ablation.py`
- `compress.py`: Huffman, bit-packing, LZ, deflate
- `lab_ext.def`, `labvm.py`: the extended language
- `bt.py`, `btsim.py`: format 3 and its toy device
- `examples.py`: the three behaviours
- `results/`: the outputs quoted here
