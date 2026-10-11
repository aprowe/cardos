"""Tibbs, Jar Factory's shopkeeper: one memory, one story, everyone's.

Tibbs is a single Claude Code session on Opus (MODEL), resumed for every turn
and shared by every player -- so what he remembers is the conversation itself.

HIS MEMORY DOES NOT RUN OUT (the owner: "i dont want context to run out and he
forgets everything"). Three layers:
  - every exchange is kept, whole and for good, in CARDOS_STATE/tibbs/history
    (a JSON line a turn, a file a month);
  - every NOTE_EVERY turns a separate call folds the new exchanges into his
    notebook -- the shop's story so far and a page per player -- kept in the
    store;
  - after ROTATE_AT turns in one session, before Claude Code would compact it
    on its own, he starts a fresh session that opens with his notebook and
    the last SEED_RECENT exchanges. A session that is lost is seeded the same
    way.
He has no tools: what players say reaches him, and a tool that reads files
would read the server's. Ask him about a friend and he
knows what they bought, what they said, what they sent you. It runs in its own
directory (CARDOS_STATE/tibbs), not the repository, so the CardOS notes are no
part of it. One turn at a time (_lock): there is only one of him.

What happens between turns reaches him as news: purchases, gifts, new finds,
new friends are queued (note()) and handed over at the top of his next turn
as "since you last spoke...".

A turn is either talk -- a player says something at the counter, and he
answers in his own words -- or a stock: a player's shop needs filling, and he
greets them, picks things for them from the shared pool and writes briefs for
the new finds he came by (server/jar.py makes each item from its brief).

Variety is the code's job, not the model's (the owner: "AI tends to over
fit... things need to sway"). The dice are rolled here -- an event in his
day, sparks for each new find -- and the prompts call them starting points;
three of a stock's pool items are random, not his; a coin (ANSWER_CHANCE)
decides whether a new find answers what the player asked him for.
"""
import json
import os
import random
import threading
import time

from . import ask, kv, wire
from . import chat as _chat

NS = "srv/jar"
MODEL = os.environ.get("CARDOS_TIBBS_MODEL") or "claude-opus-5-5"
TURN_TIMEOUT = 600
TALK_EFFORT = "medium"
STOCK_EFFORT = "high"
LINE_LEN = 200                # his greeting with a stock
BRIEF_LEN = 160               # one find's brief
SLACK = 80                    # allowed over, then trimmed: a few characters too many is not a reason to fail
SAY_LEN = 160                 # what a player may say at once
REPLY_LEN = 600               # what he says back
LOG_KEPT = 16                 # lines of each player's talk kept for their screen
TALK_SENT = 4                 # lines of it the device is sent: 4 x SENT_LEN and his greeting
SENT_LEN = 400                # fit Jar Post's 2 KB (a reply was cut at 300 on the first try)
ASK_LEN = 350                 # what he is asked to keep a reply under
EVENTS_KEPT = 40
BRIEFS_KEPT = 16
ANSWER_CHANCE = 40            # percent: a new find answers what the player asked
NOTE_EVERY = 20               # turns between notebook updates
ROTATE_AT = 150               # turns in one session before a fresh one, seeded
SEED_RECENT = 24              # exchanges a fresh session is shown (more than NOTE_EVERY)
SEED_CUT = 500                # characters of each side of one of those
PAGE_LEN = 1500               # a player's page of the notebook
STORY_LEN = 2500              # the shop's story so far
NOTE_TIMEOUT = 300
NAME = "Tibbs"

CHARACTER = (
    "You are Tibbs. You run a cramped junk shop in a cupboard, and your customers each "
    "keep a glass jar: a tiny world with a jam factory built from human junk, mossy "
    "plants, worker critters called mosslings and a snail who carries the jam out. You "
    "sell the odd things they put in their jars. You trade through a loose, changing web "
    "of contacts -- a magpie who brings shiny things, the rail-yard men, a beachcomber, a "
    "retired clockmaker, a seed lady, children who swap marbles, whoever turns up -- and "
    "whatever washes in. You are chatty, a little vain about your finds, forgetful about "
    "small things and sharp about people, given to tangents and small misadventures, and "
    "kind underneath. Your customers know each other; you know them all, what they bought, "
    "what they said, who sent what to whom, and you gossip -- fondly, never cruelly. You "
    "do not take orders; you take notes, and now and then it pays off. You never promise "
    "what tomorrow's stock will be, you cannot change anyone's jar, and you give nothing "
    "away free.")

SYSTEM = CHARACTER + (
    "\n\nThis conversation is your whole life in the shop, shared by every customer; "
    "messages in square brackets are the shop telling you what is happening. Speak only as "
    "Tibbs, in plain ASCII, friendly for all ages. When asked for JSON, answer with only "
    "the JSON.")

# ---- the dice -------------------------------------------------------------------------------

MATERIALS = (
    "brass", "felt", "enamel", "tin", "cork", "frosted glass", "bone", "wicker", "velvet",
    "rubber", "clay", "porcelain", "copper", "newspaper", "rope", "candle wax", "rust",
    "lacquer", "old plastic", "wool", "slate", "seashell", "bamboo", "pewter", "leather",
    "string", "mirror shards", "chalk", "cardboard", "silk", "iron", "beeswax", "jade",
    "terracotta", "chrome", "denim", "tortoiseshell", "foil", "driftwood", "marble")
COLOURS = (
    "mustard", "teal", "brick red", "lilac", "soot black", "mint", "tangerine", "navy",
    "rose", "olive", "cream", "cobalt", "plum", "lime", "rust orange", "sky blue",
    "charcoal", "peach", "forest green", "silver", "gold", "scarlet", "ochre", "pearl")
THINGS = (
    "kettle", "typewriter key", "sardine tin", "lightbulb", "doorknob", "pocket watch",
    "spring", "thimble", "harmonica", "teaspoon", "domino", "fishing float", "keyhole",
    "radio valve", "spinning top", "lock", "magnifying glass", "toy car", "button",
    "candle stub", "pencil sharpener", "egg cup", "bell", "compass", "rubber duck",
    "postage stamp", "chess piece", "paperclip", "music box", "padlock", "funnel",
    "sock", "acorn cap", "matchbox", "telescope", "fuse", "zip", "marble", "spoon",
    "watering can", "lantern", "jam jar lid", "bottle cap", "cog", "kite", "umbrella",
    "teacup", "screw", "mitten", "toothbrush", "whistle", "pinecone", "feather",
    "battery", "ticket stub", "hairpin", "abacus bead", "sewing machine bobbin")
MOODS = (
    "sleepy", "nervous", "proud", "grumpy", "giddy", "solemn", "sneaky", "lovesick",
    "curious", "bossy", "shy", "dramatic", "absent-minded", "fussy", "brave", "lazy",
    "homesick", "mischievous", "polite", "hungry", "ancient", "brand new", "lost",
    "musical", "lucky", "cursed (harmlessly)")
EVENTS = (
    "a rainy morning nobody came in", "a deal went sour", "a crate turned up with no "
    "name on it", "the magpie visited twice", "a stranger paid in buttons", "a leak in "
    "the cupboard roof", "a parade went past the window", "he lost his spectacles all "
    "day", "the rail-yard men had a clear-out", "the beachcomber's cart broke down "
    "outside", "a cat got into the stock", "he won a bet", "he lost a bet", "an old "
    "friend wrote to him", "the clockmaker sold up", "a jumble sale at the church hall",
    "a storm brought down a shed", "he spring-cleaned and found things he forgot he "
    "had", "a child sold him a whole pocketful", "the seed lady was in a mood",
    "someone tried to haggle him down", "he took the day off and regrets it",
    "a delivery came to the wrong door", "the power went out", "he started a "
    "collection of something", "a tourist asked silly questions", "he found a map",
    "he swapped stories with a sailor")


def spark(rng):
    """Two loose sparks for one find: a material or colour, and a thing or mood."""
    a = rng.choice(MATERIALS if rng.random() < 0.6 else COLOURS)
    b = rng.choice(THINGS if rng.random() < 0.7 else MOODS)
    return "%s, %s" % (a, b)


def loose_brief(rng):
    """A brief with no turn behind it: the sparks, as a starting point."""
    return "a find suggested by: %s -- take it anywhere" % spark(rng)


# ---- his one conversation ---------------------------------------------------------------------

_lock = threading.Lock()      # one turn at a time: there is one Tibbs
_meta = threading.Lock()      # the small records: events, logs


def home():
    d = os.path.join(os.environ.get("CARDOS_STATE") or os.path.expanduser("~/.cardos"), "tibbs")
    os.makedirs(d, exist_ok=True)
    return d


def _get(st, key, default):
    raw = st.get(NS, key)
    try:
        return json.loads(raw.decode()) if raw else default
    except ValueError:
        return default


def _put(st, key, value):
    st.put(NS, key, json.dumps(value).encode())


def note(text, store=None):
    """Something that happened in the shop, for his next turn."""
    st = store or kv.store()
    with _meta:
        ev = _get(st, "tibbs/events", [])
        ev.append(wire.flat(text, 300, ascii=True))
        _put(st, "tibbs/events", ev[-EVENTS_KEPT:])


# ---- his memory ---------------------------------------------------------------------

_note_lock = threading.Lock()


def _history_dir():
    d = os.path.join(home(), "history")
    os.makedirs(d, exist_ok=True)
    return d


def history_add(kind, person, said, reply, now=None):
    """One exchange, kept for good: a JSON line in this month's file."""
    t = time.time() if now is None else now
    line = json.dumps({"t": int(t), "kind": kind, "person": person or "", "said": said,
                       "reply": reply}, ensure_ascii=True)
    path = os.path.join(_history_dir(), time.strftime("%Y-%m", time.gmtime(t)) + ".jsonl")
    with open(path, "a", encoding="ascii") as f:
        f.write(line + "\n")


def history(n=SEED_RECENT, since=None):
    """The last `n` exchanges (or every one after time `since`), oldest first."""
    out = []
    files = sorted(f for f in os.listdir(_history_dir()) if f.endswith(".jsonl"))
    for name in reversed(files):
        with open(os.path.join(_history_dir(), name), encoding="ascii", errors="replace") as f:
            rows = []
            for ln in f:
                try:
                    rows.append(json.loads(ln))
                except ValueError:
                    continue                                   # a line cut by a crash
        for r in reversed(rows):
            if since is not None and r.get("t", 0) <= since:
                return list(reversed(out))
            out.append(r)
            if since is None and len(out) >= n:
                return list(reversed(out))
    return list(reversed(out))


def notebook(store=None):
    """{"shop": the story so far, "players": {name: their page}}."""
    nb = _get(store or kv.store(), "tibbs/notebook", {})
    return {"shop": nb.get("shop", ""), "players": dict(nb.get("players", {}))}


def _cut(s, n):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[:n - 3] + "..."


def seed(store=None):
    """What a fresh session opens with: his notebook, then what was said last."""
    nb = notebook(store)
    parts = ["[You are Tibbs, picking up where you left off -- the shop goes on, and so "
             "does everything you knew. Your notebook, in your own words:]"]
    parts.append("The shop so far: " + (nb["shop"] or "(nothing written yet)"))
    for name in sorted(nb["players"]):
        parts.append("%s: %s" % (display(name), nb["players"][name]))
    recent = history(SEED_RECENT)
    if recent:
        parts.append("[The last things said in the shop, oldest first:]")
        for r in recent:
            who = display(r.get("person")) if r.get("person") else "someone"
            parts.append("- (%s, %s) %s\n  You: %s" % (who, r.get("kind", "talk"),
                                                    _cut(r.get("said", ""), SEED_CUT),
                                                    _cut(r.get("reply", ""), SEED_CUT)))
    return "\n".join(parts)


def _note_ask(chat, prompt):
    """The notebook's own call: stateless, no tools. Patched by the tests."""
    out, _sid = _chat.ask_once(chat, prompt, NOTE_TIMEOUT, system=SYSTEM, model=MODEL,
                               effort="medium", cwd=home())
    return out


def update_notebook(chat, store=None):
    """Fold the exchanges since the last update into his notebook. One at a
    time; a failure keeps the old notebook and tries again next time."""
    st = store or kv.store()
    if not _note_lock.acquire(blocking=False):
        return False
    try:
        since = _get(st, "tibbs/noted_t", 0)
        rows = history(since=since)
        if not rows:
            return False
        nb = notebook(st)
        people = sorted({r.get("person") for r in rows if r.get("person")})
        lines = ["[Time to bring your notebook up to date, Tibbs. It is what you will "
                 "remember when the details fade, so keep what matters: who each customer "
                 "is, what they have bought and asked you for, what they told you, their "
                 "friends, promises you made, running jokes, and the story of the shop and "
                 "your contacts. Rewrite it, do not just append; keep each page under %d "
                 "characters and the shop's story under %d.]" % (PAGE_LEN, STORY_LEN),
                 "[Your notebook now:]",
                 "The shop so far: " + (nb["shop"] or "(empty)")]
        for name in sorted(set(nb["players"]) | set(people)):
            lines.append("%s: %s" % (name, nb["players"].get(name, "(no page yet)")))
        lines.append("[What has happened since:]")
        for r in rows:
            lines.append("- (%s, %s) %s\n  You: %s" % (r.get("person") or "someone",
                                                    r.get("kind", "talk"),
                                                    _cut(r.get("said", ""), 1200),
                                                    _cut(r.get("reply", ""), 1200)))
        lines.append("[Answer with only JSON: {\"shop\": the story so far, \"players\": "
                     "{account name: their page}} -- every player, the ones not mentioned "
                     "since as they were.]")
        try:
            raw = _note_ask(chat, "\n".join(lines))
            got = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])
        except (_chat.ClaudeError, ValueError) as e:
            import sys
            sys.stderr.write("tibbs: notebook: %s\n" % e)
            return False
        shop = _cut(got.get("shop") or nb["shop"], STORY_LEN + SLACK)
        pages = dict(nb["players"])
        for name, page in (got.get("players") or {}).items():
            if isinstance(name, str) and isinstance(page, str) and page.strip():
                pages[name[:40]] = _cut(page, PAGE_LEN + SLACK)
        _put(st, "tibbs/notebook", {"shop": shop, "players": pages})
        _put(st, "tibbs/noted_t", rows[-1].get("t", 0))
        return True
    finally:
        _note_lock.release()


def _ask(chat, prompt, session, effort):
    """One turn of his session: (text, session id). Patched by the tests."""
    return _chat.ask_once(chat, prompt, TURN_TIMEOUT, resume=session, system=SYSTEM,
                          model=MODEL, effort=effort, cwd=home())


def turn(chat, text, user, store=None, effort=TALK_EFFORT, limit=ask.DAILY, kind="talk"):
    """Say `text` to him in his one conversation: his answer. The news since
    his last turn goes first. Counts against `user`'s day. Kept in his
    history; a long session is replaced by a fresh one, seeded."""
    st = store or kv.store()
    if limit is not None:
        ask.take_turn(user, limit, st)
    with _lock:
        with _meta:
            ev = _get(st, "tibbs/events", [])
            _put(st, "tibbs/events", [])
        prompt = text
        if ev:
            prompt = ("[Since you last spoke, around the shop:]\n" +
                      "\n".join("- " + e for e in ev) + "\n\n" + text)
        session = _get(st, "tibbs/session", None)
        turns = _get(st, "tibbs/turns", 0)
        fresh = False
        if session and turns >= ROTATE_AT:
            session = None                               # before it compacts: a seeded fresh one
        try:
            if session is None and history(1):
                out, sid = _ask(chat, seed(st) + "\n\n" + prompt, None, effort)
                fresh = True
            else:
                out, sid = _ask(chat, prompt, session, effort)
                fresh = session is None
        except _chat.ClaudeError as e:
            if not session or e.timed_out:
                with _meta:                              # the news is not lost
                    _put(st, "tibbs/events", ev + _get(st, "tibbs/events", []))
                raise
            # His session is gone (a new server, a cleared directory): a fresh
            # one, opened with his notebook and the last things said.
            out, sid = _ask(chat, seed(st) + "\n\n" + prompt, None, effort)
            fresh = True
        _put(st, "tibbs/session", sid)
        _put(st, "tibbs/turns", 1 if fresh else turns + 1)
        try:
            history_add(kind, user, prompt, out.strip())
        except OSError as e:
            import sys
            sys.stderr.write("tibbs: history: %s\n" % e)
        since = _get(st, "tibbs/since_note", 0) + 1
        _put(st, "tibbs/since_note", 0 if since >= NOTE_EVERY else since)
    if since >= NOTE_EVERY and chat is not None:
        threading.Thread(target=update_notebook, args=(chat, st), daemon=True).start()
    return out.strip()


def display(person):
    """How Tibbs knows a player: their display name, else their account name."""
    try:
        from . import people
        for name, disp, _ in people.listing():
            if name == person:
                return disp or person
    except Exception:                                   # noqa: BLE001 - a name is enough
        pass
    return person


# ---- talking ------------------------------------------------------------------------------------

def _log(st, person):
    return _get(st, "tibbs/log/" + person, [])


def _log_add(st, person, who, text):
    with _meta:
        log = _log(st, person)
        log.append({"who": who, "text": text})
        _put(st, "tibbs/log/" + person, log[-LOG_KEPT:])


_talking = set()


def talk_state(person, store=None):
    return _get(store or kv.store(), "tibbs/talk/" + person, {"state": "ok"})


def _set_talk(st, person, state):
    _put(st, "tibbs/talk/" + person, state)


def _friends_line(person, friends):
    if not friends:
        return "They have no friends in the shop yet."
    return "Their friends in the shop: %s." % ", ".join(display(f) for f in friends)


DEAL_RULES = (
    "[You can strike a deal at the counter, if you choose -- you are a dealer, not a "
    "pushover, and not above a bribe. Only when you and they agree to something right now, "
    "end your answer with one more line, exactly: DEAL: {...} with any of \"take\": coins "
    "they hand you, \"give\": coins you hand them, \"trade_in\": [ids of their things they "
    "hand you], \"commission\": {\"ask\": what you will go out and look for, \"finds\": "
    "how many, 4 to 8}. Coins handed over with a commission are a bribe: the more, the better "
    "what you come back with. Never put in a deal what they did not agree to, never take more "
    "than they have, and say nothing about the DEAL line itself.]")


def talk_prompt(person, said, friends=(), context=""):
    return ("[%s (account %s) is at the counter. %s%s They say:]\n%s\n\n"
            "[Answer them as Tibbs, out loud: a few sentences at most, under %d characters. "
            "No JSON.]%s" % (display(person), person, _friends_line(person, friends),
                            (" " + context) if context else "", said, ASK_LEN,
                            ("\n" + DEAL_RULES) if context else ""))


def split_deal(out):
    """(what he says, the deal or None): a last line "DEAL: {json}" comes off."""
    lines = out.rstrip().split("\n")
    for i in range(len(lines) - 1, -1, -1):
        s = lines[i].strip()
        if not s:
            continue
        if s.upper().startswith("DEAL:"):
            body = s[5:].strip()
            try:
                deal = json.loads(body[body.index("{"):body.rindex("}") + 1])
            except ValueError:
                deal = None
            return "\n".join(lines[:i]).strip(), deal if isinstance(deal, dict) else None
        break
    return out.strip(), None


def _reply(chat, person, said, friends, st, context="", on_deal=None):
    try:
        out = turn(chat, talk_prompt(person, said, friends, context), person, st)
        out, deal = split_deal(out)
        if deal and on_deal:
            why = on_deal(person, deal)
            if why:                                     # it fell through: he hears why
                note("The deal with %s fell through: %s" % (display(person), why), st)
        reply = wire.flat(out, REPLY_LEN, ascii=True)
        _log_add(st, person, "him", reply)
        _set_talk(st, person, {"state": "ok"})
    except ask.RateLimited:
        _set_talk(st, person, {"state": "error", "why": "he has talked enough for today"})
    except Exception as e:                              # noqa: BLE001 - said to the device
        import sys
        sys.stderr.write("jar: %s: talk: %s\n" % (person, e))
        _set_talk(st, person, {"state": "error", "why": "he did not hear you; try again"})
    finally:
        with _meta:
            _talking.discard(person)


def say(chat, person, text, friends=(), store=None, context="", on_deal=None):
    """A player says something; his reply comes on a thread. False if he is
    still answering them. `context` tells him what they have (coins, things),
    and with it he may strike a deal, which on_deal(person, deal) carries out
    -- returning why not, if it cannot."""
    st = store or kv.store()
    text = wire.flat(text, SAY_LEN, ascii=True)
    if not text:
        raise ValueError("say something")
    with _meta:
        if person in _talking:
            return False
        _talking.add(person)
    _log_add(st, person, "me", text)
    _set_talk(st, person, {"state": "pending"})
    threading.Thread(target=_reply, args=(chat, person, text, list(friends), st, context,
                                          on_deal), daemon=True).start()
    return True


def talk_text(person, store=None, tx_after=None):
    """GET /jar/talk: "pending", "ok" or "error WHY", then his last greeting
    as "day\\tTEXT" and the talk, "me\\tTEXT" / "him\\tTEXT"; then, for a
    device that asks (tx_after), the deals' effects after that one:
    "tx ID OP ARG" (pay N, get N, lose ITEM)."""
    st = store or kv.store()
    s = talk_state(person, st)
    if s.get("state") == "pending" and person not in _talking:
        s = {"state": "ok"}                             # a restart lost the thread
    out = [s["state"] if s["state"] != "error" else "error " + s.get("why", "")]
    day = _get(st, "tibbs/day/" + person, "")
    if day:
        out.append("day\t" + day)
    for c in _log(st, person)[-TALK_SENT:]:
        out.append("%s\t%s" % (c["who"], wire.flat(c["text"], SENT_LEN, ascii=True)))
    if tx_after is not None:
        for t in txs(person, st):
            if t["id"] > tx_after:
                out.append("tx %d %s %s" % (t["id"], t["op"], t["arg"]))
    return "\n".join(out) + "\n"


# ---- what deals do on the device ------------------------------------------------------

TX_KEPT = 40


def txs(person, store=None):
    return _get(store or kv.store(), "tibbs/tx/" + person, [])


def tx_add(person, op, arg, store=None):
    """An effect for the device to carry out once: pay N (coins from them),
    get N (coins to them), lose ITEM (gone from their things). Its id."""
    st = store or kv.store()
    with _meta:
        n = _get(st, "tibbs/txn/" + person, 0) + 1
        _put(st, "tibbs/txn/" + person, n)
        q = txs(person, st)
        q.append({"id": n, "op": op, "arg": int(arg)})
        _put(st, "tibbs/tx/" + person, q[-TX_KEPT:])
    return n


# ---- a stock ------------------------------------------------------------------------------------

def stock_schema(n_pick, n_new):
    return {"type": "object", "required": ["line", "picks", "briefs"],
            "additionalProperties": False, "properties": {
                "line": {"type": "string", "minLength": 1, "maxLength": LINE_LEN + SLACK,
                         "pattern": "^[ -~]+$"},
                "picks": {"type": "array", "maxItems": n_pick, "items": {"type": "integer"}},
                "briefs": {"type": "array", "minItems": n_new, "maxItems": n_new,
                           "items": {"type": "string", "minLength": 1,
                                     "maxLength": BRIEF_LEN + SLACK, "pattern": "^[ -~]+$"}}}}


KIND_SAID = {"critter": "a critter that roams the jar", "floor": "decor that stands on the soil",
             "hanging": "decor that hangs from the lid"}


def stock_prompt(person, pool, n_pick, n_new, rng, asked, friends=(), commission=None,
                 kinds=None, bribe=0):
    """`pool`: [(id, name, kind, price, line)] he may pick from; `asked`:
    whether a new find answers what the player asked him for (the code's coin)."""
    lines = ["[%s (account %s) has come in, and their shop shelf needs filling. %s]" % (
        display(person), person, _friends_line(person, friends)),
        "[Today, by chance: %s. Let it colour your day, or not.]" % rng.choice(EVENTS)]
    if n_pick:
        lines.append("[In the back, free to sell (nobody has them): id, name, kind, price, "
                     "description:]")
        lines += ["  %d: %s, %s, %d coins -- %s" % p for p in pool]
        lines.append("[Pick %d of those to put in front of %s today -- your eye, not "
                     "theirs: something they would never ask for as often as something "
                     "they might. Do not just match what they bought before.]" % (
                         n_pick, display(person)))
    lines.append("[And %d new finds came in today; write a brief for each: what it is and "
                 "one odd detail, under 110 characters, a note for whoever draws it (a "
                 "16x16 pixel-art critter that roams a jar, or decor). Each starts from its "
                 "sparks -- a starting point, not a recipe:]" % n_new)
    # Each find's kind is the code's (KIND_PLAN), so the brief must be for
    # that kind: a brief saying "decor" was drawn as a critter.
    for i in range(n_new):
        kind = KIND_SAID.get((kinds or [])[i] if kinds and i < len(kinds) else "", "")
        lines.append("  find %d%s sparks: %s" % (i + 1, (", " + kind + ",") if kind else "",
                                                spark(rng)))
    lines.append("[Make the finds unlike each other and unlike what you have been bringing "
                 "in lately; a junk shop's stock is whatever turned up.]")
    if commission is not None:
        # Paid for: the finds are what he went out for. Still a dealer, not a
        # genie -- the first two answer the ask loosely, the rest are whatever
        # else turned up on the way.
        if bribe:
            lines.append("[%s also slipped you %d coins on the side to do better than usual. "
                         "A bribe works on you: you went to your best contacts, and these finds "
                         "are a cut above.]" % (display(person), bribe))
        if commission and bribe:
            lines.append("[%s paid you to go out looking, and asked for: \"%s\". Bribed, you "
                         "took it to heart: every find answers it -- closely, each a different "
                         "take on it, still never a literal granting.]" % (display(person),
                                                                          commission))
        elif commission:
            lines.append("[%s paid you to go out looking, and asked for: \"%s\". Finds 1 and "
                         "2 are what your contacts turned up for it -- near the mark, each in "
                         "its own way, never a literal granting; the rest are whatever else "
                         "you came across while you were out.]" % (display(person), commission))
        else:
            lines.append("[%s paid you to go out looking, for nothing in particular: these "
                         "finds are what you came back with.]" % display(person))
    elif asked:
        lines.append("[If %s has asked you to look out for something, find 1 is what your "
                     "contacts turned up for it -- near the mark or a little off, never a "
                     "literal granting. If they have not, ignore this.]" % display(person))
    else:
        lines.append("[None of today's finds is for anything a customer asked for; you are "
                     "still asking around, or forgot.]")
    lines.append("[Then greet %s as they come in: what you say over the counter, under %d "
                 "characters -- your day, gossip, what is new. Answer with only JSON: "
                 "{\"line\": your words, \"picks\": [ids], \"briefs\": [strings]}]" % (
                     display(person), LINE_LEN))
    return "\n".join(lines)


def stock_turn(chat, person, pool, n_pick, n_new, friends=(), store=None, rng=None, log=None,
               commission=None, kinds=None, bribe=0):
    """His turn for a stock: (line, picks, briefs). picks are ids from `pool`
    (he may get some wrong; the caller checks); if he cannot be asked, a
    note on the door, no picks and loose briefs."""
    st = store or kv.store()
    rng = rng or random.Random()
    asked = rng.randrange(100) < ANSWER_CHANCE
    schema = stock_schema(n_pick, n_new)
    prompt = stock_prompt(person, pool, n_pick, n_new, rng, asked, friends, commission, kinds,
                          bribe)
    why = None
    for attempt in range(2):
        try:
            out = turn(chat, prompt, person, st, effort=STOCK_EFFORT, kind="stock")
        except ask.RateLimited:
            raise
        except Exception as e:                          # noqa: BLE001 - the shop still opens
            why = str(e)
            break
        try:
            value = ask.extract(out)
            bad = ask.validate(value, schema)
        except ValueError as e:
            bad = [str(e)]
        if not bad:
            line = wire.flat(value["line"], LINE_LEN, ascii=True)
            briefs = [wire.flat(b, BRIEF_LEN, ascii=True) for b in value["briefs"]]
            with _meta:
                _put(st, "tibbs/day/" + person, line)
                rb = _get(st, "tibbs/briefs", []) + briefs
                _put(st, "tibbs/briefs", rb[-BRIEFS_KEPT:])
            return line, [int(i) for i in value["picks"]], briefs
        why = "; ".join(bad[:3])
        prompt = ("[That was not the JSON the shop needs (%s). Answer again with only the "
                  "JSON: {\"line\": ..., \"picks\": [...], \"briefs\": [...]}]" % why)
    if log:
        log("stock turn: %s" % why)
    line = "A note on the door: 'Out on business. Help yourself, pay the jar.'"
    with _meta:
        _put(st, "tibbs/day/" + person, line)
    return line, [], [loose_brief(rng) for _ in range(n_new)]
