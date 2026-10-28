"""Server code that belongs to an app, kept with the app.

Most apps need no server code of their own: what they keep goes in the store
(server/kv.py -- values, counters, queues, pub/ for friends), what they share
in rooms (server/rooms.py), and what they ask Claude in server/ask.py. An app
important enough to need more -- Jar Factory's shop, Chat, Crossword's puzzle
library -- has a server plugin: apps/NAME/server/, a Python package, imported
here as `apps.NAME.server` when the server starts. Nothing in server/ names
an app; this finds them.

A plugin may define, at its top level:

    ROUTES        [(METHOD, PATH, fn, AUTH)], as every server module does
                  (server/routes.py says what AUTH may be). Every PATH starts
                  /NAME, or with a prefix the app's source declares:
                  `@app routes /msg /share` (tools/appmeta.py). Two plugins
                  may not claim one path.
    start()       called once by `python -m server` after the routes are
                  mounted: background work (Crossword's hourly NYT Midi).
                  Not called by the tests or by a plugin's own command line.
    notify_poll(args, user) -> (head, lines)
                  its part of /notify/poll (server/msg.py), which every
                  device asks every half minute: `head` the number the poll's
                  first line carries for it (Chat's last message id) or None,
                  `lines` its own lines. One plugin may give a head (Chat):
                  the wire format has one place for it.
    TALK_KINDS    {kind: {"prompt": TEXT, "check": fn(text) -> (line, why)
                  or None}}: a kind of document Claude may revise in /talk
                  (server/talk.py) -- what Claude is told about its format, and
                  the check a revision must pass before it is offered (MIDI's
                  songs).
    DASH_VIEWS    [{"id": "notes", "title": "Notes", "icon": SVG, "html":
                  "dash/view.html", "js": "dash/view.js", "order": 30}]: a
                  page of the dashboard (server/dashboard.html), with an entry
                  in both its menus (the side one and the phone's bottom one;
                  "short" is a shorter label for the bottom one). `icon` is
                  the inside of a 24x24 stroked <svg>, paths and all; `order`
                  places it among the built-in pages, which are 0 Overview,
                  10 SD card, 20 Terminal, 60 Accounts, 70 Server, 80 Updates,
                  90 Settings. The page is at #/ID; its id must be new.
    DASH_CARDS    [{"id": "toggl", "view": "accounts", "html": ..., "js":
                  ...}]: a card put into a built-in page instead of a page of
                  its own (Toggl's, on Accounts & keys -- the only page that
                  takes cards so far).
    dash_state()  -> a JSON-able value, put in /dash/api/state under the
                  plugin's name: what its card or the Overview shows without
                  another request (Toggl: connected, and what is running).

The dashboard's fragments are files in the plugin's folder (paths relative
to it), read on every /dash/api/plugins, as dashboard.html is read on every
/dash, so editing one needs no restart. The page fetches that list once at
load, writes each fragment's HTML into a <section class="view block">
(a page) or the page's card row (a card), and runs its script after its own,
as a plain script in the page's scope. A fragment uses the page's helpers --
$(), $$(), esc(), ic(), api(), toast(), modal(), fmtEpoch(), signedOut(),
ApiError, and the state S (S.st is /dash/api/state) -- and tells the page
what it wants with

    dashPlugin(ID, {show(), render(), tick(), glance() -> [[label, dot,
                    html, href], ...], acts: {NAME: fn(button, event)}})

show() when its page opens, render() whenever the page redraws (the state
has arrived or changed), tick() each second, glance() its rows on the
Overview (a "warn" or "bad" dot counts as something needing you, and on a
card's page's menu dot too), and acts the data-act values its buttons carry.
A <style> goes in the HTML. Same origin, the dashboard's cookie, nothing
loaded from anywhere else.

These names are the hooks: a plugin's own function may not be called one
(Toggl's "start a timer" is start_entry, or every server start would have
called it).

A plugin reaches the server through its modules by their full names --
`from server import accounts, ask, kv, people, friends, rooms, sign, store,
wire, msg, dash, dashapi, jobs, routes, pixels` -- and through nothing else
of it. (dashapi for dashboard routes: `dashapi.api` makes fn(h, args) a
cookie route, `dashapi.reply` answers JSON, `dashapi.body_json` reads it.) Those are the ones
kept stable for plugins; the HTTP layer (server/app.py) is not one of them.

A plugin that fails to import is left out with a line on stderr: one app's
broken plugin must not take the server, and every other app, down with it.
"""
import importlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools import appmeta  # noqa: E402

_loaded = None


def _prefixes(name, metas):
    """The URL prefixes the app folder `name` may serve: /NAME and what its
    apps' @app routes lines add."""
    out = ["/" + name]
    for a in metas.values():
        if a.own_dir and os.path.basename(a.own_dir) == name:
            out += a.routes
    return out


def _allowed(path, prefixes):
    return any(path == p or path.startswith(p.rstrip("/") + "/") or
               (p.endswith("*") and path.startswith(p[:-1])) for p in prefixes)


def load(dirs=None, strict=False):
    """{name: module} for every plugin, imported once. strict: a plugin that
    fails to import, or claims a path it may not, is an exception (the
    tests), not a line on stderr (the server)."""
    global _loaded
    if _loaded is not None and dirs is None:
        return _loaded
    metas = appmeta.apps(*(dirs or ()))
    out, claimed = {}, {}
    for name, path in appmeta.plugin_dirs(*(dirs or ())).items():
        try:
            mod = importlib.import_module("apps.%s.server" % name)
            routes = getattr(mod, "ROUTES", [])
            pre = _prefixes(name, metas)
            for r in routes:
                if not _allowed(r[1], pre):
                    raise ValueError("%s serves %s, outside %s (say it with @app routes)"
                                     % (name, r[1], " ".join(pre)))
                key = (r[0], r[1])
                if key in claimed:
                    raise ValueError("%s and %s both serve %s %s" % (claimed[key], name, r[0], r[1]))
                claimed[key] = name
            _check_dash(name, mod)
        except Exception as e:                       # noqa: BLE001 -- see the docstring
            if strict:
                raise
            sys.stderr.write("plugin %s: left out (%s: %s)\n" % (name, type(e).__name__, e))
            continue
        out[name] = mod
    if dirs is None:
        _loaded = out
    return out


# The dashboard's own pages (server/dashboard.html): a plugin's page may not
# take one of their ids, and a card must go in one that takes cards.
DASH_BUILTIN = ("home", "files", "term", "accounts", "server", "updates", "settings")
DASH_CARD_VIEWS = ("accounts",)
_DASH_ID = re.compile(r"^[a-z][a-z0-9-]{0,23}$")


def _fragment(mod, rel):
    """A dashboard fragment's path, inside the plugin's folder."""
    base = os.path.dirname(os.path.abspath(mod.__file__))
    p = os.path.normpath(os.path.join(base, rel or ""))
    if not rel or not p.startswith(base + os.sep):
        raise ValueError("%r is not a file in %s" % (rel, base))
    return p


def _check_dash(name, mod):
    """A plugin's DASH_VIEWS and DASH_CARDS make sense and their files are
    there -- found when the server starts, not when someone opens the page."""
    for kind in ("DASH_VIEWS", "DASH_CARDS"):
        for v in getattr(mod, kind, []):
            if not _DASH_ID.match(v.get("id") or ""):
                raise ValueError("%s %s: id %r is not a-z, 0-9, -" % (name, kind, v.get("id")))
            if v["id"] in DASH_BUILTIN:
                raise ValueError("%s %s: %s is the dashboard's own" % (name, kind, v["id"]))
            if kind == "DASH_VIEWS" and not v.get("title"):
                raise ValueError("%s DASH_VIEWS %s: no title" % (name, v["id"]))
            if kind == "DASH_CARDS" and v.get("view") not in DASH_CARD_VIEWS:
                raise ValueError("%s DASH_CARDS %s: view must be one of %s"
                                 % (name, v["id"], ", ".join(DASH_CARD_VIEWS)))
            for key in ("html", "js"):
                if v.get(key) and not os.path.isfile(_fragment(mod, v[key])):
                    raise ValueError("%s %s %s: %s is missing" % (name, kind, v["id"], v[key]))


def _read(mod, rel):
    if not rel:
        return ""
    with open(_fragment(mod, rel), encoding="utf-8") as f:
        return f.read()


def dash_parts():
    """{"views": [...], "cards": [...]} for /dash/api/plugins, each with its
    plugin's name and its HTML and script as they are on disk now. An id
    another plugin took, or a fragment that will not read, is left out with
    a line on stderr."""
    out, seen = {"views": [], "cards": []}, set()
    for name, mod in load().items():
        for kind, key in (("DASH_VIEWS", "views"), ("DASH_CARDS", "cards")):
            for v in getattr(mod, kind, []):
                if v["id"] in seen:
                    sys.stderr.write("plugin %s: dashboard id %s is taken\n" % (name, v["id"]))
                    continue
                try:
                    part = {"plugin": name, "id": v["id"], "html": _read(mod, v.get("html")),
                            "js": _read(mod, v.get("js"))}
                except (OSError, ValueError) as e:
                    sys.stderr.write("plugin %s: dashboard %s left out (%s)\n" % (name, v["id"], e))
                    continue
                seen.add(v["id"])
                if key == "views":
                    part.update(title=v["title"], short=v.get("short") or v["title"],
                                icon=v.get("icon", ""), order=v.get("order", 50))
                else:
                    part["view"] = v["view"]
                out[key].append(part)
    out["views"].sort(key=lambda v: v["order"])
    return out


def dash_states(skip=()):
    """{plugin name: its dash_state()} for /dash/api/state. A name the state
    already has (`skip`) is not overwritten; a dash_state that fails gives
    {"error": why} rather than taking the whole state down with it."""
    out = {}
    for name, mod in load().items():
        fn = getattr(mod, "dash_state", None)
        if not fn or name in skip:
            continue
        try:
            out[name] = fn()
        except Exception as e:                       # noqa: BLE001
            out[name] = {"error": "%s: %s" % (type(e).__name__, e)}
    return out


def routes():
    """Every plugin's routes, in plugin order."""
    out = []
    for mod in load().values():
        out += list(getattr(mod, "ROUTES", []))
    return out


def start_all():
    for name, mod in load().items():
        fn = getattr(mod, "start", None)
        if fn:
            try:
                fn()
            except Exception as e:                   # noqa: BLE001
                sys.stderr.write("plugin %s: start failed (%s: %s)\n" % (name, type(e).__name__, e))


def notify_parts(args, user):
    """(head or None, [lines]) from every plugin's notify_poll."""
    head, lines = None, []
    for name, mod in load().items():
        fn = getattr(mod, "notify_poll", None)
        if not fn:
            continue
        h, ls = fn(args, user)
        if h is not None:
            head = h
        lines += list(ls)
    return head, lines


def talk_kind(kind):
    """What a plugin says about a /talk document kind, or None."""
    for mod in load().values():
        k = getattr(mod, "TALK_KINDS", {}).get(kind)
        if k:
            return k
    return None
