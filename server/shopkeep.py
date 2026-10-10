"""Tibbs, Jar Factory's shopkeeper: an ongoing story, and someone to talk to.

The shop's stock used to be made from "today's tags" -- the shelf's, the
garden's, the season's -- and every item had to fit them: a request for
industrial scrap on an autumn new-moon day came out a jack-o'-lantern with a
gear in it. Now the shop is a person. Each player has a journal with him
(kv, srv/jar keep/PERSON): his last few story beats, the last few things said
between them, what the player bought lately, and what is in their jar. Each
new stock begins with his beat -- a line about his day and a one-line brief
for each new find -- and each item is made from its brief (server/jar.py).

Variety is the code's job, not the model's. Left to choose, a model drifts
to the same few ideas, and it latches on: sell one cute thing and every find
is cute. So:
- the dice are rolled here -- an event in his day, two random "sparks" per
  find (a material or colour, an object or a mood) -- and the prompt calls
  them starting points;
- purchases are history, given as such, with a rule against catering to them;
- whether a find answers something the player said is a coin the code tosses
  (ANSWER_CHANCE), not a choice the model makes, so a request is a maybe;
- the last few briefs are passed as themes not to repeat.

Talking is asynchronous, like a stock: POST /jar/talk starts his reply on a
thread, GET /jar/talk polls it (server/jar.py has the routes).
"""
import json
import random
import sys
import threading
import time

from . import ask, kv, wire

NS = "srv/jar"
BEATS_KEPT = 4                # story beats he remembers
CHAT_KEPT = 10                # lines of talk he remembers
TALK_SENT = 6                 # lines of it the device is sent
BOUGHT_KEPT = 8               # purchases he remembers
BRIEFS_KEPT = 16              # recent finds, not to be repeated
LINE_LEN = 140                # his line of the day
BRIEF_LEN = 120               # one find's brief (the schema allows more: trimmed, not refused --
                              # one of 91 against a limit of 90 once cost a whole beat)
SLACK = 60
SAY_LEN = 120                 # what the player may say at once
REPLY_LEN = 220               # what he says back
ANSWER_CHANCE = 40            # percent: a find answers what the player said
BEAT_TIMEOUT = 300
TALK_TIMEOUT = 300

NAME = "Tibbs"

CHARACTER = (
    "Tibbs runs a cramped junk shop in the cupboard beside the player's jar. The jar "
    "is a tiny world: a jam factory built from human junk, mossy plants, worker "
    "critters called mosslings and a snail who carries the jam out. Tibbs sells the "
    "odd things the player puts in it. He trades through a loose, changing web of "
    "contacts -- a magpie who brings shiny things, the rail-yard men, a beachcomber, "
    "a retired clockmaker, a seed lady, children who swap marbles, whoever turns up "
    "-- and whatever washes in. He is chatty, a little vain about his finds, "
    "forgetful, given to tangents and small misadventures, and kind underneath. He "
    "does not take orders; he takes notes, and now and then it pays off.")

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
    """A brief with no beat behind it: the sparks, as a starting point."""
    return "a find suggested by: %s -- take it anywhere" % spark(rng)


# ---- the journal ---------------------------------------------------------------------------

def _key(person):
    return "keep/" + person


def journal(person, store=None):
    raw = (store or kv.store()).get(NS, _key(person))
    try:
        j = json.loads(raw.decode()) if raw else {}
    except ValueError:
        j = {}
    for k in ("beats", "chat", "bought", "briefs"):
        j.setdefault(k, [])
    j.setdefault("jar", [])
    return j


def _save(person, j, store=None):
    j["beats"] = j["beats"][-BEATS_KEPT:]
    j["chat"] = j["chat"][-CHAT_KEPT:]
    j["bought"] = j["bought"][-BOUGHT_KEPT:]
    j["briefs"] = j["briefs"][-BRIEFS_KEPT:]
    (store or kv.store()).put(NS, _key(person), json.dumps(j).encode())


_lock = threading.Lock()


def note_bought(person, items, store=None):
    """What the player bought from the last stock: [(name, kind word, price)]."""
    if not items:
        return
    with _lock:
        j = journal(person, store)
        for name, kind, price in items:
            j["bought"].append({"name": name, "kind": kind, "price": price})
        _save(person, j, store)


def note_jar(person, names, store=None):
    """What is in the player's jar now (the device says, with each request)."""
    with _lock:
        j = journal(person, store)
        j["jar"] = [wire.flat(n, 16, ascii=True) for n in names][:16]
        _save(person, j, store)


def _said(j, since=None):
    """The player's lines since `since` (an index into chat) -- unanswered asks."""
    return [c["text"] for c in j["chat"][since or 0:] if c["who"] == "me"]


def _context(j):
    out = []
    if j["beats"]:
        out.append("His story so far, most recent last:\n" +
                   "\n".join("- " + b["line"] for b in j["beats"]))
    if j["chat"]:
        out.append("What was said between them lately, oldest first:\n" +
                   "\n".join("- %s: %s" % ("Player" if c["who"] == "me" else NAME, c["text"])
                             for c in j["chat"]))
    if j["jar"]:
        out.append("In the player's jar just now: %s." % ", ".join(j["jar"]))
    if j["bought"]:
        out.append("Bought from him lately (history, nothing more): %s." % ", ".join(
            "%s (%s, %d coins)" % (b["name"], b["kind"], b["price"]) for b in j["bought"]))
    return out


# ---- the day's beat -------------------------------------------------------------------------

def beat_schema(n):
    return {"type": "object", "required": ["line", "briefs"], "additionalProperties": False,
            "properties": {
                "line": {"type": "string", "minLength": 1, "maxLength": LINE_LEN + SLACK,
                         "pattern": "^[ -~]+$"},
                "briefs": {"type": "array", "minItems": n, "maxItems": n,
                           "items": {"type": "string", "minLength": 1,
                                     "maxLength": BRIEF_LEN + SLACK, "pattern": "^[ -~]+$"}}}}


def beat_prompt(j, n, rng, answer):
    """The prompt for today's beat; `answer` is whether find 1 answers the player."""
    sparks = [spark(rng) for _ in range(n)]
    lines = [CHARACTER, ""]
    lines += _context(j)
    lines += [
        "",
        "Write today's beat of his story, and what came into the shop.",
        "Today, by chance: %s. Let it shape his day, or wander off from it." % rng.choice(EVENTS),
        "- line: one or two short sentences (under %d characters) in his own voice, as he would say them to the "
        "player over the counter: what happened, what he found, a grumble or a boast. Carry "
        "the story on from where it was; no greetings, no 'welcome back'." % LINE_LEN,
        "- briefs: %d finds, one line each (under 90 characters): what the thing is and one odd detail, as a note "
        "for whoever draws it (it becomes a 16x16 pixel-art item: a critter that roams the "
        "jar, or decor). Each starts from its sparks -- a starting point, not a recipe; "
        "twist them, combine them, or follow where they lead:" % n,
    ]
    for i, s in enumerate(sparks):
        lines.append("  find %d sparks: %s" % (i + 1, s))
    lines += [
        "Make the finds unlike each other -- different materials, colours, sizes, moods -- "
        "and surprising. A junk shop's stock is whatever turned up, not what anyone ordered.",
        "Do not make them like what the player bought or what is in their jar: that is "
        "history, and a dealer who only restocked one customer's taste would be dull.",
    ]
    asked = _said(j)
    if answer and asked:
        lines.append("Find 1 is his loose answer to something the player said to him -- "
                     "what he turned up through a contact, near the mark or a little off it, "
                     "never a literal granting. Mention it in his line if he likes.")
    elif asked:
        lines.append("None of today's finds answers what the player said; he is still "
                     "asking around, or forgot, or got sidetracked.")
    if j["briefs"]:
        lines.append("Recent finds -- do not repeat their themes: %s." %
                     "; ".join(j["briefs"][-10:]))
    lines.append("Friendly for all ages; plain ASCII.")
    return "\n".join(lines)


def day_beat(chat, person, n, store=None, rng=None, log=None):
    """(line, [n briefs]) for a new stock, and it goes in the journal. If
    Claude cannot be asked, a note on his door and briefs from the sparks."""
    st = store or kv.store()
    rng = rng or random.Random()
    j = journal(person, st)
    answer = rng.randrange(100) < ANSWER_CHANCE
    try:
        got = ask.ask_shape(chat, beat_prompt(j, n, rng, answer), beat_schema(n), user=person,
                            limit=ask.DAILY, timeout=BEAT_TIMEOUT, store=st, effort="low")
        line = wire.flat(got["line"], LINE_LEN, ascii=True)
        briefs = [wire.flat(b, BRIEF_LEN, ascii=True) for b in got["briefs"]]
    except ask.RateLimited:
        raise
    except Exception as e:                              # noqa: BLE001 - the shop still opens
        if log:
            log("beat: %s" % e)
        # Not part of his story: a day he was out is not remembered as one.
        return ("A note on the door: 'Out on business. Help yourself, pay the jar.'",
                [loose_brief(rng) for _ in range(n)])
    with _lock:
        j = journal(person, st)
        j["beats"].append({"at": int(time.time()), "line": line})
        j["briefs"] += briefs
        _save(person, j, st)
    return line, briefs


# ---- talking ----------------------------------------------------------------------------------

def talk_schema():
    return {"type": "object", "required": ["reply"], "additionalProperties": False,
            "properties": {"reply": {"type": "string", "minLength": 1, "maxLength": REPLY_LEN,
                                     "pattern": "^[ -~]+$"}}}


def talk_prompt(j, said):
    lines = [CHARACTER, ""] + _context(j) + [
        "",
        "The player says to him over the counter: \"%s\"" % said,
        "Reply as Tibbs: one to three short sentences, in his voice, plain ASCII, friendly "
        "for all ages. He chats, tells on himself, goes off on small tangents. If asked to "
        "find something he makes no promise -- he'll ask around, or knows a fellow -- and "
        "he never says what tomorrow's stock will be. He knows the jar only as the player "
        "has it; he cannot change it or give anything away for free.",
    ]
    return "\n".join(lines)


_talking = set()


def talk_state(person, store=None):
    raw = (store or kv.store()).get(NS, "talk/" + person)
    try:
        return json.loads(raw.decode()) if raw else {"state": "ok"}
    except ValueError:
        return {"state": "ok"}


def _set_talk(person, state, store):
    store.put(NS, "talk/" + person, json.dumps(state).encode())


def _reply(chat, person, said, store):
    try:
        j = journal(person, store)
        got = ask.ask_shape(chat, talk_prompt(j, said), talk_schema(), user=person,
                            limit=ask.DAILY, timeout=TALK_TIMEOUT, store=store, effort="low")
        reply = wire.flat(got["reply"], REPLY_LEN, ascii=True)
        with _lock:
            j = journal(person, store)
            j["chat"].append({"who": "him", "text": reply})
            _save(person, j, store)
        _set_talk(person, {"state": "ok"}, store)
    except ask.RateLimited:
        _set_talk(person, {"state": "error", "why": "he has talked enough for today"}, store)
    except Exception as e:                              # noqa: BLE001 - said to the device
        sys.stderr.write("jar: %s: talk: %s\n" % (person, e))
        _set_talk(person, {"state": "error", "why": "he did not hear you; try again"}, store)
    finally:
        with _lock:
            _talking.discard(person)


def say(chat, person, text, store=None):
    """The player says something; his reply comes on a thread. False if he
    is still answering the last thing."""
    st = store or kv.store()
    text = wire.flat(text, SAY_LEN, ascii=True)
    if not text:
        raise ValueError("say something")
    with _lock:
        if person in _talking:
            return False
        _talking.add(person)
        j = journal(person, st)
        j["chat"].append({"who": "me", "text": text})
        _save(person, j, st)
    _set_talk(person, {"state": "pending"}, st)
    threading.Thread(target=_reply, args=(chat, person, text, st), daemon=True).start()
    return True


def talk_text(person, store=None):
    """GET /jar/talk: "pending", "ok" or "error WHY", then the talk, a line
    each: "me\\tTEXT" or "him\\tTEXT", and his line of the day as "day\\tTEXT"."""
    st = store or kv.store()
    s = talk_state(person, st)
    if s.get("state") == "pending" and person not in _talking:
        s = {"state": "ok"}                             # a restart lost the thread
    head = s["state"] if s["state"] != "error" else "error " + s.get("why", "")
    j = journal(person, st)
    out = [head]
    if j["beats"]:
        out.append("day\t" + j["beats"][-1]["line"])
    for c in j["chat"][-TALK_SENT:]:                    # the device's buffer is 2 KB
        out.append("%s\t%s" % (c["who"], c["text"]))
    return "\n".join(out) + "\n"
