"""The jar's day, written: world events made for one person's jar on one day
(stage H of docs/superpowers/specs/2026-10-11-jar-living-world-design.md).

The sim already rolls world events of its own (ants, a leak, a breeze, a
visitor, the dark, a bloom). These are more: a few a day, each one of the
same kinds -- so every item's `on world ants:` still answers -- but named
and told for this jar and this day. Never at a time of day (the owner,
2026-10-11: "you will never catch it"): they come in order, the jar playing
the next one every 5-15 minutes it is open. The code rolls how many and
what (variety is the code's job), and Claude writes the words: a title for
the banner and a line Tibbs can gossip about, from what is in the jar, the
weather and the season.

    GET /jar/events?day=YYYYMMDD&tz=TZ
    event N KIND TITLE            N: its turn, from 1

Cached per person per day; made on the first ask of the day. Without
Claude (or over the day's limit) the day has plain titles. v2 only.
"""
import json
import random
import sys
import threading

from . import ask, jarvm, kv, shopkeep, wire

NS = "srv/jar"
EVENTS_A_DAY = (3, 6)
TITLE_LEN = 30
KINDS = ("ants", "leak", "breeze", "visitor", "dark", "bloom")
PLAIN = {"ants": "Ants on the march", "leak": "A leak in the lid", "breeze": "A breeze blows through",
         "visitor": "A visitor on the lid", "dark": "The lights go out", "bloom": "Everything blooms"}

_making = set()
_lock = threading.Lock()


def roll(day, person, rng=None):
    """[(turn, kind)] for the day: the code's dice, the same all day."""
    rng = rng or random.Random("%s/%s" % (person, day))
    n = rng.randint(*EVENTS_A_DAY)
    return [(i + 1, rng.choice(KINDS)) for i in range(n)]


SCHEMA = {"type": "object", "required": ["events"], "additionalProperties": False,
          "properties": {"events": {"type": "array", "items": {
              "type": "object", "required": ["title", "tell"], "additionalProperties": False,
              "properties": {
                  "title": {"type": "string", "minLength": 1, "maxLength": TITLE_LEN,
                            "pattern": "^[ -~]+$"},
                  "tell": {"type": "string", "minLength": 1, "maxLength": 140,
                           "pattern": "^[ -~]+$"}}}}}}

WHAT = {"ants": "ants march in after the berries", "leak": "the lid leaks and a puddle spreads",
        "breeze": "a breeze blows through and the belt runs fast",
        "visitor": "a visitor lands on the lid and leaves jars on the dock",
        "dark": "the lights go out for a minute", "bloom": "every bush ripens at once"}


def prompt(plan, jar_names, weather, season):
    lines = ["Jar Factory is a cosy idle game: a glass jar with a jam factory, critters and "
             "odd collectible things. Today in this player's jar, these happen, one after another:"]
    for n, k in plan:
        lines.append("  %d. %s" % (n, WHAT[k]))
    lines += [
        "In the jar: %s. Outside: %s, %s." % (", ".join(jar_names) or "not much yet",
                                              weather or "no weather known", season or "a day"),
        "Give each its own title for a banner (under %d characters, playful, not a pun on the "
        "same thing twice) and a line for the shopkeeper to tell as gossip. Keep it what "
        "happens -- only the telling is yours. Friendly for all ages." % TITLE_LEN,
        "Answer with only JSON: {\"events\": [{\"title\", \"tell\"}, ...]} in that order.",
    ]
    return "\n".join(lines)


def _key(person, day):
    return "events/%s/%s" % (person, day)


def get(person, day, store=None):
    raw = (store or kv.store()).get(NS, _key(person, day))
    try:
        return json.loads(raw.decode()) if raw else None
    except ValueError:
        return None


def make(chat, person, day, jar_names=(), weather=None, season=None, store=None, rng=None):
    """The day's events, made and kept: [{n, kind, title}], in turn."""
    st = store or kv.store()
    plan = roll(day, person, rng)
    titles = [PLAIN[k] for _m, k in plan]
    tells = []
    if chat is not None:
        try:
            got = ask.ask_shape(chat, prompt(plan, list(jar_names), weather, season), SCHEMA,
                                user=person, store=st, effort="low", model=shopkeep.MODEL,
                                cwd=shopkeep.home())
            for i, e in enumerate(got.get("events", [])[:len(plan)]):
                t = wire.flat(e.get("title", ""), TITLE_LEN, ascii=True)
                if t:
                    titles[i] = t
                tells.append(wire.flat(e.get("tell", ""), 140, ascii=True))
        except Exception as e:                       # noqa: BLE001 - plain titles, then
            sys.stderr.write("jar: %s: events: %s\n" % (person, e))
    out = [{"n": m, "kind": k, "title": titles[i]} for i, (m, k) in enumerate(plan)]
    st.put(NS, _key(person, day), json.dumps(out).encode(), ttl=3 * 86400)
    for t in tells:
        if t:
            shopkeep.note("Today in %s's jar: %s" % (shopkeep.display(person), t), st)
    return out


def text(evs):
    return "".join("event %d %s %s\n" % (i + 1, e["kind"], e["title"]) for i, e in enumerate(evs))


def answer(chat, person, day, jar_names=(), weather=None, season=None, store=None):
    """The day's lines, or "pending" while they are being written (on a thread)."""
    st = store or kv.store()
    evs = get(person, day, st)
    if evs is not None:
        return text(evs)
    if not jarvm.V2:
        return ""
    with _lock:
        if (person, day) in _making:
            return "pending\n"
        _making.add((person, day))

    def run():
        try:
            make(chat, person, day, jar_names, weather, season, st)
        finally:
            with _lock:
                _making.discard((person, day))

    threading.Thread(target=run, daemon=True).start()
    return "pending\n"
