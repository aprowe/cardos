# JarScript 3 on CardOS -- design

2026-10-14. The owner chose: v3 **replaces** v2 -- every item owned or on a
shop shelf is **converted** to v3, and the pool's v2 items are **retired**
(never stocked again); the v2 machine stays on the device only until the
converted records have reached it, then goes. Scripts are **tiered** 0 / 512 / 1024
bytes, chosen by the code; frames stay 16x16 (big scenery uses `scale 2`);
approach A -- measure on the device, then the language, then physics sized by
the numbers. "Write all the code, with the fact in mind we might need to make
trims; look for places to get extra memory."

The language is the draft in `tools/jarsandbox/jarscript3.md` (the browser
sandbox, the design reference). This document is what the **device** and the
**server** run. Where the sandbox was loose (JS doubles, order-dependent
delivery) this pins it down so a C machine and a Python machine agree byte for
byte; the differences are listed at the end.

## Step 0: measured (2026-10-14, the device, release of the branch)

`jv3bench` (throwaway, since deleted) on a Cardputer with WiFi up, the
fixtures' scripts placed 24 at a time, 1200 ticks (a minute of jar):

| jar | per tick | of the 50 ms tick | worst second |
|---|---|---|---|
| 12 different scripts (the mug, the branch, mover, physics, events...), 9.5 KB | 260 us | 0.5% | 6 ms |
| 24 x the step-budget burner (929 B) and the 9-state caterpillar, 20 KB | 776 us | 1.6% | 17 ms |
| 24 physics bodies at once | 263 us | 0.5% | 7 ms |

`sizeof(Jv3Item)` is 252 bytes (the estimate was 170: a 16-entry queue and
32-bit path points). The decision rule was 3 ms a tick for 20 Hz and 2 ms
for physics: both pass by a wide margin, so 20 Hz, the full budgets, and
physics in phase 1, the jar capping bodies at 12 (`max_bodies`).

Heap, the same day: 66 KB free and a 31.7 KB largest block before the jar
opens; big-mode room 31.7 KB after a boot, 23.5 KB after 19 minutes. The jar
takes one 16 KB block for v3 (less 2 KB of what there is, when there is
less).

## Pieces

| | |
|---|---|
| `apps/jar/jarvm3.def` | THE table: ops, events, senses, props, functions, actions, statement words. C and Python both read it. |
| `apps/jar/jv3.h` | the C machine: portable, libc-free, no allocation (the caller hands it memory). Host-tested. |
| `server/jar/jarvm3.py` | the compiler (source -> bytes), the same machine, `simulate`, the language guide. |
| `tools/make_jarvm3_fixtures.py` | compiles fixture scripts, runs them in Python against the fixture world, writes `test/fixtures/jv3_*.bin` + `.trace`; `test/test_jv3.c` replays them in C and needs the traces equal. |
| `apps/jar/jarsim.h`, `jarlive.h`, `jar.c` | the jar runs v3 items through jv3, draws them, and keeps their memory in one big-mode block. |
| `apps/jv3bench.c` | step 0, throwaway: a CLI app timing the machine on the device. |

## Numbers

Every value is a signed 16-bit **raw** number; its meaning is raw/16 (four
fraction bits): -2048 .. 2047.9375. Literals are rounded to the nearest 1/16
(halves away from zero). Booleans are 0 and 16 (1.0); anything raw != 0 is
true.

- `+ -`: saturate to -32768..32767.
- `*`: floor((a * b) / 16) on 32 bits, saturated.
- `/`: b == 0 -> 0. Else trunc((a * 16) / b) (toward zero), saturated.
- `%`: b == 0 -> 0. Else floored: r = a - b * floor(a / b) on raw values.
- comparisons on raw values; `and`/`or` evaluate both sides, give 0 or 16.
- `neg`: saturating. `not`: 16 if 0 else 0.

Items are **handles**: slot + 1 in the jar (1..24), 0 nobody, carried as
integers (raw = handle * 16). A field of 0 or of an empty slot reads 0 (no
error, unlike the sandbox).

Time: the machine ticks **20 times a second** (TICK = 1/20 s). Seconds become
ticks as `(raw * 20 + 8) >> 4` (floor). `statetime` and `time` read ticks as
seconds: `ticks * 16 / 20` (trunc); `time` wraps every 1024 s.

Angles: stored as a step 0..255 (360/256 degrees). Writing a degree value d:
step = floor(d * 256 / 360) mod 256 (on raw: floor(raw * 256 / (360*16))
mod 256, floored for negatives). Reading: raw = (step * 45) >> 1 (step *
22.5 in raw).

`sin(d)`/`cos(d)`: d in degrees -> its step k -> `SIN[k]` (cos: SIN[(k+64) &
255]), where SIN[k] = round(sin(2 pi k / 256) * 16), a 256-entry table both
sides generate the same way (round half away from zero).

`sqrt(v)`: v <= 0 -> 0; else isqrt(raw * 16) (floor integer square root).

`random(a, b)`: if both are whole (raw % 16 == 0) a whole number in [a, b]
inclusive: a + 16 * (r % (span + 1)) where span = (b - a) / 16; else a raw in
[a, b]: a + r % (b - a + 1). If b < a they are swapped. `chance(p)`: (r % 1600)
< raw p. `pick(...)`: argument r % n. The generator is xorshift32 (x ^= x <<
13; x ^= x >> 17; x ^= x << 5), one per jar, seeded by the host; every draw is
one call. Particles and the host never draw from it.

`dist(item)` / `dist(x, y)`: from my centre (x, y - h*scale/2 in pixels;
raw); isqrt(dx*dx + dy*dy) on raw values in unsigned 32 bits.
`angleto(item)`: the step k in 0..255 maximising dx * ANG[(k+64) & 255] +
dy * ANG[k] (ties: the lowest k), returned as degrees raw. 0 is right, 90
down. ANG is angleto's own table, ANG[k] = round(sin(2 pi k / 256) * 16384)
(halves away from zero) -- the 1/16 SIN table was off by up to 14 degrees
(straight left read 165.94). dx and dy are the raw centre differences,
clamped to -65535..65535; each product is under 2^30 and the sum is at most
|(dx, dy)| * 16385 < 2^31, so the device needs no 64-bit maths. The eight
compass points come out exact. `sin`/`cos` keep the 1/16 table.

`abs`, `min`, `max`, `sign` (-16/0/16), `clamp(v, lo, hi)`, `round`
(nearest whole, halves away from zero), `floor` -- on raw values.

## Traits

32 trait words, bit numbers 0..31 (the table's `trait` group). The first 16
are v2's; the second 16 are for living things and places. An item's traits are
its record's tag words. `find(T [, R])`: the nearest other item with trait T
(centre distance, ties to the lower slot), within R pixels if given (<=); 0 if
none. `count(T)`, `has(item, T)`. A trait argument is a trait word, compiled to
its bit number.

## Signals

`signal NAME` -- the name becomes a number 1..255: FNV-1a 32-bit over its
ASCII bytes (h = 2166136261; h ^= c; h *= 16777619), then `h % 255 + 1`. So
two items that never met agree on what `signal landed` is. `signal NAME to
ITEM` reaches one item (itself allowed); without `to`, every other scripted
item.

## The script's bytes

Little-endian. Offsets are from the script's first byte; at most 1024 bytes.

```
0   format = 3                       (v2 is 1; the v2 machine ignores 3)
1   flags: bit 0 scale 2
2   nvars 0..8
3   nstates 1..16
4   nhandlers 0..24
5   nanims 0..8
6   nperches 0..4
7   nstrings 0..16
8   layer (signed)
9   body radius in px (0: default)
10  solid x, y, w, h in sprite px (w == 0: none)
14  var initial values: nvars x int16 raw
..  states: nstates x u16 code offset
..  handlers: nhandlers x { event u8, arg u8, state u8 (255 = any state),
                            param u16, code u16 }        (7 bytes)
      param: every -> period in ticks (>= 1); near -> radius in px;
      chime -> minutes; otherwise 0
..  anims: nanims x { fps u8 (1..30), n u8 (1..8), n x frame u8 (0-based) }
..  perches: nperches x { x u8, y u8 }  (sprite px)
..  strings: nstrings x { len u8 (0..24), len bytes ASCII }
..  code to the end
```

`arg` filters the event: 0 any; otherwise the event's argument must equal it
(a near trait is stored as bit + 1; a signal is its number; time, weather,
jam, world are their group values). Handlers are listed state handlers first
(in state order), then the any-state ones, each group in source order; the
first match wins, so a state's handler wins over a top-level one for the
same event and argument.

`near` is the exception: the event carries which handler saw the arrival
(its arg is that handler's index in the table + 1, not the trait), and only
that handler runs it -- if it is still active then; otherwise the event is
dropped. So two near handlers on one trait at different radii (a state's
`within 22`, a top-level `within 48`) each run only for their own
arrivals. The queue entry is the same size.

`jv3_check` walks it all before it is used: sizes add up, every offset lands on
an instruction, every operand in range (vars, states, anims, strings, senses,
props, functions, actions), jump targets on instructions, a handler's code
never contains a time-taking op (WAIT, AWAIT, JZY), each body and handler ends
in HALT or RET reachable... A script that fails is not run: the item is idle.

## Instructions

`apps/jar/jarvm3.def` has the list. One byte of opcode, then operands. Jumps
are absolute u16. The stack is 16 values; overflow or underflow is a fault.
Every executed instruction costs one step; an AWAIT that waits refunds its
step; a JZY that waits costs the steps of its condition.

## Runtime state

Per scripted item (`Jv3Item`, about 170 bytes): pc, stack (16) and sp of its
body thread; state, ticks in state, halted, faults; wait ticks; anim (index,
ticks, once, done); frame, facing; the mover (path: x0 y0 x1 y1 cx cy, t in
1/4096ths, step per tick; walk: px left, dir, speed); perch (owner slot,
index); physics (on, roll, vx, vy, gravity, bounce, friction, spin, angle,
grounded, blocked, touching mask); other, impact; the event queue (8 x {ev,
arg, other}); near masks (one 32-bit mask per near handler, 4 at most); every
counters (one u16 per every handler, 6 at most); the say text (24 chars and
ticks left); sway (amp, ticks, t0); shake ticks. Vars live in the record's 8
memory slots, as v2's did: persisted. They are set to their initial values
when an item is placed with all eight slots 0.

Position (x = centre, y = feet, raw) is the machine's while an item is
scripted; the host reads it after each tick and writes it back where it moves
items itself (a gift, a throw, being placed).

## One tick

1. **Near**, for each scripted item in slot order, for each of its near
   handlers active now (the current state's, then any-state): the set of
   other items with the trait within the radius (centre distance <=); queue
   `near` (other = the item, arg = this handler's index + 1) for each not in
   last tick's mask; keep the new mask. Inactive handlers' masks are cleared.
2. **Run**, for each scripted item in slot order, with a budget of steps (100
   for a full script, 50 for a small one, by length: > 512 bytes is full):
   a. take the queue as it stood (events queued during this tick wait for the
      next one -- signals included, whatever the slot order);
   b. each queued event, in order: the first matching handler runs (other,
      impact set first; a near event: the handler that saw it);
   c. `tick` (one event, whatever the number of tick handlers);
   d. each `every` handler active now, in table order, whose counter reaches
      0 (counters start at the period on entering the state, count down one a
      tick, reload when they fire);
   e. a pending `go` from any handler: enter that state (the body restarts at
      pc 0, stack cleared, wait 0, halted cleared, movers cancelled, every
      counters reloaded, near masks of the new state's handlers cleared);
   f. the body, if not halted and its wait is over: run until it waits,
      halts, faults or the budget runs out (then it resumes next tick where
      it was). A GO in the body enters the new state and continues running
      it in the same tick (at most 4 times a tick).
   A handler that starts and runs out of budget mid-way, or faults, is
   abandoned and counts a fault; a body that faults halts and counts one. A
   handler that cannot start because the tick's budget is already spent is
   not run and is not a fault: its event is dropped silently (other and
   impact are not set; an every counter that reached 0 still reloads), so a
   burst of events cannot stop a well-behaved item. At 8 faults the item's
   script is stopped for good (it is idle; the jar shows "?").
3. **Move**, for each scripted item in slot order: the mover (path or walk)
   or physics, perch following, then the anim advances one tick, then say,
   sway and shake count down. `land`, `hit` and `bump` are queued for the
   next tick.

## Statements (what the ops do)

- `wait T`: wait ticks = max(1, ticks(T)). `wait` alone is one tick.
  `wait until C`: if C is false, the body yields and evaluates C again next
  tick.
- `go S`: in a handler, sets the pending state (the last one wins; the
  handler ends); in a body, enters S at once.
- `frame F`: a still frame (the anim stops). `play A`: loop A (if A is already
  the looping anim, it keeps its place). `play A once`: from the start; a body
  then waits until it has shown its last frame. `stop`: no anim.
- `move to X, Y | perch [T] | ground | ITEM [speed S] [arc A]`: a path.
  Target: a point; the top-centre of an item; a free perch (any item's
  declared perch with trait T, or the rim spots if T is absent or `rim`; one
  chosen by the generator; none free: the ground); the ground: a random x in
  14..226 at the ground under it. x is clamped to the jar, y to 8..ground.
  The path is a quadratic curve with its control point at the chord's middle
  raised by 2 * arc; its length is measured as a 12-piece line; it takes
  max(3, ticks(length / max(5, speed))) ticks, eased (3t^2 - 2t^3). The body
  waits. On arrival the item is at the end point exactly, takes the perch if
  it aimed at one, and `land` is queued (other = the perch's owner, 0 the
  rim or none).
- `walk DX [speed S]`: along the ground at S px/s (default 10), stopping at
  the glass (x within half its width + 2 of the wall), a solid, or a solid's
  edge when standing on one; a stop sets `blocked`. The body waits.
- `hop [DX [H]]`: a path of height H (default 5) over DX (default 0) at speed
  40, with no `land`; on a perch DX is 0 and it keeps the perch.
- `face V` (V's sign; 0 keeps it), `face toward ITEM`.
- `say TEXT [T]`: TEXT is a string, optionally joined with values by `+`
  (compiled to a template: the string pool entry with value slots); at most
  24 characters shown, for max(0.3, T) s (default 2).
- `log ...`: the device ignores it; the server's simulation records it.
- `emit KIND [N]` (N 0..12, default 1): leaf, dust, feather, spark, heart,
  note -- the jar draws them with its particles.
- `sway A [T]`, `shake [T]`: drawn only.
- `physics on|off`, `roll on|off`: physics on leaves any perch and cancels
  a path or walk.
- `signal NAME [to ITEM]`.
- The jar's actions, from v2: `sound NAME`, `glow on|off|toggle`, `eat`,
  `throw`, `drop`, `boost [N]`, `nudge`, `quake` (v2's `shake`: the jar
  shakes), `fly`, `home`. The host carries them out as v2 did.

## Physics (phase 2 of the build; same code, a host switch)

Semi-implicit Euler in raw units at 1/20 s: vy += gravity / 20; when grounded
and friction > 0, vx moves toward 0 by friction / 20 without crossing; x +=
vx / 20, y += vy / 20 (all trunc toward zero on raw). The body is a circle of
radius r (declared, else min(w, h) * scale / 2 - 1) centred at y - h*scale/2.
Soil (the ground under it), glass (x 3 and 237) and lid (y 8) clamp it and
reflect the normal speed by bounce/100; a vertical speed under 12 px/s after a
floor bounce becomes 0. An impact (incoming normal speed) over 15 px/s queues
`hit` with `impact`. Solids are other items' declared boxes and the jar's
ledges: pushed out along the closest-point normal (inside: on top); bump to
both when approaching faster than 10 px/s and not touching it the tick before.
Moving bodies do not collide with each other. `roll` turns distance into
angle. The host caps how many items may have physics on at once
(`JV3_MAX_BODIES`, from the measurement); a `physics on` past the cap is
refused and the sense `physics` stays 0.

## Memory on the device

- The machine has no memory of its own: `jv3` works on caller-owned
  `Jv3Item`s and script bytes.
- The jar takes **one big-mode block** when it opens: `min(big_room() -
  slack, JV3_POOL_MAX)` with JV3_POOL_MAX 20 KB. In it: the `Jv3Item`s of
  scripted items and their scripts, packed by a tiny allocator that
  compacts on removal (as the spool does).
- If an item's script does not fit, it is placed idle (motion parts only),
  logged "v3: no room for N bytes" -- never a failed open.
- The 28 KB arena does not grow: a record is now up to 2112 bytes, and the
  one decoded record (`JIo`) lives in the `SCR` union, which the shop's
  buffers already make 6 KB.
- Trims, if the measurement asks: v2's 768-byte spool and the 3 KB frame
  pool can move into the block; v3 items need no `bub[5][13]` (1.5 KB of
  `JPlaced` over 24) -- their words are in the script's strings.

## Server

- `CARDOS_JAR_V3=1` makes new items v3 (it needs devices with this jar.capp:
  an older one refuses a record whose script is over 256 bytes outright).
- The code rolls the **script tier** with the rest of a find: none for a
  still thing that is not a critter, small (512) for other non-critters, full
  (1024) for critters. The behaviour writer is told its tier's limit; the
  compiler refuses more and the error goes back to that stage once. A none
  tier skips the behaviour stage.
- **Conversion** (`python -m server.jar v3convert [--dry] [--id N]`): for
  every item owned by someone or on a shelf, its behaviour is written again in
  v3 from its kept concept (`concept/ID`; for items made before concepts, from
  its name, line, kind and traits), at the tier the code gives it, compiled,
  simulated, the record rebuilt with its art, price and memory unchanged and
  re-signed, and the owner sent `tx remake ID` so their device fetches it
  (as Pip's work is). Shelf items are replaced in place. One Claude call an
  item, a few at a time, resumable (`v3/ID` marks one done); a failure keeps
  the old record and is listed.
- **Retiring the pool**: a pool item whose script is not format 3 is never
  picked again (`to_pool` keeps it, `pick_from_pool` skips it), so v2 items
  leave circulation without being deleted.
- `simulate` runs an item in a small world for a simulated hour with pokes,
  signals, the jar's events and a neighbour, and fails it on a fault, a
  stopped script, a body that never waits, or nothing visible happening.

## Differences from the sandbox, on purpose

- Fixed point everywhere (the sandbox rounded only at some points).
- Events raised during a tick are handled the next tick, whatever the order
  of items (the sandbox depended on array order).
- One `tick` event a tick (the sandbox ran a state's tick handler twice when
  a top-level one existed too).
- A field of nobody reads 0; `/0` and `%0` give 0 -- no faults for these.
- Strings only in `say` and `log`.
- Traits from the 32-word list (the record has room for no other).
- Items are handles 1..24, not record ids.
- `time` wraps every 1024 s.
