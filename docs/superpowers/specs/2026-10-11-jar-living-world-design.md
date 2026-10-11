# Jar Factory: a living jar -- design

2026-10-11. The owner's asks, in their words where they matter:

- "items need more complexity ... one you interact with and it does something,
  or interesting interactions with each other. they kind of just sit or walk
  around and say lines." Fireworks on a poke; one that throws your stuff
  around; one that eats your berries and poops.
- "the scripting language needs to be more complex to allow for more complex
  interactions."
- "an event system, where stuff can happen, good or bad. different things
  react differently."
- "when a new item is introduced, other items can perhaps have their scripts
  altered so they interact with the new item. again not all items have
  scripts."
- "a toggleable soundtrack and sounds. soundtrack should be very minimal.
  items can have their own sounds."
- "keep the screen awake when jar is opened."
- "occasionally things break, or need some sort of player interaction, so
  you cant just run it over night. still very idle."
- "some items need to be furniture, like other things can go on top of it --
  a shelf, a table, a ledge. and other things need to be background
  fixtures, like a big plant in the background. or a birdhouse with a bird
  and the bird flies around and comes back. bigger things, smaller things,
  multiple part things, background things ... a music player, a fog horn on
  the hour."
- "eventually i want scripts / events to be AI generated on the fly."
- Done already (2026-10-11): the stock is 2 random, 2 of Tibbs's picks, 4 new;
  Tibbs keeps his whole history and a notebook; bribes and trades are struck
  in conversation ("no need for actual button").

## The constraint

`jar.capp`'s data is 28 624 of 28 672 bytes on master. Nothing that adds a
table, a string or a buffer to the device fits until the constants-in-flash
work (cardos-6f, API 46: constants in DROM, overlays) lands; on that branch
jar uses 15 672. So the order is: everything portable and server-side first,
host-tested; the device pieces as soon as API 46 is on main. Code size is not
the limit (XIP).

## Stages

### A. Done (2026-10-11)

Stock mix; Tibbs's memory (history, notebook, seeded sessions); deals in
conversation (`coins N`, `DEAL:`, `tx` lines, `deal` on /jar/day).

### B. Device: deals, and the screen kept on (needs API 46)

- `api->keep_awake(1)` while Jar Factory is open, any screen.
- Talk sends `coins N` first; GET /jar/talk asks `?tx=LAST` and carries out
  each new `tx`: `pay N` (coins down, never below 0), `get N`, `lose ID`
  (gone from things and from the jar; the item file removed). LAST is kept
  in the save so a tx is done once, across restarts.
- `deal` in /jar/day: the shop asks for a fresh stock (its report included)
  without the `r` charge.

### C. The language, version 2 (portable now: jarvm.def, jarvm.h, jarvm.py)

New events:
- `on hour [H]` -- the clock reaches the hour (a fog horn); `on signal [N]` --
  another item `signal`led N; `on world [NAME]` -- a world event starts (D);
  `on new` -- an item is placed in the jar (its tags are a sense); `on
  berry` -- a berry ripens; `on bumped` -- something is thrown or lands on it.

New actions:
- `sound N` -- one of the jar's sound bank (F);
- `burst PARTICLE N` -- N particles at once (fireworks: `burst sparkle 12`);
- `throw` -- fling the nearest small thing (critter or floor decor) in an arc;
  it lands, bumps, settles back toward home;
- `eat` -- take a ripe berry from the nearest bed (the item's `mem` can count
  them); `drop` -- leave a pellet on the soil that feeds the nearest bed
  (grows faster) and is eventually cleared by mosslings;
- `signal N` -- every item's `on signal` (N narrows it);
- `seek TAG` -- walk to the nearest item carrying a tag;
- `fly` / `home` -- a part (E) leaves its home and comes back;
- `boost N` -- the belt runs faster for N seconds (good luck); `nudge` --
  pokes the snail along.

New senses: `hour`, `berries` (ripe in the jar), `near_tag` (does the nearest
item carry tag K: tags are numbered per jar from the items' tag field),
`world` (the current world event, 0 none), `fed` (berries it has eaten),
`music` (the soundtrack is on).

Bytecode stays 256 bytes and 64 steps a handler; the server compiles and
simulates a day as now, with the new events in the simulation. Old records
keep working: v1 bytecode is a subset.

**Tags are the interaction glue, with no AI and no re-signing.** The record's
24-byte tag field (unused since tags stopped steering the stock) becomes the
item's traits: `food, shiny, fragile, noisy, cosy, light, water, music ...`
from a fixed vocabulary in jarvm.def. Scripts react to traits, not to names
(`on near tag shiny: hop; say 1`), so an item made today reacts to one made
next month. The generator is given the vocabulary and the jar's current
traits.

### D. World events and chores (jarsim.h, portable)

A scheduler in the sim rolls a world event every 20-60 minutes of jar time,
weighted by what is in the jar and the real weather:

| Event | Good/bad | What it does | Who reacts |
|---|---|---|---|
| ant raid | bad | ants march for the beds and carry berries off | critters with `on world ants`; a `throw` or `seek` item can rout them |
| leak | bad | drips from the lid; a puddle grows; the belt slows | the player fixes it (D2) |
| breeze | good | everything sways; the belt runs fast for a minute | hanging decor flutters |
| visitor | good | a bird lands on the lid, a coin drops | `on world visitor` items greet it |
| lights out | neutral | the jar goes dark for a minute | glow items light up |
| bloom | good | every bed ripens at once | berry-eaters feast |

Items react through `on world NAME`; anything with no handler ignores it.

**Chores, so it cannot run all night (D2).** Production needs the player now
and then, about once an hour of jar time, never punishing:
- the belt jams (exists) and now stays jammed until fixed;
- the snail sulks after N trips: shipping stops until it is cheered (a poke,
  or an item that `nudge`s);
- the leak: the puddle stops the beds until mopped;
- mould on a bed: that bed stops until cleared.
Each shows an icon in the bar and a banner; a fix is one key from the menu
(or poking the thing). Nothing is lost while it waits: it only stops making.

### E. What an item can be (record v2)

The header grows (its length is in byte 1; v1 readers skip what they do not
know, so a v2 record on old firmware is a plain v1 item):

| off | size | field |
|---|---|---|
| 208 | 1 | role: 0 thing, 1 furniture, 2 background, 3 fixture-with-part |
| 209 | 1 | scale: 1 or 2 (a 16x16 picture drawn at 2x -- a big plant, a cupboard) |
| 210 | 1 | surface: for furniture, the row (from the top, in the picture's pixels) others stand on; 0 none |
| 211 | 1 | part frame: the frame that is the part (the bird), 0 none |
| 212 | 1 | part movement: flies, hops, circles |
| 213 | 2 | part home: x, y offset from the item, in pixels |
| 215 | 1 | voice: the item's sound in the bank (F), 0 none |

- **Furniture** is a ledge that moves with the item: decorate can put things
  on its surface, exactly as the ruler and matchbox ledges (the sim already
  has levels and lifts; a furniture ledge is a dynamic one).
- **Background** is drawn after the sky and before the ledges, at its scale,
  cannot be walked into, and is not a near target.
- **A part** is drawn from its frame and moves on its own (`fly`/`home`), with
  the item's script steering it.

### F. Sound

One audio channel and WAV files only (`api->audio()->play`). So:
- The sound bank: about twelve short sounds (pop, chirp, boing, ding, horn,
  whoosh, crunch, plop, coin, fizz, tick, splash) synthesised once into
  /cache/jar_*.wav as Iron's are.
- The soundtrack is very minimal on purpose: a phrase of a few plucked notes
  (pentatonic, 8 kHz, a second or two) every 8-20 s, chosen from four, quieter
  at night, silent while another sound plays. Sounds always win over it; it
  never cuts music the player is playing elsewhere.
- `m` toggles the soundtrack, `M`... the jar's sounds; both kept in the save.
- An item's `sound N` / its voice uses the bank; `on hour` + `sound horn` is
  the fog horn.

### G. Items reacting to a new item (AI, later)

When an item is bought, the server may write a *patch* for one or two items
already in that jar: extra handlers (`on near tag ...`, `on signal ...`)
compiled, simulated and signed by the server like an item, delivered with
the stock, stored on the card beside the item, run after the item's own
script. Not every item gets one; one with no script can get one. Until G,
traits (C) give the interactions.

### H. Events and scripts made on the fly (AI, later)

The same patch path carries world events written for the day by Claude from
the jar and the real weather; and Tibbs can hand over a scripted event as
part of a deal.

## Status (2026-10-11, end of day)

All stages A-H are built and host-tested on branch `jar-living`; none has
run on a device. B-H need API 46 on main (jar data is 31 KB on this
branch, most of the growth const tables that go to flash). Then: rebase,
build, test on both devices, set CARDOS_JAR_V2=1 on the droplet, deploy.

## Order of work

1. C (language v2 + traits) and D (events, chores) in the portable headers,
   host-tested; the server compiler and generator updated, so new items use
   them as soon as devices can run them.
2. E's record format on the server and in jaritem.h, with tests.
3. When API 46 is on main: B, then the device halves of C/D/E, then F.
4. G, then H.
