"""Jar items made in stages (docs/superpowers/specs/2026-10-11-jar-item-
pipeline-design.md), behind CARDOS_JAR_STAGES=1 (jar.STAGES).

One call used to do everything -- name, behaviour, palette, frames, script
-- and the art was the bulk of the answer; nearly every item came out a
critter walking its zone. Now, per find:

    code:      kind, tier, form, activity        (jar.py: the dice)
    DM:        a concept, from Tibbs's search    (knows almost nothing of the game)
    fitter:    the game sheet                     (translates; may not replace)
    artist  }  palette and frames                 (in parallel)
    behaviour} habits and/or a script, or nothing
    combine -> check_item -> seal -> store        (as before)

A refusal goes back to the stage that made the part at fault, once. The
concept and the search are kept beside the record (concept/ID), never sent
to a device. Every stage is one ask.ask_shape with its own schema, run
with limit=None: a stock counts once, for Tibbs's question.
"""
import json
import re
import threading
import time

from . import ask, jar, jarvm, kv, shopkeep

CONCEPT_LEN = 700
LOOK_LEN = 400
NOTE_LEN = 240
STAGE_TIMEOUT = 600


class Drop(Exception):
    """This find comes to nothing (counted against MAX_FAILS)."""


# ---- the DM --------------------------------------------------------------------------
#
# Plain input and output for now. alex may give the DM a strategy later --
# scenes, roles within a haul, hooks, a campaign notebook -- so everything it
# is told is built here, in dm_prompt, from DM_WORLD and the find's few facts.

DM_WORLD = (
    "Somewhere there is a glass jar with a tiny world inside it: small creatures and small "
    "objects living among moss, pebbles and odd bits of human junk. A junk-shop keeper called "
    "Tibbs goes out searching for things for that world and sells what he finds.")

ACTIVITY_WORDS = {
    "still": "It keeps completely still -- an object, a fixture or a sleeper.",
    "reacts": "It does nothing until something happens to it; then it does something.",
    "ambles": "It moves about now and then, unhurried.",
    "restless": "It is never still for long.",
}

TIER_WORDS = {
    "common": "a cheap trinket", "everyday": "an everyday find",
    "rare": "a rare find", "treasure": "a treasure, the best thing in the shop",
}


def dm_prompt(search, activity, tier_name, earlier=()):
    """Everything the DM is told about one find. It never sees the item
    format, the enums, the player, their jar or the script language."""
    lines = ["You invent things. " + DM_WORLD, ""]
    if search:
        lines.append("Tibbs says where he went searching today:\n\"%s\"" % search)
    else:
        lines.append("Tibbs has not said where he went today; imagine somewhere yourself.")
    lines += [
        "",
        "Invent one thing he found there: %s. %s" % (TIER_WORDS[tier_name],
                                                     ACTIVITY_WORDS[activity]),
    ]
    if earlier:
        lines.append("Already found today, so not these, nor anything like them: %s."
                     % " / ".join(earlier))
    lines += [
        "Write about 80 words: what it is, how it looks, where it came from, and one "
        "surprising detail. Make it its own thing, specific and a little strange.",
        "Keep it friendly for all ages.",
    ]
    return "\n".join(lines)


def dm_schema():
    return {"type": "object", "required": ["concept"], "additionalProperties": False,
            "properties": {"concept": {"type": "string", "minLength": 20,
                                       "maxLength": CONCEPT_LEN, "pattern": "^[ -~\n]+$"}}}


# ---- the fitter ------------------------------------------------------------------------

MOTION_SAID = {
    "travel": {"none": "stays where it is put", "crawl": "creeps along the ground",
               "hop": "hops", "slide": "glides along the ground", "float": "drifts in the air",
               "fly": "flies about"},
    "idle": {"none": "nothing", "sway": "sways", "bob": "bobs up and down",
             "wobble": "wobbles", "spin": "turns round", "pulse": "swells and shrinks",
             "flicker": "flickers (two frames, swapped quickly)",
             "blink": "blinks now and then (an extra frame)"},
}


def fitter_prompt(concept, kind, form, activity, tier):
    e = jar.enums()
    rules = jar.MOTION_RULES[activity]
    lines = [
        jar.GAME,
        "",
        "Fit this concept into the game. Translate it, do not replace it: keep what it is. If "
        "it cannot fit as it is (too big, the wrong sort of thing for its slot), shrink or adapt "
        "it and say how in note.",
        "The concept:\n\"%s\"" % concept,
        "",
        "It must be %s." % jar.KIND_WORDS[kind],
    ]
    if form and (jarvm.V2 or form == "furniture"):
        lines.append(jar.FORM_WORDS[form])
    lines += [
        "%s (%s)" % (ACTIVITY_WORDS[activity], tier[0]),
        "",
        "The game sheet:",
        "- name: up to 12 characters; line: a one-line description, up to 32, playful.",
        "- zone: where it likes to be, one of %s." % ", ".join(e["zones"]),
        "- travel, how it gets about, one of: %s." % "; ".join(
            "%s (%s)" % (t, MOTION_SAID["travel"][t]) for t in rules["travel"]),
        "- idle, what it does in place, one of: %s." % "; ".join(
            "%s (%s)" % (t, MOTION_SAID["idle"][t]) for t in rules["idle"]),
        "- liveliness: calm, normal or jittery -- how often and how strongly the idle plays.",
        "- speed: %s." % " or ".join(rules["speed"]),
        "- bubbles: 0 to 4 short things it might say, up to 12 characters each (none for a "
        "thing that would not speak).",
        "- frames: how many 16x16 pictures it needs, 1 to 4: 1 for most still things; more "
        "only for a flicker, a blink, or a pose a reaction switches to.",
        "- look: a note for the artist, up to %d characters: its shape, colours and size in a "
        "16x16 picture, and what each extra frame shows (\"frame 2 is the flame low\")."
        % LOOK_LEN,
        "- intent: what it does, in plain words, or \"nothing\".",
        "- note: how you adapted the concept, or \"\".",
    ]
    if jarvm.V2:
        lines += [
            "- traits: 1 to 4 words from: %s -- what it is, for other things to react to."
            % ", ".join(jarvm.GROUPS["trait"]),
            "- voice: its sound, one of: %s, or none." % ", ".join(jarvm.GROUPS["sound"]),
        ]
    if form == "furniture":
        lines.append("- surface: the row (0-13, from the top of the picture) of its flat top.")
    lines.append("Keep everything friendly for all ages.")
    return "\n".join(lines)


def fitter_schema(kind, form, activity):
    e = jar.enums()
    rules = jar.MOTION_RULES[activity]
    printable = "^[ -~]+$"
    p = {
        "name": {"type": "string", "minLength": 1, "maxLength": jar.NAME_LEN,
                 "pattern": printable},
        "line": {"type": "string", "minLength": 1, "maxLength": jar.LINE_LEN,
                 "pattern": printable},
        "kind": {"enum": [kind]},
        "zone": {"enum": list(e["zones"])},
        "travel": {"enum": list(rules["travel"])},
        "idle": {"enum": list(rules["idle"])},
        "liveliness": {"enum": list(jar.LIVELY)},
        "speed": {"enum": list(rules["speed"])},
        "bubbles": {"type": "array", "maxItems": jar.MAX_BUB, "items": {
            "type": "string", "minLength": 1, "maxLength": jar.BUB_LEN, "pattern": printable}},
        "frames": {"type": "integer", "minimum": 2 if form == "part" else 1,
                   "maximum": jar.MAX_FRAMES},
        "look": {"type": "string", "minLength": 1, "maxLength": LOOK_LEN},
        "intent": {"type": "string", "minLength": 1, "maxLength": NOTE_LEN},
        "note": {"type": "string", "maxLength": NOTE_LEN},
    }
    req = ["name", "line", "kind", "zone", "travel", "idle", "liveliness", "speed", "bubbles",
           "frames", "look", "intent", "note"]
    if jarvm.V2:
        p["traits"] = {"type": "array", "minItems": 1, "maxItems": 4,
                       "items": {"enum": list(jarvm.GROUPS["trait"])}}
        p["voice"] = {"enum": list(jarvm.GROUPS["sound"]) + ["none"]}
        req += ["traits", "voice"]
    if form == "furniture":
        p["surface"] = {"type": "integer", "minimum": 0, "maximum": 13}
        req.append("surface")
    return {"type": "object", "required": req, "additionalProperties": False, "properties": p}


# ---- the artist ----------------------------------------------------------------------

ART_FORM = {
    "furniture": "It is furniture: draw its top as a flat row running most of its width, at the "
                 "row the sheet gives (surface %s).",
    "scenery": "It is scenery, drawn twice the size behind everything: fill the 16x16 grid.",
    "part": "Its LAST frame is its part alone (small, centred), the others the thing itself.",
    "big": "It is drawn at twice the size: keep the picture bold and simple.",
}


def artist_prompt(sheet, kind, form, tier):
    lines = [
        "Draw a tiny pixel-art sprite for a cosy game set inside a glass jar.",
        "It is %s, called %s: %s." % (jar.KIND_WORDS[kind], sheet["name"], sheet["line"]),
        "How it looks: %s" % sheet["look"],
    ]
    if form in ART_FORM:
        lines.append(ART_FORM[form] % sheet.get("surface", 4) if "%s" in ART_FORM[form]
                     else ART_FORM[form])
    lines += [
        "Draw exactly %d frame%s." % (sheet["frames"], "" if sheet["frames"] == 1 else "s"),
        "",
        "- palette: 8 colours '#rrggbb'. Slot 0 is transparent (its colour is ignored); choose "
        "7 that suit it, with a dark outline colour and highlights.",
        "- frames: each 16 strings of 16 digits 0-7, a digit being a palette slot (0 "
        "transparent). Frame 1 is the shop picture; the others are what the note above asks "
        "for. Draw a clear, recognisable silhouette with an outline, filling a good part of the "
        "grid, using at least three colours; no single colour should cover most of it.",
        "%s Write the JSON compactly, without indentation." % tier[3].split(":")[0],
    ]
    return "\n".join(lines)


def artist_schema(n, form):
    return {"type": "object", "required": ["palette", "frames"], "additionalProperties": False,
            "properties": {
                "palette": {"type": "array", "minItems": 8, "maxItems": 8, "items": {
                    "type": "string", "pattern": "^#[0-9a-fA-F]{6}$"}},
                "frames": {"type": "array", "minItems": n, "maxItems": n, "items": {
                    "type": "array", "minItems": 16, "maxItems": 16, "items": {
                        "type": "string", "pattern": "^[0-7]{16}$"}}}}}


# ---- the behaviour writer ---------------------------------------------------------------

BEHAVIOUR_RULE = {
    "still": "It keeps still. It may do nothing at all -- an empty answer (no habits, no "
             "script) is a good answer for most still things. Give it a reaction only if the "
             "sheet's intent asks for one.",
    "reacts": "It does nothing until something happens to it, then reacts: it MUST have at "
              "least one reaction -- a habit, or a script with an 'on' handler -- to a poke, "
              "a passer-by, the time, the weather, a jar shipping.",
    "ambles": "It already moves about by itself (that is not your job); reactions are optional.",
    "restless": "It already moves about by itself (that is not your job); reactions are "
                "optional.",
}


def behaviour_prompt(sheet, kind, activity, nframes, tier):
    e = jar.enums()
    events, actions = jar._habit_words(e)
    bubbles = sheet.get("bubbles") or []
    lines = [
        "Write the behaviour of a thing in a cosy game set inside a glass jar.",
        "It is %s, called %s: %s." % (jar.KIND_WORDS[kind], sheet["name"], sheet["line"]),
        "What it does: %s" % sheet["intent"],
        "It likes to be at %s; it %s." % (sheet["zone"],
                                          MOTION_SAID["travel"][sheet["travel"]]),
        "Its bubbles: %s." % (", ".join('%d "%s"' % (i + 1, b) for i, b in enumerate(bubbles))
                              if bubbles else "none (it cannot say anything)"),
        "Its frames: %d (frame 1 is how it usually looks)." % nframes,
    ]
    if jarvm.V2 and sheet.get("traits"):
        lines.append("Its traits: %s." % ", ".join(sheet["traits"]))
    lines += [
        "",
        BEHAVIOUR_RULE[activity],
        "",
        "- habits: up to 3 'when this, do that' pairs. when: %s. do: %s. tick's when_arg is "
        "every N quarter-seconds (at least 4); near's when_arg: %s; time's: 0 any, 1 dawn, 2 "
        "day, 3 dusk, 4 night; say's do_arg is a bubble from 0; frame's a frame from 0; glow's "
        "0 off, 1 on, 2 toggle; particle's: %s.%s" % (
            ", ".join(events), ", ".join(actions), jar._numbered(e["near"]),
            jar._numbered(e["particles"]),
            " Hanging things cannot hop or walk." if kind == "hanging" else ""),
        "- script: \"\" or a short behaviour script, at most 12 lines, in the language below. It "
        "runs instead of the habits for the events it handles. Bubble and frame numbers in "
        "scripts count from 1. React to traits, never to names.",
        "Both may be empty. Keep it small and charming, friendly for all ages.",
        "",
        "The script language:",
        jarvm.LANGUAGE_GUIDE_V2 if jarvm.V2 else jarvm.LANGUAGE_GUIDE_V1,
    ]
    return "\n".join(lines)


def behaviour_schema():
    habit = jar.item_schema()["properties"]["habits"]
    return {"type": "object", "required": ["habits", "script"], "additionalProperties": False,
            "properties": {"habits": habit,
                           "script": {"type": "string", "maxLength": jar.SCRIPT_SRC_MAX}}}


# ---- one find ----------------------------------------------------------------------------

def _ask(chat, prompt, schema, person, effort, st):
    return ask.ask_shape(chat, prompt, schema, user=person or "pool", limit=None,
                         timeout=STAGE_TIMEOUT, store=st, effort=effort, model=shopkeep.MODEL,
                         cwd=shopkeep.home())


def combine(sheet, art, beh, kind, form):
    """The three parts as the dict check_item and to_item take."""
    raw = {
        "name": sheet["name"], "line": sheet["line"], "kind": kind,
        "movement": jar.move_fallback(sheet["travel"], sheet["idle"]),
        "speed": sheet["speed"], "zone": sheet["zone"],
        "travel": sheet["travel"], "idle": sheet["idle"], "liveliness": sheet["liveliness"],
        "habits": list(beh.get("habits") or []), "bubbles": list(sheet.get("bubbles") or []),
        "palette": art["palette"], "frames": art["frames"],
    }
    if (beh.get("script") or "").strip():
        raw["script"] = beh["script"]
    if jarvm.V2:
        raw["traits"] = list(sheet.get("traits") or [])
        raw["voice"] = sheet.get("voice", "none")
    if form == "furniture":
        raw["surface"] = sheet.get("surface", 4)
    return raw


def _script_error(raw, form):
    src = (raw.get("script") or "").strip()
    if not src:
        return None
    nframes = len(raw["frames"]) - (1 if form == "part" and len(raw["frames"]) >= 2 else 0)
    try:
        jarvm.prepare_script(src, nbub=len(raw["bubbles"]), nframes=nframes)
        return None
    except jarvm.ScriptError as e:
        return "the script does not work: %s" % e


def make_find(chat, person, kind, tier, form, activity, search, earlier, taken, st, log=None):
    """One find through the stages: (raw, concept, sheet). Drop if it comes
    to nothing."""
    say = log or (lambda s: None)
    try:
        concept = _ask(chat, dm_prompt(search, activity, tier[0], earlier), dm_schema(), person,
                       tier[2], st)["concept"].strip()
    except Exception as e:                              # noqa: BLE001 - the find is dropped
        raise Drop("DM: %s" % e)
    try:
        sheet = _ask(chat, fitter_prompt(concept, kind, form, activity, tier),
                     fitter_schema(kind, form, activity), person, "medium", st)
    except Exception as e:                              # noqa: BLE001
        raise Drop("fitter: %s" % e)
    if sheet.get("note"):
        say("fitter's note on %r: %s" % (sheet["name"], sheet["note"]))

    def draw(extra=""):
        p = artist_prompt(sheet, kind, form, tier) + extra
        return _ask(chat, p, artist_schema(sheet["frames"], form), person, tier[2], st)

    def behave(extra=""):
        p = behaviour_prompt(sheet, kind, activity, sheet["frames"], tier) + extra
        return _ask(chat, p, behaviour_schema(), person, tier[2], st)

    def twice(fn, what):
        try:
            return fn()
        except Exception as e:                          # noqa: BLE001 - asked again, once
            say("%s for %r: %s; again" % (what, sheet["name"], e))
        try:
            return fn()
        except Exception as e:                          # noqa: BLE001
            raise Drop("%s: %s" % (what, e))

    got = {}

    def run(key, fn, what):
        try:
            got[key] = twice(fn, what)
        except Drop as e:
            got[key] = e
    threads = [threading.Thread(target=run, args=("art", draw, "artist"), daemon=True),
               threading.Thread(target=run, args=("beh", behave, "behaviour"), daemon=True)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for key in ("art", "beh"):
        if isinstance(got[key], Drop):
            raise got[key]
    art, beh = got["art"], got["beh"]
    redone = False
    while True:
        raw = combine(sheet, art, beh, kind, form)
        why = jar.check_item(raw, taken, form, activity)
        parts = jar.check_parts(raw, taken, activity) if why else \
            {"sheet": [], "art": [], "behaviour": []}
        if why and not any(parts.values()):             # a schema error: the sheet's
            parts["sheet"] = why
        err = _script_error(raw, form)
        if err:
            parts["behaviour"].append(err)
        if not any(parts.values()):
            return raw, concept, sheet
        if redone:
            if parts["behaviour"] == [err] and not parts["sheet"] and not parts["art"]:
                raw.pop("script", None)                 # as the one-call path: the recipe ships
                if not jar.check_item(raw, taken, form, activity):
                    say("script of %r dropped: %s" % (raw["name"], err))
                    return raw, concept, sheet
            raise Drop("refused after a redo: %s" % "; ".join(
                (parts["sheet"] + parts["art"] + parts["behaviour"])[:3]))
        redone = True
        fix = "\n\nThe last answer was refused: %s. Fix that."
        if parts["sheet"]:
            say("sheet for %r refused: %s" % (sheet["name"], "; ".join(parts["sheet"][:2])))
            try:
                sheet = _ask(chat, fitter_prompt(concept, kind, form, activity, tier)
                             + fix % "; ".join(parts["sheet"][:4]),
                             fitter_schema(kind, form, activity), person, "medium", st)
            except Exception as e:                      # noqa: BLE001
                raise Drop("fitter, again: %s" % e)
            if len(art["frames"]) != sheet["frames"]:   # the artist must draw to the new sheet
                parts["art"].append("draw %d frames" % sheet["frames"])
        if parts["art"]:
            say("art for %r refused: %s" % (sheet["name"], "; ".join(parts["art"][:2])))
            try:
                art = draw(fix % "; ".join(parts["art"][:4]))
            except Exception as e:                      # noqa: BLE001
                raise Drop("artist, again: %s" % e)
        if parts["behaviour"]:
            say("behaviour for %r refused: %s" % (sheet["name"],
                                                  "; ".join(parts["behaviour"][:2])))
            try:
                beh = behave(fix % "; ".join(parts["behaviour"][:4]))
            except Exception as e:                      # noqa: BLE001
                raise Drop("behaviour, again: %s" % e)


# ---- a stock's new finds -------------------------------------------------------------------

def generate(chat, person, req, date, store=None, signer=None, now=None, log=None,
             on_item=None, want=None, kind_at=0, search="", rng=None, force_form=None):
    """jar.generate, in stages: new items for `person` (None: the pool),
    [signed records]. Each find's kind, tier, form and activity are rolled
    first; a find that comes to nothing is dropped, MAX_FAILS of them in
    all. Fewer than asked for is fewer, not a failure."""
    import random
    st = store or kv.store()
    now = time.time() if now is None else now
    rng = rng or random.Random()
    want = jar.ITEMS if want is None else want
    owned = [n.lower() for n in req.get("owned", [])]
    say = log or (lambda s: None)
    if not search:
        say("no search from Tibbs: the DM works from the rolls alone")
    names, concepts, records, fails, last = [], [], [], 0, None
    for k in range(want):
        if fails >= jar.MAX_FAILS:
            break
        kind = jar.KIND_PLAN[(kind_at + k) % len(jar.KIND_PLAN)]
        tier = jar.pick_tier(rng, req.get("bribe", 0) if req.get("finds") else 0)
        form = force_form or (jar.roll_form(kind, rng) if jarvm.V2 else None)
        activity = jar.roll_activity(rng)
        if form in ("scenery", "furniture") and activity in ("ambles", "restless"):
            activity = "still"                          # a backdrop or a shelf does not wander
        taken = set(owned) | {n.strip().lower() for n in names}
        try:
            raw, concept, sheet = make_find(chat, person, kind, tier, form, activity, search,
                                            concepts, taken, st, log)
        except Drop as e:
            fails += 1
            last = e
            say("find %d dropped: %s" % (k + 1, e))
            continue
        names.append(raw["name"])
        concepts.append(_short(concept))
        item_id = jar.ID_BASE + st.incr(jar.NS, "next_id")
        rec = jar.seal(jar.encode(jar.to_item(raw, item_id, now, jar.record_tags([tier[0], kind]),
                                              log, tier[1], form)), signer)
        st.put(jar.NS, "rec/%d" % item_id, rec)
        st.put(jar.NS, "concept/%d" % item_id, json.dumps({
            "concept": concept, "search": search, "activity": activity,
            "intent": sheet.get("intent", ""), "note": sheet.get("note", ""),
            "look": sheet.get("look", "")}).encode())
        if person:
            st.put(jar.NS, "own/%d" % item_id, person.encode())
        records.append(rec)
        say("%s: %s %s %s, %d coins, %s/%s/%s" % (
            raw["name"], tier[0], activity, kind, tier[1], raw["travel"], raw["idle"],
            raw["liveliness"]))
        if on_item:
            on_item(len(records) - 1, rec)
    if not records:
        raise ValueError("no find came through the stages: %s" % (last or "none asked"))
    return records


def concept_of(item_id, store=None):
    """What the DM wrote for an item, and where Tibbs was: a dict, or None."""
    raw = (store or kv.store()).get(jar.NS, "concept/%d" % item_id)
    return json.loads(raw) if raw else None


def _short(concept):
    """A concept's first sentence, for the DM's list of today's finds."""
    s = re.split(r"(?<=[.!?])\s", concept.strip(), maxsplit=1)[0]
    return s[:140]
