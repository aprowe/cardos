"""Prompt Lab: the staged item pipeline (server/jarstages.py) on a bench, with
a behaviour writer for Jarscript 3 (tools/jarsandbox) beside today's.

serve.py is the HTTP side; this is everything else, importable by the tests.

Safety, before anything else in this file runs:
- CARDOS_STATE is pointed at a fresh temporary directory (PROMPTLAB_STATE
  names one instead) BEFORE any server module is imported, so a Tibbs with
  no past, an empty store and no signing key are all it can see. The real
  state (CARDOS_STATE as it was, or ~/.cardos) is never opened.
- Nothing is put in the pool, the ledger or rec/ID: finds are made here, not
  through jarstages.generate. Records are sealed with a dummy signer (64 zero
  bytes); sign.py is never called, so no key is made or read.
- server/ is only read: its prompt builders, schemas, rolls and checks are
  called as they are, so the defaults here are what the server would send.

Prompts are edited as templates. A stage's default prompt is the server's,
with the values that flow into it from upstream (the search, the concept,
the sheet's words, the language guide) turned back into {{placeholders}}:
render(template, values) gives the server's prompt back exactly. An edited
template therefore still follows its inputs when a stage upstream runs again.
What is not a placeholder (the lines a roll adds or leaves out) is frozen in
an edit; the page says when the default has moved under one.
"""
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from contextlib import contextmanager

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SANDBOX = os.path.join(ROOT, "tools", "jarsandbox")


def _scratch_state():
    real = os.path.realpath(os.environ.get("CARDOS_STATE") or os.path.expanduser("~/.cardos"))
    state = os.environ.get("PROMPTLAB_STATE") or tempfile.mkdtemp(prefix="promptlab-state-")
    os.makedirs(state, exist_ok=True)
    if os.path.realpath(state) == real or os.path.realpath(state) == os.path.realpath(
            os.path.expanduser("~/.cardos")):
        raise SystemExit("prompt lab: refusing to run in the real state directory %s" % state)
    os.environ["CARDOS_STATE"] = state
    return state, real


STATE, REAL_STATE = _scratch_state()                    # before any server import
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from server import ask, jar, jarlab, jarstages, jarvm, shopkeep, wire   # noqa: E402
from server import chat as chatmod                                # noqa: E402

STAGES = ("tibbs", "dm", "fitter", "artist", "behaviour", "behaviour_js3")
VARIANTS = {"jarvm": "behaviour", "js3": "behaviour_js3"}
STAGE_TIMEOUT = jarstages.STAGE_TIMEOUT
EXP_DIR = os.path.join(HERE, "experiments")
EXP_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.-]{0,47}$")
JS3_MD = os.path.join(SANDBOX, "jarscript3.md")
JS3_CHECK = os.path.join(HERE, "js3check.js")
HISTORY_MAX = 100
BATCH_MAX = 12


def dummy_signer(message):
    """Never a real signature: 64 zero bytes."""
    return bytes(64)


# ---- configuration (serve.py sets it) ------------------------------------------------

class Config:
    dry = False
    dry_delay = 0.0
    model = shopkeep.MODEL
    v2 = bool(jarvm.V2)
    chat = None
    node = shutil.which("node")


CFG = Config()


def configure(dry=False, dry_delay=0.0, claude=None, model=None, v2=None):
    CFG.dry = bool(dry)
    CFG.dry_delay = float(dry_delay or 0)
    CFG.model = model or shopkeep.MODEL
    CFG.v2 = bool(jarvm.V2) if v2 is None else bool(v2)
    CFG.chat = chatmod.ChatService(claude=claude) if claude else chatmod.ChatService()
    return CFG


# ---- jarvm.V2 for the pure calls -----------------------------------------------------
#
# fitter_prompt, behaviour_prompt, combine, check_item and to_item read jarvm.V2.
# The lab may run either version; it is set under one lock for the few
# milliseconds those calls take, never across a Claude call.

_v_lock = threading.RLock()


@contextmanager
def v2ctx(v2):
    with _v_lock:
        old = jarvm.V2
        jarvm.V2 = bool(CFG.v2 if v2 is None else v2)
        try:
            yield
        finally:
            jarvm.V2 = old


# ---- the dice ------------------------------------------------------------------------

def tier_tuple(rolls):
    return jarlab.tier_for(rolls.get("tier") or "everyday", rolls.get("price"))


def roll(seed=None, k=0, bribe=0, v2=None, kind=None):
    """One find's dice, in the order jarstages.generate rolls them: kind from
    KIND_PLAN, tier, form (version 2 only), activity; a backdrop or a shelf
    does not wander."""
    seed = random.SystemRandom().randrange(1 << 30) if seed in (None, "") else int(seed)
    v2 = CFG.v2 if v2 is None else bool(v2)
    rng = random.Random(seed)
    k = int(k or 0)
    kind = kind or jar.KIND_PLAN[k % len(jar.KIND_PLAN)]
    tier = jar.pick_tier(rng, int(bribe or 0))
    form = jar.roll_form(kind, rng) if v2 else None
    activity = jar.roll_activity(rng)
    if form in ("scenery", "furniture") and activity in ("ambles", "restless"):
        activity = "still"
    source = jarstages.roll_source(rng)
    return {"seed": seed, "k": k, "kind": kind, "tier": tier[0], "price": tier[1],
            "effort": tier[2], "form": form or "", "activity": activity, "v2": v2,
            "source": source}


def _rolls_seq(n, seed, kind_at=0, bribe=0, v2=None):
    """n finds' dice from one generator, as generate rolls a stock."""
    v2 = CFG.v2 if v2 is None else bool(v2)
    rng = random.Random(seed)
    out = []
    for k in range(n):
        kind = jar.KIND_PLAN[(kind_at + k) % len(jar.KIND_PLAN)]
        tier = jar.pick_tier(rng, bribe)
        form = jar.roll_form(kind, rng) if v2 else None
        activity = jar.roll_activity(rng)
        if form in ("scenery", "furniture") and activity in ("ambles", "restless"):
            activity = "still"
        source = jarstages.roll_source(rng)
        out.append({"source": source, "seed": seed, "k": kind_at + k, "kind": kind, "tier": tier[0],
                    "price": tier[1], "effort": tier[2], "form": form or "",
                    "activity": activity, "v2": v2})
    return out


def dice():
    return {
        "kind_plan": list(jar.KIND_PLAN),
        "kinds": {k: w for k, w in jar.KIND_WORDS.items()},
        "tiers": [{"name": n, "chance": c, "lo": lo, "hi": hi, "effort": e, "words": w}
                  for n, c, (lo, hi), e, w in jar.TIERS],
        "forms": [{"name": n, "chance": c, "kinds": list(k), "words": jar.FORM_WORDS.get(n, "")}
                  for n, c, k in jar.FORMS],
        "activities": [{"name": n, "chance": p, "words": jarstages.ACTIVITY_WORDS[n]}
                       for n, p in jar.ACTIVITIES],
        "travels": list(jar.TRAVELS), "idles": list(jar.IDLES), "lively": list(jar.LIVELY),
        "motion_rules": {a: {k: list(v) for k, v in r.items()} for a, r in jar.MOTION_RULES.items()},
        "motion_said": jarstages.MOTION_SAID,
        "zones": list(jar.enums()["zones"]),
        "traits": list(jarvm.GROUPS["trait"]), "sounds": list(jarvm.GROUPS["sound"]),
    }


# ---- templates ---------------------------------------------------------------------

PLACEHOLDER = re.compile(r"\{\{([a-z_0-9]+)\}\}")
DROP_IF_EMPTY = {"earlier", "form_words", "art_form", "form_line", "traits_line"}


def templatize(prompt, values):
    """The prompt with each value (longest first) put back as {{key}}."""
    out = prompt
    for key, val in sorted(values.items(), key=lambda kv: -len(kv[1] or "")):
        if val and len(val) >= 3:
            out = out.replace(val, "{{%s}}" % key)
    return out


def render(template, values):
    """(text, missing keys). A line whose only purpose was an empty value
    (the earlier finds, a form's line) is left out."""
    missing = []
    lines = []
    for line in template.split("\n"):
        keys = PLACEHOLDER.findall(line)
        if any(k in DROP_IF_EMPTY and k in values and not values[k] for k in keys):
            continue
        lines.append(line)
    text = "\n".join(lines)

    def sub(m):
        k = m.group(1)
        if k in values:
            return values[k]
        missing.append(k)
        return m.group(0)
    return PLACEHOLDER.sub(sub, text), sorted(set(missing))


# ---- the stages' contexts ----------------------------------------------------------

def stand_in_sheet(kind, form, activity):
    """A sheet for previewing the artist's and behaviour writer's prompts
    before the fitter has run."""
    rules = jar.motion_rules(activity, kind)
    e = jar.enums()
    s = {"name": "Sample", "line": "the fitter's line goes here", "kind": kind,
         "zone": list(e["zones"])[0], "travel": rules["travel"][0], "idle": rules["idle"][0],
         "liveliness": "calm", "speed": rules["speed"][0], "bubbles": [],
         "frames": 2 if form == "part" else 1, "look": "the fitter's note for the artist",
         "intent": "the fitter's plain words for what it does", "note": ""}
    if jarvm.V2:
        s["traits"] = [list(jarvm.GROUPS["trait"])[0]]
        s["voice"] = "none"
    if form == "furniture":
        s["surface"] = 4
    return s


def js3_reference():
    with open(JS3_MD, encoding="utf-8") as f:
        md = f.read()
    md = re.sub(r"<!--.*?-->", "", md, flags=re.S)
    md = re.sub(r"^# .*\n", "", md, count=1, flags=re.M)
    return md.strip()


def frame_names(n):
    return ["f%d" % (i + 1) for i in range(max(1, int(n or 1)))]


JS3_TEMPLATE = """Write the behaviour of a thing in a cosy game set inside a glass jar, as a script in Jarscript 3, the language described below.
It is {{kind_words}}, called {{name}}: {{line}}.
What it does: {{intent}}
How it moves -- already chosen, and the script must make it so: {{motion}}.
{{bubbles_line}}
Its frames are named {{frame_names}} (f1 is how it usually looks; any others are poses or animation drawn for it).
{{traits_line}}
{{form_line}}

{{activity_rule}}

The jar it lives in already holds:
{{jar}}
{{tie}}

Make it part of the jar's life, as the two examples at the end are:
- A body that keeps living: states whose bodies take time and loop, with pick, random and chance, so it never does exactly the same thing twice. Even a still thing answers when something happens: it rustles, glows, rings, emits.
- At least one link with the jar above, by trait and never by name: go to their perches (move to perch TRAIT), stand on their tops, listen for the signals they send (on signal NAME), send the ones they listen for, or notice them coming (on near TRAIT).
- Something for others: if it has a top, a branch, a roof, a rim or a hollow, declare solid and perch points; and when something happens to it (a visitor lands, it is poked, it wakes), send a signal saying so -- a new signal name is fine, later things will learn to answer it.
- Poke does something worth seeing.

- script: the whole script, at most 50 lines. Begin with its traits line. The first state is where it starts. Timed things (wait, move, walk, hop, play ... once) only in a state's body; a handler runs within one tick, so it may only set things, say, emit, sway, shake, signal or go to a state. Close every block with end.
- notes: one line on what you made it do.
Keep it small and charming, friendly for all ages.

The language:
{{reference}}

Two scripts from the jar, to show the kind of life wanted -- not to copy:
{{examples}}"""

JS3_RULE = {
    "still": "It keeps still: a state that shows f1 (an idle may cycle frames, sway or shake gently "
             "now and then) and nothing else, unless its intent asks for a reaction.",
    "reacts": "It does nothing until something happens to it, then reacts: it MUST have at least one "
              "handler -- on poke, on near TRAIT, on signal, on bump or on every -- and a reaction "
              "may go to a state that acts and then goes back.",
    "ambles": "It moves about now and then, unhurried: its state loops, moving a little and pausing "
              "a few seconds. Reactions are optional.",
    "restless": "It is never still for long: its state loops with short pauses. Reactions are "
                "optional.",
}

JS3_FORM = {
    "big": "It is drawn at twice the size: declare scale 2.",
    "scenery": "It is scenery, drawn at twice the size behind everything: declare scale 2 and "
               "layer 0; it does not move.",
    "furniture": "It is furniture: its flat top is row %d of its 16x16 picture; declare solid 1, "
                 "%d, 14, %d so others can stand on it, and two or three perch points along that "
                 "row.",
    "part": "Its last frame is a part of it that leaves home and comes back (a bird from a nest, a "
            "spark from a lamp); this jar has no separate parts, so show it with frames.",
}


def js3_schema():
    return {"type": "object", "required": ["script", "notes"], "additionalProperties": False,
            "properties": {"script": {"type": "string", "minLength": 10, "maxLength": 4000},
                           "notes": {"type": "string", "maxLength": 300}}}


def _motion_words(sheet):
    ms = jarstages.MOTION_SAID
    return "it %s (travel %s); in place: %s (idle %s), %s; speed %s" % (
        ms["travel"].get(sheet.get("travel"), sheet.get("travel")), sheet.get("travel"),
        ms["idle"].get(sheet.get("idle"), sheet.get("idle")), sheet.get("idle"),
        sheet.get("liveliness", "calm"), sheet.get("speed", "slow"))


def _bubbles_words(sheet):
    b = sheet.get("bubbles") or []
    if not b:
        return "It says nothing (no speech bubbles)."
    return "Things it may say, with say \"...\": %s." % ", ".join('"%s"' % x for x in b)


def js3_values(sheet, kind, form, activity):
    if kind == "hanging":
        form_line = ("It hangs from the jar's lid: it stays where it is put -- no walk, hop, move "
                     "or physics; sway, shake and frames are fine.")
    elif form == "furniture":
        s = int(sheet.get("surface", 4))
        form_line = JS3_FORM["furniture"] % (s, s, max(1, 16 - s))
    else:
        form_line = JS3_FORM.get(form, "")
    traits = sheet.get("traits") or []
    return {
        "kind_words": jar.KIND_WORDS[kind], "name": sheet["name"], "line": sheet["line"],
        "intent": sheet.get("intent", ""), "motion": _motion_words(sheet),
        "bubbles_line": _bubbles_words(sheet),
        "frame_names": ", ".join(frame_names(sheet.get("frames"))),
        "traits_line": ("Its traits (others react to these): %s." % ", ".join(traits)) if traits else "",
        "form_line": form_line, "activity_rule": JS3_RULE[activity],
        "reference": js3_reference(),
        "jar": "", "tie": "", "examples": js3_examples(),
    }


# ---- the jar a find joins ----------------------------------------------------------
#
# Each resident is described to the DM and the behaviour writer by what its
# script really offers -- perches, a top, the signals it sends and hears, what
# it notices -- read from the script by code, so a later find can answer an
# earlier one. The four sandbox examples are the jar a run starts from.

SANDBOX_HTML = os.path.join(SANDBOX, "index.html")
RESIDENT_KEYS = ("tree", "fence", "rolly", "bird")
RESIDENT_ABOUT = {"tree": ("Sapling", "a young tree; birds perch in it"),
                  "fence": ("Fence", "a little wooden fence"),
                  "rolly": ("Rolly Polly", "a pill bug that rolls up into a ball"),
                  "bird": ("Wren", "a wren that swoops between perches")}


def sandbox_scripts():
    """The example scripts in tools/jarsandbox/index.html, by key."""
    with open(SANDBOX_HTML, encoding="utf-8") as f:
        t = f.read()
    blk = t[t.index("const SCRIPTS = {"):t.index("const PALS")]
    return {m.group(1): m.group(2) for m in
            re.finditer(r"^(\w+): `(.*?)`,?[ \t]*$", blk, re.S | re.M)}


def contract(name, line, script):
    """What a script offers the jar, read from the script."""
    c = {"name": name, "line": line, "traits": [], "perches": 0, "solid": False,
         "sends": [], "hears": [], "notices": [], "visits": [], "events": []}

    def add(k, v):
        if v not in c[k]:
            c[k].append(v)
    for raw in (script or "").split("\n"):
        s = raw.split("#", 1)[0].strip()
        if s.startswith("traits "):
            c["traits"] = [x.strip() for x in s[7:].split(",") if x.strip()]
        elif s.startswith("perch "):
            c["perches"] += 1
        elif s.startswith("solid "):
            c["solid"] = True
        m = re.match(r"on signal (\w+)", s)
        if m:
            add("hears", m.group(1))
        m = re.match(r"on near (\w+)", s)
        if m:
            add("notices", m.group(1))
        m = re.match(r"on (poke|bump|hit|land)\b", s)
        if m:
            add("events", m.group(1))
        body = re.sub(r"^on signal \w+", "", s)
        for m in re.finditer(r"\bsignal (\w+)", body):
            add("sends", m.group(1))
        for m in re.finditer(r"move to perch (\w+)", s):
            add("visits", m.group(1))
    return c


def describe(c):
    """One line for the DM: name, what it is, its traits."""
    return "%s: %s" % (c["name"], c["line"])


def describe_full(c):
    """One line for the behaviour writer: what it offers and answers."""
    bits = []
    if c["perches"] or c["solid"]:
        bits.append("offers %s" % " and ".join(
            ([("%d perch%s" % (c["perches"], "es" if c["perches"] > 1 else ""))]
             if c["perches"] else []) + (["a solid top"] if c["solid"] else [])))
    if c["visits"]:
        bits.append("flies to the perches of %s" % ", ".join(c["visits"]))
    if c["sends"]:
        bits.append("sends signal %s" % ", ".join(c["sends"]))
    if c["hears"]:
        bits.append("answers signal %s" % ", ".join(c["hears"]))
    if c["notices"]:
        bits.append("notices things with trait %s" % ", ".join(c["notices"]))
    if c["events"]:
        bits.append("reacts to %s" % ", ".join(c["events"]))
    return "- %s, %s (traits %s): %s." % (c["name"], c["line"], ", ".join(c["traits"]) or "none",
                                          "; ".join(bits) or "keeps to itself")


_RESIDENTS = None


def default_residents():
    global _RESIDENTS
    if _RESIDENTS is None:
        sc = sandbox_scripts()
        _RESIDENTS = [contract(RESIDENT_ABOUT[k][0], RESIDENT_ABOUT[k][1], sc[k])
                      for k in RESIDENT_KEYS]
    return [dict(c) for c in _RESIDENTS]


def js3_examples():
    sc = sandbox_scripts()
    return "\n".join("```\n%s```" % sc[k] for k in ("tree", "bird"))


def jar_values(inp):
    """The {{jar}} and {{tie}} a find's behaviour is written against."""
    res = inp["jar"]
    tie = inp["rolls"].get("tie") or ""
    return {"jar": "\n".join(describe_full(c) for c in res) or "- nothing yet",
            "tie": ("Its story with them: it %s." % tie) if tie else ""}


def _norm_inputs(inp):
    inp = dict(inp or {})
    r = dict(inp.get("rolls") or {})
    r.setdefault("kind", "critter")
    r.setdefault("tier", "everyday")
    r.setdefault("activity", "still")
    r["form"] = r.get("form") or None
    r["source"] = r.get("source") or ""
    r["tie"] = r.get("tie") or ""
    inp["jar"] = default_residents() if inp.get("jar") is None else list(inp["jar"])
    if r["kind"] not in jar.KIND_WORDS:
        raise ValueError("no kind %r" % r["kind"])
    if r["form"] and r["form"] not in jar.FORM_WORDS:
        raise ValueError("no form %r" % r["form"])
    if r["activity"] not in jarstages.ACTIVITY_WORDS:
        raise ValueError("no activity %r" % r["activity"])
    inp["rolls"] = r
    inp["search"] = inp.get("search") or ""
    inp["earlier"] = [str(x) for x in inp.get("earlier") or [] if str(x).strip()]
    inp["taken"] = [str(x).strip().lower() for x in inp.get("taken") or [] if str(x).strip()]
    inp["concept"] = inp.get("concept") or ""
    inp["person"] = inp.get("person") or "lab"
    return inp


def _sheet(inp, kind, form, activity):
    s = inp.get("sheet")
    if isinstance(s, str) and s.strip():
        s = json.loads(s)
    if isinstance(s, dict) and s.get("name"):
        return dict(s), False
    return stand_in_sheet(kind, form, activity), True


def stage_ctx(stage, inp, v2=None):
    """What one stage would be sent for these inputs: {default, template,
    values, schema, effort, system, append_schema, stand_in}."""
    inp = _norm_inputs(inp)
    r = inp["rolls"]
    kind, form, activity = r["kind"], r["form"], r["activity"]
    tier = tier_tuple(r)
    ctx = {"stage": stage, "system": ask.SYSTEM, "append_schema": True, "stand_in": []}
    with v2ctx(v2):
        if stage == "tibbs":
            n_new = int(inp.get("n_new") or jar.NEW_ITEMS)
            commission = inp.get("commission") or None
            prompt = shopkeep.stock_prompt(inp["person"], [], 0, n_new, bool(inp.get("asked")),
                                           (), commission, int(inp.get("bribe") or 0),
                                           [s for s in inp.get("searches") or [] if s])
            values = {"commission": commission or ""}
            schema = shopkeep.stock_schema(0, n_new)
            ctx.update(system=shopkeep.SYSTEM, append_schema=False, effort=shopkeep.STOCK_EFFORT)
        elif stage == "dm":
            residents = [describe(c) for c in inp["jar"]]
            prompt = jarstages.dm_prompt(inp["search"], activity, tier[0], inp["earlier"],
                                         kind, form, r["source"], residents, r["tie"])
            values = {"search": inp["search"], "earlier": " / ".join(inp["earlier"]),
                      "tie": r["tie"], "residents": "\n".join("- %s" % x for x in residents),
                      "slot_words": jarstages.slot_of(kind, form), "source": r["source"],
                      "tier_words": jarstages.TIER_WORDS[tier[0]],
                      "activity_words": jarstages.ACTIVITY_WORDS[activity],
                      "dm_world": jarstages.DM_WORLD}
            schema = jarstages.dm_schema()
            ctx["effort"] = tier[2]
        elif stage == "fitter":
            concept = inp["concept"]
            if not concept:
                concept = "(the DM's concept goes here)"
                ctx["stand_in"].append("concept")
            rules = jar.motion_rules(activity, kind)
            ms = jarstages.MOTION_SAID
            prompt = jarstages.fitter_prompt(concept, kind, form, activity, tier)
            values = {"concept": concept, "game": jar.GAME, "kind_words": jar.KIND_WORDS[kind],
                      "activity_words": jarstages.ACTIVITY_WORDS[activity],
                      "tier_paren": "(%s)" % tier[0],
                      "form_words": jar.FORM_WORDS[form] if form and (jarvm.V2 or form == "furniture") else "",
                      "travel_options": "; ".join("%s (%s)" % (t, ms["travel"][t]) for t in rules["travel"]),
                      "idle_options": "; ".join("%s (%s)" % (t, ms["idle"][t]) for t in rules["idle"])}
            schema = jarstages.fitter_schema(kind, form, activity)
            ctx["effort"] = "medium"
        elif stage in ("artist", "behaviour", "behaviour_js3"):
            sheet, stand = _sheet(inp, kind, form, activity)
            if stand:
                ctx["stand_in"].append("sheet")
            ctx["effort"] = tier[2]
            base = {"name": sheet["name"], "line": sheet["line"], "kind_words": jar.KIND_WORDS[kind]}
            if stage == "artist":
                n = int(sheet.get("frames") or 1)
                prompt = jarstages.artist_prompt(sheet, kind, form, tier)
                art_form = ""
                if form in jarstages.ART_FORM:
                    t = jarstages.ART_FORM[form]
                    art_form = t % sheet.get("surface", 4) if "%s" in t else t
                values = dict(base, look=sheet.get("look", ""), art_form=art_form,
                              draw_frames="Draw exactly %d frame%s." % (n, "" if n == 1 else "s"),
                              tier_care=tier[3].split(":")[0])
                schema = jarstages.artist_schema(n, form)
            elif stage == "behaviour":
                n = int(sheet.get("frames") or 1)
                prompt = jarstages.behaviour_prompt(sheet, kind, activity, n, tier)
                bubbles = sheet.get("bubbles") or []
                values = dict(base, intent=sheet.get("intent", ""),
                              behaviour_rule=jarstages.BEHAVIOUR_RULE[activity],
                              language_guide=jarvm.LANGUAGE_GUIDE_V2 if jarvm.V2 else jarvm.LANGUAGE_GUIDE_V1,
                              bubbles_line="Its bubbles: %s." % (
                                  ", ".join('%d "%s"' % (i + 1, b) for i, b in enumerate(bubbles))
                                  if bubbles else "none (it cannot say anything)"),
                              frames_line="Its frames: %d (frame 1 is how it usually looks)." % n)
                schema = jarstages.behaviour_schema()
            else:
                values = js3_values(sheet, kind, form, activity)
                values.update(jar_values(inp))
                prompt, _ = render(JS3_TEMPLATE, values)
                schema = js3_schema()
        else:
            raise ValueError("no stage %r" % stage)
    template = JS3_TEMPLATE if stage == "behaviour_js3" else templatize(prompt, values)
    ctx.update(default=prompt, template=template, values=values, schema=schema)
    return ctx


def prompt_for(stage, inp, template=None, v2=None):
    """(ctx, the prompt to send, missing placeholders): the stage's default,
    or `template` rendered with this find's values."""
    ctx = stage_ctx(stage, inp, v2)
    if template is None or template == "":
        return ctx, ctx["default"], []
    text, missing = render(template, ctx["values"])
    return ctx, text, missing


# ---- asking -----------------------------------------------------------------------

class Dropped(Exception):
    pass


def _retry_prompt(ctx, sent, prompt, schema, text, why):
    if ctx["append_schema"]:
        return "%s\n\nYour last answer did not fit:\n%s\n%s\n\nAnswer again, only the JSON." % (
            sent, text[:2000], "\n".join("- " + w for w in why[:8]))
    return ("[That was not the JSON the shop needs (%s). Answer again with only the JSON: "
            "{\"line\": ..., \"picks\": [...], \"search\": \"...\"}]" % "; ".join(why[:3]))


def ask_raw(stage, ctx, prompt, schema=None, effort=None, model=None, retry=True, stub_ctx=None,
            resume=None):
    """One stage's call, as ask.ask_shape makes it (one more try when the
    answer does not fit) but keeping everything: {ok, value, raw, sent,
    attempts: [{text, errors, secs}], errors, secs, model, effort, dry}."""
    schema = schema or ctx["schema"]
    ask.check_schema(schema)
    effort = effort or ctx.get("effort")
    model = model or CFG.model
    sent = ask._prompt(prompt, schema) if ctx["append_schema"] else prompt
    out = {"stage": stage, "sent": sent, "attempts": [], "model": model, "effort": effort,
           "dry": CFG.dry, "ok": False, "value": None, "raw": "", "errors": []}
    t_all = time.time()
    asked, session = sent, resume
    for attempt in range(2 if retry else 1):
        t0 = time.time()
        try:
            if CFG.dry:
                if CFG.dry_delay:
                    time.sleep(CFG.dry_delay)
                text = stub_answer(stage, asked, schema, stub_ctx or {}, attempt)
            else:
                text, session = chatmod.ask_once(
                    CFG.chat, asked, STAGE_TIMEOUT, resume=session if stage == "tibbs" else None,
                    system=ctx["system"], effort=effort, model=model, cwd=shopkeep.home())
        except Exception as e:                          # noqa: BLE001 - the lab shows it
            out["attempts"].append({"text": "", "errors": ["%s: %s" % (type(e).__name__, e)],
                                    "secs": round(time.time() - t0, 2)})
            out["errors"] = out["attempts"][-1]["errors"]
            break
        why, value = [], None
        if len(text) > ask.ANSWER_MAX:
            why = ["the answer was %d bytes, more than %d" % (len(text), ask.ANSWER_MAX)]
        else:
            try:
                value = ask.extract(text)
                why = ask.validate(value, schema)
            except ValueError as e:
                why = [str(e)]
        out["attempts"].append({"text": text, "errors": why, "secs": round(time.time() - t0, 2)})
        out["raw"], out["value"], out["errors"] = text, value, why
        if not why:
            out["ok"] = True
            break
        asked = _retry_prompt(ctx, sent, prompt, schema, text, why)
    out["secs"] = round(time.time() - t_all, 2)
    out["session"] = session if stage == "tibbs" else None
    return out


# ---- an offline Claude (--dry) -------------------------------------------------------

PLACES = ["a flooded allotment shed", "the lost-property cupboard at a railway station",
          "a beach the morning after a storm", "a clockmaker's skip", "the attic of a closed theatre",
          "an orchard at dusk", "the cellar under a sweet shop", "a canal lock at low water",
          "a jumble sale in a church hall", "the hedge behind a bus depot"]
THINGS = ["thimble lantern", "button snail", "spool owl", "teacup fern", "matchbox beetle",
          "key moth", "bottlecap frog", "pencil heron", "sock mole", "marble jelly", "cork newt",
          "zip mouse", "bell crab", "pin cushion toad", "ink bottle bat"]
ADJ = ["tarnished", "mossy", "humming", "sleepy", "glass-eyed", "patched", "rusty", "velvet",
       "chalky", "speckled", "lopsided", "polished"]
SAYS = ["hm?", "hello!", "tee hee", "zzz", "oh!", "mine!", "boop", "shh"]


def _ascii(s):
    return re.sub(r"[^ -~\n]", "", s or "")


def stub_answer(stage, prompt, schema, sctx, attempt=0):
    """What a well-behaved Claude might answer: valid against `schema`, a
    little different each time, built from the inputs where it can be. A
    prompt that says the last answer was refused gets a safe answer; the
    `fault` option makes the first behaviour answer a bad one (to see the
    refusal go back to the behaviour writer)."""
    rng = random.Random(hash((stage, prompt, attempt)) & 0xffffffff)
    refused = "was refused" in prompt or "did not fit" in prompt
    if stage == "tibbs":
        place = rng.choice(PLACES)
        return json.dumps({"line": "Back from %s, and my boots are full of it. Have a look!" % place,
                           "picks": [],
                           "search": "I went to %s, poking about for anything with a story in it -- "
                                     "%s, mostly, and whatever the damp had loosened." % (
                                         place, rng.choice(["old tins", "lost buttons", "small brass things",
                                                            "anything that still ticks"]))})
    if stage == "dm":
        search = _ascii(sctx.get("search") or "")
        where = search.split(".")[0][:90] if search else rng.choice(PLACES)
        thing = rng.choice(THINGS)
        return json.dumps({"concept": (
            "A %s %s, found where Tibbs went: %s. %s It is about the size of a thumbnail, %s and "
            "%s, with a dark rim. Surprising detail: %s." % (
                rng.choice(ADJ), thing, where.strip(), sctx.get("activity_words", ""),
                rng.choice(["green", "rust-red", "pale blue", "honey yellow", "plum"]),
                rng.choice(["striped", "spotted", "glossy", "dusty"]),
                rng.choice(["it hums when it rains", "it keeps a single seed in its pocket",
                            "it is warm to the touch at night", "it was once a door knob",
                            "it remembers one song"])))[:jarstages.CONCEPT_LEN]})
    if stage == "fitter":
        p = schema["properties"]
        thing = rng.choice(THINGS)
        name = " ".join(w.capitalize() for w in thing.split())
        if len(name) > jar.NAME_LEN:
            name = thing.split()[-1].capitalize()
        taken = set(sctx.get("taken") or [])
        n = 2
        while name.lower() in taken:
            name = (name[:jar.NAME_LEN - 2].strip() + " %d" % n)
            n += 1
        idle = rng.choice(p["idle"]["enum"])
        frames = p["frames"]["minimum"]
        if idle in ("flicker", "blink"):
            frames = max(frames, 2)
        sheet = {
            "name": name, "line": ("a %s %s" % (rng.choice(ADJ), thing))[:jar.LINE_LEN],
            "kind": p["kind"]["enum"][0], "zone": rng.choice(p["zone"]["enum"]),
            "travel": rng.choice(p["travel"]["enum"]), "idle": idle,
            "liveliness": rng.choice(p["liveliness"]["enum"]),
            "speed": rng.choice(p["speed"]["enum"]),
            "bubbles": rng.sample(SAYS, rng.randrange(0, 3)) if p["kind"]["enum"][0] == "critter" else [],
            "frames": frames,
            "look": "A round %s body with a dark outline and one bright highlight%s." % (
                rng.choice(["green", "brown", "blue", "orange"]),
                "; frame 2 is the same with its light low" if frames > 1 else ""),
            "intent": {"still": "nothing", "reacts": "when poked it says something and sparkles",
                       "ambles": "potters about its zone", "restless": "darts about"}[
                sctx.get("activity", "still")] if sctx.get("activity") else "nothing",
            "note": "" if rng.random() < 0.6 else "shrunk it to fit a jar",
        }
        if "traits" in p:
            sheet["traits"] = rng.sample(p["traits"]["items"]["enum"], rng.randrange(1, 3))
            sheet["voice"] = rng.choice(p["voice"]["enum"])
        if "surface" in p:
            sheet["surface"] = 9
        return json.dumps(sheet)
    if stage == "artist":
        n = schema["properties"]["frames"]["minItems"]
        return json.dumps({"palette": _stub_palette(rng), "frames": _stub_frames(rng, n)},
                          separators=(",", ":"))
    if stage == "behaviour":
        nbub = int(sctx.get("nbub") or 0)
        bad = sctx.get("fault") == "behaviour" and attempt == 0 and not refused
        if bad:                                         # refused by the check: no reaction
            return json.dumps({"habits": [], "script": ""})
        habits = [{"when": "poke", "do": "say", "do_arg": 0}] if nbub else \
            [{"when": "poke", "do": "particle", "do_arg": 0}]
        if sctx.get("activity") == "still" and rng.random() < 0.5:
            habits = []
        return json.dumps({"habits": habits, "script": ""})
    if stage == "behaviour_js3":
        sheet = sctx.get("sheet") or {}
        bad = sctx.get("fault") == "behaviour" and attempt == 0 and not refused
        if bad:
            return json.dumps({"script": "state idle:\n  frame f1\nend\non poke:\n  wait 1\nend",
                               "notes": "a handler that waits (refused)"})
        src = motion_script(sheet, sctx.get("kind", "critter"), sctx.get("form"),
                            sctx.get("activity", "still"), header="# written offline (--dry)")
        return json.dumps({"script": src, "notes": "moves by its motion parts; poke it to hear it"})
    raise ValueError("no stub for %s" % stage)


def _stub_palette(rng):
    h = rng.random()
    import colorsys
    cols = ["#000000", "#1b1a24"]
    for light, sat in ((0.32, 0.55), (0.48, 0.6), (0.64, 0.55), (0.8, 0.45)):
        r, g, b = colorsys.hls_to_rgb(h, light, sat)
        cols.append("#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255)))
    r, g, b = colorsys.hls_to_rgb((h + 0.45) % 1, 0.6, 0.75)
    cols.append("#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255)))
    cols.append("#f6f2e6")
    return cols


def _stub_frames(rng, n):
    """n frames of a simple outlined shape: a blob, a tall one, a wide one."""
    shape = rng.choice(("blob", "tall", "wide"))
    rx, ry = {"blob": (5.5, 5.5), "tall": (4.0, 6.8), "wide": (6.8, 4.2)}[shape]
    frames = []
    for f in range(n):
        cy = 8.5 + (0.0 if f == 0 else -0.8 * (f % 2 + 1))
        rows = []
        for y in range(16):
            row = ""
            for x in range(16):
                d = ((x - 7.5) / rx) ** 2 + ((y - cy) / ry) ** 2
                if d > 1.0:
                    row += "0"
                elif d > 0.72:
                    row += "1"
                elif y < cy - ry * 0.35 and abs(x - 6) < 1.5:
                    row += "7"                          # a highlight
                elif abs(y - (cy - 1)) < 0.6 and x in (6, 9):
                    row += "1"                          # eyes
                elif y > cy + ry * 0.3:
                    row += "2"
                elif (x + y + f) % 5 == 0:
                    row += "6"
                else:
                    row += "4" if x < 8 else "5"
            rows.append(row)
        frames.append(rows)
    return frames


# ---- Jarscript 3: checking, and a script from the motion parts ---------------------

def js3_check(script, nframes=1):
    """Compile `script` with tools/jarsandbox/jarscript3.js under node:
    {ok, error, line, bytes, states, handlers, traits}; ok None when there is
    no node to ask (the page still compiles it in the browser)."""
    if not CFG.node:
        return {"ok": None, "error": "node is not on PATH: checked in the browser only"}
    req = json.dumps({"script": script or "", "frames": frame_names(nframes)})
    try:
        r = subprocess.run([CFG.node, JS3_CHECK], input=req, capture_output=True, text=True,
                           encoding="utf-8", timeout=20)
        return json.loads(r.stdout or "{}") if r.stdout else {"ok": None, "error": r.stderr[:300]}
    except (OSError, ValueError, subprocess.TimeoutExpired) as e:
        return {"ok": None, "error": "node: %s" % e}


def _q(s):
    return '"%s"' % re.sub(r'["\\\n]', "", str(s))[:24]


def motion_script(sheet, kind, form, activity, habits=(), header=None):
    """A Jarscript 3 script that does what the sheet's motion parts say (and
    a poke reaction from its bubbles or habits): what an item made with
    today's behaviour language does when it is sent to the sandbox, and the
    offline behaviour writer's answer."""
    nf = max(1, int(sheet.get("frames") or 1))
    travel = sheet.get("travel") or "none"
    idle = sheet.get("idle") or "none"
    lively = sheet.get("liveliness") or "calm"
    speed = sheet.get("speed") or "slow"
    if kind == "hanging" or form in ("scenery", "furniture"):
        travel = "none"
    lo, hi = {"calm": (40, 90), "normal": (20, 50), "jittery": (6, 20)}[lively]
    if activity == "restless":
        lo, hi = max(4, lo // 3), max(10, hi // 3)
    amp = {"calm": 1, "normal": 2, "jittery": 3}[lively]
    sp = {"slow": 8, "medium": 16, "fast": 28}.get(speed, 8)
    traits = [t for t in (sheet.get("traits") or []) if re.match(r"^[a-z]+$", t)] or [kind]
    L = [header or "# made by the prompt lab from the item's motion parts",
         "# travel %s, idle %s, %s, %s" % (travel, idle, lively, activity),
         "traits " + ", ".join(traits)]
    if form in ("big", "scenery"):
        L.append("scale 2")
    if form == "scenery":
        L.append("layer 0")
    if form == "furniture":
        s = max(0, min(13, int(sheet.get("surface", 4))))
        L += ["layer 0", "solid 1, %d, 14, %d" % (s, max(1, 16 - s)),
              "perch 4, %d" % s, "perch 11, %d" % s]
    if idle == "flicker" and nf >= 2:
        L.append("anim idle = f1 f2 @ %d" % {"calm": 4, "normal": 8, "jittery": 14}[lively])
    L += ["", "state main:"]
    L.append("  play idle" if idle == "flicker" and nf >= 2 else "  frame f1")
    moves = activity in ("ambles", "restless") or travel != "none" or idle not in ("none", "flicker")
    if moves:
        L += ["  loop", "    wait random(%d, %d) / 10" % (lo, hi)]
        if travel in ("crawl", "slide"):
            L.append("    walk pick(left, right) * random(8, 30) speed %d" % (sp * (2 if travel == "slide" else 1)))
        elif travel == "hop":
            L.append("    hop random(-16, 16) %d" % (4 + amp))
        elif travel == "float":
            L.append("    move to random(20, 220), random(30, 90) speed %d arc 6" % max(8, sp))
        elif travel == "fly":
            L.append("    move to random(20, 220), random(20, 100) speed %d arc -14" % (sp * 3))
        if idle == "sway":
            L.append("    sway %d 1.5s" % amp)
        elif idle == "bob":
            L.append("    sway %d 0.6s" % max(1, amp - 1))
        elif idle == "wobble":
            L.append("    shake 0.%ds" % (2 + amp))
        elif idle == "spin":
            L += ["    spin = %d" % (90 * amp), "    wait 2", "    spin = 0", "    angle = 0"]
        elif idle in ("pulse", "blink") and nf >= 2:
            L += ["    frame f%d" % nf, "    wait 0.%d" % (2 if idle == "blink" else 5), "    frame f1"]
        elif idle == "pulse":
            L.append("    shake 0.2s")
        L.append("  end")
    L.append("end")
    said = [b for b in (sheet.get("bubbles") or []) if b]
    poke = []
    for h in habits or ():
        if h.get("when") == "poke" and h.get("do") == "say" and h.get("do_arg", 0) < len(said):
            poke.append("  say %s 1.5s" % _q(said[h.get("do_arg", 0)]))
        elif h.get("when") == "poke" and h.get("do") == "particle":
            poke.append("  emit spark 4")
        elif h.get("when") == "poke" and h.get("do") == "glow":
            poke.append("  emit spark 2")
    if not poke:
        poke = (["  say %s 1.5s" % _q(said[0])] if said else []) + ["  emit heart 2"]
    L += ["", "on poke:"] + poke + ["  sway %d 0.8s" % amp, "end", ""]
    return "\n".join(L)


# ---- one find, staged ----------------------------------------------------------------

def _stub_ctx(stage, inp, sheet=None, spec=None):
    r = inp["rolls"]
    sheet = sheet or {}
    return {"search": inp.get("search"), "activity": r["activity"], "kind": r["kind"],
            "form": r["form"], "taken": inp.get("taken"),
            "activity_words": jarstages.ACTIVITY_WORDS[r["activity"]],
            "sheet": sheet, "nbub": len(sheet.get("bubbles") or []),
            "fault": (spec or {}).get("dry_fault")}


def run_stage(spec):
    """One stage, with an edited template or its default: {prompt, template,
    missing, ...ask_raw's result, extra}. Uses spec.inputs as they are."""
    stage = spec.get("stage")
    if stage not in STAGES:
        raise ValueError("no stage %r" % stage)
    inp = _norm_inputs(spec.get("inputs"))
    v2 = spec.get("v2")
    ctx, prompt, missing = prompt_for(stage, inp, spec.get("template"), v2)
    schema = spec.get("schema") or ctx["schema"]
    if isinstance(schema, str):
        schema = json.loads(schema)
    sheet = _sheet(inp, inp["rolls"]["kind"], inp["rolls"]["form"], inp["rolls"]["activity"])[0]
    res = ask_raw(stage, ctx, prompt, schema, (spec.get("efforts") or {}).get(stage) or spec.get("effort"),
                  spec.get("model"), spec.get("retry", True),
                  _stub_ctx(stage, inp, sheet, spec), resume=spec.get("resume"))
    res.update(prompt=prompt, edited=bool(spec.get("template")), missing=missing,
               stand_in=ctx["stand_in"], schema=schema)
    res["extra"] = _stage_extra(stage, res, inp, sheet, v2)
    return res


def _stage_extra(stage, res, inp, sheet, v2):
    v = res.get("value")
    if not res.get("ok") or not isinstance(v, dict):
        return {}
    r = inp["rolls"]
    if stage == "behaviour" and (v.get("script") or "").strip():
        nf = int(sheet.get("frames") or 1) - (1 if r["form"] == "part" and int(sheet.get("frames") or 1) >= 2 else 0)
        sim = jarlab.simulate(v["script"], len(sheet.get("bubbles") or []), max(1, nf),
                              CFG.v2 if v2 is None else v2)
        sim.pop("trace", None)
        return {"jarvm": sim}
    if stage == "behaviour_js3":
        return {"js3": js3_check(v.get("script", ""), sheet.get("frames"))}
    return {}


def _run(stage, inp, spec, progress, extra="", sheet=None):
    """A stage inside run_find: (result, value) or Dropped."""
    overrides = spec.get("overrides") or {}
    ctx, prompt, missing = prompt_for(stage, inp, overrides.get(stage), spec.get("v2"))
    prompt += extra
    if progress:
        progress("%s%s..." % (stage, " (again)" if extra else ""))
    res = ask_raw(stage, ctx, prompt, None, (spec.get("efforts") or {}).get(stage), spec.get("model"),
                  True, _stub_ctx(stage, inp, sheet, spec))
    res.update(prompt=prompt, edited=bool(overrides.get(stage)), missing=missing)
    return res


def _js3_problem(beh, nframes):
    chk = js3_check(beh.get("script", ""), nframes)
    if chk.get("ok") is False:
        return chk, "the script does not compile: line %s: %s" % (chk.get("line"), chk.get("error"))
    return chk, None


def _has_handler(src):
    return bool(re.search(r"^\s*(on|every)\s", src or "", re.M))


def check(inputs, variant="jarvm", v2=None, sheet=None, art=None, beh=None, taken=None):
    """Combine the three parts and check them as make_find does: {raw, why,
    parts, js3}. Parts are sheet / art / behaviour reasons."""
    inp = _norm_inputs(inputs)
    r = inp["rolls"]
    kind, form, activity = r["kind"], r["form"], r["activity"]
    if sheet is None:
        sheet, stand = _sheet(inp, kind, form, activity)
        if stand:
            raise ValueError("no sheet to check: run the fitter first")
    art = art if art is not None else inp.get("art")
    beh = beh if beh is not None else inp.get("behaviour") or {}
    taken = set(inp["taken"] if taken is None else taken)
    if not isinstance(art, dict) or "frames" not in art:
        raise ValueError("no art to check: run the artist first")
    js3 = None
    record_beh = beh if variant == "jarvm" else {"habits": [], "script": ""}
    with v2ctx(v2):
        raw = jarstages.combine(sheet, art, record_beh, kind, form)
        why = jar.check_item(raw, taken, form, activity)
        parts = jar.check_parts(raw, taken, activity) if why else {"sheet": [], "art": [], "behaviour": []}
        if why and not any(parts.values()):
            parts["sheet"] = list(why)
        if variant == "jarvm":
            err = jarstages._script_error(raw, form)
            if err:
                parts["behaviour"].append(err)
    if variant == "js3":
        src = beh.get("script", "")
        parts["behaviour"] = [p for p in parts["behaviour"]
                              if not (p.startswith("a thing that reacts") and _has_handler(src))]
        js3, err = _js3_problem(beh, sheet.get("frames"))
        if err:
            parts["behaviour"].append(err)
        if activity == "reacts" and not _has_handler(src):
            parts["behaviour"].append("a thing that reacts must react to something: give it an "
                                      "on handler")
    return {"raw": raw, "why": why, "parts": parts, "js3": js3, "ok": not any(parts.values())}


def finish(raw, inputs, variant, beh, v2=None, form=None):
    """The record a passed find would be (sealed with the dummy signer, never
    stored) and what to put in the sandbox."""
    inp = _norm_inputs(inputs)
    r = inp["rolls"]
    tier = tier_tuple(r)
    form = r["form"] if form is None else form
    logs = []
    with v2ctx(v2):
        it = jar.to_item(raw, 0, time.time(), jar.record_tags([tier[0], r["kind"]]), logs.append,
                         tier[1], form)
        rec = jar.seal(jar.encode(it), dummy_signer)
    view = jarlab._record_view(rec)
    return {"item": view, "record_size": len(rec), "record_log": logs,
            "sandbox": sandbox_def(raw, r, variant, beh)}


def sandbox_def(raw, rolls, variant, beh):
    """The find as tools/jarsandbox takes an item: palette and frames as a
    sprite, the Jarscript 3 behaviour (or one made from its motion parts)."""
    sheet = {k: raw.get(k) for k in ("travel", "idle", "liveliness", "speed", "bubbles", "traits")}
    sheet["frames"] = len(raw["frames"])
    sheet["surface"] = raw.get("surface", 4)
    sheet["traits"] = raw.get("traits") or [t for t in [rolls["kind"]]]
    script, source = None, "motion"
    if variant == "js3" and (beh or {}).get("script", "").strip():
        script, source = beh["script"], "js3"
    if script is None:
        script = motion_script(sheet, rolls["kind"], rolls.get("form"), rolls["activity"],
                               raw.get("habits") or [])
    rng = random.Random(raw["name"])
    y = None
    if rolls["kind"] == "hanging":
        y = 40
    elif raw.get("travel") in ("float", "fly"):
        y = 60
    return {"name": raw["name"][:12], "w": 16, "h": 16, "palette": list(raw["palette"]),
            "frames": [{"name": n, "rows": rows} for n, rows in zip(frame_names(len(raw["frames"])), raw["frames"])],
            "script": script, "script_source": source,
            "start": {"x": rng.randrange(24, 216), "y": y}}


def run_find(spec, progress=None):
    """The whole pipeline for one find, as jarstages.make_find runs it, with
    per-stage templates and every intermediate kept."""
    t0 = time.time()
    variant = spec.get("variant") or "jarvm"
    if variant not in VARIANTS:
        raise ValueError("no variant %r" % variant)
    inp = _norm_inputs(spec.get("inputs"))
    r = inp["rolls"]
    kind, form, activity = r["kind"], r["form"], r["activity"]
    out = {"kind": "find", "variant": variant, "behaviour_stage": VARIANTS[variant],
           "rolls": dict(r), "v2": CFG.v2 if spec.get("v2") is None else bool(spec.get("v2")),
           "dry": CFG.dry, "model": spec.get("model") or CFG.model,
           "stages": {}, "checks": [], "redo": [], "log": [], "dropped": None}
    say = out["log"].append

    def note(s):
        say(s)
        if progress:
            progress(s)

    try:
        # Tibbs's search
        if spec.get("ask_tibbs"):
            res = _run("tibbs", inp, spec, note)
            out["stages"]["tibbs"] = res
            if res["ok"]:
                inp["search"] = wire.flat(res["value"]["search"], shopkeep.SEARCH_LEN, ascii=True)
                out["search_from"] = "tibbs"
            else:
                inp["search"] = ""
                out["search_from"] = "none"
                note("Tibbs gave no search: the DM works from the rolls alone")
        else:
            out["search_from"] = "given" if inp["search"] else "none"
        out["search_used"] = inp["search"]
        # the DM
        if spec.get("skip_dm") and inp["concept"]:
            out["concept_from"] = "given"
        else:
            res = _run("dm", inp, spec, note)
            out["stages"]["dm"] = res
            if not res["ok"]:
                raise Dropped("DM: %s" % "; ".join(res["errors"][:3]))
            inp["concept"] = res["value"]["concept"].strip()
            out["concept_from"] = "dm"
        out["concept"] = inp["concept"]
        # the fitter
        res = _run("fitter", inp, spec, note)
        out["stages"]["fitter"] = res
        if not res["ok"]:
            raise Dropped("fitter: %s" % "; ".join(res["errors"][:3]))
        sheet = res["value"]
        inp["sheet"] = sheet
        out["sheet"] = sheet
        out["fitter_note"] = sheet.get("note", "")
        if sheet.get("note"):
            note("fitter's note on %r: %s" % (sheet["name"], sheet["note"]))
        bstage = VARIANTS[variant]

        def draw(extra=""):
            return _run("artist", inp, spec, note, extra, sheet)

        def behave(extra=""):
            return _run(bstage, inp, spec, note, extra, sheet)

        got = {}

        def twice(key, fn):
            try:
                _twice(key, fn)
            except Exception as e:                      # noqa: BLE001 - a stage that raised
                got[key] = ({"ok": False, "errors": ["%s: %s" % (type(e).__name__, e)]},
                            "%s: %s" % (key, e))

        def _twice(key, fn):
            first = fn()
            if first["ok"]:
                got[key] = (first, None)
                return
            note("%s for %r: %s; again" % (key, sheet["name"], "; ".join(first["errors"][:2])))
            second = fn()
            second["first_try"] = first
            got[key] = (second, None if second["ok"] else "%s: %s" % (key, "; ".join(second["errors"][:3])))

        threads = [threading.Thread(target=twice, args=("artist", draw), daemon=True),
                   threading.Thread(target=twice, args=("behaviour", behave), daemon=True)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        out["stages"]["artist"], err_a = got["artist"]
        out["stages"]["behaviour"], err_b = got["behaviour"]
        for e in (err_a, err_b):
            if e:
                raise Dropped(e)
        art, beh = out["stages"]["artist"]["value"], out["stages"]["behaviour"]["value"]
        redone = False
        while True:
            chk = check(inp, variant, spec.get("v2"), sheet, art, beh)
            out["checks"].append({"round": len(out["checks"]) + 1, "parts": chk["parts"],
                                  "why": chk["why"], "js3": chk["js3"], "ok": chk["ok"]})
            if chk["ok"]:
                raw = chk["raw"]
                break
            parts = chk["parts"]
            if redone:
                only_script = parts["behaviour"] and not parts["sheet"] and not parts["art"]
                if only_script and variant == "jarvm" and all("script" in p for p in parts["behaviour"]):
                    raw = dict(chk["raw"])
                    raw.pop("script", None)
                    with v2ctx(spec.get("v2")):
                        still_bad = jar.check_item(raw, set(inp["taken"]), form, activity)
                    if not still_bad:
                        note("script of %r dropped: %s" % (raw["name"], parts["behaviour"][0]))
                        out["script_dropped"] = parts["behaviour"][0]
                        break
                if only_script and variant == "js3" and all("does not compile" in p for p in parts["behaviour"]):
                    raw = chk["raw"]
                    note("Jarscript 3 of %r still does not compile: the sandbox gets its motion "
                         "parts instead" % raw["name"])
                    out["js3_failed"] = parts["behaviour"][0]
                    beh = {}
                    break
                raise Dropped("refused after a redo: %s" % "; ".join(
                    (parts["sheet"] + parts["art"] + parts["behaviour"])[:3]))
            redone = True
            fix = "\n\nThe last answer was refused: %s. Fix that."
            if parts["sheet"]:
                note("sheet for %r refused: %s" % (sheet["name"], "; ".join(parts["sheet"][:2])))
                res = _run("fitter", inp, spec, note, fix % "; ".join(parts["sheet"][:4]))
                out["redo"].append({"stage": "fitter", "reasons": parts["sheet"], "result": res})
                if not res["ok"]:
                    raise Dropped("fitter, again: %s" % "; ".join(res["errors"][:3]))
                sheet = res["value"]
                inp["sheet"] = out["sheet"] = sheet
                if len(art["frames"]) != sheet["frames"]:
                    parts["art"].append("draw %d frames" % sheet["frames"])
            if parts["art"]:
                note("art for %r refused: %s" % (sheet["name"], "; ".join(parts["art"][:2])))
                res = draw(fix % "; ".join(parts["art"][:4]))
                out["redo"].append({"stage": "artist", "reasons": parts["art"], "result": res})
                if not res["ok"]:
                    raise Dropped("artist, again: %s" % "; ".join(res["errors"][:3]))
                art = res["value"]
            if parts["behaviour"]:
                note("behaviour for %r refused: %s" % (sheet["name"], "; ".join(parts["behaviour"][:2])))
                res = behave(fix % "; ".join(parts["behaviour"][:4]))
                out["redo"].append({"stage": bstage, "reasons": parts["behaviour"], "result": res})
                if not res["ok"]:
                    raise Dropped("behaviour, again: %s" % "; ".join(res["errors"][:3]))
                beh = res["value"]
        out["art"], out["behaviour"] = art, beh
        out["raw"] = raw
        out.update(finish(raw, inp, variant, beh, spec.get("v2")))
        note("%s: %s %s %s, %d coins, %s/%s/%s" % (
            raw["name"], r["tier"], activity, kind, out["item"]["price"], raw["travel"], raw["idle"],
            raw["liveliness"]))
    except Dropped as e:
        out["dropped"] = str(e)
        note("dropped: %s" % e)
    out["secs"] = round(time.time() - t0, 2)
    out["label"] = (out.get("raw") or {}).get("name") or ("dropped: %s" % out["dropped"])
    return out


def run_batch(spec, progress=None, partial=None):
    """N finds from one search (Tibbs asked once if wanted), rolled as a stock
    is rolled; the DM sees the concepts already written, as in generate."""
    n = max(1, min(BATCH_MAX, int(spec.get("n") or 4)))
    seed = spec.get("seed")
    seed = random.SystemRandom().randrange(1 << 30) if seed in (None, "") else int(seed)
    rolls = _rolls_seq(n, seed, int(spec.get("kind_at") or 0), int(spec.get("bribe") or 0),
                       spec.get("v2"))
    base = _norm_inputs(dict(spec.get("inputs") or {}, rolls=rolls[0]))
    out = {"kind": "batch", "n": n, "seed": seed, "variant": spec.get("variant") or "jarvm",
           "finds": [], "dry": CFG.dry}
    t0 = time.time()
    if spec.get("ask_tibbs"):
        if progress:
            progress("asking Tibbs where he went...")
        ctx, prompt, _ = prompt_for("tibbs", base, (spec.get("overrides") or {}).get("tibbs"), spec.get("v2"))
        res = ask_raw("tibbs", ctx, prompt, stub_ctx={})
        out["tibbs"] = res
        base["search"] = res["value"]["search"] if res["ok"] else ""
    out["search"] = base["search"]
    names, concepts = [], []
    parallel = bool(spec.get("parallel"))
    grow = bool(spec.get("grow")) and not parallel
    residents = list(base["jar"])
    tie_rng = random.Random(seed * 7919 + 1)            # apart from the stock's dice
    out["grow"] = grow

    def one(k, earlier, taken):
        rk = dict(rolls[k])
        if grow or not rk.get("tie"):
            rk["tie"] = jarstages.roll_tie(tie_rng, [c["name"] for c in residents])
        inp = dict(base, rolls=rk, earlier=earlier, taken=taken, concept="", sheet=None,
                   jar=list(residents))
        sub = dict(spec, inputs=inp, ask_tibbs=False)
        res = run_find(sub, (lambda s, k=k: progress("find %d: %s" % (k + 1, s))) if progress else None)
        res["index"] = k
        return res

    if parallel:
        results = [None] * n
        threads = []
        for k in range(n):
            def go(k=k):
                results[k] = one(k, [], list(base["taken"]))
                if partial is not None:
                    partial.append(_card(results[k]))
            threads.append(threading.Thread(target=go, daemon=True))
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    else:
        results = []
        for k in range(n):
            res = one(k, list(concepts), list(base["taken"]) + [x.lower() for x in names])
            results.append(res)
            if not res["dropped"]:
                names.append(res["raw"]["name"])
                concepts.append(jarstages._short(res["concept"]))
                sb = res.get("sandbox") or {}
                if grow and sb.get("script_source") == "js3":
                    residents.append(contract(res["raw"]["name"], res["raw"]["line"],
                                              sb["script"]))
            if partial is not None:
                partial.append(_card(res))
    out["finds"] = results
    out["cards"] = [_card(x) for x in results]
    out["residents"] = residents
    out["secs"] = round(time.time() - t0, 2)
    out["label"] = "batch of %d: %s" % (n, ", ".join(c["name"] for c in out["cards"]))
    return out


def _card(res):
    """A batch find, small: enough to draw a card."""
    raw = res.get("raw") or {}
    return {"index": res.get("index"), "name": raw.get("name") or "(dropped)",
            "line": raw.get("line", ""), "rolls": res["rolls"], "concept": res.get("concept", ""),
            "dropped": res.get("dropped"), "travel": raw.get("travel"), "idle": raw.get("idle"),
            "liveliness": raw.get("liveliness"), "palette": raw.get("palette"),
            "frames": raw.get("frames"), "bubbles": raw.get("bubbles"),
            "redo": [x["stage"] for x in res.get("redo", [])],
            "sandbox": res.get("sandbox"), "secs": res.get("secs"),
            "tie": res["rolls"].get("tie", ""), "notes": (res.get("behaviour") or {}).get("notes", "")}


def tibbs(spec):
    """Tibbs's stock question on its own: {line, search, ...ask_raw}."""
    inp = _norm_inputs(spec.get("inputs"))
    ctx, prompt, missing = prompt_for("tibbs", inp, spec.get("template"), spec.get("v2"))
    res = ask_raw("tibbs", ctx, prompt, None, spec.get("effort"), spec.get("model"), True, {},
                  resume=spec.get("resume"))
    res.update(prompt=prompt, edited=bool(spec.get("template")), missing=missing)
    if res["ok"]:
        res["search"] = wire.flat(res["value"]["search"], shopkeep.SEARCH_LEN, ascii=True)
        res["line"] = res["value"]["line"]
    return res


def defaults(inputs=None, v2=None):
    """Every stage's default template and schema for `inputs` (sample
    inputs when none), the dice tables, the configuration."""
    inp = inputs or {"search": "", "rolls": {"kind": "critter", "tier": "everyday",
                                             "activity": "reacts", "form": ""}}
    stages = {}
    for s in STAGES:
        try:
            c = stage_ctx(s, inp, v2)
            stages[s] = {"template": c["template"], "default": c["default"], "values": c["values"],
                         "schema": c["schema"], "effort": c.get("effort"), "stand_in": c["stand_in"],
                         "append_schema": c["append_schema"]}
        except Exception as e:                          # noqa: BLE001 - shown on that stage
            stages[s] = {"error": "%s: %s" % (type(e).__name__, e)}
    return {"stages": stages, "dice": dice(), "dry": CFG.dry, "model": CFG.model,
            "v2": CFG.v2, "node": bool(CFG.node), "state": STATE,
            "variants": {"jarvm": "today's language (server/jarvm.py)",
                         "js3": "Jarscript 3 (tools/jarsandbox)"},
            "efforts": ["low", "medium", "high", "xhigh", "max"],
            "drop_if_empty": sorted(DROP_IF_EMPTY), "batch_max": BATCH_MAX}


# ---- jobs and history ---------------------------------------------------------------

class Jobs:
    def __init__(self):
        self.lock = threading.Lock()
        self.jobs = {}
        self.history = []
        self.n = 0

    def start(self, kind, fn, spec, label=""):
        with self.lock:
            self.n += 1
            jid = "%s-%d" % (kind, self.n)
            job = {"id": jid, "kind": kind, "state": "pending", "progress": [], "partial": [],
                   "result": None, "error": None, "started": time.time(), "label": label}
            self.jobs[jid] = job

        def progress(s):
            job["progress"].append(s)

        def run():
            try:
                if kind == "batch":
                    job["result"] = fn(spec, progress, job["partial"])
                else:
                    job["result"] = fn(spec, progress) if kind == "find" else fn(spec)
                job["state"] = "done"
            except Exception as e:                      # noqa: BLE001 - the page shows it
                job["error"] = "%s: %s" % (type(e).__name__, e)
                job["trace"] = traceback.format_exc()[-2000:]
                job["state"] = "error"
            job["secs"] = round(time.time() - job["started"], 2)
            self._remember(job)
        threading.Thread(target=run, daemon=True).start()
        return jid

    def run_now(self, kind, fn, spec):
        """The same, on this thread (wait=true): the finished job."""
        job = {"id": None, "kind": kind, "state": "pending", "progress": [], "partial": [],
               "started": time.time(), "label": ""}
        with self.lock:
            self.n += 1
            job["id"] = "%s-%d" % (kind, self.n)
            self.jobs[job["id"]] = job
        try:
            if kind == "batch":
                job["result"] = fn(spec, job["progress"].append, job["partial"])
            elif kind == "find":
                job["result"] = fn(spec, job["progress"].append)
            else:
                job["result"] = fn(spec)
            job["state"] = "done"
        except Exception as e:                          # noqa: BLE001
            job["error"] = "%s: %s" % (type(e).__name__, e)
            job["trace"] = traceback.format_exc()[-2000:]
            job["state"] = "error"
        job["secs"] = round(time.time() - job["started"], 2)
        self._remember(job)
        return job

    def _remember(self, job):
        if job["state"] != "done":
            return
        res = job["result"] or {}
        label = res.get("label") or (res.get("stage") and "%s: %s" % (
            res["stage"], "ok" if res.get("ok") else "; ".join(res.get("errors") or [])[:80])) or job["kind"]
        with self.lock:
            self.history.append({"id": job["id"], "kind": job["kind"], "at": time.time(),
                                 "label": label, "secs": job.get("secs")})
            del self.history[:-HISTORY_MAX]
            done = [j for j in self.jobs.values() if j["state"] != "pending"]
            keep = {h["id"] for h in self.history}
            for j in done:
                if j["id"] not in keep and time.time() - j["started"] > 3600:
                    self.jobs.pop(j["id"], None)

    def get(self, jid):
        with self.lock:
            return self.jobs.get(jid)


JOBS = Jobs()


# ---- experiments -------------------------------------------------------------------

def _exp_path(name):
    if not isinstance(name, str) or not EXP_NAME.match(name):
        raise ValueError("a name is letters, digits, space, _ . - (up to 48)")
    return os.path.join(EXP_DIR, name + ".json")


def save_experiment(obj):
    path = _exp_path(obj.get("name"))
    os.makedirs(EXP_DIR, exist_ok=True)
    obj = dict(obj, saved=time.strftime("%Y-%m-%d %H:%M:%S"))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)
    return {"name": obj["name"], "saved": obj["saved"], "path": path}


def load_experiment(name):
    with open(_exp_path(name), encoding="utf-8") as f:
        return json.load(f)


def list_experiments():
    if not os.path.isdir(EXP_DIR):
        return []
    out = []
    for fn in sorted(os.listdir(EXP_DIR)):
        if fn.endswith(".json"):
            try:
                with open(os.path.join(EXP_DIR, fn), encoding="utf-8") as f:
                    o = json.load(f)
                out.append({"name": o.get("name", fn[:-5]), "saved": o.get("saved", ""),
                            "note": o.get("note", "")})
            except (OSError, ValueError):
                continue
    return out


def delete_experiment(name):
    path = _exp_path(name)
    if os.path.exists(path):
        os.remove(path)
        return True
    return False
