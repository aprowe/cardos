# Jar items made in stages, and things that keep still — design

Approved in conversation with alex, 2026-10-11. Replaces the one-call item
generation in `server/jar.py` (`generate`) with a pipeline of narrow agents,
and gives items motion built from parts instead of one movement word.

## The problem

Nearly every item comes out as a small critter that walks or hops round its
zone. Two causes, both in the generator rather than the device:

- **One call does everything.** One `ask_shape` call returns the name, kind,
  movement, habits, bubbles, palette, up to four 16x16 frames, a script and
  (v2) traits, under 12,000 characters. The art is the bulk of the answer, and
  behaviour gets whatever attention is left.
- **Movement is required, and the easy answer is to walk.** `item_schema`
  requires `movement`, `speed` and `zone` for every item, and the prompt's
  main list of actions is hop, walk and float. `JM_SITS` exists in
  `apps/jaritem.h`, but the model almost never picks it.

The five movement values (sits, hops, wanders, sways, floats) are also too
narrow. A lantern should flicker, a jellyfish pulse, a reed sway in place,
and none of those is a way of getting around.

## What alex asked for

- Items must be able to keep still.
- Generation is split across agents: a "dungeon master" (DM) that invents
  interesting concepts knowing little about the game, a stage that fits a
  concept to the game, one agent that only does art, and one that only does
  behaviour, including deciding there is none.
- The DM gets its material by **asking Tibbs where he is searching today and
  what he is after**. No random flavour words anywhere.
- How much an item moves is rolled by the code (the "variety is the code's
  job" rule in CLAUDE.md). How it moves is built from parts, chosen to suit
  the concept.

## Design

### 1. The pipeline, per stock

```
code: roll kind, tier, form, activity  (per find)
  |
Tibbs (his session): "where are you searching today, what are you after?"   once per stock
  |
DM: one concept per find          sees Tibbs's answer, the rolls, the stock's earlier concepts
  |
fitter: the game sheet            sees the concept, the rolls, the game's rules
  |
  +-- artist: palette + frames            } in parallel
  +-- behaviour writer: habits / script   }
  |
combine -> check_item -> script simulator -> seal -> store  (as today)
```

**0. The code rolls, per find** (no Claude call):
- **kind**: `KIND_PLAN`, unchanged;
- **tier**: `pick_tier`, unchanged, and still setting each stage's effort;
- **form**: `roll_form` (furniture, scenery, part, big, none), unchanged;
- **activity** (new): `still` 35%, `reacts` 30%, `ambles` 25%, `restless`
  10%. The table is `ACTIVITIES` in `server/jar.py`, beside `FORMS`.

`shopkeep.spark` and `shopkeep.loose_brief` are no longer used by the
pipeline.

**1. Tibbs is asked where he is searching.** This replaces the per-find
`briefs` in `shopkeep.stock_turn`. His stock turn keeps its `line` and
`picks`. Instead of one brief per find, it returns `search`: a few lines in
his voice about where he went today and what he hoped to turn up. The
question includes his last few `search` answers (kept like
`tibbs/briefs`, as `tibbs/searches`) and asks for somewhere he has not been
lately. A commission's request goes into the question, so a paid
commission is still answered.

**2. The DM writes one concept per find.** Its whole picture of the game is
one paragraph: a tiny world inside a jar, with small creatures and objects
that a shopkeeper finds and sells. It does not see the item format, the
enums, the player, the player's jar, or the script language.

- **Input:** Tibbs's `search`, the find's activity in words ("it keeps
  completely still", "it does nothing until something happens to it", ...),
  the tier in words ("a trinket", "a treasure"), and the concepts already
  written for this stock, which it must not repeat.
- **Output:** `concept`, about 80 words: what the thing is, how it looks,
  where it came from, and one surprising detail.
- **Effort:** by tier, as today.

**3. The fitter writes the game sheet.** It translates the concept into the
game and may not replace it. When the concept cannot fit the rolls (a whale
for a hanging slot), it shrinks or adapts it and says how in `note`, which
is logged.

- **Input:** the concept; the rolled kind, form and activity; the game's
  rules: kinds, zones, the motion parts and their rules for the activity (§2),
  traits, voices, name, line and bubble limits, furniture's surface.
- **Does not see:** the art format or the script language.
- **Output (the sheet):** `name`, `line`, `kind` (fixed to the roll), `zone`,
  `travel`, `idle`, `liveliness`, `speed`, `traits`, `voice`, `bubbles`,
  `surface` for furniture, `look` (a note for the artist, including which
  extra frames are needed: "frame 2 is the flame low, for the flicker"),
  `intent` (what it does, in plain words, or "nothing"), `note`.
- **Effort:** medium.

**4a. The artist draws.** It sees `name`, `line`, `look`, the kind and form
(furniture needs its top row flat; a part's last frame is the part), and the
art format: 8 colours and 16x16 frames of digits 0-7, as in the current
prompt. It returns `palette` and `frames`. The frame count follows the
sheet: 1 for most still things, more only where `look` asks for them.
Effort by tier.

**4b. The behaviour writer writes reactions.** It sees the sheet (not
`look`), the activity, and the language guide (`LANGUAGE_GUIDE_V1` or
`_V2`). It returns `habits` and/or `script`, both allowed to be empty.

- `still`: may return nothing at all. An empty answer is a valid item.
- `reacts`: must return at least one reaction (a habit or a script
  handler), or it is no different from a still thing.
- `ambles`, `restless`: reactions are optional. Constant motion comes from
  the motion parts, not from habits.

Effort by tier.

**5. Combine and check.** The sheet, the art and the behaviour are put
together into the dict `to_item` already takes (plus the motion parts), and
go through `check_item`, the script compiler and the simulated day, `seal`
and `st.put`, unchanged. The concept and Tibbs's `search` are kept on the
server beside the record (`concept/ID` in `srv/jar`) so Pip's alterations can
start from the original idea. They never go to the device.

All stages use `shopkeep.MODEL` through `ask.ask_shape`, each with its own
schema.

### 2. Motion as parts

The activity says how much a thing moves. Three parts, chosen by the fitter,
say how:

| Part | Values | Examples |
|---|---|---|
| `travel`: how it gets about | none, crawl, hop, slide, float, fly | a snail crawls; a balloon floats |
| `idle`: what it does in place | none, sway, bob, wobble, spin, pulse, flicker, blink | a reed sways; a jellyfish pulses; a lantern flickers |
| `liveliness` | calm, normal, jittery | how often and how strongly the idle plays |

**What each activity allows** (the fitter's schema enforces it, and
`check_item` checks it again):

| Activity | travel | idle | speed |
|---|---|---|---|
| still | none | none, pulse, flicker, blink | — |
| reacts | none | none (motion comes from its reactions) | — |
| ambles | any | any | slow |
| restless | any but none | any but none | medium or fast |

**In the record** (`apps/jaritem.h`, the longer header, bytes 215-223 are
zero today):
- 215 `travel`, 216 `idle`, 217 `liveliness`. Zero is none / none / calm, so
  every existing item reads as it does now.
- `ji_ext` also says yes when any of them is set, so such an item gets the
  224-byte header.
- Byte 72 (`move`) is still written with the nearest old value (crawl and
  slide become wanders, fly becomes floats, travel none with idle sway
  becomes sways, and everything else with no travel becomes sits), so a Jar
  build that knows only 208 bytes shows something sensible.
- An item with no motion parts gets them from byte 72 on the device: hops is
  travel hop, wanders is crawl, sways is idle sway, floats is travel float,
  sits is none.

**On the device** (`apps/jarsim.h` and `apps/jarlive.h`; a `jar.capp`
update, no OS change, no API bump):
- `travel` drives how the sim moves a thing between places. Crawl and slide
  stay on the ground, hop arcs, float and fly leave it.
- `idle` is drawn by the scene. Sway, bob, wobble, spin and pulse are offsets
  or a scale applied while drawing, so they need no frames. Flicker and blink
  cycle the item's own frames, which is why the fitter asks the artist for
  them.
- `liveliness` sets the idle's period and size.

Jar's data is about 430 bytes under the 28 KB budget (CLAUDE.md). Three
bytes per item in the sim's item table must be measured against that before
it is merged.

### 3. When a stage fails

- Each `ask_shape` already retries once when an answer does not fit its
  schema.
- **DM or fitter fails:** the find is dropped, counted against `MAX_FAILS`
  (4) as today. A stock with fewer new items is still a stock.
- **Artist or behaviour writer fails:** that stage is asked again with the
  same sheet. A second failure drops the find.
- **`check_item` or the simulator refuses the combined item:** the reasons
  go back to the stage that made the bad part: frames and palette to the
  artist, habits and script to the behaviour writer, everything else to the
  fitter. One such redo per find, then it is dropped.
- **Tibbs gives no `search`** (no Claude, a lost session): the DM works from
  the rolls alone, and the log says so. No random words are substituted.

### 4. Cost and limits

- **Calls:** one Tibbs question per stock, plus four calls per new find. A
  normal stock (4 new) is about 17 calls instead of 4. A commission (8 new)
  is about 33.
- **The daily limit:** a stock counts once against the person's limit (the
  Tibbs question), and the per-find stages run with `limit=None`, as
  server-scheduled work does now. Otherwise one stock could use a fifth of
  the 150-call day.
- **Time:** the artist and the behaviour writer run in parallel (two
  threads), so a find takes about three calls' time. The shop already shows
  finds one by one as they land.

### 5. Switching it on

- `CARDOS_JAR_STAGES=1` in `/etc/cardos/env` turns the pipeline on. Without
  it, `generate` makes items in one call as today, so the change can be
  reversed with one line.
- The pipeline works with `CARDOS_JAR_V2` on or off. With v2 off, the
  behaviour writer gets the v1 guide and the fitter gets no traits or voice.
- Motion parts need the new `jar.capp` on devices before items that use
  them are sold. Older builds show the byte 72 fallback.

## Testing

**Host, with Claude stubbed** (`server/tests/test_jar.py`):
- activities are rolled in their proportions over many draws;
- each stage's prompt holds what it should and not what it must not: the
  DM's prompt never contains the player's items or the format; the artist's
  never contains the script language; the behaviour writer's never contains
  `look`;
- the fitter's motion choices follow the table in §2, and `check_item`
  refuses one that does not;
- a `still` item with no habits and no script passes; a `reacts` item with
  neither is refused;
- a refusal is sent back to the right stage;
- with `CARDOS_JAR_STAGES` off, `generate` is byte-for-byte the current
  path.

**Device** (`test/test_jar.c`, then on hardware):
- the record round-trips bytes 215-217, and the byte 72 fallback is right
  both ways;
- each idle motion and travel is drawn and moves as described;
- an old item and an old Jar build behave as before;
- Jar's data stays inside the 28 KB arena.

**Real output, before switching it on:** generate about 20 finds on the
droplet with the pipeline and read them. Look at how many are still,
whether the concepts surprise, and whether consecutive stocks repeat
places. The old prompts were tuned this way, and the new ones need the same.

## Who builds it

`server/jar.py`, `server/shopkeep.py` and the Jar headers are being changed
by another session (cardos-78) as this is written. Implementation should be
done by that session, or after its work is committed, in this worktree's
branch (`jar-item-pipeline`).
