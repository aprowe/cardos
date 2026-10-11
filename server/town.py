"""The lane's trades: what Pip, Bramble and Wren do when a deal is struck in
talk (server/shopkeep.py has the people; server/jar.py the items).

A device that talks says what it has, lines before the words:
    coins N
    have spores=S nectar=N twigs=T
    beds berry1,reed2,shroom1,flower3     the beds in order: plant and level
The NPC sees that (and the player's things, from the ledger) and may end a
reply with a DEAL line. Checked here; what it does reaches the device as the
effects every deal uses (shopkeep.tx_add), each carried out once:
    use<res> N     resources spent (usespores, usenectar, usetwigs)
    pay N          coins to them
    remake ID      the item changed -- fetch its new record (GET /jar/item?id=)
    give ID        a new item of yours -- fetch it the same way
    bedup K        bed K (from 0) is a level better
Pip's alterations and Bramble's evolutions are the same item made again by
Claude from the old one and the request: the same id, a new signed record.
Wren's furniture is a new item, made to the description. Each is a job on a
thread; the effect is queued when it is done, and the lane hears about it.
"""
import random
import sys
import threading
import time

from . import ask, jar, jarvm, kv, shopkeep, wire

RES_MAX = 99
HOW_LEN = 160
BED_LEVEL_MAX = 3
BED_COST = {2: 3, 3: 6}
PLANTS = ("berry", "reed", "shroom", "flower")
PLANT_RESOURCE = {"berry": "jam", "reed": "twigs", "shroom": "spores", "flower": "nectar"}


def parse_have(body):
    """The device's lines before its words -> (coins, have, beds, words)."""
    coins, have, beds = None, None, []
    lines = body.split("\n")
    while lines:
        head = lines[0].strip()
        if head.startswith("coins ") and head[6:].strip().isdigit():
            coins = int(head[6:].strip())
        elif head.startswith("have "):
            have = {}
            for part in head[5:].split():
                k, _, v = part.partition("=")
                if k in shopkeep.RESOURCES and v.isdigit():
                    have[k] = min(int(v), 9999)
        elif head.startswith("beds "):
            for part in head[5:].split(","):
                part = part.strip()
                name = part.rstrip("0123456789")
                lvl = part[len(name):]
                if name in PLANTS and lvl.isdigit():
                    beds.append((name, int(lvl)))
        else:
            break
        lines.pop(0)
    return coins, have, beds[:8], "\n".join(lines)


def context(person, coins, have, beds, store=None):
    """What an NPC is told the customer has."""
    parts = [jar.deal_context(person, coins, store)]
    if have is not None:
        parts.append("Their stores: %s." % ", ".join("%d %s" % (have.get(r, 0), r)
                                                   for r in shopkeep.RESOURCES))
    if beds:
        parts.append("Their garden beds: %s." % ", ".join(
            "%d: %s, level %d" % (i + 1, n, lv) for i, (n, lv) in enumerate(beds)))
    return " ".join(parts)


def _spend(deal, res, have):
    s = deal.get("spend") if isinstance(deal.get("spend"), dict) else {}
    try:
        n = int(s.get(res) or 0)
    except (TypeError, ValueError):
        n = 0
    n = max(0, min(RES_MAX, n))
    if have is not None and n > have.get(res, 0):
        return None
    return n


def _int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def make_deal(who, person, deal, coins=None, have=None, beds=(), chat=None, store=None,
              sync=False):
    """Carry out a deal Pip, Bramble or Wren struck: None, or why it cannot be."""
    st = store or kv.store()
    res = {"pip": "spores", "bramble": "nectar", "wren": "twigs"}.get(who)
    if not res:
        return "nobody here takes that kind of deal"
    n = _spend(deal, res, have)
    if n is None:
        return "they have only %d %s" % ((have or {}).get(res, 0), res)
    take = max(0, min(jar.BRIBE_MAX, _int(deal.get("take")) or 0))
    if coins is not None and take > coins:
        return "they have only %d coins" % coins
    mine = {i for i, _name, _p in jar.owned_by(person, st)}
    job = None
    if who == "pip" and isinstance(deal.get("modify"), dict):
        item = _int(deal["modify"].get("item"))
        if item not in mine:
            return "they do not own item %s" % deal["modify"].get("item")
        job = ("remake", item, wire.flat(str(deal["modify"].get("how") or ""), HOW_LEN, ascii=True),
               "modify")
    elif who == "bramble" and isinstance(deal.get("evolve"), dict):
        item = _int(deal["evolve"].get("item"))
        if item not in mine:
            return "they do not own item %s" % deal["evolve"].get("item")
        rec = st.get(jar.NS, "rec/%d" % item)
        if rec is None or jar.decode(rec)["kind"] != jar.enums()["kinds"]["critter"]:
            return "only a critter can be evolved"
        job = ("remake", item, wire.flat(str(deal["evolve"].get("how") or ""), HOW_LEN, ascii=True),
               "evolve")
    elif who == "bramble" and isinstance(deal.get("grow"), dict):
        bed = _int(deal["grow"].get("bed"))
        if bed is None or not 1 <= bed <= max(len(beds), 1) or (beds and bed > len(beds)):
            return "there is no bed %s" % deal["grow"].get("bed")
        level = beds[bed - 1][1] if beds else 1
        if level >= BED_LEVEL_MAX:
            return "that bed is as good as it gets"
        if n < BED_COST[level + 1]:
            return "growing it to level %d is %d nectar" % (level + 1, BED_COST[level + 1])
        job = ("bedup", bed - 1, "", "grow")
    elif who == "wren" and isinstance(deal.get("build"), dict):
        what = wire.flat(str(deal["build"].get("what") or ""), HOW_LEN, ascii=True)
        if not what or not jar.clean(what):
            return "build what?"
        job = ("build", 0, what, "build")
    if not job:
        return "no job in it"
    if n:
        shopkeep.tx_add(person, "use" + res, n, st)
    if take:
        shopkeep.tx_add(person, "pay", take, st)
    kind, arg, how, verb = job
    if kind == "bedup":
        shopkeep.tx_add(person, "bedup", arg, st)
        shopkeep.town_note("%s grew %s's garden bed %d a level." % (
            shopkeep.name_of(who), shopkeep.display(person), arg + 1), st, besides=who)
        return None
    run = (lambda: _job(chat, who, person, kind, arg, how, verb, st, n))
    if sync:
        run()
    else:
        threading.Thread(target=run, daemon=True).start()
    sys.stderr.write("jar: %s: %s took a job: %s %s %s\n" % (person, who, verb, arg, how))
    return None


# ---- the jobs ---------------------------------------------------------------------------

def _rows(frame):
    px = jar.unpack_frame(frame)
    return ["".join(str(v) for v in px[r * 16:(r + 1) * 16]) for r in range(16)]


def _form_of(it):
    if it.get("role") == 1:
        return "furniture"
    if it.get("role") == 2:
        return "scenery"
    if it.get("part"):
        return "part"
    if it.get("scale", 1) == 2:
        return "big"
    return None


def remake_prompt(old, how, verb, kind_name):
    e = jar.enums()
    moves = {v: k for k, v in e["moves"].items()}
    speeds = {v: k for k, v in e["speeds"].items()}
    zones = {v: k for k, v in e["zones"].items()}
    pal = ["#%02x%02x%02x" % (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                              (c & 31) * 255 // 31) for c in old["pal"]]
    lines = [
        jar.GAME,
        "",
        ("A tinkerer is altering one of a player's things." if verb == "modify" else
         "A grower is helping one of a player's critters grow into its next form."),
        "The thing as it is now:",
        "  name: %s; line: %s; kind: %s; movement: %s; speed: %s; zone: %s; traits: %s" % (
            old["name"], old["line"], kind_name, moves.get(old["move"], "sits"),
            speeds.get(old["speed"], "medium"), zones.get(old["zone"], "anywhere"),
            old.get("tags") or "none"),
        "  bubbles: %s" % ", ".join('"%s"' % b for b in old["bubbles"]),
        "  palette: %s" % " ".join(pal),
    ]
    for k, f in enumerate(old["frames"]):
        lines.append("  frame %d: %s" % (k, " ".join(_rows(f))))
    lines += [
        "",
        ("What the player asked for: \"%s\"." % how) if how else "Nothing in particular was asked.",
        ("Make the altered thing: the SAME thing, recognisably, changed as asked -- keep its "
         "name (or a small variation), keep most of its picture and colours, and change what "
         "the request says. Give it a better script if that suits." if verb == "modify" else
         "Make its evolved form: the same creature, grown -- a new name that echoes the old one, "
         "a bolder picture that is clearly the same lineage, a richer behaviour script, and "
         "something it can do now that it could not before."),
        "Keep its kind (%s)." % kind_name,
    ]
    return "\n".join(lines)


def _job(chat, who, person, kind, arg, how, verb, st, spent=0):
    """A job, done: the effect queued and the lane told; on failure the
    resources come back (use<res> is undone with a gain)."""
    res = {"pip": "spores", "bramble": "nectar", "wren": "twigs"}[who]
    try:
        if kind == "remake":
            name = remake(chat, person, arg, how, verb, st)
            shopkeep.tx_add(person, "remake", arg, st)
            shopkeep.town_note("%s %s %s's %s%s." % (
                shopkeep.name_of(who), "altered" if verb == "modify" else "evolved",
                shopkeep.display(person), name, (" (" + how + ")") if how else ""), st)
        else:
            item_id, name = build(chat, person, how, st)
            shopkeep.tx_add(person, "give", item_id, st)
            shopkeep.town_note("Wren built %s a %s: %s." % (shopkeep.display(person), name, how), st)
    except Exception as e:                              # noqa: BLE001 - the customer is told
        sys.stderr.write("jar: %s: %s's job failed: %s\n" % (person, who, e))
        if spent:
            shopkeep.tx_add(person, "gain" + res, spent, st)   # what was paid comes back
        shopkeep.note("The job for %s fell through (%s); you owe them a redo." % (
            shopkeep.display(person), str(e)[:80]), st, who)


def remake(chat, person, item_id, how, verb, store=None):
    """The item made again from the old one and the request: same id, new
    signed record, its memory reset. Its new name."""
    st = store or kv.store()
    old = jar.decode(st.get(jar.NS, "rec/%d" % item_id))
    kinds = {v: k for k, v in jar.enums()["kinds"].items()}
    kind_name = kinds[old["kind"]]
    form = _form_of(old)
    tier = ("rare", old["price"] or 200, "high",
            "Take care over it: a detailed sprite, two or three frames, distinctive habits and a "
            "script with personality.")
    last = None
    full = jar.prompt_for("the thing above", kind=kind_name, tier=tier, form=form)
    fields = full[full.index("Fields:"):]               # the fields and the language guide
    for _ in range(3):
        got = ask.ask_shape(chat, remake_prompt(old, how, verb, kind_name) + "\n\n" + fields,
                            jar.batch_schema(1, kind_name, form), user=person, store=st,
                            effort="high", model=shopkeep.MODEL, cwd=shopkeep.home(
                                "pip" if verb == "modify" else "bramble"))
        raw = (got.get("items") or [None])[0]
        why = jar.check_item(raw, (), form) if isinstance(raw, dict) else ["no item"]
        if not why:
            break
        last = why
    else:
        raise ValueError("; ".join((last or ["no item"])[:2]))
    it = jar.to_item(raw, item_id, time.time(), old.get("tags") or "", None, old["price"], form)
    if not jarvm.V2 or not it.get("tags"):
        it["tags"] = old.get("tags") or it.get("tags", "")
    rec = jar.seal(jar.encode(it))
    st.put(jar.NS, "rec/%d" % item_id, rec)
    return raw["name"]


def build(chat, person, what, store=None):
    """Furniture to the description: a new item of theirs. (id, name)."""
    st = store or kv.store()
    recs = jar.generate(chat, person, {"owned": []}, None, store=st, want=1, kind_at=1,
                        briefs=["made to order by Wren the carpenter: " + what],
                        rng=random.Random(), force_form="furniture")
    if not recs:
        raise ValueError("nothing came of it")
    it = jar.decode(recs[0])
    return it["id"], it["name"]
