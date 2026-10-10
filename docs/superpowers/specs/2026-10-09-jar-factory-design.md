# Jar Factory: Cardputer idle game -- design spec

Oct 9, 2026 · Alex Rowe. The owner's spec, kept as written, followed by the
decisions taken for CardOS (section "CardOS decisions", at the end).

## Summary

Jar Factory is an idle game for the M5Stack Cardputer: a terrarium with a tiny
factory inside that runs unattended, earns coins, and sells AI-generated
collectible items you place in your jar or send to friends.

The player leaves it running, glances at it, and sees small creatures working.
Nothing ever requires attention. Everything the player does is optional and
takes under a minute.

## Core loop

1. Plants in the jar grow raw materials.
2. Critters carry materials to machines, which turn them into jars of jam.
3. Finished jars ship out through a cork door. Each one earns coins.
4. Coins buy items from a shop whose stock is generated daily by an AI.
5. Items are placed in the jar, where each runs its own small behaviour script,
   or are sent to a friend.
6. What the player grows and displays slowly steers what the shop generates next.

In scope: one space (the terrarium factory), the shop, the garden and shelf that
steer generation, the item format and script machine, friends and gifting, and
a small server that generates items and relays gifts.

Out of scope for now: other spaces (a mine was sketched), sound, trading or
selling items between players, real-money purchases, and any free-text request
for a specific item.

This spec fixes behaviour, data shapes and limits. Engine, language, libraries
and file layout are the implementer's choice. Numbers marked placeholder are
starting values to tune, not requirements.

## Device and screen

240x135 colour screen in landscape, a 56-key keyboard, wifi, no touch. Treat
memory as tight and confirm the exact figures on the hardware before fixing any
limit below.

| Region | Position | Size | Holds |
|---|---|---|---|
| Top bar | y 0 to 12 | 240x13 | Coin count and rate, space name, mail indicator |
| Scene or screen body | y 13 to 121 | 240x109 | The jar, or the current menu |
| Bottom bar | y 122 to 134 | 240x13 | Key hints for the current screen |

- All art is pixel art on the native grid. No scaling, no anti-aliasing.
- Text uses one bitmap font about 7 to 8 pixels tall; roughly 34 characters fit.
- Every screen shows its available keys in the bottom bar. No hidden shortcuts.

### Controls

| Key | Action |
|---|---|
| Arrow cluster (; . , /) | Move selection |
| Enter | Confirm |
| Esc (the backtick key) | Back |
| Tab | Next tab or next field |
| Letter keys | Screen shortcuts, shown in the bottom bar |

From the jar: S shop, D decorate, P plant (garden), H shelf, F friends, M mail.

### Running unattended

- The jar screen is the default and never times out to a menu.
- After a period without key presses (placeholder: 2 minutes) the screen dims
  but keeps animating. On the jar screen the top and bottom bars slide away after
  10 seconds without a key press (placeholder), leaving the jar on the full
  240x135 screen. Any key brings the bars back and is otherwise ignored, so
  waking the bars never triggers an action. Coin gains still show as small
  numbers at the dock while the bars are hidden. Menu screens always keep their
  bars.
- State is saved on every purchase, placement and gift, and at least every 5
  minutes.
- The device needs the real date and time for plant growth, daily stock and away
  earnings. With no time available, those three features pause and everything
  else works.

## The space: a factory in a jar

A side view of the inside of a glass jar, drawn for the full 240x135 screen,
with a small factory built from human junk (thimbles, matchboxes, bottle caps,
twigs) among the plants.

| Zone | Contents |
|---|---|
| Garden | 6 planting beds along the soil line (placeholder count). Plants grow here. |
| Works | 2 to 4 machines joined by a bottle-cap conveyor: a thimble vat, a matchbox press, a twig crane. |
| Dock | A crate stack and a cork door in the glass at the right edge. Jars leave here; gifts arrive here. |
| Anywhere | Placed items: floor decor on the soil line, hanging decor from the top edge, critters roaming. |

### Production loop

1. A mature plant produces one unit of raw material on a timer.
2. A free worker critter (a mossling) walks to the plant, picks the unit up, and
   carries it to the first machine.
3. Each machine holds the unit for a fixed time, then puts it on the conveyor to
   the next.
4. The last machine outputs a finished jar onto the dock.
5. A snail carries the jar out through the cork door. Coins are added when it
   exits the screen.

Every coin must come from a jar the player could have watched leave. No hidden
income.

### Economy

- One currency: coins. Used for shop items, postage, new plants and upgrades.
- Upgrades are always visible in the scene: another mossling, another machine, a
  faster belt, another planting bed.
- Critters that pass a placed item they like get a short speed boost
  (placeholder: +20% for 60 seconds). This is the only way decor affects income.

| Tunable (placeholder) | Starting value |
|---|---|
| Jar value | 1 coin |
| Starting output | 3 jars per minute |
| Shop item price range | 45 to 200 coins |
| Postage per gift | 10 coins |
| Away earnings cap | 8 hours of output |

### Idle life

The scene always shows at least three things moving. Required ambient events,
each occasional and self-resolving:

- A mossling falls asleep on a crate, then wakes.
- The vat puffs steam.
- The belt jams and two mosslings fix it.
- Two critters meet and exchange a speech bubble.
- Fireflies appear at night. The background shifts with the real time of day.

### Away time

On start, compute earnings for the time the device was off from the current
output rate, capped as above, and show them as a pile of jars on the dock that
ships out quickly. Plants advance by the real time elapsed.

## Items

A small self-contained record: a sprite, a name, a one-line description, and a
behaviour. Under 1 KB so hundreds can be stored and one sent in a single small
message.

| Field | Shape | Notes |
|---|---|---|
| Format version | 1 byte | Devices ignore fields they do not know and fall back to the recipe. |
| ID | Unique, assigned by the server | Shown as a short number, e.g. No. 0042. |
| Name | Up to 12 characters | |
| Line | Up to 32 characters | Shown when the item is selected. |
| Kind | floor decor, hanging decor, or critter | Decides where it can be placed. |
| Palette | 8 colours | Slot 0 is transparent. Chosen fresh for each item. |
| Frames | 1 to 4 frames, each 16x16 at 3 bits per pixel | 96 bytes per frame. Frame 0 is the shop picture. |
| Recipe | Movement, speed, favourite zone, up to 3 habits | The phase 1 behaviour. Always present. |
| Bubbles | Up to 4 strings of up to 12 characters | Speech bubbles the item can show. |
| Script | Optional bytecode, up to 256 bytes | Phase 2. Empty in phase 1. |
| Memory | 8 small integers | Persisted per item. Used by scripts in phase 2. |
| Provenance | Maker's name, date made, the seeding tags, who gifted it | e.g. "made on a rainy night in Maya's jar". |

### Recipe (phase 1 behaviour)

A filled-in form, not code. The game implements every option.

- Movement: sits, hops, wanders, sways, floats.
- Speed: slow, medium, fast.
- Favourite zone: garden, works, dock, near water, up high, anywhere.
- Habits: up to 3 pairs of when this, do that, from the event and action lists
  below. Example: when a snail passes, show bubble 1.

### Rules

- Each item exists once. Gifting moves it; nothing is ever copied.
- Placement is in decorate mode: pick an item, move it along its allowed line
  with the arrow keys, confirm.
- The player can own more items than are placed. Unplaced items live in My Stuff.
- At most 24 placed items are active at once (placeholder).
- A device accepts an item only if it carries a valid signature from the server.

## Item behaviour: events, actions, and later scripts

Phase 1 ships with recipes only. Phase 2 adds a tiny bytecode machine. Both use
the same events, actions and senses, so build those once and build them first.

### Events

| Event | Fires when |
|---|---|
| tick | 4 times a second while the item is active |
| poke | The player selects the item |
| near | Another item or critter comes within 16 pixels; carries its kind |
| jar shipped | A jar leaves through the cork door |
| belt jam | The conveyor jams or is fixed |
| gift arrived | A parcel enters the jar |
| time change | Dawn, day, dusk or night begins |
| weather change | The day's weather tag changes, when known |

### Actions

- Move: hop, walk toward a point or zone, float, turn to face something, stop.
- Show: switch frame, flip, glow on or off, emit one particle (sparkle, heart,
  note, zzz, puff).
- Say: show one of its own bubbles for 2 seconds.
- Wait: do nothing for a number of ticks.

### Senses

Its own position and zone, the nearest item's kind and distance, time of day,
weather tag, day count since it was made, whether it was a gift, and a random
number.

### Phase 1: habits

A habit is one event paired with one action. The game checks each active item's
habits when an event fires.

### Phase 2: bytecode (later)

- A short list of numbered instructions run by a loop in the game.
- Instructions: the actions above, reading a sense, reading and writing the 8
  memory slots, compare, jump, return.
- One entry point per event; none means the event is ignored.
- Step limit 64 per entry call (placeholder); on reaching it the script stops,
  the item shows a ? bubble, and its recipe takes over until the next event.
- Unknown instruction or newer format: ignore the script and use the recipe.
- No access to files, network, coins, other items' memory, or anything not
  listed.
- Written by the AI in a small readable language and compiled to bytecode on the
  server. The device only sees bytecode. One shared instruction table; the
  server runs the same machine to test scripts before delivery.

What to do now so phase 2 is cheap: keep the script and memory fields in the
record, route every recipe habit through the same event and action functions a
script would call, and persist the 8 memory slots.

## How items are generated (steps 5+)

Each day the server generates a fresh stock for each player from what they grow
(the garden: mushroom = odd/glowing/spooky, berry = sweet/round/food, fern and
moss = soft/sleepy/cosy, flower = fancy/dressed-up, cactus = spiky/deserty/tough;
3 real days to mature, placeholder; only mature beds count), what they display
(the 4-slot shelf's tags), and the day (date, season, moon, local weather; some
days allow one special item). Daily flow: the device sends garden mix, shelf
tags, owned names and locale; the server turns them into a short tag list,
generates 8 items (sprite, palette, frames, name, line, kind, recipe, bubbles;
a script in phase 2), validates (format and sizes; 8-colour sprite, not blank,
not mostly one colour; text filter; known recipe options; phase 2 script
simulated a day within limits with a visible action), regenerates failures,
signs, and returns the batch with the tag list. Offline, yesterday's stock
stays. The shop shows the tag list as a "Today" card.

## Shop

Two tabs, Stock and My Stuff; no way to request a specific item.

- Stock: left, a 4x2 grid of 28x28 tiles (16x16 sprite and price), with "time
  until new stock" above; below, the Today card; right, a detail panel (sprite
  at 2x, name, line, kind and movement, price). Each item is sold once; a bought
  tile is empty until the next batch. Enter buys and opens the item card. G buys
  and goes to Send a Gift. Unaffordable: price in a warning colour, Enter does
  nothing.
- Item card: after buying, and on any owned item: sprite, name, line, kind,
  provenance, number; actions place in jar, put on shelf, send as gift.
- My Stuff: a scrolling grid of every owned item, with a marker for placed and
  shelved ones. Enter opens the item card.

## Garden (P), shelf (H), friends and gifting (steps 4, 6)

- Garden: the 6 beds with plant type and days to maturity, a flavour-mix bar;
  selecting a bed offers the 5 plant types with price and flavour; replacing a
  mature plant asks first.
- Shelf: 4 slots, filled from My Stuff; shelved items can still be placed.
- Friends: a display name up to 8 characters and a short friend code; both add
  each other before gifts flow; the list shows last online.
- Sending: Send as Gift from an item card or the shop, pick a friend, optional
  24-character note, confirm; postage charged, item in transit; removed from the
  sender only when the server confirms it holds the gift; refunded on failure.
- Receiving: collected when online; the mail indicator counts; each gift floats
  down on a parachute, critters look up, a banner names the sender; Enter opens
  it (item card with the note); T sends a one-key thank-you that shows as a
  heart on the sender's device; unopened parcels wait on the dock; nothing
  expires. Gifts are final; notes pass the text filter; only server-signed items
  are relayed.

## Screens and look

Jar (default), Decorate (D), Garden (P), Shelf (H), Shop: Stock (S), Shop: My
Stuff (S then Tab), Item card (Enter on an item), Send a Gift, Friends (F), Mail
(M). The scene keeps nothing important in the top or bottom 13 rows.

Pixel art, dark and cosy; selection is a 1-pixel yellow outline; key hints are a
light key cap followed by a word.

| Use | Colour |
|---|---|
| Interface background | #17132a |
| Panel | #2a2447 |
| Text | #f2ecdc |
| Secondary text | #a59fc4 |
| Coins and selection | #ffd166 |
| Gifts and mail | #ff8fab |
| Item traits | #7fd6a6 |
| Jar interior | #1f3340 |

## Build order

1. Jar on its own: scene, production loop, coins, idle events, hiding bars, save
   and load. Fake clock allowed.
2. Items, offline: item record, sprite drawing, recipes and habits, decorate
   mode, My Stuff, with a handful of hand-made items.
3. Shop, offline: Stock tab, item card, buying, from a fixed batch on the device.
4. Garden and shelf.
5. Server: generation (daily request, AI generation, validation, signing,
   delivery; real time and weather).
6. Server: friends (codes, gift relay, arrival, thank-you).
7. Scripts (bytecode machine on device and server, compiler, script generation
   and simulated-day test).

Each step should leave a game that runs and is pleasant to watch.

## CardOS decisions (2026-10-09)

Taken with the owner:

- **Scope now: steps 1 to 3, offline.** Garden and shelf (4), the server (5, 6)
  and scripts (7) come later.
- **Factory: human junk, one product, jam** (thimble vat, matchbox press, twig
  crane, bottle-cap belt).
- **Weather: the region of the device's time zone** (`env TZ`), looked up by the
  server, no typing.
- **Server (steps 5 and 6): a general-purpose store on arowe.net** rather than a
  new Python module for the game -- per-user and shared documents, lists/queues
  and expiry, probably a generic "ask Claude for this schema" endpoint, used by
  the app directly. To be designed before step 5. Players are likely the
  existing CardOS accounts.

How it fits CardOS:

- A `.capp` in Games, `apps/jar.c` with its logic in portable headers
  (`apps/jarsim.h` for the factory and items, `apps/jaritem.h` for the item
  record), host-tested like Forklift and Pet.
- App data must stay under the build's 28 KB budget (`tools/build_apps.py`
  enforces it): owned items live on the card, only placed items' frames are in
  RAM.
- The scene animates every frame, so the app composes its own 16-row strips and
  sets `CAPP_PAINT_DIRECT`, as Kart and Noodle do.
- The 6x8 font is the bitmap font (40 columns, not 34).
- The OS dims the screen after its own timeout; the app does not fight it.
- Items made by hand for steps 2 and 3 are built in and trusted; the signature
  check applies to items that arrive from the server (steps 5 and 6).
