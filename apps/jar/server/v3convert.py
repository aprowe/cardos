"""Every item someone has -- owned, or on a shop's shelf -- rewritten in
JarScript 3 (2026-10-14, the owner: "convert all items owned or in a shop to
jarscript 3 and deprecate the pool of jarscript 2 items"; "make sure it's
roll backable. I don't want to fudge current behaviours of the items").

    python -m apps.jar.server v3convert [--dry] [--id N] [--limit N]
    python -m apps.jar.server v3undo [--id N]

Per item: its behaviour is written again in v3 from what it does now -- its
concept, its intent, and its v2 habits and script spelled out -- at the tier
the code gives it (a thing that did something is never given none). Its art,
name, line, price, traits and recipe are kept; its memory is cleared, since
v2's slots mean nothing to a v3 script. The record is re-signed and stored;
the ORIGINAL goes to v2rec/ID first, and stays until undone. An owner is sent
`tx remake ID`, the same effect Pip's work sends, so their device fetches it;
an item on a shelf is replaced in place, and its buyer is sent the same
effect when the purchase is told (jar._note_bought). Done ones are marked
v3/ID, so a run that stops can be run again. A failure keeps the old record.

Undo puts every v2rec/ID back the same way. Nothing here runs unless asked.
"""
import json
import sys
import time

from server import kv
from apps.jar import server as jar
from . import jarvm, jarstages, shopkeep

MARK = "v3/%d"
BACKUP = "v2rec/%d"


def _holders(st):
    """{id: (person, where)} for every item owned or on a shelf; where is
    "own" or "shelf"."""
    shelf = {}
    for key, _s, _e in st.list(jar.NS, "shown/", limit=100000):
        person = key[len("shown/"):]
        for i in jar.shown(person, st):
            shelf[i] = person
    out = {}
    for key, _s, _e in st.list(jar.NS, "own/", limit=100000):
        tail = key[4:]
        if not tail.isdigit():
            continue
        i = int(tail)
        who = st.get(jar.NS, key)
        if not who or who == jar.TRASHED:
            continue
        person = who.decode()
        out[i] = (person, "shelf" if shelf.get(i) == person else "own")
    return out


def _form_of(old):
    return ("furniture" if old["role"] == 1 else "scenery" if old["role"] == 2 else
            "part" if old["part"] else "big" if old["scale"] == 2 else None)


def _what_it_does(old):
    """Its v2 behaviour in words and code, for the writer to keep."""
    e = jar.enums()
    ev = {v: k for k, v in e["events"].items()}
    ac = {v: k for k, v in e["actions"].items()}
    lines = []
    for h in old.get("habits") or []:
        lines.append("when %s (%d): %s (%d)" % (ev.get(h[0], h[0]), h[1], ac.get(h[2], h[2]), h[3]))
    out = []
    if lines:
        out.append("Its habits now: " + "; ".join(lines) + ".")
    if old.get("bubbles"):
        out.append("Its bubbles: %s." % ", ".join('"%s"' % b for b in old["bubbles"]))
    script = bytes(old.get("script") or b"")
    if script:
        try:
            out.append("Its v2 script now, disassembled:\n" + jarvm.disassemble(script))
        except Exception:                               # noqa: BLE001 - only a description
            out.append("It has a v2 script (%d bytes) that could not be read back." % len(script))
    return "\n".join(out) or "It does nothing beyond how it moves."


def tier_for(old, kind, form, activity):
    t = jarstages.script_tier(kind, form, activity)
    if t == "none" and (old.get("habits") or old.get("script")):
        t = "small"                                     # it did something: it keeps doing it
    return t


def convert_one(chat, item_id, st, log=None):
    """The new sealed record for item_id (not stored), and its tier; or
    raises ValueError with why not."""
    from . import jarvm3
    say = log or (lambda s: None)
    rec = st.get(jar.NS, "rec/%d" % item_id)
    if rec is None:
        raise ValueError("no record")
    if jar.is_v3(rec):
        raise ValueError("already v3")
    old = jar.decode(rec)
    kind = {v: k for k, v in jar.enums()["kinds"].items()}[old["kind"]]
    form = _form_of(old)
    was = jarstages.concept_of(item_id, st) or {}
    activity = was.get("activity") or jarstages._activity_of(old)
    stier = tier_for(old, kind, form, activity)
    if stier == "none":
        new = dict(old, script=b"", mem=[0] * jar.MEM)
        return jar.seal(jar.encode(_unsigned(new))), stier
    zones = {v: k for k, v in jar.enums()["zones"].items()}
    travel = jar.TRAVELS[old.get("travel") or 0] if old.get("travel") is not None else "none"
    sheet = {"name": old["name"], "line": old["line"],
             "intent": was.get("intent") or old["line"],
             "zone": zones.get(old["zone"], "anywhere"), "travel": travel,
             "traits": [w for w in (old.get("tags") or "").split(",")
                        if w in jarvm3.GROUPS["trait"]]}
    nframes = len(old["frames"]) - (1 if form == "part" and len(old["frames"]) >= 2 else 0)
    keep = ("\n\nTHIS IS AN EXISTING ITEM being moved to JarScript 3. Keep what it does: the "
            "same reactions to the same things, the same character -- do not invent new "
            "behaviour, only express its current behaviour in the new language (its bubbles "
            "become `say` lines; its v2 waits are in quarter-seconds). What it does now:\n"
            + _what_it_does(old))
    prompt = jarstages.behaviour_prompt_v3(sheet, kind, activity, nframes, stier, form) + keep
    scale = 2 if form in ("big", "scenery") else 1
    err = ""
    for attempt in range(2):
        ans = jarstages._ask(chat, prompt + err, jarstages.behaviour_schema_v3(), None, "high", st)
        try:
            script = jarvm3.prepare_script(ans["script"], nframes, tier=stier, scale=scale)
            break
        except jarvm3.ScriptError as e:
            say("v3convert %d: %s" % (item_id, e))
            err = "\n\nThe last answer was refused: %s. Fix that." % e
    else:
        raise ValueError("the script would not compile: %s" % err.strip())
    new = dict(old, script=script, mem=[0] * jar.MEM)
    return jar.seal(jar.encode(_unsigned(new))), stier


def _unsigned(it):
    return dict(it, sig=b"")


def convert(chat, ids=None, dry=False, limit=None, store=None, log=None):
    """Convert what is held (or `ids`): {"done": [...], "failed": [(id, why)],
    "skipped": n}."""
    st = store or kv.store()
    say = log or (lambda s: sys.stderr.write(s + "\n"))
    holders = _holders(st)
    todo = sorted(ids if ids else holders)
    out = {"done": [], "failed": [], "skipped": 0}
    for i in todo:
        if limit is not None and len(out["done"]) >= limit:
            break
        rec = st.get(jar.NS, "rec/%d" % i)
        if rec is None or jar.is_v3(rec) or st.get(jar.NS, MARK % i):
            out["skipped"] += 1
            continue
        if dry:
            old = jar.decode(rec)
            kind = {v: k for k, v in jar.enums()["kinds"].items()}[old["kind"]]
            was = jarstages.concept_of(i, st) or {}
            act = was.get("activity") or jarstages._activity_of(old)
            say("would convert %d %s (%s, %s)" % (i, old["name"], holders.get(i, ("-", "?"))[1],
                                                  tier_for(old, kind, _form_of(old), act)))
            out["done"].append(i)
            continue
        try:
            new, stier = convert_one(chat, i, st, say)
        except Exception as e:                          # noqa: BLE001 - listed, old kept
            out["failed"].append((i, str(e)))
            say("v3convert %d failed: %s" % (i, e))
            continue
        if st.get(jar.NS, BACKUP % i) is None:
            st.put(jar.NS, BACKUP % i, rec)
        st.put(jar.NS, "rec/%d" % i, new)
        st.put(jar.NS, MARK % i, json.dumps({"at": int(time.time()), "tier": stier}).encode())
        person, where = holders.get(i, (None, None))
        if person and where == "own":
            shopkeep.tx_add(person, "remake", i, st)
        _restock(st, i, new)
        out["done"].append(i)
        say("v3convert %d %s: %s, %d bytes" % (i, jar.decode(new)["name"], stier,
                                              len(jar.decode(new)["script"])))
    return out


def _restock(st, item_id, rec):
    """A shelf's cached copy (stock/PERSON/K) follows the record."""
    for key, _s, _e in st.list(jar.NS, "stock/", limit=100000):
        cur = st.get(jar.NS, key)
        try:
            if cur and jar.decode(cur)["id"] == item_id:
                st.put(jar.NS, key, rec, ttl=jar.STOCK_TTL)
        except ValueError:
            continue


def undo(ids=None, store=None, log=None):
    """Every converted item (or `ids`) back to its v2 record. How many."""
    st = store or kv.store()
    say = log or (lambda s: sys.stderr.write(s + "\n"))
    holders = _holders(st)
    keys = [BACKUP % i for i in ids] if ids else \
        [k for k, _s, _e in st.list(jar.NS, "v2rec/", limit=100000)]
    n = 0
    for key in keys:
        old = st.get(jar.NS, key)
        if old is None:
            continue
        i = int(key.split("/")[1])
        st.put(jar.NS, "rec/%d" % i, old)
        st.delete(jar.NS, key)
        st.delete(jar.NS, MARK % i)
        person, where = holders.get(i, (None, None))
        if person and where == "own":
            shopkeep.tx_add(person, "remake", i, st)
        _restock(st, i, old)
        n += 1
        say("v3undo %d: back to v2" % i)
    return n


def converted(item_id, store=None):
    return (store or kv.store()).get(jar.NS, BACKUP % item_id) is not None
