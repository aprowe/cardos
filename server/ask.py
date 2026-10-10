"""Ask Claude for data of a given shape: a JSON value that fits a schema.

Every Claude helper on this server (daily, midi, talk) wrote its own prompt,
its own reading of the answer and its own "try once more". This is that,
general: a prompt and a schema in, a checked Python value out.

    ask_shape(chat, prompt, schema, user=..., limit=DAILY)
        -> the parsed value, valid against `schema`
        raises RateLimited (the person's day is used up), Invalid (two
        answers that did not fit), BadSchema, chat.ClaudeError

It asks through chat.ask_once -- no tools, no repository -- with the schema
in the prompt; reads the first JSON value out of the answer (a ```json
fence or prose round it is forgiven); checks it with validate() below; and
if it does not fit, asks once more with the reasons. Each call counts one
against the person's day (UTC), in the store (kv.sqlite3, namespace
"sys/ask", which no route reaches), whether or not it succeeds.

The schema is a small part of JSON Schema, checked strictly: a keyword this
does not enforce is refused (BadSchema) rather than ignored, so a caller
never believes a rule holds that does not.

    type        "object" "array" "string" "integer" "number" "boolean"
                "null", or a list of them
    enum, const
    properties, required, additionalProperties (true, false or a schema)
    items, minItems, maxItems
    minLength, maxLength, pattern (re.search; anchor it yourself)
    minimum, maximum

The route, for devices; server code should call ask_shape directly:

    POST /ask   body: line 1 the schema as one line of JSON, then the prompt
                -> "ID\\n"; 400 a bad schema or sizes, 429 the day is used
    GET  /ask?id=ID
                -> "pending\\n" | "ok\\n" then the value as one line of JSON
                   and "\\n" | "error WHY\\n" (404 for an id that is not yours)

Limits: a prompt of PROMPT_MAX bytes, a schema of SCHEMA_MAX, an answer of
ANSWER_MAX; ROUTE_DAILY asks a day through the route (server callers pass
their own limit, DAILY by default -- one count serves both, so the route
stops at ROUTE_DAILY of the day's total).
"""
import json
import os
import re
import secrets
import sys
import threading
import time

from . import chat as _chat
from . import jobs, kv
from .jobs import MINE

DAILY = int(os.environ.get("CARDOS_ASK_DAILY") or 50)
ROUTE_DAILY = int(os.environ.get("CARDOS_ASK_ROUTE_DAILY") or 20)
PROMPT_MAX = 4000
SCHEMA_MAX = 2000
ANSWER_MAX = 16 * 1024
TIMEOUT = 180
NS = "sys/ask"

SYSTEM = ("You answer for a program, not a person. Reply with exactly one JSON value "
          "that matches the JSON Schema you are given, and nothing else: no prose, "
          "no code fence.")


class AskError(Exception):
    pass


class RateLimited(AskError):
    pass


class Invalid(AskError):
    pass


class BadSchema(AskError, ValueError):
    pass


# ---- the schema -------------------------------------------------------------------

TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}
KEYWORDS = {"type", "enum", "const", "properties", "required", "additionalProperties",
            "items", "minItems", "maxItems", "minLength", "maxLength", "pattern",
            "minimum", "maximum", "description", "title"}


def check_schema(schema, path="$"):
    """BadSchema if this is not a schema validate() enforces whole."""
    if not isinstance(schema, dict):
        raise BadSchema("%s: a schema is an object" % path)
    unknown = set(schema) - KEYWORDS
    if unknown:
        raise BadSchema("%s: not enforced here: %s" % (path, ", ".join(sorted(unknown))))
    t = schema.get("type")
    for name in ([t] if isinstance(t, str) else t or []):
        if name not in TYPES:
            raise BadSchema("%s: no type %r" % (path, name))
    if "pattern" in schema:
        try:
            re.compile(schema["pattern"])
        except (re.error, TypeError) as e:
            raise BadSchema("%s: pattern: %s" % (path, e))
    for name, sub in (schema.get("properties") or {}).items():
        check_schema(sub, "%s.%s" % (path, name))
    if isinstance(schema.get("additionalProperties"), dict):
        check_schema(schema["additionalProperties"], path + ".*")
    if "items" in schema:
        check_schema(schema["items"], path + "[]")


def validate(value, schema, path="$"):
    """What is wrong with `value` against `schema`: a list of lines, empty if
    nothing is."""
    errs = []
    t = schema.get("type")
    if t is not None:
        names = [t] if isinstance(t, str) else t
        if not any(TYPES[n](value) for n in names):
            return ["%s: should be %s" % (path, " or ".join(names))]
    if "const" in schema and value != schema["const"]:
        errs.append("%s: should be %s" % (path, json.dumps(schema["const"])))
    if "enum" in schema and value not in schema["enum"]:
        errs.append("%s: should be one of %s" % (path, json.dumps(schema["enum"])))
    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            errs.append("%s: at least %d characters" % (path, schema["minLength"]))
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errs.append("%s: at most %d characters, not %d" % (path, schema["maxLength"], len(value)))
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errs.append("%s: should match %s" % (path, schema["pattern"]))
    if TYPES["number"](value):
        if "minimum" in schema and value < schema["minimum"]:
            errs.append("%s: at least %s" % (path, schema["minimum"]))
        if "maximum" in schema and value > schema["maximum"]:
            errs.append("%s: at most %s" % (path, schema["maximum"]))
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errs.append("%s: at least %d items" % (path, schema["minItems"]))
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errs.append("%s: at most %d items, not %d" % (path, schema["maxItems"], len(value)))
        if "items" in schema:
            for i, v in enumerate(value):
                errs += validate(v, schema["items"], "%s[%d]" % (path, i))
    if isinstance(value, dict):
        props = schema.get("properties") or {}
        for name in schema.get("required") or []:
            if name not in value:
                errs.append("%s: missing %s" % (path, name))
        extra = schema.get("additionalProperties", True)
        for name, v in value.items():
            sub = "%s.%s" % (path, name)
            if name in props:
                errs += validate(v, props[name], sub)
            elif extra is False:
                errs.append("%s: not allowed" % sub)
            elif isinstance(extra, dict):
                errs += validate(v, extra, sub)
    return errs


def extract(text):
    """The first JSON value in an answer: the whole of it, or inside a code
    fence, or from the first { or [. ValueError if there is none."""
    text = (text or "").strip()
    m = re.search(r"```(?:json)?\s*\n(.*?)```", text, re.S)
    if m:
        text = m.group(1).strip()
    try:
        return json.loads(text)
    except ValueError:
        pass
    dec = json.JSONDecoder()
    for i, c in enumerate(text):
        if c in "{[":
            try:
                return dec.raw_decode(text, i)[0]
            except ValueError:
                continue
    raise ValueError("no JSON in the answer")


# ---- the day's count ---------------------------------------------------------------------

def take_turn(user, limit, store=None):
    """Count one ask for `user` today; RateLimited past `limit`."""
    st = store or kv.store()
    day = time.strftime("%Y-%m-%d", time.gmtime(st.clock()))
    n = st.incr(NS, "%s/%s" % (user or kv.NOBODY, day), ttl=2 * 86400)
    if n > limit:
        raise RateLimited("%d asks a day; try tomorrow" % limit)
    return n


# ---- asking -----------------------------------------------------------------------------

def _prompt(prompt, schema):
    return "%s\n\nAnswer with one JSON value matching this JSON Schema:\n%s" % (
        prompt, json.dumps(schema, separators=(",", ":")))


def ask_shape(chat, prompt, schema, user=MINE, limit=DAILY, timeout=TIMEOUT, system=SYSTEM,
              store=None, effort="medium"):
    """The value Claude gives for `prompt`, checked against `schema`; one
    more try if it does not fit. `user` is whose day it counts against
    (the request's by default; pass it from a thread). limit=None counts
    nothing -- for the server's own scheduled work."""
    check_schema(schema)
    if user is MINE:
        user = kv.me()
    if limit is not None:
        take_turn(user, limit, store)
    asked = _prompt(prompt, schema)
    why = None
    for attempt in range(2):
        text, _ = _chat.ask_once(chat, asked, timeout, system=system, effort=effort)
        if len(text) > ANSWER_MAX:
            why = ["the answer was %d bytes, more than %d" % (len(text), ANSWER_MAX)]
        else:
            try:
                value = extract(text)
                why = validate(value, schema)
            except ValueError as e:
                why = [str(e)]
            if not why:
                return value
        asked = "%s\n\nYour last answer did not fit:\n%s\n%s\n\nAnswer again, only the JSON." % (
            _prompt(prompt, schema), text[:2000], "\n".join("- " + w for w in why[:8]))
    raise Invalid("; ".join(why[:4]))


# ---- the route ----------------------------------------------------------------------------

_jobs = jobs.Table(1800)


def _run(jid, chat, prompt, schema, user):
    try:
        result = ("ok", ask_shape(chat, prompt, schema, user=user, limit=None))
    except Exception as e:                      # Claude, the check
        result = ("error", str(e) or type(e).__name__)
    _jobs.replace(jid, result)
    sys.stderr.write("ask: %s %s\n" % (jid, result[0]))


def post_ask(h, path, args):
    """ask Claude for JSON of a shape: line 1 the schema, then the prompt"""
    try:
        raw = h.body(PROMPT_MAX + SCHEMA_MAX + 2).decode("utf-8", "replace")
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    head, _, prompt = raw.partition("\n")
    prompt = prompt.strip()
    if len(head.encode()) > SCHEMA_MAX or len(prompt.encode()) > PROMPT_MAX:
        h.text("error a schema is %d bytes at most, a prompt %d\n" % (SCHEMA_MAX, PROMPT_MAX), 400)
        return
    if not prompt:
        h.text("error what should it answer?\n", 400)
        return
    try:
        schema = json.loads(head)
        check_schema(schema)
    except (ValueError, BadSchema) as e:
        h.text("error schema: %s\n" % e, 400)
        return
    if not (h.chat and h.chat.claude):
        h.text("error this server runs without Claude\n", 503)
        return
    user = kv.me()
    try:
        take_turn(user, ROUTE_DAILY)
    except RateLimited as e:
        h.text("error %s\n" % e, 429)
        return
    jid = secrets.token_hex(5)
    _jobs.put(jid, ("pending", None))
    threading.Thread(target=_run, args=(jid, h.chat, prompt, schema, user), daemon=True).start()
    h.text(jid + "\n")


def get_ask(h, path, args):
    """the answer to an ask, once it is there"""
    jid = (args.get("id") or [""])[0]
    job = _jobs.get(jid)
    if not job:
        h.text("error no such request\n", 404)
        return
    state, value = job
    if state == "pending":
        h.text("pending\n")
    elif state == "ok":
        h.text("ok\n%s\n" % json.dumps(value, separators=(",", ":")))
    else:
        h.text("error %s\n" % " ".join(str(value).split()))


ROUTES = [
    ("POST", "/ask", post_ask, "device_or_dash"),
    ("GET", "/ask", get_ask, "device_or_dash"),
]
