"""Items that learn about a newcomer: Jar Factory's patches (stage G of
docs/superpowers/specs/2026-10-11-jar-living-world-design.md).

The owner: "when a new item is introduced, other items can perhaps have
their scripts altered so they interact with the new item. again not all
items have scripts." An item's record is signed and stays as it is; what
changes is a patch beside it -- a few more handlers in the same language,
written for that item about the newcomer, compiled, given a simulated day,
and signed. The device runs a patch for any event the item's own script
does not handle (a recipe-only item gets behaviour this way too).

When the device reports a purchase and says what is in its jar (injar), a
coin (PATCH_CHANCE) decides whether the newcomer is noticed at all, and up
to PATCH_MAX of the jar's other items are chosen at random -- by the code,
not the model. Each gets one Claude call. Their patches wait for the device
at GET /jar/patches?after=N:

    patch N ITEM NEW <base64 bytecode> <base64 signature>

The signature (server/sign.py's key) covers PATCH_MAGIC, ITEM and NEW
(4 bytes each, little-endian) and the bytecode, so a device can check a
patch is the server's before it runs it. Each person's last PATCH_KEPT are
kept; a newer patch for an item replaces the older on the device.

Only with language v2 (server/jarvm.py V2): a patch is v2 bytecode.
"""
import base64
import json
import random
import sys
import threading

from . import ask, jarvm, kv, shopkeep, sign

NS = "srv/jar"
PATCH_MAGIC = b"JARPATCH1"
PATCH_CHANCE = 60              # percent: a newcomer is noticed
PATCH_MAX = 2                  # items that learn about one newcomer
PATCH_KEPT = 32
PATCH_SENT = 3                 # a reply's worth: the device reads 2 KB at a time
PATCH_SRC_MAX = 600
PATCH_LINES = 8


def message(item_id, new_id, code):
    return (PATCH_MAGIC + int(item_id).to_bytes(4, "little") +
            int(new_id).to_bytes(4, "little") + bytes(code))


def _list(st, person):
    raw = st.get(NS, "patch/" + person)
    try:
        return json.loads(raw.decode()) if raw else []
    except ValueError:
        return []


def patches(person, store=None, after=0):
    """[{n, item, new, code, sig}] after n, oldest first."""
    return [p for p in _list(store or kv.store(), person) if p["n"] > after]


def add(person, item_id, new_id, code, store=None, signer=None):
    """A patch, signed and kept: its number."""
    st = store or kv.store()
    sig = (signer or sign.sign)(message(item_id, new_id, code))
    n = st.incr(NS, "patchn/" + person)
    lst = _list(st, person)
    lst.append({"n": n, "item": int(item_id), "new": int(new_id),
                "code": base64.b64encode(bytes(code)).decode(),
                "sig": base64.b64encode(sig).decode()})
    st.put(NS, "patch/" + person, json.dumps(lst[-PATCH_KEPT:]).encode())
    return n


def text(person, after=0, store=None):
    """GET /jar/patches: a line a patch after `after`, PATCH_SENT at most --
    the device asks again from the last it got."""
    return "".join("patch %d %d %d %s %s\n" % (p["n"], p["item"], p["new"], p["code"], p["sig"])
                   for p in patches(person, store, after)[:PATCH_SENT])


SCHEMA = {"type": "object", "required": ["script", "note"], "additionalProperties": False,
          "properties": {
              "script": {"type": "string", "minLength": 1, "maxLength": PATCH_SRC_MAX},
              "note": {"type": "string", "minLength": 1, "maxLength": 120,
                       "pattern": "^[ -~]+$"}}}


def _traits(it):
    return it.get("tags") or "none"


def prompt(old, new, old_script_text=""):
    """What Claude is asked: a few handlers for `old` about `new` (decoded items)."""
    lines = [
        "Jar Factory is a cosy idle game: a glass jar with a jam factory, critters and odd "
        "collectible items, each with a little behaviour script.",
        "",
        "A new item has just come into this player's jar: \"%s\" -- %s (traits: %s)." % (
            new["name"], new["line"], _traits(new)),
        "Write a short addition to the behaviour of an item already there, so it notices and "
        "plays with the newcomer: \"%s\" -- %s (traits: %s; %d speech bubble%s, %d frame%s)." % (
            old["name"], old["line"], _traits(old), len(old["bubbles"]),
            "" if len(old["bubbles"]) == 1 else "s", len(old["frames"]),
            "" if len(old["frames"]) == 1 else "s"),
    ]
    if old["bubbles"]:
        lines.append("Its bubbles: %s." % ", ".join('%d "%s"' % (i + 1, b)
                                                    for i, b in enumerate(old["bubbles"])))
    if old_script_text:
        lines.append("It already has handlers for: %s -- write for other events, or narrower "
                     "ones (on near critter:, on signal 7:)." % old_script_text)
    lines += [
        "React to the newcomer's TRAITS, never its name (has(neartraits, X), on new:, seek X): "
        "anything with those traits counts, now and later. Keep it to %d lines at most; make "
        "something visible or audible happen. Use only bubbles and frames it has." % PATCH_LINES,
        "Also give a one-line note, in the shopkeeper's gossipy voice, of what changed (\"the "
        "brass owl has taken to hooting at anything shiny\").",
        "Answer with only JSON: {\"script\": the handlers, \"note\": the line}.",
        "",
        "The script language:",
        jarvm.LANGUAGE_GUIDE_V2,
    ]
    return "\n".join(lines)


def handled(code):
    """The events an item's own bytecode handles, as words."""
    if not code or len(code) < 2:
        return ""
    words = []
    for e in range(code[1]):
        ev = code[2 + 3 * e]
        if ev < len(jarvm.EVENT_NAME):
            words.append(jarvm.EVENT_NAME[ev])
    return ", ".join(dict.fromkeys(words))


def make(chat, person, new_ids, injar, store=None, rng=None, decode=None, signer=None, log=None):
    """Patches for up to PATCH_MAX of the jar's other items, about each new
    item; the coin and the choice are the code's. Returns how many were made."""
    st = store or kv.store()
    rng = rng or random.Random()
    if decode is None:
        from .jar import decode
    made = 0
    for new_id in new_ids:
        if rng.randrange(100) >= PATCH_CHANCE:
            continue
        nrec = st.get(NS, "rec/%d" % new_id)
        if nrec is None:
            continue
        new = decode(nrec)
        others = [i for i in injar if i != new_id and st.get(NS, "own/%d" % i) == person.encode()]
        rng.shuffle(others)
        for item_id in others[:PATCH_MAX]:
            rec = st.get(NS, "rec/%d" % item_id)
            if rec is None:
                continue
            old = decode(rec)
            try:
                got = ask.ask_shape(chat, prompt(old, new, handled(old.get("script"))), SCHEMA,
                                    user=person, store=st, effort="medium",
                                    model=shopkeep.MODEL, cwd=shopkeep.home())
                code = jarvm.prepare_script(got["script"], nbub=len(old["bubbles"]) or 1,
                                            nframes=len(old["frames"]), v2=True)
            except ask.RateLimited:
                return made
            except Exception as e:                     # noqa: BLE001 - one patch lost
                if log:
                    log("patch for %s: %s" % (old["name"], e))
                continue
            add(person, item_id, new_id, code, st, signer)
            shopkeep.note("In %s's jar: %s" % (shopkeep.display(person), got["note"]), st)
            made += 1
            if log:
                log("patch: %s learned about %s (%d bytes)" % (old["name"], new["name"], len(code)))
    return made


def start(chat, person, new_ids, injar, store=None):
    """make() on a thread, when v2 is on and there is something to do."""
    if not jarvm.V2 or not new_ids or not injar or chat is None:
        return False
    log = lambda s: sys.stderr.write("jar: %s: %s\n" % (person, s))   # noqa: E731
    threading.Thread(target=make, args=(chat, person, list(new_ids), list(injar), store),
                     kwargs={"log": log}, daemon=True).start()
    return True
