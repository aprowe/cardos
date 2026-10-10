"""Tibbs, Jar Factory's shopkeeper: one memory, one story, everyone's.

Tibbs is a single Claude Code session on Opus (MODEL), resumed for every turn
and shared by every player -- so what he remembers is the conversation itself,
and Claude Code compacts it when it grows long. Ask him about a friend and he
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
TALK_SENT = 6                 # lines of it the device is sent (its buffer is 2 KB)
EVENTS_KEPT = 40
BRIEFS_KEPT = 16
ANSWER_CHANCE = 40            # percent: a new find answers what the player asked
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


def _ask(chat, prompt, session, effort):
    """One turn of his session: (text, session id). Patched by the tests."""
    return _chat.ask_once(chat, prompt, TURN_TIMEOUT, resume=session, system=SYSTEM,
                          model=MODEL, effort=effort, cwd=home())


def turn(chat, text, user, store=None, effort=TALK_EFFORT, limit=ask.DAILY):
    """Say `text` to him in his one conversation: his answer. The news since
    his last turn goes first. Counts against `user`'s day."""
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
        try:
            out, sid = _ask(chat, prompt, session, effort)
        except _chat.ClaudeError as e:
            if not session or e.timed_out:
                with _meta:                              # the news is not lost
                    _put(st, "tibbs/events", ev + _get(st, "tibbs/events", []))
                raise
            # His session is gone (a new server, a cleared directory): a fresh
            # one, told so. What he knew is lost; the shop goes on.
            out, sid = _ask(chat, "[Your memory of the shop has gone hazy -- a long nap. "
                                  "Carry on as Tibbs.]\n\n" + prompt, None, effort)
        _put(st, "tibbs/session", sid)
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


def talk_prompt(person, said, friends=()):
    return ("[%s (account %s) is at the counter. %s They say:]\n%s\n\n"
            "[Answer them as Tibbs, out loud: a few sentences at most, under %d characters. "
            "No JSON.]" % (display(person), person, _friends_line(person, friends), said,
                           REPLY_LEN - 100))


def _reply(chat, person, said, friends, st):
    try:
        out = turn(chat, talk_prompt(person, said, friends), person, st)
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


def say(chat, person, text, friends=(), store=None):
    """A player says something; his reply comes on a thread. False if he is
    still answering them."""
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
    threading.Thread(target=_reply, args=(chat, person, text, list(friends), st),
                     daemon=True).start()
    return True


def talk_text(person, store=None):
    """GET /jar/talk: "pending", "ok" or "error WHY", then his last greeting
    as "day\\tTEXT" and the talk, "me\\tTEXT" / "him\\tTEXT"."""
    st = store or kv.store()
    s = talk_state(person, st)
    if s.get("state") == "pending" and person not in _talking:
        s = {"state": "ok"}                             # a restart lost the thread
    out = [s["state"] if s["state"] != "error" else "error " + s.get("why", "")]
    day = _get(st, "tibbs/day/" + person, "")
    if day:
        out.append("day\t" + day)
    for c in _log(st, person)[-TALK_SENT:]:
        out.append("%s\t%s" % (c["who"], wire.flat(c["text"], 300, ascii=True)))
    return "\n".join(out) + "\n"


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


def stock_prompt(person, pool, n_pick, n_new, rng, asked, friends=()):
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
    for i in range(n_new):
        lines.append("  find %d sparks: %s" % (i + 1, spark(rng)))
    lines.append("[Make the finds unlike each other and unlike what you have been bringing "
                 "in lately; a junk shop's stock is whatever turned up.]")
    if asked:
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


def stock_turn(chat, person, pool, n_pick, n_new, friends=(), store=None, rng=None, log=None):
    """His turn for a stock: (line, picks, briefs). picks are ids from `pool`
    (he may get some wrong; the caller checks); if he cannot be asked, a
    note on the door, no picks and loose briefs."""
    st = store or kv.store()
    rng = rng or random.Random()
    asked = rng.randrange(100) < ANSWER_CHANCE
    schema = stock_schema(n_pick, n_new)
    prompt = stock_prompt(person, pool, n_pick, n_new, rng, asked, friends)
    why = None
    for attempt in range(2):
        try:
            out = turn(chat, prompt, person, st, effort=STOCK_EFFORT)
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
