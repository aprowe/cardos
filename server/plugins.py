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

A plugin reaches the server through its modules by their full names --
`from server import accounts, ask, kv, people, friends, rooms, sign, store,
wire, msg, dash, jobs` -- and through nothing else of it. Those are the ones
kept stable for plugins; the HTTP layer (server/app.py) is not one of them.

A plugin that fails to import is left out with a line on stderr: one app's
broken plugin must not take the server, and every other app, down with it.
"""
import importlib
import importlib.machinery
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools import appmeta  # noqa: E402

_loaded = None


def _import(name, path):
    """apps.NAME.server for this repository's apps; a plugin in another
    repository's apps folder (CARDOS_APP_DIRS) is imported from where it is,
    as cardos_apps.NAME.server, its own relative imports intact."""
    inside = os.path.normcase(os.path.abspath(path)).startswith(
        os.path.normcase(os.path.join(ROOT, "apps")) + os.sep)
    if inside:
        return importlib.import_module("apps.%s.server" % name)
    full = "cardos_apps.%s.server" % name
    if full in sys.modules:
        return sys.modules[full]
    for pkg in ("cardos_apps", "cardos_apps." + name):        # empty parents
        if pkg not in sys.modules:
            m = importlib.util.module_from_spec(importlib.machinery.ModuleSpec(pkg, None, is_package=True))
            m.__path__ = []
            sys.modules[pkg] = m
    spec = importlib.util.spec_from_file_location(
        full, os.path.join(path, "__init__.py"), submodule_search_locations=[path])
    mod = importlib.util.module_from_spec(spec)
    sys.modules[full] = mod
    try:
        spec.loader.exec_module(mod)
    except BaseException:
        del sys.modules[full]
        raise
    return mod


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
            mod = _import(name, path)
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
        except Exception as e:                       # noqa: BLE001 -- see the docstring
            if strict:
                raise
            sys.stderr.write("plugin %s: left out (%s: %s)\n" % (name, type(e).__name__, e))
            continue
        out[name] = mod
    if dirs is None:
        _loaded = out
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
