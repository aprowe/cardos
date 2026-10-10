# Changelog

Releases are git tags, `vMAJOR.MINOR.PATCH`. The firmware reports its
version from `git describe` (ESP-IDF stamps it into the image): the boot
banner, `bootinfo` and About show `v0.9.0` on a tagged build and
`v0.9.0-3-gabc1234` between tags (`-dirty` if built with uncommitted
changes). `CAPP_API_VERSION` in `kernel/app/capp.h` is a separate number:
the app table, which every `.capp` must match.

To release: add a section here, commit, `git tag -a vX.Y.Z -m "..."`, then
`bash tools/deploy_droplet.sh sync`, which pushes the tags to GitHub and the
droplet so its builds carry the same version.

## v0.15.0 -- 2026-10-10

- **App code runs from flash (XIP).** This was the other session's work,
  merged here: the `appcode` partition, a 28 KB data arena for the app on
  screen, and no code cap for apps.
  - Jar now costs 0.36 KB of heap instead of 45 KB.
  - A device needs one full USB flash to get the new partition table.
    Without it, app code loads into RAM as before, and Jar (50 KB of code)
    will not fit.
- **Jar Factory is one app again.**
  - The shop and the post are screens of it, not separate apps, so there is
    no reload when you switch between them.
  - When it starts, the old Jar Shop and Jar Post apps are removed from the
    card.
  - Its data fits the 28 KB arena because the screens borrow the scene's
    item memory while they are up.
- **API 45:** `paint_direct(on)`, for an app with screens that compose
  their own strips and screens the OS composes. Every app is rebuilt;
  devices need `update all`.

## v0.14.0 -- 2026-10-10

- **Tibbs is one person for everyone.** He is a single Claude Code session
  on Opus 5.5, resumed for every turn and shared by every player, so his
  memory is the conversation itself.
  - He knows every customer: what they bought, what they said, who sent
    what to whom. Ask him about a friend.
  - Purchases, gifts, new finds and new friends reach him as news on his
    next turn.
  - He runs in his own directory, not the repository.
- **Stocks are 8 again:** what you hold, 3 random items from the shared
  pool, 3 Tibbs picks for you, and 2 new finds from his briefs. New items
  are made on Opus, with more effort for dearer ones.
- Measured on the laptop with real Opus: a stock turn takes 14 s and a
  reply 7–8 s. Asked "what has britney been up to?", he knew her purchase
  and the gift she sent, with its note.

## v0.13.0 -- 2026-10-10

- **Tibbs, the shopkeeper.** The shop is a person with an ongoing story.
  - **How stocks are made:** each new stock starts with his line of the day
    (shown in the shop) and a brief for each new find, and every item is
    made from its brief.
  - **No more tags:** the shelf, the garden and the season no longer steer
    anything.
  - **What he knows:** your recent talk, your purchases (as history only)
    and what is in your jar.
  - **Variety comes from the code:**
    - random events in his day;
    - random sparks for each find;
    - a coin toss deciding whether a find answers what you asked;
    - recent themes are not repeated.

    This keeps the AI from latching onto what you bought.
  - **Talking:** talk to him from the shop (`t`), the jar menu (Tibbs) or
    Jar Post (`t`).
- **Prices are chosen first**, from common (30–80) up to a rare treasure
  (450–900). Dearer items get more care and more AI effort. The price is
  part of the signed item.
- **Seed packets:** about 30% of stocks include one, and the Garden plants
  from seeds (everyone starts with one of each).
- **Gifts from the friends list:** in Jar Post, press Enter on a friend,
  choose one of your things, add a note and send.
- **Ledges:** a ruler shelf on bottle caps and a matchbox tower at the back
  of the jar.
  - In Decorate, Up/Down puts floor decor and critters on a ledge.
  - Critters climb up, potter about and come down; "high" critters like it
    up there.
  - Saves keep each item's level; old saves load with everything on the
    soil.
- **Nothing is made off screen:** coins and jam come only while the jar is
  open, and the "while you were away" banner is gone. Plants still grow by
  the real clock.
- The shelf is gone.

## v0.12.1 -- 2026-10-10

- **Fix: Jar Shop said "0 here" while every item was being refused.** A
  device that had been up about 2 hours refused every signed item as
  "signature bad". The same records verify on the server. The device's own
  decode built the exact message the server signed (checked byte for byte
  by running the app's C code on a real record), and a fresh boot of the
  same source accepted all four. The cause is still unknown. If it comes
  back:
  - `api->sig_verify` now logs the crypto library's status, the size and
    the free heap to `log`;
  - the shop says "N failed the check (log)" rather than looking empty;
  - a refused item is no longer fetched again every 5 s while the rest of
    the stock is made.
- The firmware's version banner is stamped when the build is configured, so
  an incremental build can show an old tag (the device said v0.10.0 while
  running v0.11.0's API 44).

## v0.12.0 -- 2026-10-10

- **The item pool.** Every item made goes into one pool shared by everyone.
  A shop's stock is 4 items:
  - first, what you hold;
  - then items from the pool;
  - then new ones.

  With nothing held that is 2 from the pool and 2 new; there are always at
  least 2 besides what is held.
  - **Unique:** an item is in one place at a time (the pool, one person's
    shop, or someone's things), so items stay unique.
  - **Turnover:** when you next ask for a stock (a new day, or `r`),
    whatever you neither bought nor held goes back to the pool. It never
    comes straight back to you.
  - **Picking:** pool items are chosen to fit your garden, shelf and hint.
  - **Speed:** they show up at once, so only the new ones wait for Claude,
    and a stock costs 2 calls instead of 8.
  - **Seeding:** `python -m server.jar seed 20` fills the pool;
    `python -m server.jar pool` lists it.
- **Hold:** Space in the shop holds an item, up to 4. It stays in your
  shop through new stocks until you buy it or press Space again. The shop
  tells the server what you bought and what you hold with each request.
- **Fix: a stock that was still being made never reached the shop.** The
  shop gave up after 3 minutes, and on reopening it saw a stock dated
  today and never asked again. Now:
  - each stock carries a batch number;
  - every time the shop opens it checks quietly and picks up the rest of an
    unfinished stock, or a new one;
  - it keeps trying for 15 minutes.
- **Fix: a server restart mid-stock left it pending for good.** A poll now
  notices and starts it again, keeping what was on show.

## v0.11.5 -- 2026-10-10

- **The shop's stock arrives one item at a time.** The server asks Claude
  for one item per call and stores each as soon as it passes the checks.
  `/jar/day` answers with the items made so far plus a `more` line until
  the stock is whole, and Jar Shop shows each item as it lands. Anything
  bought meanwhile stays bought. On the laptop the first item took 38 s,
  where all eight used to take 105 s together. Each call is told its kind
  (3 critters, 3 floor decor, 2 hanging) and the names already made, so
  the mix and the variety hold. A hint goes to items 2, 5 and 7. A call
  that comes to nothing is tried again, up to 4 times. If the day's asks
  run out partway, the stock keeps what was made.

## v0.11.4 -- 2026-10-10

- **Fix: the shop's stock could vanish.** Once the day's stock was already
  made, the server answered the shop's request with "ok DATE" and no item
  count. Jar Shop read that as a stock of nothing and saved it over the
  real one. Now the server sends the whole answer (the same one GET gives),
  and the shop treats a reply with no count as "ask again", never as empty.
- **A word to the shopkeeper:** `e` in the shop. It is a line of up to 34
  characters, like "could use more industrial stuff", kept in
  /var/jar/hint.txt and sent with every day's request. The server tells
  Claude the shopkeeper is a junk dealer with connections, not a
  wish-granter. A hint leans two or three items that way, still in keeping
  with the day's tags, and nothing simply grants it. A hint that fails the
  word filter is dropped.
- **Jar menu:** Zoom leaves the menu (Z still works and stays on the bar).
  A **My Stuff** tile (`i`) takes its place and opens your inventory, where
  the bar now shows `G gift`.

## v0.11.3 -- 2026-10-10

- **Jar Factory menu:** the bottom bar is just "Ent menu  Z zoom" now.
  Enter opens a full-screen menu of eight tiles in 2x text (Shop, Decorate,
  Garden, Shelf, Upgrades, Friends, Mail, Zoom), each tile showing its key.
  Arrows choose, Enter opens, Esc closes. With parcels waiting it opens on
  Mail, showing the count. The letter keys still work straight from the jar.
- **Jar Shop testing keys:** `r` asks the server for a new stock straight
  away (`POST /jar/day?fresh=1`; this still counts toward the daily asks),
  and `$` adds 1000 coins.

## v0.11.2 -- 2026-10-10

- **Jar Factory zoom:** Z shows the jar at exactly 2x -- every scene pixel
  as 2x2, so the pixel art stays crisp. The view follows the first mossling
  and glides with it; Tab follows the next critter (then the snail), arrows
  pan freely, Z or Esc goes back. Banners still show; a "2x" mark sits in the
  corner. No extra memory: the scene draws 4 rows into the second half of
  the strip and they are doubled into the whole of it. A host test checks
  the zoomed frame against the scene pixel for pixel. The hint bar's
  "Upgrade" is "Up" now, to fit "Zoom".
- The daily stock allows 900 s a round: on the droplet, at effort medium, 8
  items took 391 s (all verified against /sign/pubkey, 4 with scripts).

## v0.11.1 -- 2026-10-09

- The daily stock asks Claude at effort medium (`ask_shape`'s new default;
  `ask_once` takes an effort). On the droplet the CLI's own default made one
  round of generation outlast its 420 s; on the laptop, whose settings say
  medium, a whole day took 105 s.
- The server's signing key is backed up on the droplet
  (`/root/sign_key.pem.bak`): a new key would orphan every item given out.

## v0.11.0 -- 2026-10-09

**Jar Factory, all seven steps of the owner's spec**, and the server's
general-purpose store under it. **API 44** (`api->sig_verify`, ECDSA P-256):
every app is rebuilt, so a device needs `update all`, not `update apps`.

- **The server store** (`server/kv.py`, `docs/superpowers/specs/2026-10-09-
  server-store-design.md`): values, counters and queues in SQLite with
  per-person and shared namespaces, `/people`, `ask.ask_shape` (Claude for a
  JSON schema, with retries and a daily cap) and `server/sign.py` (the
  server's signing key). New features build on it instead of a module each.
- **The daily stock** (`server/jar.py`): eight items a day per person, drawn
  and written by Claude from the garden, the shelf and the day (season,
  moon, the time zone's weather), checked, signed; an ownership ledger,
  friend codes, gifts re-signed and relayed through the store's queues,
  thank-yous. Tried live: 8 items in 105 s, all verified, 5 with scripts.
- **Scripts** (step 7): `apps/jarvm.def` is the one instruction table for the
  device's machine (`apps/jarvm.h`) and the server's compiler and simulated
  day (`server/jarvm.py`); fixtures pin that both machines agree. The item
  signature leaves out the 8 memory slots scripts write.
- Untested on hardware (no device attached): the signature check, the app
  switching between the three Jar apps, real gifts between two devices.

Tests: host 21914 checks, server 341 tests.

### Jar Factory, device notes

**Jar Factory** (`apps/jar.c`, Games): an idle game, steps 1 to 3 of
`docs/superpowers/specs/2026-10-09-jar-factory-design.md`, offline. A
terrarium with a jam factory inside built from junk -- thimble vat, matchbox
press, cotton spool, twig crane, bottle-cap belt -- that runs by itself:
mosslings carry berries from the beds, the snail takes the jars out through
the cork door, and coins land only when it leaves the screen. Upgrades
(mosslings, machines, beds, a faster belt) appear in the scene; time away is
paid as a pile of jars on the dock (from `api->epoch()`, capped at 8 h);
naps, steam, jams, chatter, fireflies and the background follow the real
hour. The bars slide away after 10 s and any key only brings them back.
Twelve hand-made items, four to start with and eight in the shop; Decorate
places them (at most 24), My Stuff and the item card show them. The item
record (`apps/jaritem.h`, under 1 KB, versioned) is the format the server
will send in steps 5-6; habits run through the same event and action
functions phase 2's scripts will call (`apps/jarsim.h`). Art is text in
`tools/jar_art.txt` (`python tools/make_jar_art.py`). Host tests:
`test_jarsim.c`, `test_jaritem.c`, `test_jar.c` (`JAR_DUMP=dir` writes
frames). Code 23.0 KB + data 21.5 KB, of 44 KB.

**Jar Factory, steps 4 to 6 on the device: one game in three apps.** The
jar was 44.5 KB of a 45 KB budget, against a heap whose largest piece is
often 27 KB, so its menus moved out: **Jar Shop** (`apps/jarshop.c`: the
shop, My Stuff, the item card, upgrades, the garden, the shelf, the daily
stock) and **Jar Post** (`apps/jarpost.c`: friends, gifts, mail), both
Games. The jar keeps the scene and Decorate, saves, and opens them with
`api->run` at a screen -- S shop, P garden, H shelf, U upgrades, F friends,
M mail, and Decorate's N -- and quitting either (Esc at its top, or fn-`)
comes back to a jar that starts afresh from its save, paid for the time
away. They share `apps/jarstore.h` (the files under /var/jar) and
`apps/jarui.h` (the look, the card, the server); they keep the jar without
running it (`JS_KEEP_ONLY`, 6.6 KB less data). The launcher now treats an
app opening one already on its back stack as going back to it, so the shop
and the jar do not stack up. Sizes, code + data: jar 20.8 + 17.2 KB, Jar
Shop 17.3 + 18.4 KB, Jar Post 14.0 + 14.7 KB.
- **Garden** (step 4): six beds of berry, fern, mushroom, flower or cactus,
  bought with coins, three real days to grow (by `api->epoch`; waiting with
  no clock); only grown beds make jam, and their mix -- a bar on the garden
  screen -- steers the daily stock. Pulling up a grown plant asks first.
  **Shelf**: four items whose tags steer it too.
- **The daily stock** (step 5): `POST /jar/day` with the garden, the shelf's
  tags, owned names and the TZ setting; `GET /jar/day` until it is made;
  `GET /jar/item?i=K`, one signed record each, checked with
  `api->sig_verify` against the key pinned from `GET /jar/pubkey` (a forged
  one is dropped and logged). The batch replaces the old one only whole;
  offline, yesterday's stays. The Today card shows the tags; "new stock in"
  counts to UTC midnight. A record carries no price, so `jst_price` works
  one out (45 to 200). `ji_signed_message` (`apps/jaritem.h`) is the rule
  agreed with the server: no signature, its length byte and the memory
  slots zeroed, the total length less the signature.
- **Friends and gifts** (step 6): your code and name, add by code, the list
  with mutual/waiting and last seen; Send a Gift picks a mutual friend and
  a note of up to 24, takes 10 coins postage, and the item leaves only on
  the server's "ok" (postage back otherwise). Parcels are collected from
  `jar.gifts`, checked, kept and only then acknowledged; opened, they become
  owned items, the card shows the sender and note, and T sends a heart.
  The jar asks how many wait (on open, then every three minutes while the
  network is up): a new one floats down on a parachute, the critters look
  up, a banner names the sender, and it waits on the crates; Enter opens the
  post. Thank-yous arrive as hearts over the dock.
- Records from the server are kept byte for byte, so a gift still verifies.
  Base64 is `apps/b64.h`. Host tests: `test_b64.c`, `test_jarstore.c`,
  `test_jarshop.c`, `test_jarpost.c` and the reworked `test_jar.c`, against
  a stand-in server (`test/jarfake.h`).

## v0.10.0 -- 2026-10-09

The cleanup: a multi-agent audit (seven reviewers, 78 findings, the eight
most severe re-checked against the code) and five workstreams fixing it.
API 43, unchanged: apps built for v0.9.0 still load.

**Flicker.** Full app repaints are composed off the panel a 16-row strip at a
time and sent whole (`draw_offscreen`, `kernel/ui/apphost.c`), so nothing
filled-then-drawn reaches the screen; apps that draw their own strips opt out
with `CAPP_PAINT_DIRECT`. The launcher, banner, picker, desktop and alarm use
the same strip; the busy badge waits 350 ms and repaints only its corner (it
repainted the whole shell on every request -- every ~1.2 s in Build). Tetris,
Pet, the toolbar and Quoridor's lobby stopped redrawing what had not changed.

**Memory.** About 32 KB more heap at boot (static RAM 172 KB -> 140.5 KB):
buffers held for the uptime now live only while used, the old memory
manager/swapper/scheduler and device-side Google sign-in are out of the
firmware, colour icons share 16 slots instead of a malloc each, the notify
poll no longer takes 8 KB every 30 s. A failed app load says how much it
needed and the largest block there was; the Memory app shows `mem`. The build
fails an app over its size budget; Notes, MIDI and Forklift were slimmed.

**Bugs fixed.**
- Stocks, Chat and Hub wrote past their reply buffers (`http_poll` now
  returns what it copied).
- Dashboard Link's uploads were cut at 511 bytes.
- Voice could take the mic pin while audio held it; G0 during music now stops
  the music first.
- WiFi could be torn down under a running request.
- Music and Photos could delete files from the card when the server's list
  did not fit their buffer.
- Web cut URLs at `&`; Explorer took Enter as yes on delete; Files opened
  pictures in Edit.
- Voice "open notes" closed the app it had just opened; a notification due
  under the lock screen was dropped.
- Server: a Google re-sign-in kept the old token (Todo 403s) for an hour;
  saving a Toggl token wiped the targets; one person's new Build
  conversation could stop another's.

**Structure.** One HTTP layer with fixed error codes, one owner for the audio
pins, one shell state and one key path, release hooks registered per module,
shared app headers (`str.h`, `confirm.h`, `syncset.h`, `dirmodel.h`,
`termlog.h`, `datetime.h`) and a shared test fake, a shared server store
(0600 files, a lock per file), route auth kinds, job expiry. Removed: dead
routes and commands, `google pull`.

Tests: host 19740 checks (was 19532), server 242 tests (was 196).

## v0.9.0 -- 2026-10-09

The first tagged release: everything up to here, as a baseline before the
cleanup. CardOS on the Cardputer and Cardputer ADV, API 43.

- Shells: the launcher (animated carousel, folders, search, favourites,
  hotkeys), the desktop, the console.
- Apps as native `.capp` files loaded from the card; built-ins Memory, About
  and Settings.
- The server: updates (debug and release firmware, apps), Build on the
  droplet (planned and stepped, `--effort xhigh`), fenced Build for people
  who are not the owner, accounts, the dashboard, Google, Toggl, notes,
  photos, music, MIDI, chat, rendering for Web, the Claude app's messages.
- Notifications (banner, centre, scheduled, polled), the lock screen (dim
  clock or black), voice on G0, the printer, audio, USB disk mode, ESP-NOW
  local multiplayer (Quoridor), Hub (M5Launcher's firmware catalog).
- New Update app: a list with a mark per item and real progress bars.
