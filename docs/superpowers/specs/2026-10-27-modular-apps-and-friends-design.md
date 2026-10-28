# Modular apps, and friends

2026-10-27. The owner's brief: apps keep adding bespoke code to scripts, the
server and the tools, and that has to stop before CardOS is released. Every
app's pieces should live with the app; apps worth server code (Chat, Jar)
may have it, as a plugin; and two or three people on one server should be
able to play and compare across apps -- "we chat, then maybe do words with
friends, then do Wordle and see how she did, then send her something in Jar
Factory" -- with no new server code per game.

**Anti-goal: social media.** No feed, no automatic posting, no reactions or
counts. A friend's data is shown inside the app it belongs to (Wordle's result
screen shows "her: 4/6" under yours). Anything that reaches her is something
you chose to send. Banners only for what needs you: an invite, your move, a
gift.

Scale: two devices, maybe three. SQLite on the one server. The wire formats
are the part meant to survive a bigger backend.

**Scope as built.** The platform below, and the restructure. No app uses
friends yet: the owner did not want the shared crossword built, and Wordle's
friends' results are left for when they ask. What an app would do is under
"First users (not built)".

## 1. Friends, public data, rooms, sharing (platform, `server/`)

### Friends -- `server/friends.py`

A friendship is mutual. Kept in the store (`srv/friends`, key `of/NAME`, one
`OTHER STATE` a line; STATE `friend`, `asked` -- I asked them, `asking` --
they asked me), both sides written under the store's lock. Without accounts
there are no friends.

    GET  /friends                 -> NAME <tab> DISPLAY <tab> STATE <tab> LAST_SEEN
    POST /friends/ask?who=NAME    -> "ok asked" or "ok friend" (they had asked)
                                     notifies NAME: app Friends
    POST /friends/drop?who=NAME   -> "ok"; ends a friendship or an ask, both sides

Asking and saying yes is the dashboard's Accounts page (a Friends panel,
`server/dashboard.html`), where a person types; any app may also call
`/friends/ask`.

### Public data -- `pub/APP`, a fourth kind of namespace

Each person writes only their own keys; they and their friends can read them.
Stored as `p/PERSON/APP`.

    GET /kv/get?ns=pub/APP&key=K[&of=PERSON]   one value: yours, or a friend's
    GET /kv/each?ns=pub/APP&key=K              yours and every friend's:
          PERSON <tab> DISPLAY <tab> SIZE \n  SIZE bytes \n   (yours first)

`of=` someone who is not a friend is 403.

### Rooms -- `server/rooms.py`

A room is one app's, has 2 to 8 members (its creator and friends of the
creator), a small shared state with a version, and a log of moves that only
grows. Live play and play hours apart are the same mechanism: read moves
after the last one seen. Tables `rooms` and `moves` in the store's database;
moves count against their sender's quota, the state against the creator's.

    POST /room/new?app=APP&with=A[,B][&title=T]  body: initial state -> "ok ID"
    GET  /room/list?app=APP     ID <tab> TITLE <tab> MEMBERS <tab> SEQ <tab> UPDATED <tab> CREATOR
    GET  /room/get?id=ID        "ID <tab> VERSION <tab> SEQ <tab> MEMBERS\n" + state
    POST /room/state?id=ID[&if=VERSION]   -> "ok VERSION"; 409 "error changed V"
    POST /room/move?id=ID[&say=TEXT]      -> "ok SEQ"; say= notifies the others
    GET  /room/moves?id=ID&after=SEQ[&max=N]
          SEQ <tab> FROM <tab> AT <tab> SIZE \n  SIZE bytes \n
    POST /room/leave?id=ID      the room goes with its last member

Limits: 16 KB a state or move, 20000 moves a room, 90 days idle. Membership is
checked, game rules never.

### Sharing -- `POST /share?app=APP[&name=CHATNAME]`

One line, posted into Chat as `[APP] line` (Chat's plugin owns the route).
Chat's own poll makes the banner.

### On the device -- `apps/social.h`

URL builders for all of the above (`soc_friends`, `soc_pub_put`,
`soc_pub_each`, `soc_share`, `soc_room_*`), parsers (`soc_friend_parse`,
`soc_block_next` for the replies that carry bytes), and a friend picker
drawn over the app (`soc_pick_open/tick/key/paint`). A header, not a kernel
dialog -- no firmware or API change. Host-tested in `test/test_social.c`;
compiles under the device's flags.

### First users (not built)

- Wordle: publish `pub/wordle stats` after a daily; show friends' results for
  the day on the result screen; `s` shares "Wordle #N 3/6".
- A turn game (words with friends): one room, a move per turn with `say=`.
- Jar Factory: its gifts through the picker. Jar has its own friends today
  (`/jar/me`, `/jar/friend`, `/jar/unfriend`, in apps/jar/server) -- they
  should become the platform's.

## 2. An app is one folder

Everything about an app lives beside its source (`tools/appmeta.py`):

    NAME.c           code; `@app` lines in a comment say what the build needs
    NAME.icon.txt    the colour icon, beside the source
    NAME.md / README.md   its notes (one-file app / folder app)
    test/test_*.c    host tests (folder apps), collected with test/
    tools/           generators and import scripts (folder apps)
    server/          a server plugin (folder apps)

`@app` lines: `folder` (required), `release core|extra`, `embed yes`,
`budget WHY`, `multi {JSON}` (dirs relative to the app), `routes /prefix ...`.
`apps/folders.txt`, `apps/icons/`, and build_apps.py's `EMBED`, `MULTI`,
`OVER_BUDGET` are gone; the app icons drawn inside `make_color_icons.py`
moved to files (only the system's stay). build_apps writes
`build/apps/folders.txt`, which `/update` and voice read (with the apps'
own lines as the fallback). `server/fence.py`: a fenced Build makes
`apps/NAME.c` (with its `@app folder`) and `apps/NAME.icon.txt`; nothing
shared needs a line.

Moved: every folder app's tests (and Jar's fixtures) into `apps/X/test/`;
`make_wordle`, `make_hangman`, `make_kart`, `make_emoji`, Jar's generators,
labs and sandbox, Crossword's import tools, Python's build scripts into
`apps/X/tools/`. Wiki and Hub became folders, for their plugins.

## 3. Server plugins -- `server/plugins.py`

`apps/NAME/server/` is imported as `apps.NAME.server` at start. A plugin may
define `ROUTES` (paths under `/NAME` or its `@app routes` prefixes; checked,
no two plugins on one path), `start()` (background work), `notify_poll(args,
user) -> (head, lines)` (its part of `/notify/poll`), and `TALK_KINDS`
(document kinds `/talk` revises, with the prompt and the check). Plugins
import platform modules by name (`from server import kv, ask, ...`); the
HTTP layer is not one of them. A plugin that fails to import is left out
with a line on stderr.

Moved: Crossword, Jar (with Jar Lab), Chat (the /msg half of msg.py, plus
/share), Wiki, MIDI (`TALK_KINDS` for songs), Hub (`/m5hub`). Each one's
server tests are `apps/X/server/tests/`. `python -m apps.jar.server seed N`,
`python -m apps.crossword.server import ...` replace the `server.` forms.
`wire.ascii_text` moved out of wiki (Crossword used it). Still in `server/`,
because the dashboard's pages are written into dashboard.html: Notes,
Photos, Music, Toggl, Google (Calendar/Todo) -- a later step gives plugins
dashboard pages.

## 4. Apps that outlive a firmware

An app built for API version V loads on firmware F when
`CAPP_API_MIN <= V <= F` (`kernel/app/elfload.h`, `capp_api_check`).
`CAPP_API_MIN` is 48: CappUi gained `save` then and capprun copies CappUi
whole; since 48 only CardApi has grown (49, 50), at its end. Rules in
capp.h: the table only grows; a new field in an app-provided struct raises
MIN or is guarded by the app's version; an app checks `api->version` before
calling an entry newer than MIN. Too new: "needs a newer CardOS"; too old:
"built for an older CardOS; rebuild it".

**The SDK**: `python tools/make_sdk.py OUT` writes capp.h, capprun.h,
capp.ld, the shared app headers, the host-test helpers and fonts, and
build_apps.py with appmeta and make_color_icons, plus `VERSION`.
`python OUT/tools/build_apps.py --apps REPO/apps` builds another repo's apps
(checked: a copy of Timer built outside the repo came out the same size).

## 5. Release

`build_apps.py --release` builds only `@app release core` (17 apps): Files,
Edit, Update, Dashboard Link, Notes, Todo, Calendar, Clock, Timer, Music,
Chat, Memo, Calc, Vim, Python, cat, grep -- my call. Everything else is
`extra`, ready to move to its own repo; moving them is not part of this.
CLAUDE.md keeps the platform; each app's paragraphs moved beside it.
