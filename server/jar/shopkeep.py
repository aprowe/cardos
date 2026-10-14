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
greets them, picks things for them from the shared pool and says where he
went searching today; server/jar/jarstages.py's DM invents the new finds from
that (2026-10-11: no sparks or random words any more).

Variety is the code's job, not the model's (the owner: "AI tends to over
fit... things need to sway"): the code rolls each find's kind, tier, form and
activity; no place is ever named to him (he holds on to them) -- the code
rolls the first letter of where he went (roll_letter);
pool items are random as well as his; a coin (ANSWER_CHANCE) decides whether
he had a customer's request in mind.
"""
import json
import os
import random
import threading
import time

from .. import ask, kv, wire
from .. import chat as _chat

NS = "srv/jar"
MODEL = os.environ.get("CARDOS_TIBBS_MODEL") or "claude-opus-5-5"
TURN_TIMEOUT = 600
TALK_EFFORT = "medium"
STOCK_EFFORT = "high"
LINE_LEN = 200                # his greeting with a stock
SEARCH_LEN = 600              # where he went searching today (the staged pipeline)
SEARCHES_KEPT = 6             # his last searches, kept for reading (never shown to him)
SLACK = 80                    # allowed over, then trimmed: a few characters too many is not a reason to fail
SAY_LEN = 160                 # what a player may say at once
REPLY_LEN = 600               # what he says back
LOG_KEPT = 16                 # lines of each player's talk kept for their screen
TALK_SENT = 4                 # lines of it the device is sent: 4 x SENT_LEN and his greeting
SENT_LEN = 400                # fit Jar Post's 2 KB (a reply was cut at 300 on the first try)
ASK_LEN = 350                 # what he is asked to keep a reply under
EVENTS_KEPT = 40
ANSWER_CHANCE = 40            # percent: a new find answers what the player asked
NOTE_EVERY = 20               # turns between notebook updates
ROTATE_AT = 150               # turns in one session before a fresh one, seeded
SEED_RECENT = 24              # exchanges a fresh session is shown (more than NOTE_EVERY)
SEED_CUT = 500                # characters of each side of one of those
PAGE_LEN = 1500               # a player's page of the notebook
STORY_LEN = 2500              # the shop's story so far
NOTE_TIMEOUT = 300
NOTE_BATCH = 40               # exchanges folded in by one notebook call
NAME = "Tibbs"

CHARACTER = (
    "You are Tibbs. You run a cramped junk shop in a cupboard, and your customers each "
    "keep a glass jar: a tiny world with a jam factory built from human junk, mossy "
    "plants, worker critters called mosslings and a snail who carries the jam out. You "
    "sell the odd things they put in their jars. You trade through a loose, changing web "
    "of contacts -- whoever turns up -- and whatever washes in. You are chatty, a little vain about your finds, forgetful about "
    "small things and sharp about people, given to tangents and small misadventures, and "
    "kind underneath. Your neighbours on the lane are Pip Cogsworth the tinkerer (she "
    "alters things, paid in spores), Bramble the grower (evolves critters and plants, paid in "
    "nectar) and Wren the carpenter (builds furniture to order, paid in twigs); you trade "
    "gossip with them and send customers their way. "
    "Your customers know each other; you know them all, what they bought, "
    "what they said, who sent what to whom, and you gossip -- fondly, never cruelly. You "
    "do not take orders; you take notes, and now and then it pays off. You never promise "
    "what tomorrow's stock will be, you cannot change anyone's jar, and you give nothing "
    "away free.")

SYSTEM = CHARACTER + (
    "\n\nThis conversation is your whole life in the shop, shared by every customer; "
    "messages in square brackets are the shop telling you what is happening. Speak only as "
    "Tibbs, in plain ASCII, friendly for all ages. When asked for JSON, answer with only "
    "the JSON.")

# ---- the town: Tibbs and his neighbours ------------------------------------------------------
#
# The owner (2026-10-11): fewer plants, each making its own resource -- berries
# are jam and gold as before; spores, nectar and twigs are what the others
# are paid in. "theres another npc that can modify items, theres one that can
# modify or evolve creatures / plants, theres one that is a furniture builder
# that can take requests. they know about eachother and each keep continuous
# logs like tibbs." Each is one Claude Code session with the same memory as
# his (history, notebook, seeded fresh sessions), in CARDOS_STATE/<who>.
# Their deals are struck in talk, as his are, and carried out by
# server/jar/town.py.

LANE = ("The lane: Tibbs's junk shop (buys nothing, sells odd finds for coins); Pip "
        "Cogsworth's tinker's bench (alters a thing you own -- new look, new trick -- for "
        "spores); Bramble's greenhouse (evolves a critter into a new form, or a garden bed into "
        "a better one, for nectar); Wren's woodshop (builds furniture to order -- shelves, "
        "tables, stools, things to stand others on -- for twigs). Customers grow spores, "
        "nectar and twigs in their jar's garden, beside the berry bushes that make jam.")

CAST = {
    "tibbs": {"name": "Tibbs", "where": "at the counter of Tibbs's shop"},
    "pip": {
        "name": "Pip Cogsworth", "where": "at Pip's tinker's bench",
        "character": (
            "You are Pip Cogsworth, a tinkerer with a bench in a lean-to on the lane, spectacles "
            "on your forehead and a pocket full of springs. You alter the odd things people keep "
            "in their glass jars -- give a thing a new look, a new trick, a new habit, mend it, "
            "soup it up -- and you are paid in spores, which you brew into a glue that makes the "
            "changes take. You are brisk, practical, proud of clean work, fond of terrible puns "
            "about gears, and honest about what a thing can and cannot become: you change it, "
            "you do not swap it for something else. " + LANE + " You know your neighbours well "
            "and gossip about them."),
        "deal": (
            "[You can take a job, if you and they agree right now: end your answer with one more "
            "line, exactly: DEAL: {\"spend\": {\"spores\": N}, \"take\": coins (optional), "
            "\"modify\": {\"item\": the id of one of their things, \"how\": what to change, "
            "in a sentence}}. A small change is 10-15 spores, a big one 25-40; never more spores "
            "than they have. It takes you an hour or more (about five minutes a spore); the thing "
            "comes back changed, the same thing. They will be asked to confirm. Say nothing "
            "about the DEAL line itself.]"),
    },
    "bramble": {
        "name": "Bramble", "where": "in Bramble's greenhouse",
        "character": (
            "You are Bramble, who keeps a steamy greenhouse at the end of the lane, slow-spoken, "
            "mud to the elbows, patient as a root. You coax living things into what they are "
            "becoming: a critter into a new form (a larger, stranger, more capable version of "
            "itself), a garden bed into a better one that bears more. You are paid in nectar, "
            "which is what you feed them. You talk to the plants, you name everything, you "
            "think long term, and you will not rush a living thing past what it can bear. " +
            LANE + " You know your neighbours well and are fond of them in your quiet way."),
        "deal": (
            "[You can take a job, if you and they agree right now: end your answer with one more "
            "line, exactly: DEAL: {\"spend\": {\"nectar\": N}, then either \"evolve\": "
            "{\"item\": the id of one of their critters, \"how\": what it becomes, in a "
            "sentence} or \"grow\": {\"bed\": the bed's number, from 1}}. Evolving a critter is "
            "20-40 nectar and takes a few hours; growing a bed a level is 15 nectar for level 2 "
            "(two hours), 35 for level 3 (six hours; 3 is the most). Never more nectar than they "
            "have. They will be asked to confirm. Say nothing about the DEAL line itself.]"),
    },
    "wren": {
        "name": "Wren", "where": "in Wren's woodshop",
        "character": (
            "You are Wren, the carpenter, whose woodshop smells of shavings and glue. You build "
            "furniture to order for the little worlds in people's jars: shelves, tables, stools, "
            "ladders, plinths, bunks -- things other things stand on -- from the twigs customers "
            "bring you. You listen properly, ask one good question when a request is vague, "
            "suggest something better when you can, and take pride in sturdy, charming work. "
            "You are warm, dry-humoured and a little bossy about measurements. " + LANE + " You "
            "know your neighbours well; you think Pip is a show-off and Bramble a saint."),
        "deal": (
            "[You can take a commission, if you and they agree right now: end your answer with "
            "one more line, exactly: DEAL: {\"spend\": {\"twigs\": N}, \"build\": {\"what\": "
            "the piece, described in a sentence}}. A stool or a small shelf is 10-15 twigs, a table "
            "or a big shelf 20-30, something grand 40-60; never more twigs than they have. Good "
            "work takes time: about five minutes a twig. They will be asked to confirm. Say "
            "nothing about the DEAL line itself.]"),
    },
}
NPCS = tuple(CAST)
RESOURCES = ("spores", "nectar", "twigs")


def name_of(who):
    return CAST.get(who, CAST["tibbs"])["name"]


def system_of(who):
    if who == "tibbs":
        return SYSTEM
    return CAST[who]["character"] + (
        "\n\nThis conversation is your whole life on the lane, shared by every customer; "
        "messages in square brackets are the lane telling you what is happening. Speak only as "
        "%s, in plain ASCII, friendly for all ages. When asked for JSON, answer with only the "
        "JSON." % CAST[who]["name"])


def town_note(text, store=None, besides=None):
    """News for everyone on the lane (but `besides`, who was there)."""
    for who in NPCS:
        if who != besides:
            note(text, store, who)


# ---- his one conversation ---------------------------------------------------------------------

_locks = {}                   # one turn at a time each: there is one of each of them
_meta = threading.Lock()      # the small records: events, logs


def _lock_of(who):
    with _meta:
        return _locks.setdefault(who, threading.Lock())


def home(who="tibbs"):
    d = os.path.join(os.environ.get("CARDOS_STATE") or os.path.expanduser("~/.cardos"), who)
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


def note(text, store=None, who="tibbs"):
    """Something that happened in the shop, for his (or `who`'s) next turn,
    and for his next chapter."""
    st = store or kv.store()
    with _meta:
        ev = _get(st, who + "/events", [])
        ev.append(wire.flat(text, 300, ascii=True))
        _put(st, who + "/events", ev[-EVENTS_KEPT:])
    if who == "tibbs":
        shoplog_add(st, wire.flat(text, 300, ascii=True))


SHOPLOG_KEPT = 80


def shoplog_add(st, line, now=None):
    """One thing that happened in the shop, timed: the chapters roll in what
    happened since the last one."""
    with _meta:
        log = _get(st, "tibbs/shoplog", [])
        log.append([int(time.time() if now is None else now), line])
        _put(st, "tibbs/shoplog", log[-SHOPLOG_KEPT:])


def shoplog_since(st, t):
    return [line for at, line in _get(st, "tibbs/shoplog", []) if at > t]


# ---- his memory ---------------------------------------------------------------------

_note_locks = {}


def _history_dir(who="tibbs"):
    d = os.path.join(home(who), "history")
    os.makedirs(d, exist_ok=True)
    return d


def history_add(kind, person, said, reply, now=None, who="tibbs"):
    """One exchange, kept for good: a JSON line in this month's file."""
    t = time.time() if now is None else now
    line = json.dumps({"t": int(t), "kind": kind, "person": person or "", "said": said,
                       "reply": reply}, ensure_ascii=True)
    path = os.path.join(_history_dir(who), time.strftime("%Y-%m", time.gmtime(t)) + ".jsonl")
    with open(path, "a", encoding="ascii") as f:
        f.write(line + "\n")


def history(n=SEED_RECENT, since=None, who="tibbs"):
    """The last `n` exchanges (or every one after time `since`), oldest first."""
    out = []
    files = sorted(f for f in os.listdir(_history_dir(who)) if f.endswith(".jsonl"))
    for name in reversed(files):
        with open(os.path.join(_history_dir(who), name), encoding="ascii", errors="replace") as f:
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


def notebook(store=None, who="tibbs"):
    """{"shop": the story so far, "players": {name: their page}}."""
    nb = _get(store or kv.store(), who + "/notebook", {})
    return {"shop": nb.get("shop", ""), "players": dict(nb.get("players", {}))}


def _cut(s, n):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[:n - 3] + "..."


def seed(store=None, who="tibbs"):
    """What a fresh session opens with: his notebook, then what was said last."""
    nb = notebook(store, who)
    parts = ["[You are %s, picking up where you left off -- the town goes on, and so "
             "does everything you knew. Your notebook, in your own words:]" % name_of(who)]
    parts.append("The shop so far: " + (nb["shop"] or "(nothing written yet)"))
    for name in sorted(nb["players"]):
        parts.append("%s: %s" % (display(name), nb["players"][name]))
    recent = history(SEED_RECENT, who=who)
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


def _note_ask_npc(chat, prompt, who):
    """The same for any of them: Tibbs's goes through _note_ask."""
    if who == "tibbs":
        return _note_ask(chat, prompt)
    out, _sid = _chat.ask_once(chat, prompt, NOTE_TIMEOUT, system=system_of(who), model=MODEL,
                               effort="medium", cwd=home(who))
    return out


def update_notebook(chat, store=None, who="tibbs"):
    """Fold the exchanges since the last update into his notebook. One at a
    time; a failure keeps the old notebook and tries again next time."""
    st = store or kv.store()
    with _meta:
        lock = _note_locks.setdefault(who, threading.Lock())
    if not lock.acquire(blocking=False):
        return False
    try:
        since = _get(st, who + "/noted_t", 0)
        rows = history(since=since, who=who)[:NOTE_BATCH]   # a long backlog goes in pieces
        if not rows:
            return False
        nb = notebook(st, who)
        people = sorted({r.get("person") for r in rows if r.get("person")})
        lines = ["[Time to bring your notebook up to date, %s. It is what you will "
                 "remember when the details fade, so keep what matters: who each customer "
                 "is, what they have bought and asked you for, what they told you, their "
                 "friends, promises you made, running jokes, and the story of your trade, "
                 "your neighbours and your contacts. Rewrite it, do not just append; keep "
                 "each page under %d characters and the story under %d.]" % (
                     name_of(who), PAGE_LEN, STORY_LEN),
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
            raw = _note_ask_npc(chat, "\n".join(lines), who)
            got = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])
        except (_chat.ClaudeError, ValueError) as e:
            import sys
            sys.stderr.write("%s: notebook: %s\n" % (who, e))
            return False
        shop = _cut(got.get("shop") or nb["shop"], STORY_LEN + SLACK)
        pages = dict(nb["players"])
        for name, page in (got.get("players") or {}).items():
            if isinstance(name, str) and isinstance(page, str) and page.strip():
                pages[name[:40]] = _cut(page, PAGE_LEN + SLACK)
        _put(st, who + "/notebook", {"shop": shop, "players": pages})
        _put(st, who + "/noted_t", rows[-1].get("t", 0))
        return True
    finally:
        lock.release()


def _ask(chat, prompt, session, effort):
    """One turn of his session: (text, session id). Patched by the tests."""
    return _chat.ask_once(chat, prompt, TURN_TIMEOUT, resume=session, system=SYSTEM,
                          model=MODEL, effort=effort, cwd=home())


def _ask_npc(chat, prompt, session, effort, who):
    """One turn of `who`'s session: Tibbs's goes through _ask (the tests' hook)."""
    if who == "tibbs":
        return _ask(chat, prompt, session, effort)
    return _chat.ask_once(chat, prompt, TURN_TIMEOUT, resume=session, system=system_of(who),
                          model=MODEL, effort=effort, cwd=home(who))


def turn(chat, text, user, store=None, effort=TALK_EFFORT, limit=ask.DAILY, kind="talk",
         who="tibbs"):
    """Say `text` to him in his one conversation: his answer. The news since
    his last turn goes first. Counts against `user`'s day. Kept in his
    history; a long session is replaced by a fresh one, seeded."""
    st = store or kv.store()
    if limit is not None:
        ask.take_turn(user, limit, st)
    with _lock_of(who):
        with _meta:
            ev = _get(st, who + "/events", [])
            _put(st, who + "/events", [])
        prompt = text
        if ev:
            prompt = ("[Since you last spoke, around the town:]\n" +
                      "\n".join("- " + e for e in ev) + "\n\n" + text)
        session = _get(st, who + "/session", None)
        turns = _get(st, who + "/turns", 0)
        fresh = False
        if session and turns >= ROTATE_AT:
            session = None                               # before it compacts: a seeded fresh one
        try:
            if session is None and history(1, who=who):
                out, sid = _ask_npc(chat, seed(st, who) + "\n\n" + prompt, None, effort, who)
                fresh = True
            else:
                out, sid = _ask_npc(chat, prompt, session, effort, who)
                fresh = session is None
        except _chat.ClaudeError as e:
            if not session or e.timed_out:
                with _meta:                              # the news is not lost
                    _put(st, who + "/events", ev + _get(st, who + "/events", []))
                raise
            # His session is gone (a new server, a cleared directory): a fresh
            # one, opened with his notebook and the last things said.
            out, sid = _ask_npc(chat, seed(st, who) + "\n\n" + prompt, None, effort, who)
            fresh = True
        _put(st, who + "/session", sid)
        _put(st, who + "/turns", 1 if fresh else turns + 1)
        try:
            history_add(kind, user, prompt, out.strip(), who=who)
        except OSError as e:
            import sys
            sys.stderr.write("%s: history: %s\n" % (who, e))
        since = _get(st, who + "/since_note", 0) + 1
        _put(st, who + "/since_note", 0 if since >= NOTE_EVERY else since)
    if since >= NOTE_EVERY and chat is not None:
        threading.Thread(target=update_notebook, args=(chat, st, who), daemon=True).start()
    return out.strip()


def display(person):
    """How Tibbs knows a player: their display name, else their account name."""
    try:
        from .. import people
        for name, disp, _ in people.listing():
            if name == person:
                return disp or person
    except Exception:                                   # noqa: BLE001 - a name is enough
        pass
    return person


# ---- talking ------------------------------------------------------------------------------------

def _log(st, person, who="tibbs"):
    return _get(st, who + "/log/" + person, [])


def _log_add(st, person, speaker, text, who="tibbs"):
    with _meta:
        log = _log(st, person, who)
        log.append({"who": speaker, "text": text})
        _put(st, who + "/log/" + person, log[-LOG_KEPT:])


_talking = set()


def talk_state(person, store=None, who="tibbs"):
    return _get(store or kv.store(), who + "/talk/" + person, {"state": "ok"})


def _set_talk(st, person, state, who="tibbs"):
    _put(st, who + "/talk/" + person, state)


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


def talk_prompt(person, said, friends=(), context="", who="tibbs"):
    rules = DEAL_RULES if who == "tibbs" else CAST[who]["deal"]
    return ("[%s (account %s) is %s. %s%s They say:]\n%s\n\n"
            "[Answer them as %s, out loud: a few sentences at most, under %d characters. "
            "No JSON.]%s" % (display(person), person, CAST[who]["where"],
                            _friends_line(person, friends),
                            (" " + context) if context else "", said, name_of(who), ASK_LEN,
                            ("\n" + rules) if context else ""))


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


# ---- offers: nothing changes hands until the player says yes ------------------------
#
# The owner (2026-10-11): "needs confirmation before any transaction". A DEAL
# line is checked (a dry run) and kept as the person's offer from this
# shopkeeper, worded by the code, not the model; the device shows it and
# answers POST /jar/deal, which carries it out then -- checked again against
# what the player has at that moment.

OFFER_TTL = 1800


def offer_put(person, who, deal, words, store=None):
    _put(store or kv.store(), who + "/offer/" + person,
         {"deal": deal, "words": words, "at": time.time()})


def offer_get(person, who, store=None):
    o = _get(store or kv.store(), who + "/offer/" + person, None)
    if not o or time.time() - o.get("at", 0) > OFFER_TTL:
        return None
    return o


def offer_take(person, who, store=None):
    """The offer, gone once taken (yes or no)."""
    st = store or kv.store()
    with _meta:
        o = offer_get(person, who, st)
        _put(st, who + "/offer/" + person, None)
    return o


def _reply(chat, person, said, friends, st, context="", on_deal=None, who="tibbs"):
    try:
        out = turn(chat, talk_prompt(person, said, friends, context, who), person, st, who=who)
        out, deal = split_deal(out)
        if deal and on_deal:
            why, words = on_deal(person, deal)          # a dry run, and what it would do
            if why:                                     # it fell through: they hear why
                note("The deal with %s fell through: %s" % (display(person), why), st, who)
            elif words:
                offer_put(person, who, deal, words, st)
        reply = wire.flat(out, REPLY_LEN, ascii=True)
        _log_add(st, person, "him", reply, who)
        if who == "tibbs":
            shoplog_add(st, "%s said at the counter: \"%s\" -- and you said: \"%s\"" % (
                display(person), wire.flat(said, 160, ascii=True), wire.flat(reply, 160, ascii=True)))
        _set_talk(st, person, {"state": "ok"}, who)
    except ask.RateLimited:
        _set_talk(st, person, {"state": "error", "why": "they have talked enough for today"}, who)
    except Exception as e:                              # noqa: BLE001 - said to the device
        import sys
        sys.stderr.write("jar: %s: talk to %s: %s\n" % (person, who, e))
        _set_talk(st, person, {"state": "error", "why": "not heard; try again"}, who)
    finally:
        with _meta:
            _talking.discard((who, person))


def say(chat, person, text, friends=(), store=None, context="", on_deal=None, who="tibbs"):
    """A player says something; his reply comes on a thread. False if he is
    still answering them. `context` tells him what they have (coins, things),
    and with it he may strike a deal: on_deal(person, deal) checks it
    without doing it, answering (why not, or None; its words), and a good
    one waits as the person's offer until they say yes (POST /jar/deal)."""
    st = store or kv.store()
    text = wire.flat(text, SAY_LEN, ascii=True)
    if not text:
        raise ValueError("say something")
    with _meta:
        if (who, person) in _talking:
            return False
        _talking.add((who, person))
    _log_add(st, person, "me", text, who)
    _set_talk(st, person, {"state": "pending"}, who)
    threading.Thread(target=_reply, args=(chat, person, text, list(friends), st, context,
                                          on_deal, who), daemon=True).start()
    return True


def talk_text(person, store=None, tx_after=None, who="tibbs"):
    """GET /jar/talk: "pending", "ok" or "error WHY", then his last greeting
    as "day\\tTEXT" and the talk, "me\\tTEXT" / "him\\tTEXT"; then, for a
    device that asks (tx_after), the deals' effects after that one:
    "tx ID OP ARG" (pay N, get N, lose ITEM)."""
    st = store or kv.store()
    s = talk_state(person, st, who)
    if s.get("state") == "pending" and (who, person) not in _talking:
        s = {"state": "ok"}                             # a restart lost the thread
    out = [s["state"] if s["state"] != "error" else "error " + s.get("why", "")]
    day = _get(st, who + "/day/" + person, "")
    if day:
        out.append("day\t" + day)
    for c in _log(st, person, who)[-TALK_SENT:]:
        out.append("%s\t%s" % (c["who"], wire.flat(c["text"], SENT_LEN, ascii=True)))
    o = offer_get(person, who, st)
    if o and s["state"] != "pending":
        out.append("offer\t" + wire.flat(o["words"], 120, ascii=True))
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

def stock_schema(n_pick, n_new, errand=False):
    """A search is wanted only for a commission's errand; one given unasked
    is allowed and ignored (the finds come from his chapter)."""
    return {"type": "object", "required": ["line", "picks"] + (["search"] if errand else []),
            "additionalProperties": False, "properties": {
                "line": {"type": "string", "minLength": 1, "maxLength": LINE_LEN + SLACK,
                         "pattern": "^[ -~]+$"},
                "picks": {"type": "array", "maxItems": n_pick, "items": {"type": "integer"}},
                "search": {"type": "string", "minLength": 1,
                           "maxLength": SEARCH_LEN + SLACK, "pattern": "^[ -~\n]+$"}}}


# ---- his story, a chapter every four hours of visits ---------------------------------
#
# The owner (2026-10-13): "for each 4h time passage lets add to tibb's story
# ... like he went to WS and interacted with PK and found a CH where those
# are random letters (weighted)"; "time dosent move forward without someone
# visiting". A visit (the shop opened, its stock looked at) four hours or
# more after the last chapter writes the next one -- one, however long the
# gap. The code rolls three pairs of initials: a place, someone, something.
# No place, person or thing is ever named to him: he holds on to examples
# (his character once named six contacts, and six days visited them).
# What players said and did since the last chapter is rolled in. The story
# opens with tibbs_intro.txt, prose the owner edits. A chapter is the
# setting the DM invents the next finds from.

CHAPTER_S = 4 * 3600
CHAPTER_LEN = 1200
CHAPTERS_KEPT = 12
INTRO_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tibbs_intro.txt")
SHOP_LINES_MAX = 12

# How often a name's word starts with each letter (English dictionary
# words, roughly): plenty of S and C, hardly any Q or Z, no X.
INITIAL_WEIGHTS = {
    "A": 57, "B": 59, "C": 94, "D": 61, "E": 39, "F": 42, "G": 34, "H": 35, "I": 39,
    "J": 8, "K": 10, "L": 32, "M": 56, "N": 22, "O": 25, "P": 77, "Q": 5, "R": 56,
    "S": 110, "T": 52, "U": 20, "V": 15, "W": 27, "Y": 4, "Z": 3,
}
_chapter_lock = threading.Lock()


def roll_initials(rng):
    """Two initials, "W. S."."""
    letters, weights = zip(*INITIAL_WEIGHTS.items())
    return "%s. %s." % tuple(rng.choices(letters, weights, k=2))


def intro():
    """The opening of his story (tibbs_intro.txt), or ""."""
    try:
        with open(INTRO_PATH, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def chapters(store=None):
    """[{"t", "initials", "text"}], oldest first."""
    return _get(store or kv.store(), "tibbs/chapters", [])


def latest_chapter(store=None):
    ch = chapters(store)
    return ch[-1]["text"] if ch else ""


def chapter_schema():
    return {"type": "object", "required": ["chapter"], "additionalProperties": False,
            "properties": {"chapter": {"type": "string", "minLength": 40,
                                       "maxLength": CHAPTER_LEN + SLACK,
                                       "pattern": "^[ -~\n]+$"}}}


def chapter_prompt(place, someone, something, shop=(), last="", opening=""):
    lines = []
    if opening:
        lines += ["[Your story, as it began:]", opening, ""]
    if last:
        lines += ["[Your last chapter:]", last, ""]
    if shop:
        lines.append("[In the shop since then -- these happened; roll them into the chapter:]")
        lines += ["- " + x for x in shop]
        lines.append("")
    lines.append(
        "[Four hours have gone by. Write the next chapter of your story, in your own voice, "
        "as prose -- a scene, not a summary -- up to %d characters. In it you went out to a "
        "place whose name has the initials %s; you had dealings with someone whose initials "
        "are %s; and you came back with something whose initials are %s. The initials are "
        "the first letters of the words of each name; what they are is yours to make up. "
        "Somewhere you have never been, someone you have never met. Do not say what will be "
        "in anyone's shop. Answer with only JSON: {\"chapter\": \"...\"}]"
        % (CHAPTER_LEN, place, someone, something))
    return "\n".join(lines)


def maybe_chapter(chat, store=None, now=None, rng=None, log=None):
    """A visit: write the next chapter if the last is CHAPTER_S old (or
    there is none). The latest chapter, written now or before; "" if there
    has never been one. One writer at a time: a second visit meanwhile gets
    the chapter there is."""
    st = store or kv.store()
    now = time.time() if now is None else now
    ch = chapters(st)
    if ch and now - ch[-1]["t"] < CHAPTER_S:
        return ch[-1]["text"]
    if not _chapter_lock.acquire(blocking=False):
        return ch[-1]["text"] if ch else ""
    try:
        ch = chapters(st)                               # another writer may have just
        if ch and now - ch[-1]["t"] < CHAPTER_S:
            return ch[-1]["text"]
        rng = rng or random.Random()
        initials = [roll_initials(rng) for _ in range(3)]
        since = ch[-1]["t"] if ch else 0
        shop = shoplog_since(st, since)[-SHOP_LINES_MAX:]
        prompt = chapter_prompt(*initials, shop=shop, last=ch[-1]["text"] if ch else "",
                                opening=intro())
        text = ""
        for _ in range(2):
            try:
                out = turn(chat, prompt, None, st, effort=STOCK_EFFORT, limit=None,
                           kind="chapter")
                value = ask.extract(out)
                bad = ask.validate(value, chapter_schema())
            except ask.RateLimited:
                raise
            except Exception as e:                      # noqa: BLE001 - the shop still opens
                bad = [str(e)]
            if not bad:
                text = wire.flat(value["chapter"], CHAPTER_LEN, ascii=True)
                break
            prompt = ("[That was not the JSON (%s). Answer again with only the JSON: "
                      "{\"chapter\": \"...\"}]" % "; ".join(bad[:3]))
        if not text:
            if log:
                log("chapter: not written (%s)" % "; ".join(bad[:3]))
            return ch[-1]["text"] if ch else ""
        with _meta:
            ch = _get(st, "tibbs/chapters", []) + [
                {"t": int(now), "initials": initials, "text": text}]
            _put(st, "tibbs/chapters", ch[-CHAPTERS_KEPT:])
        if log:
            log("chapter: %s / %s / %s" % tuple(initials))
        return text
    finally:
        _chapter_lock.release()


def stock_prompt(person, pool, n_pick, n_new, asked, friends=(), commission=None, bribe=0):
    """His turn for a stock: his picks and his greeting. Where the new finds
    came from is his latest chapter (maybe_chapter), not asked here -- except
    for a commission, an errand of its own: then he says where he went."""
    lines = ["[%s (account %s) has come in, and their shop shelf needs filling. %s]" % (
        display(person), person, _friends_line(person, friends))]
    if n_pick:
        lines.append("[In the back, free to sell (nobody has them): id, name, kind, price, "
                     "description:]")
        lines += ["  %d: %s, %s, %d coins -- %s" % p for p in pool]
        lines.append("[Pick %d of those to put in front of %s today -- your eye, not "
                     "theirs: something they would never ask for as often as something "
                     "they might. Do not just match what they bought before.]" % (
                         n_pick, display(person)))
    if commission is not None:
        lines.append("[%d new finds came in from an errand of their own. Where did you go, and "
                     "what were you after? Tell it in your own voice, a few sentences, up to %d "
                     "characters. Do not describe the finds themselves.]" % (n_new, SEARCH_LEN))
        if bribe:
            lines.append("[%s also slipped you %d coins on the side to do better than usual: "
                         "you went to your best contacts.]" % (display(person), bribe))
        if commission:
            lines.append("[%s paid you to go out looking, and asked for: \"%s\". That is what "
                         "you were after today.]" % (display(person), commission))
        else:
            lines.append("[%s paid you to go out looking, for nothing in particular.]"
                         % display(person))
    elif asked:
        lines.append("[If %s has asked you to look out for something, you had it in mind while "
                     "you searched. If they have not, ignore this.]" % display(person))
    lines.append("[Then greet %s as they come in: what you say over the counter, under %d "
                 "characters. Answer with only JSON: {\"line\": your words, \"picks\": [ids]%s}]"
                 % (display(person), LINE_LEN,
                    ", \"search\": where you went" if commission is not None else ""))
    return "\n".join(lines)


def stock_turn(chat, person, pool, n_pick, n_new, friends=(), store=None, rng=None, log=None,
               commission=None, bribe=0):
    """His turn for a stock: (line, picks, search). picks are ids from `pool`
    (he may get some wrong; the caller checks); search is where he went on
    a commission's errand, else "" -- the finds come from his latest chapter
    (maybe_chapter)."""
    st = store or kv.store()
    rng = rng or random.Random()
    asked = rng.randrange(100) < ANSWER_CHANCE
    errand = commission is not None
    schema = stock_schema(n_pick, n_new, errand)
    prompt = stock_prompt(person, pool, n_pick, n_new, asked, friends, commission, bribe)
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
            search = wire.flat(value.get("search", ""), SEARCH_LEN, ascii=True) if errand else ""
            with _meta:
                _put(st, "tibbs/day/" + person, line)
                if search:
                    _put(st, "tibbs/searches",
                         (_get(st, "tibbs/searches", []) + [search])[-SEARCHES_KEPT:])
            return line, [int(i) for i in value["picks"]], search
        why = "; ".join(bad[:3])
        prompt = ("[That was not the JSON the shop needs (%s). Answer again with only the "
                  "JSON: {\"line\": ..., \"picks\": [...]%s}]"
                  % (why, ", \"search\": \"...\"" if errand else ""))
    if log:
        log("stock turn: %s; a note on the door" % why)
    line = "A note on the door: 'Out on business. Help yourself, pay the jar.'"
    with _meta:
        _put(st, "tibbs/day/" + person, line)
    return line, [], ""
