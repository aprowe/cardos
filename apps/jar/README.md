# Jar Factory

_Moved from CLAUDE.md on 2026-10-27._

**Jar Factory is one app again** (2026-10-10, API 45). The shop and the post
are screens of `apps/jar/jar.c`: `apps/jar/jarshop.h` and `apps/jar/jarpost.h` (each still
compiles alone for its host tests) are included with their clashing names
renamed, and `jar_open` switches screens. Two things make it fit:
- **Code** runs from flash (XIP, the `appcode` partition), so the 50 KB of
  code is no longer the limit.
- **Data** has to fit the 28 KB arena. While a screen is up, the scene is
  saved, its placed items become `want` lines, and the screens' state lives
  in the scene's item memory (`screen_bind`): the post in `J.placed`, the
  shop with `U` and the picture buffer in the frame pool. Their big buffers
  share `SCR` with the scene's strip. The scene reloads from the card when
  it comes back.

Data is about 430 bytes under the budget: measure before adding.
`api->paint_direct` lets the scene paint its own strips while the screens
are composed by the OS. A device whose partition table predates `appcode`
(updated only over the air) loads app code into RAM, where 50 KB will not
fit: it needs one full USB flash.

**Jar Factory** (2026-10-09, `apps/jar/jar.c`, Games): an idle
terrarium jam factory with AI-made collectible items, the owner's spec in
`docs/superpowers/specs/2026-10-09-jar-factory-design.md` (all seven build
steps). The world and items are portable headers (`apps/jar/jarsim.h`,
`apps/jar/jaritem.h` -- the item record, under 1 KB, the server's packer mirrors
it byte for byte), host-tested. `apps/jar/server/__init__.py` makes each person's daily
stock with Claude from their garden, shelf and the day (8 items, sprites
included, checked, signed), keeps the ownership ledger, friends and gifts on
the store. Items run recipes (habits) or, in phase 2, bytecode scripts:
`apps/jar/jarvm.def` is the one instruction table, read by the device machine
(`apps/jar/jarvm.h`) and by `apps/jar/server/jarvm.py` (compiler, the same machine, a
simulated day); test fixtures pin that both machines agree. The signature
covers the record minus its signature and its 8 memory slots (scripts write
those).
The jar's light is the real sky (2026-10-10): `GET /jar/sky?tz=` (open-meteo,
cached per city per half hour) gives the weather and today's sunrise and
sunset; the jar moves its clock so they fall in dawn and dusk, tints the sky,
dims everything in front by the hour and the weather (glowing items light
their corner), draws rain, snow and lightning, and fires `on weather rainy:`.

**Jar's friends are the platform's** (2026-10-27). Jar had its own list --
friend codes, `friends/NAME` in srv/jar, one-sided adds -- and now uses
`server/friends.py`, the friendship every app shares: a gift (`/jar/gift`)
or a thank-you (`/jar/thanks`) goes only between two people who are
friends there (`friends.are_friends`), and Tibbs hears of `friends_of`.
Jar Post's Friends screen reads `GET /friends` (friend, asked, or "wants
you"), `A` asks someone by their account name (`/friends/ask`), Enter on
one who asked says yes; the list stays Jar's own (gifting goes friend,
then thing, then note, which a picker over the app would not shorten), so
apps/social.h's picker is not used and the data block did not grow. For the
jar.capp already on devices the old routes stay, as translations:
`/jar/me` lists platform friends as `mutual` and your asks as `waiting`,
`/jar/friend?code=` is `friends.ask` of the code's owner, `/jar/unfriend`
is `friends.drop`. `python -m apps.jar.server migrate-friends` turns the old
lists into platform friendships once (both had added: friends; one had:
an ask), notifies nobody and leaves the old lists in place. A gift is still
only ever something a person chose to send.

**The living jar** (2026-10-11, branch `jar-living` until API 46 is on
main; design in docs/superpowers/specs/2026-10-11-jar-living-world-design.md).
- **Language v2** (apps/jar/jarvm.def, both machines, cross-checked fixtures):
  `on hour/signal/world/new/berry/bumped`; `sound`, `burst`, `throw`, `eat`,
  `drop`, `signal`, `seek`, `fly`/`home`, `boost`, `nudge`, `shake`; senses
  incl. `traits`/`neartraits`/`newtraits` read with `has(X, TRAIT)` (a BIT op).
  Traits are the record's tag field, sixteen words; scripts react to traits,
  never names. A name must mean one number (water was a zone and a trait).
- **Gated:** apps/jar/server/jarvm.py makes and accepts only v1 until
  `CARDOS_JAR_V2=1` -- a device needs a jar.capp built with v2. Turn it on in
  /etc/cardos/env after jar-living reaches devices.
- **The sim** (apps/jar/jarsim.h): world events (ants, leak, breeze, visitor,
  dark, bloom) every 20-60 min; chores that stop production until the player
  comes (snail sulks, mould, puddle; js_chores/js_fix, saved); furniture
  (js_put_on, rides along, a fifth number on "place"); scenery; parts.
  Coins are still only jars that left: a visitor leaves jars on the dock.
- **The record's longer header** (224 bytes, still version 1): role, scale,
  surface, part, part home, voice. Things keep 208 byte for byte.
- **The scene** (apps/jar/jarlive.h): drawing for all of it, a sound bank and a
  sparse soundtrack (apps/synth.h, b cycles), x for chores, keep_awake.
- **AI on the fly:** patches (apps/jar/server/jarpatch.py) -- a few handlers for an
  item in the jar about a newcomer's traits, signed, fetched with the post,
  run for events the item's own script leaves; the day's events
  (apps/jar/server/jarevents.py) -- times and kinds rolled by the code, named by Claude.
- **Deals in talk:** `coins N` before the words; `DEAL:` lines; `tx` effects
  done once (txseen); a commission comes as a `deal` line on /jar/day.
- **Tibbs's memory:** CARDOS_STATE/tibbs/history (every exchange), a
  notebook every 20 turns, a seeded fresh session after 150 or a lost one;
  apps/jar/tools/tibbs_backfill.py put his past in.

**A shop's stock is 8 items** (2026-10-11): 2 random, 2 of Tibbs's picks, 4
new; a paid commission is 8 new. The older note below says 4.

**A shop's stock is 4 items, from a shared pool** (2026-10-10). Every item
ever made goes into one pool (`apps/jar/server/__init__.py`, "the pool"), and `own/ID` is
where an item is: absent means the pool; a name means that person's shop or
things. That keeps every item unique. A stock is:
- what the person holds (Space in the shop, 4 at most);
- then pool items, a random mix;
- then new ones, made one Claude call each and shown as each lands
  (`/jar/day` says `more` until the stock is whole, and `batch N` says
  which stock it is).

The device buys offline and reports `bought` and `held` ids with its next
request; the rest goes back to the pool then. The shop checks `/jar/day`
quietly on every open, so it picks up a stock it missed. `python -m
server.jar seed N` adds N items to the pool, counted against nobody.

**The shop is a person: Tibbs** (2026-10-10, `apps/jar/server/shopkeep.py`). There
are no tags any more. The shelf, the garden and the season used to steer
the stock, and a request for industrial scrap on an autumn day came out a
jack-o'-lantern with a gear in it. Now each new stock begins with Tibbs's
beat: his line of the day (shown in the shop) and a one-line brief per new
find, each item then made from its brief. He keeps a journal per player:
- his last few beats;
- the recent talk;
- purchases, as history only;
- what is in their jar.

**Variety is the code's job, not the model's** -- the owner's rule. Models
latch on: sell one cute thing and every find is cute. So:
- the server rolls the dice: an event in his day, and two sparks per find
  from wide word lists;
- a coin (ANSWER_CHANCE) decides whether a find answers what the player
  said;
- recent briefs are passed as themes not to repeat;
- purchases are framed as history with a rule against catering to them.

Change the prompts with care: they were tuned against real output.

**Items are made in stages** (2026-10-11, v0.20.0, spec
docs/superpowers/specs/2026-10-11-jar-item-pipeline-design.md). The only
path: `jar.generate(search=)` -> `apps/jar/server/jarstages.py`. Per find the code
rolls kind, tier, form and activity (`jar.ACTIVITIES`); the DM writes a
concept from Tibbs's search (`dm_prompt` is the one place to grow its
strategy); the fitter writes the sheet inside `jar.motion_rules(activity,
kind)` (only critters travel); the artist and the behaviour writer run on
two threads; `check_parts` routes a refusal to the stage at fault, once.
`concept/ID` is kept beside the record (never sent to devices); remakes
(`jarstages.remake`) start from it. Stages run with limit=None. Motion
parts are record bytes 215 travel, 216 idle, 217 lively (agreed with the
device side: a still thing with no idle is written lively=normal so it
never reads as all-zero, which means "an old item, use byte 72"; the
fixture is apps/jar/test/fixtures/jar_item_motion.bin). Tibbs's stock turn now
answers `search` (his last six in tibbs/searches); sparks, briefs and the
chance event are gone. `apps/jar/tools/jar_stages_trial.py` runs real finds in a
scratch state. Jar Lab still previews the old one-call prompt.

**No examples in the prompts that make items** (2026-10-13, the owner: "AI
grasps onto places so we need lots of variance without examples"). Finds
had become made objects -- 21 of 22, critters as "a painted tin hedgehog",
brass in 9 -- because the code rolled a place per find from a list ("he
found it at a model village gift shop -- let that shape what it is"), and
Tibbs's character named six contacts, whom his last six days visited in
turn. Now: no place is rolled for a find; the critter slot says "It is
alive -- not a toy, ornament or model of one"; the DM is told to decide
what a thing is first and name a material only where that is simply what
it is made of (materials never steer); his day is "the setting, not a list
of what to make"; the tier is rarity ("a common find", not "a cheap
trinket"). Tibbs is named no place, not even his own last searches
(still kept in tibbs/searches, never shown); the code rolls the first
letter of where he went. His resumed session and notebook still remember
the old contacts.

**Tibbs's story moves in chapters, and finds are made ahead** (2026-10-13).
- **Chapters** (`shopkeep.maybe_chapter`): time only moves when someone
  visits. A visit (a stock started, or its last item fetched) 4 hours or more
  after the last chapter writes the next one -- one, however long the gap.
  The code rolls three pairs of initials, weighted like English word starts
  (`INITIAL_WEIGHTS`): a place he went, someone he dealt with, something he
  came back with ("W. S.", "P. K.", "C. H."); he names them. What players
  said at the counter and did (`tibbs/shoplog`, filled by `note` and talk)
  since the last chapter is rolled in. Every chapter prompt carries the
  opening, `apps/jar/server/tibbs_intro.txt` -- prose the owner edits -- and the
  last chapter; the last 12 are kept in `tibbs/chapters`.
- **The finds come from the chapter.** The stock turn only greets and picks
  from the pool; it asks "where did you go" only for a commission's errand.
- **The deck** (`jar.premake`, `deck/PERSON`): once a person has fetched
  the last item of a whole stock, their next NEW_ITEMS finds are made in the
  background from the latest chapter, theirs and out of the pool. A new stock
  takes its finds off the deck first and makes only the rest; a commission is
  made fresh. Nothing is made for someone who has not looked, and a deck
  never expires.
- **A paid errand takes real time** (2026-10-16): paid, he sets off at once
  but is out `ERRAND_S` (45 min). The deal keeps `back`; until then
  /jar/day's `say` is "Out on your errand ... back in about N min", there is
  no `deal` line (so the shop does not ask for the stock) and `r` waits;
  talk is told he answers a note from the road. Unpaid, he waits for the
  next chapter, as before.
- **Bans** (`jar.ban`, `banned/ID`): out of the pool for good. An item
  passed on (shown, neither bought nor held) twice is banned by itself
  (`passed_on`, `passes/ID`). `POST /jar/ban?id=N[&off=1]` and `GET
  /jar/bans` (the owner only), or `python -m server.jar ban N | unban N |
  bans`.
- **Jar keys**: Tab (or j) is the next jar, 1-9 jumps to one (1 the
  factory), bars or no bars; zoomed, Tab still follows a critter.

**Lively finds** (2026-10-15, the owner: "i want things lively,
interacting" -- the pill bug crawls, poked it curls and turns to physics).
`apps/jar/server/jarstages.py`, all v3:
- **A life is rolled per find** (`roll_life`): a role from `ROLES` (critter:
  grazer, hunter, timid, climber, flitter, singer, collector, player; floor:
  stage, shelter, feeder, toy, noisemaker, lamp, weathervane; hanging: lamp,
  roost, noisemaker, weathervane), a poke reaction from `POKES`, and a trait
  it notices -- one a resident of the person's jar really has, mostly. The
  role's trait is worn (`_wear_role`). Roles say what a thing does, never
  what it is: no nouns.
- **Shared signals** (`JAR_SIGNALS`: startle, call, crumbs, landed): items
  made apart can set each other off. The writer sees the residents and their
  traits (`residents_of`, from the request's `injar`).
- **Nothing moves a v3 item but its script** (the device ignores travel), and
  the writer is told so; `lively_error` refuses a script with no `on poke`,
  none that answers the jar, or a critter that never moves. Every v3 find
  has a script (rare and treasure: full); critters are never rolled still.
- **One chapter is not every find**: `CHAPTER_CHANCE` (0.35) of finds are
  told Tibbs's chapter, and borrow at most one thing from it; an errand or a
  commission always is. The DM is shown the last 20 finds; a name may not
  repeat one of the last 80 (`recent_finds`). (One chapter had made 4
  windmills, 7 clovers and two Clover Ants in ten finds.)
- Known: a v3 hanging item is placed on the soil by the device
  (`js_item_y` uses the lid only for non-v3) -- a firmware fix.

**JarScript 3** (2026-10-14, branch `jarscript3`; spec
docs/superpowers/specs/2026-10-14-jarscript3-design.md). The sandbox's
language (apps/jar/tools/jarsandbox) on the device: states that take time (`wait`,
`move to`, `walk`, `hop`, `play once`), physics, perches, signals by name,
`find(trait)`, 12.4 fixed point, 20 ticks a second.
- `apps/jar/jarvm3.def` is the one table. `apps/jar/jv3.h` (C) and
  `apps/jar/server/jarvm3.py` (compiler, the same machine, `simulate`, the guide)
  must agree byte for byte: `apps/jar/tools/make_jarvm3_fixtures.py` writes
  apps/jar/test/fixtures/jv3_*, `apps/jar/test/test_jv3.c` replays them.
- A script's first byte says its language: 1 is v1/v2, 3 is v3. The v2
  machine is untouched and never sees a v3 script.
- Records carry up to 1024 bytes of script (`JI_SCRIPT_MAX`;
  `JI_SCRIPT_V2` 256 for v2 and patches), `JI_MAX` 1728, four frames.
- The code rolls a **script tier**: none (still non-critters), small (512),
  full (1024, critters).
- On the device, `apps/jar/jarv3.h` runs v3 items in **one big-mode block**
  (16 KB, taken at launch). An item that does not fit is placed on its
  recipe, never refused.
- Measured: 260 us a tick for 24 items; 776 us for 24 copies of the worst
  scripts; 252 bytes of runtime an item.
- **Off by default**: `CARDOS_JAR_V3=1` makes new items v3 and retires the
  pool's v2 items (kept, never stocked).
- `python -m server.jar v3convert [--dry] [--id N]` rewrites held items in
  v3, keeping each original in `v2rec/ID`. `v3undo` puts them back. Both send
  the owner `tx remake ID`.
- A device needs this jar.capp before any v3 record reaches it: an older one
  refuses a script over 256 bytes.

**More jars** (2026-10-11, v0.21.0). `JU_JAR` in Upgrades buys up to
`JS_MAX_JARS` (3) extra jars: empty glass, no factory. `J.jar_at` is the
jar on screen (0 the factory), saved as `jars N AT`. One wallet: coins,
things, resources and deals stay in the main save; an extra jar's file
(/var/jar/jarN.txt, `js_save_extra` / `js_extra_parse`) holds only its
place lines. Away from the factory its places stay as want lines (the main
save writes only those while away), `js_step` skips the factory and
`js_step_away` pays by `js_rate_ph` (zero under a chore). `jar_go` in
jar.c switches; `jar_of` keeps a thing in one jar at a time.

**Jar Lab: the bench for prompts and behaviour** (2026-10-11,
`apps/jar/server/jarlab.py`, `apps/jar/server/jarlab.html`). At `/dash/jarlab` (under /dash
because the cookie's Path is /dash; `/jarlab` redirects), the dashboard's
cookie and an admin only. Pick kind, tier, price, form, brief (Sparks and
Roll use the code's own dice), see the exact `jar.prompt_for` text and
schema, edit it, and send it to Claude or a stub (dry run), or paste an
answer to examine. Each item is run through `check_item`, its script
compiled and given `simulate_day` (with the trace hook it gained for
this: every handler run, its actions and x), packed with
`to_item`/`encode` and decoded back: sprite drawn and animated, errors
listed, the day replayed in a strip of jar with a per-hour chart and an
event log. "Simulate only" iterates on a script with no Claude. Session
history diffs prompts; named experiments are JSON in
CARDOS_STATE/jarlab/. It never writes the pool, the ledger, rec/ID or
next_id, never signs, and never touches sign_key.pem; to preview the other
language version it flips `jarvm.V2` under a lock for the few ms of the
pure calls (never across a Claude call). `apps/jar/server/tests/test_jarlab.py`.

**The Town: four plants, three resources, three more people** (2026-10-11,
v0.18.0). The owner asked for fewer plant kinds, each making its own thing,
and neighbours for Tibbs who are paid in them:
- **Plants** (`apps/jar/jarsim.h`, `JPL_*`): berry (jam, coins, as before), reed
  (twigs), mushroom (spores), flower (nectar). Mosslings carry the non-berry
  produce to `J.res[]`, saved as `res S N T`; a bed's level 1-3 (`level`
  line) makes it grow 1x, 1.5x, 2x. Old saves' fern and cactus are reed.
  Only berry beds count in `js_rate_ph`.
- **People** (`apps/jar/server/shopkeep.py`, `CAST`): everything there takes `who`
  (tibbs default) -- one Claude Code session each, the same history,
  notebook and seeding as Tibbs's, in CARDOS_STATE/<who>. `town_note` tells
  the others what happened. Pip alters a thing (spores), Bramble evolves a
  critter or grows a bed (nectar), Wren builds furniture (twigs).
- **Deals** (`apps/jar/server/town.py`): the device sends `coins`, `have` and `beds`
  lines before the words (`jtx_have` in `apps/jar/jartx.h`); a `DEAL:` line is
  checked against them and becomes tx lines in the shared queue
  (`use<res>`, `gain<res>` on a failed job, `bedup K`, `remake ID`, `give
  ID`). Alteration is the old item and the request shown to Claude, made
  again under the same id and re-signed; Wren's is `jar.generate(...,
  force_form="furniture")`.
- **Confirmation and real time** (v0.19.0, the owner: "needs confirmation
  before any transaction", "upgrades and commissions should take real world
  time"). A `DEAL:` line is only a dry run (`make_deal(..., dry=True)`) kept
  as an offer worded by the code (`town.words`, `jar.deal_words`), shown as
  `offer	WORDS` on /jar/talk; `POST /jar/deal?who=` with the have lines
  and yes/no carries it out, re-checked. A job is made at once and held in
  `held/PERSON` (`town.hold`/`ready`/`release_due`) until its time; the tx
  is queued at hand-over so ids stay in order; `GET /jar/jobs` lists them.
  Shop buys and upgrades ask y/n; upgrades are built over real time
  (`js_buy_at`, `build K AT` in the save, `js_settle_build`).
  Resource beds ripen in `JS_RES_GROW_MS` (4 min at level 1).
- **On the device**: Jar Post's Town (`V_TOWN`, `post_open("town")`) lists
  the four and talks with `?who=`; `apps/jar/jartx.h` carries out tx lines for
  the talk screen and for the scene, which asks `GET /jar/tx?after=` after
  the patches and events. A `remake`/`give` stops the queue until the item
  (`GET /jar/item?id=`) is fetched and its signature checks; a remade item
  in the jar is put back where it stood and its old patch is deleted.

The rest:
- Talk replaces the old hint: Jar Post's Talk screen (`t` in the shop),
  `POST`/`GET /jar/talk`.
- Prices are chosen before an item is made (`TIERS`: common to treasure,
  30-900), kept in record byte 207 (in fives, signed), and a dear item gets
  more care and more effort.
- Plants steer nothing. Since v0.18.2 there are no seeds: the Garden plants
  any bed with any plant, free, and it grows in an hour (`JS_MATURE_S`).
  Different growth rates or looks per plant are a later idea, not now.
- The shelf is gone.
