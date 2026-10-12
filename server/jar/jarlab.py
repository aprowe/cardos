"""Jar Lab: Jar Factory's item generation and behaviour, on a bench.

A page for the owner (server/jar/jarlab.html, at /dash/jarlab) and its API: see
the exact prompt an item is made from, edit it, send it to Claude (or to a
stub, offline), and see what came back -- checked, compiled, given its
simulated day, packed into a record and drawn -- without any of it reaching
a shop. It is the generation path of server/jar/__init__.py taken apart, using jar's
own functions (prompt_for, batch_schema, check_item, to_item, encode,
decode) and jarvm's (compile_script, disassemble, simulate_day), so what the
lab shows is what the shop would do.

What it never does: write the pool, the ledger (own/ID), rec/ID or next_id;
sign anything (records here are unsigned: signature length 0); touch
sign_key.pem; or change jar.py, shopkeep.py or town.py's behaviour. Its own
files are named experiments, JSON in CARDOS_STATE/jarlab/.

Every route is the dashboard's cookie and an admin: it spends Claude calls
and shows the prompts. The page is under /dash because the cookie's Path is
/dash -- a browser would not send it to /jarlab, which only redirects here.

    GET  /dash/jarlab                    the page
    GET  /dash/jarlab/api/defaults?v2=   tiers, forms, kinds, sparks, guides,
                                         a sample prompt and its schema
    POST /dash/jarlab/api/roll           {seed?, kind?} -> the code's dice:
                                         kind, tier, price, form, brief
    POST /dash/jarlab/api/prompt         {kind, tier, price, form, brief,
                                         owned, avoid, v2, n} -> prompt, schema
    POST /dash/jarlab/api/generate       {prompt?, kind, tier, price, form, n,
                                         model, effort, dry, v2, answer?, wait}
                                         -> results (dry/wait) or {"job": id}
    GET  /dash/jarlab/api/job?id=        {"state": pending|done, progress, result}
    POST /dash/jarlab/api/simulate       {script, nbub, nframes, v2, seed} or
                                         {item: {...}} -> compile + the day
    GET  /dash/jarlab/api/experiments    [{name, saved, note}]
    GET  /dash/jarlab/api/experiment?name=
    POST /dash/jarlab/api/experiment     {name, ...anything} -> saved
    POST /dash/jarlab/api/experiment/delete  {name}

Language version: the lab may preview and check either version of the item
language. jar.prompt_for and jar.item_schema read jarvm.V2, so for the other
version it is flipped under a lock for the few milliseconds those pure calls
take (never across a Claude call); scripts are compiled with v2 passed
explicitly.
"""
import base64
import contextlib
import json
import os
import random
import re
import secrets
import threading
import time
import traceback

from .. import accounts, ask, dash, jar
from . import jarvm, shopkeep

PAGE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "jarlab.html")
BODY_MAX = 512 * 1024
N_MAX = 8                     # items one generate may ask for
TRACE_MAX = 4000              # trace entries sent back
JOB_TTL = 3600
EXP_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.-]{0,47}$")


# ---- the language version ---------------------------------------------------

_v_lock = threading.Lock()


@contextlib.contextmanager
def version(v2):
    """jarvm.V2 as `v2` for the pure calls inside; nothing if it already is."""
    if v2 is None or bool(v2) == bool(jarvm.V2):
        yield
        return
    with _v_lock:
        old = jarvm.V2
        jarvm.V2 = bool(v2)
        try:
            yield
        finally:
            jarvm.V2 = old


def _v2(spec):
    v = spec.get("v2")
    return bool(jarvm.V2) if v is None or v == "" else bool(v)


# ---- the pieces ---------------------------------------------------------------

def _tiers():
    return [{"name": n, "chance": c, "lo": lo, "hi": hi, "effort": e, "words": w}
            for n, c, (lo, hi), e, w in jar.TIERS]


def tier_for(name, price=None):
    """(name, price, effort, words) as jar.pick_tier gives it, for a tier
    chosen by hand; price defaults to the middle of the tier's range."""
    for n, _c, (lo, hi), e, w in jar.TIERS:
        if n == name:
            p = (lo + hi) // 2 // 5 * 5 if price in (None, "") else int(price)
            return n, p, e, w
    raise ValueError("no tier %r" % name)


def _words(name, default=()):
    return list(getattr(shopkeep, name, default))


def defaults(v2=None):
    v2 = bool(jarvm.V2) if v2 is None else bool(v2)
    tier = tier_for("everyday")
    brief = "a find suggested by: brass, kettle -- take it anywhere"
    with version(v2):
        prompt = jar.prompt_for(brief, kind="critter", tier=tier)
        schema = jar.batch_schema(1, "critter", None)
    return {
        "v2": v2, "server_v2": bool(jarvm.V2),
        "tiers": _tiers(),
        "forms": [{"name": n, "chance": c, "kinds": list(k), "words": jar.FORM_WORDS.get(n, "")}
                  for n, c, k in jar.FORMS],
        "kinds": [{"name": k, "words": w} for k, w in jar.KIND_WORDS.items()],
        "kind_plan": list(jar.KIND_PLAN),
        "sparks": {"materials": _words("MATERIALS"), "colours": _words("COLOURS"),
                   "things": _words("THINGS"), "moods": _words("MOODS"),
                   "events": _words("EVENTS")},
        "answer_chance": getattr(shopkeep, "ANSWER_CHANCE", None),
        "model": getattr(shopkeep, "MODEL", ""),
        "efforts": ["low", "medium", "high", "xhigh", "max"],
        "game": jar.GAME,
        "guide_v1": jarvm.LANGUAGE_GUIDE_V1, "guide_v2": jarvm.LANGUAGE_GUIDE_V2,
        "traits": list(jarvm.GROUPS.get("trait", {})),
        "sounds": list(jarvm.GROUPS.get("sound", {})),
        "sample": {"kind": "critter", "tier": "everyday", "price": tier[1], "form": "",
                   "brief": brief},
        "prompt": prompt, "schema": schema, "n_max": N_MAX,
        "stub_script": STUB_SCRIPT_V2 if v2 else STUB_SCRIPT_V1,
    }


def roll(seed=None, kind=None, v2=None):
    """The code's dice, as jar.generate rolls them for one item (a form only
    under version 2, as there)."""
    seed = secrets.randbelow(1 << 30) if seed in (None, "") else int(seed)
    v2 = bool(jarvm.V2) if v2 is None else bool(v2)
    rng = random.Random(seed)
    kind = kind or rng.choice(jar.KIND_PLAN)
    tier = jar.pick_tier(rng)
    form = jar.roll_form(kind, rng) if v2 else None
    return {"seed": seed, "kind": kind, "tier": tier[0], "price": tier[1], "effort": tier[2],
            "form": form or "", "activity": jar.roll_activity(rng), "brief": "", "spark": ""}


def _form(spec):
    f = spec.get("form") or None
    if f and f not in jar.FORM_WORDS:
        raise ValueError("no form %r" % f)
    return f


def _kind(spec):
    k = spec.get("kind") or "critter"
    if k not in jar.KIND_WORDS:
        raise ValueError("no kind %r" % k)
    return k


def _list(v):
    if isinstance(v, str):
        return [s.strip() for s in v.split(",") if s.strip()]
    return [str(s) for s in (v or [])]


def build_prompt(spec):
    """{prompt, schema, asked, tier, effort, v2}: exactly what jar.generate
    would send for this kind, tier, form and brief (no Claude call)."""
    v2 = _v2(spec)
    kind, form = _kind(spec), _form(spec)
    tier = tier_for(spec.get("tier") or "everyday", spec.get("price"))
    n = max(1, min(N_MAX, int(spec.get("n") or 1)))
    brief = spec.get("brief") or "a find suggested by: brass, kettle -- take it anywhere"
    with version(v2):
        prompt = jar.prompt_for(brief, _list(spec.get("owned")), _list(spec.get("avoid")),
                                kind, tier, form)
        schema = jar.batch_schema(1, kind, form)
    return {"prompt": prompt, "schema": schema, "asked": _asked(prompt, schema),
            "tier": {"name": tier[0], "price": tier[1], "effort": tier[2], "words": tier[3]},
            "effort": tier[2], "v2": v2, "kind": kind, "form": form or "", "n": n,
            "chars": len(prompt)}


def _asked(prompt, schema):
    """The prompt as ask.ask_shape actually sends it (with the schema)."""
    try:
        return ask._prompt(prompt, schema)
    except Exception:                                   # noqa: BLE001 - a private helper
        return prompt


# ---- an offline Claude ----------------------------------------------------------

def _blob(seed, colours=4):
    rows = []
    for y in range(16):
        row = ""
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 8.5 + (seed % 2)) ** 2
            if d > 49:
                row += "0"
            elif d > 36:
                row += "1"
            elif y == 6 and x in (5, 10):
                row += "7" if seed % 3 else "1"
            else:
                row += str(2 + (x + y + seed) % max(1, colours - 1))
        rows.append(row)
    return rows


STUB_SCRIPT_V1 = """on poke:
  say 1; hop
end
on tick:
  if rand(100) < 3 then emit sparkle end
end
on time night: glow on; end
on time dawn: glow off; end"""

STUB_SCRIPT_V2 = STUB_SCRIPT_V1 + """
on chime 15: sound ding; end
on new: if has(newtraits, food) then burst sparkle end; end"""


def stub_answer(schema, serial=1, rng=None):
    """What a well-behaved Claude might answer for `schema` (a batch_schema):
    good items of the kind it asks for, with whatever fields it requires."""
    rng = rng or random.Random(serial)
    one = schema["properties"]["items"]["items"]
    props, req = one["properties"], one["required"]
    n = schema["properties"]["items"].get("maxItems", 1)
    kinds = props["kind"]["enum"]
    out = []
    for k in range(n):
        s = serial + k
        kind = kinds[0] if len(kinds) == 1 else rng.choice(kinds)
        hanging = kind == "hanging"
        nframes = max(2, props["frames"].get("minItems", 1))
        it = {
            "name": "Stub %d" % s, "line": "A stand-in made offline",
            "kind": kind, "movement": "sits" if hanging else rng.choice(["hops", "walks"]),
            "speed": "slow", "zone": "anywhere",
            "habits": [{"when": "poke", "do": "say", "do_arg": 1},
                       {"when": "tick", "when_arg": 40, "do": "frame", "do_arg": 1}],
            "bubbles": ["hm?", "hello!"],
            "palette": ["#000000", "#1a1020", "#%06x" % rng.randrange(1 << 24),
                        "#%06x" % rng.randrange(1 << 24), "#80e0a0", "#ffd166",
                        "#40a0ff", "#ffffff"],
            "frames": [_blob(s + f) for f in range(nframes)],
            "script": STUB_SCRIPT_V2 if "traits" in props else STUB_SCRIPT_V1,
        }
        for field in ("movement", "speed", "zone"):
            if it[field] not in props[field]["enum"]:
                it[field] = props[field]["enum"][0]
        if "traits" in props:
            it["traits"] = ["shiny", "light"][:1 + s % 2]
            it["traits"] = [t for t in it["traits"] if t in props["traits"]["items"]["enum"]] \
                or [props["traits"]["items"]["enum"][0]]
        if "voice" in props:
            it["voice"] = props["voice"]["enum"][0]
        if "surface" in req:
            it["surface"] = 4
        out.append(it)
    return {"items": out}


# ---- looking at what came back ----------------------------------------------------

def _hex565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return "#%02x%02x%02x" % ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _inv(d):
    return {v: k for k, v in d.items()}


def _record_view(rec):
    """A decoded record, with numbers named and frames as index grids."""
    d = jar.decode(rec)
    e = jar.enums()
    kinds, moves, speeds, zones = (_inv(e[k]) for k in ("kinds", "moves", "speeds", "zones"))
    events, actions = _inv(e["events"]), _inv(e["actions"])
    sounds = _inv(jarvm.GROUPS.get("sound", {}))
    grids = []
    for f in d["frames"]:
        px = jar.unpack_frame(f)
        grids.append([px[16 * y:16 * y + 16] for y in range(16)])
    return {
        "id": d["id"], "name": d["name"], "line": d["line"], "kind": kinds.get(d["kind"]),
        "move": moves.get(d["move"]), "speed": speeds.get(d["speed"]),
        "zone": zones.get(d["zone"]), "price": d["price"],
        "traits": [t for t in d["tags"].split(",") if t], "tags": d["tags"],
        "voice": sounds.get(d["voice"], "") if d["voice"] else "",
        "role": jar.ROLES[d["role"]] if d["role"] < len(jar.ROLES) else d["role"],
        "scale": d["scale"], "surface": d["surface"], "part": d["part"],
        "pdx": d["pdx"], "pdy": d["pdy"],
        "bubbles": d["bubbles"],
        "habits": [{"when": events.get(h[0]), "when_arg": h[1], "do": actions.get(h[2]),
                    "do_arg": h[3]} for h in d["habits"]],
        "palette": [None] + [_hex565(v) for v in d["pal"][1:]],
        "frames": grids,
        "hdr": d["hdr"], "total": d["total"], "script_len": len(d["script"]),
        "signed": bool(d["sig"]),
    }


def _trace(code, nbub, nframes, seed=1, ticks_per_hour=120, quiet=True):
    """simulate_day with its trace: (report dict, entries). quiet: only the
    runs that did something."""
    entries = []
    total = [0]

    def keep(t):
        total[0] += 1
        if quiet and not t["actions"]:
            return
        if len(entries) < TRACE_MAX:
            entries.append({"h": t["hour"], "t": t["tick"], "ev": t["event"], "arg": t["arg"],
                            "r": t["result"], "steps": t["steps"],
                            "acts": [list(a) for a in t["actions"]], "x": t["x"]})
    rep = jarvm.simulate_day(code, nbub, nframes, seed=seed, ticks_per_hour=ticks_per_hour,
                             trace=keep)
    hours = [0] * 24
    for t in entries:
        hours[t["h"]] += len(t["acts"])
    return ({"ok": rep.ok, "problems": rep.problems, "runs": rep.runs, "events": rep.events,
             "actions": rep.actions, "visible": rep.visible, "max_steps": rep.max_steps,
             "mem": rep.mem, "ticks_per_hour": ticks_per_hour, "seed": seed,
             "traced": total[0], "kept": len(entries), "per_hour": hours}, entries)


def simulate(src, nbub=jarvm.MAX_BUB, nframes=jarvm.MAX_FRAMES, v2=None, seed=1,
             ticks_per_hour=120, quiet=True):
    """Compile `src` and give it a day: {ok, error, line, bytes, hex, size,
    disassembly, v2_words, report, trace}. A compile error is an answer, not
    an exception."""
    v2 = bool(jarvm.V2) if v2 is None else bool(v2)
    out = {"v2": v2, "nbub": nbub, "nframes": nframes}
    src = (src or "").strip()
    if not src:
        out.update(ok=False, error="no script", line=0)
        return out
    try:
        code = jarvm.compile_script(src, nbub, nframes, v2=v2)
    except jarvm.ScriptError as e:
        out.update(ok=False, error=str(e), line=e.line)
        if v2 is False:                                 # would it compile as v2?
            try:
                jarvm.compile_script(src, nbub, nframes, v2=True)
                out["hint"] = "it compiles as version 2"
            except jarvm.ScriptError:
                pass
        return out
    rep, entries = _trace(code, nbub, nframes, seed, ticks_per_hour, quiet)
    out.update(ok=rep["ok"], error="; ".join(rep["problems"]) or None, line=0,
               size=len(code), hex=code.hex(), disassembly=jarvm.disassemble(code),
               v2_words=jarvm.v2_words(code), report=rep, trace=entries)
    return out


def _script_counts(raw, form):
    nbub = len(raw.get("bubbles") or [])
    nframes = len(raw.get("frames") or [])
    if form == "part" and nframes >= 2:
        nframes -= 1                                    # as jar.compile_script counts them
    return nbub, nframes


def examine(raw, kind=None, form=None, tier=None, v2=None, seed=1, item_id=0):
    """One item as Claude gave it -> everything about it: the checks, the
    script compiled and simulated, the record packed (never signed) and
    decoded back. Each stage reports its own failure and the rest go on
    where they can."""
    v2 = bool(jarvm.V2) if v2 is None else bool(v2)
    out = {"raw": raw, "errors": [], "stage": "check"}
    if not isinstance(raw, dict):
        out["errors"] = ["not an item (an object)"]
        return out
    with version(v2):
        try:
            out["errors"] = jar.check_item(raw, (), form)
        except Exception as e:                          # noqa: BLE001 - a lab shows it
            out["errors"] = ["check_item raised %s: %s" % (type(e).__name__, e)]
    out["ok"] = not out["errors"]
    nbub, nframes = _script_counts(raw, form)
    if (raw.get("script") or "").strip():
        out["script"] = simulate(raw["script"], nbub, max(1, nframes), v2, seed)
        out["script_ships"] = bool(out["script"].get("ok"))
    else:
        out["script"] = None
        out["script_ships"] = False
    if out["errors"]:
        out["stage"] = "refused"
        return out
    try:
        bare = dict(raw)
        bare.pop("script", None)                        # compiled above, with v2 explicit
        tname, price = (tier[0], tier[1]) if isinstance(tier, (list, tuple)) else ("lab", 0)
        with version(v2):
            it = jar.to_item(bare, item_id, int(time.time()),
                             jar.record_tags([tname, kind or raw.get("kind", "")]),
                             None, price, form)
        if out["script_ships"]:
            it["script"] = bytes.fromhex(out["script"]["hex"])
        rec = jar.encode(it)                            # unsigned: never sealed here
        out["record"] = {"size": len(rec), "b64": base64.b64encode(rec).decode()}
        out["item"] = _record_view(rec)
        out["stage"] = "packed"
    except Exception as e:                              # noqa: BLE001
        out["errors"].append("packing failed: %s: %s" % (type(e).__name__, e))
        out["ok"] = False
        out["stage"] = "pack"
    return out


# ---- generating ------------------------------------------------------------------------

def generate(spec, chat=None, progress=None):
    """The lab's generate: one Claude call (or stub) an item, n of them, each
    examined. Nothing is stored or signed."""
    built = build_prompt(spec)
    v2, kind, form, n = built["v2"], built["kind"], built["form"] or None, built["n"]
    tier = tier_for(built["tier"]["name"], built["tier"]["price"])
    prompt = spec.get("prompt") or built["prompt"]
    schema = built["schema"]
    dry = bool(spec.get("dry"))
    effort = spec.get("effort") or tier[2]
    model = spec.get("model") or getattr(shopkeep, "MODEL", False)
    answer = spec.get("answer")
    if isinstance(answer, str) and answer.strip():
        answer = json.loads(answer)
    user = accounts.current()
    seed = int(spec.get("seed") or 1)
    runs = []
    t_all = time.time()
    for k in range(n):
        t0 = time.time()
        run = {"k": k, "source": "pasted" if answer else ("stub" if dry else "claude"),
               "model": None if (dry or answer) else model, "effort": effort}
        try:
            if answer:
                got = answer if isinstance(answer, dict) and "items" in answer else {"items": [answer]}
            elif dry:
                got = stub_answer(schema, serial=1 + k + int(time.time()) % 1000)
            else:
                got = ask.ask_shape(chat, prompt, schema, user=user, limit=None,
                                    timeout=jar.GEN_TIMEOUT, effort=effort, model=model,
                                    cwd=shopkeep.home())
            run["answer"] = got
        except Exception as e:                          # noqa: BLE001 - a lab shows it
            run["error"] = "%s: %s" % (type(e).__name__, e)
            got = None
        run["seconds"] = round(time.time() - t0, 2)
        if got is not None:
            items = got.get("items") if isinstance(got, dict) else None
            if not isinstance(items, list) or not items:
                run["error"] = "the answer has no items"
            else:
                run["items"] = [examine(raw, kind, form, tier, v2, seed) for raw in items]
        runs.append(run)
        if progress:
            progress(k + 1, n)
        if answer:
            break                                       # a pasted answer is examined once
    return {"spec": {k: v for k, v in spec.items() if k not in ("answer",)},
            "prompt": prompt, "edited": prompt != built["prompt"], "schema": schema,
            "tier": built["tier"], "kind": kind, "form": form or "", "v2": v2, "dry": dry,
            "runs": runs, "seconds": round(time.time() - t_all, 2)}


_jobs = {}
_jobs_lock = threading.Lock()


def _sweep():
    now = time.time()
    for jid, j in list(_jobs.items()):
        if now - j["at"] > JOB_TTL:
            del _jobs[jid]


def start_job(spec, chat):
    jid = secrets.token_hex(6)
    user = accounts.current()
    with _jobs_lock:
        _sweep()
        _jobs[jid] = {"at": time.time(), "state": "pending", "done": 0,
                      "n": build_prompt(spec)["n"], "result": None}

    def prog(done, n):
        with _jobs_lock:
            _jobs[jid].update(done=done, n=n)

    def work():
        accounts.set_current(user)
        try:
            res = generate(spec, chat, prog)
        except Exception as e:                          # noqa: BLE001
            traceback.print_exc()
            res = {"error": "%s: %s" % (type(e).__name__, e)}
        with _jobs_lock:
            _jobs[jid].update(state="done", result=res)
    threading.Thread(target=work, daemon=True, name="jarlab-" + jid).start()
    return jid


def job(jid):
    with _jobs_lock:
        j = _jobs.get(jid)
        return dict(j) if j else None


# ---- experiments ---------------------------------------------------------------------

def exp_dir():
    d = os.path.join(dash.state_dir(), "jarlab")
    os.makedirs(d, exist_ok=True)
    return d


def _exp_path(name):
    name = (name or "").strip()
    if not EXP_NAME.match(name):
        raise ValueError("a name is 1-48 letters, digits, space, _ . -")
    return os.path.join(exp_dir(), name + ".json")


def save_experiment(obj):
    path = _exp_path(obj.get("name"))
    obj = dict(obj, saved=int(time.time()))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f)
    os.replace(tmp, path)
    return {"name": obj["name"].strip(), "saved": obj["saved"]}


def load_experiment(name):
    path = _exp_path(name)
    if not os.path.exists(path):
        raise KeyError(name)
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def list_experiments():
    out = []
    for fn in sorted(os.listdir(exp_dir())):
        if not fn.endswith(".json"):
            continue
        try:
            with open(os.path.join(exp_dir(), fn), encoding="utf-8") as f:
                d = json.load(f)
            out.append({"name": fn[:-5], "saved": d.get("saved"), "note": d.get("note", "")})
        except (OSError, ValueError):
            continue
    out.sort(key=lambda e: -(e["saved"] or 0))
    return out


def delete_experiment(name):
    path = _exp_path(name)
    if os.path.exists(path):
        os.remove(path)


# ---- routes ---------------------------------------------------------------------------

def _api(fn):
    """fn(h, args) behind the cookie ("dash" in the route) and an admin."""
    def wrapped(h, path, args):
        from .. import app
        if not app.dash_configured(h):
            h.json({"error": app.DASH_UNCONFIGURED}, 503)
            return
        if not accounts.is_admin():
            h.json({"error": "Jar Lab is the server owner's", "owner_only": True}, 403)
            return
        try:
            fn(h, args)
        except (ValueError, KeyError) as e:
            h.json({"error": "%s" % (e.args[0] if e.args else type(e).__name__)}, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _body(h):
    raw = h.body(BODY_MAX)
    try:
        d = json.loads(raw or b"{}")
    except ValueError:
        raise ValueError("not JSON")
    if not isinstance(d, dict):
        raise ValueError("a JSON object, please")
    return d


def _arg(args, name, default=None):
    return (args.get(name) or [default])[0]


def get_page(h, path, args):
    """Jar Lab (browser, cookie, the owner)"""
    from .. import app
    if not app.dash_configured(h) or not dash.logged_in(h):
        h.redirect("/dash")
        return
    if not accounts.is_admin():
        h.html("<p>Jar Lab is the server owner's.</p>", 403)
        return
    with open(PAGE_FILE, encoding="utf-8") as f:
        h.html(f.read())


def get_redirect(h, path, args):
    """Jar Lab lives at /dash/jarlab (the cookie's path)"""
    h.redirect("/dash/jarlab")


@_api
def get_defaults(h, args):
    """the prompt's pieces"""
    v = _arg(args, "v2")
    h.json(defaults(None if v in (None, "") else v == "1"))


@_api
def post_roll(h, args):
    """the code's dice for one item"""
    d = _body(h)
    v = d.get("v2")
    h.json(roll(d.get("seed"), d.get("kind") or None, None if v in (None, "") else bool(v)))


@_api
def post_prompt(h, args):
    """the exact prompt and schema, no Claude"""
    h.json(build_prompt(_body(h)))


@_api
def post_generate(h, args):
    """make items (Claude or a stub), examined; never stored or signed"""
    d = _body(h)
    build_prompt(d)                                     # a bad spec is a 400 now, not in a job
    if d.get("dry") or d.get("answer") or d.get("wait"):
        h.json(generate(d, h.chat))
        return
    h.json({"job": start_job(d, h.chat)})


@_api
def get_job(h, args):
    """a generate's progress and result"""
    j = job(_arg(args, "id", ""))
    if not j:
        h.json({"error": "no such job"}, 404)
        return
    h.json(j)


@_api
def post_simulate(h, args):
    """compile a script and give it a simulated day"""
    d = _body(h)
    item = d.get("item")
    if isinstance(item, str) and item.strip():
        item = json.loads(item)
    form = d.get("form") or None
    if isinstance(item, dict):
        if "items" in item and isinstance(item["items"], list) and item["items"]:
            item = item["items"][0]
        src = item.get("script") or ""
        nbub, nframes = _script_counts(item, form)
    else:
        src = d.get("script") or ""
        nbub = int(d.get("nbub") if d.get("nbub") not in (None, "") else jarvm.MAX_BUB)
        nframes = int(d.get("nframes") if d.get("nframes") not in (None, "") else jarvm.MAX_FRAMES)
    v = d.get("v2")
    h.json(simulate(src, nbub, max(1, nframes), None if v in (None, "") else bool(v),
                    int(d.get("seed") or 1), max(10, min(600, int(d.get("ticks_per_hour") or 120))),
                    not d.get("all")))


@_api
def get_experiments(h, args):
    """saved experiments"""
    h.json({"experiments": list_experiments()})


@_api
def get_experiment(h, args):
    """one saved experiment"""
    try:
        h.json(load_experiment(_arg(args, "name", "")))
    except KeyError:
        h.json({"error": "no such experiment"}, 404)


@_api
def post_experiment(h, args):
    """save an experiment"""
    h.json(save_experiment(_body(h)))


@_api
def post_experiment_delete(h, args):
    """delete an experiment"""
    delete_experiment(_body(h).get("name"))
    h.json({"ok": True})


ROUTES = [
    ("GET", "/jarlab", get_redirect, "open"),
    ("GET", "/dash/jarlab", get_page, "open"),
    ("GET", "/dash/jarlab/api/defaults", get_defaults, "dash"),
    ("POST", "/dash/jarlab/api/roll", post_roll, "dash"),
    ("POST", "/dash/jarlab/api/prompt", post_prompt, "dash"),
    ("POST", "/dash/jarlab/api/generate", post_generate, "dash"),
    ("GET", "/dash/jarlab/api/job", get_job, "dash"),
    ("POST", "/dash/jarlab/api/simulate", post_simulate, "dash"),
    ("GET", "/dash/jarlab/api/experiments", get_experiments, "dash"),
    ("GET", "/dash/jarlab/api/experiment", get_experiment, "dash"),
    ("POST", "/dash/jarlab/api/experiment", post_experiment, "dash"),
    ("POST", "/dash/jarlab/api/experiment/delete", post_experiment_delete, "dash"),
]
