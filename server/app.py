"""The CardOS server's HTTP layer: routing, the shared secret, body limits.

Everything the device reaches over the network that is not the open internet
comes through here -- Build's conversation, voice, updates, screenshots, the
screen stream, the web renderer. None of that lives in this file. Each service
module declares its own routes:

    ROUTES = [("GET", "/update", get_manifest),
              ("GET", "/update/app/*", get_app)]    # * : a prefix

and a route function takes (handler, path, args). This file owns only what
every route shares: dispatch, the token, the error line a crashed route still
owes the device, and the helpers for writing a reply.

A route is behind the token unless its tuple says otherwise (the kinds are
in server/routes.py): "device_or_dash" also takes the dashboard's cookie,
"dash" takes only the cookie, "admin" is the token and the owner, and an
"open" route checks for itself when it has something worth protecting --
/render shows the banner to anyone but renders only for the device.

Run it with `python -m server`; see __main__.py for the flags.
"""
import hmac
import json
import os
import sys
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler

from . import accounts, agent, chat, m5hub, daily, dash, dashapi, files, google, images, midi, msg, music, notes, photos, shots, talk, toggl, tz, updates, voice
from .chat import ROOT as ROOT_DIR


def int_arg(args, name, default):
    """A query parameter as an int, or the default; a value that is not a
    number is a client error, not a traceback."""
    raw = (args.get(name) or [None])[0]
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError:
        raise ValueError("%s=%r is not a number" % (name, raw))


# ---- routes that belong to the server itself ------------------------------

def get_status(h, path, args):
    """what is running here, and what it will hand the agent

    There is no way to tell a stale server from a fresh one by looking at it,
    and a stale one answers every question with the last bug you fixed."""
    c = h.chat
    # Behind the token when there is one: it names the repo and the open
    # session, which is more than a stranger needs.
    if c and c.token and not h.authorised():
        return
    env = c._child_env() if c else {}
    h.text("claude:        %s\n" % (c.claude if c else "-") +
           "api key given: %s\n" % ("yes" if "ANTHROPIC_API_KEY" in env else "no") +
           "session:       %s\n" % (c.session_id if c and c.session_id else "none yet") +
           "token needed:  %s\n" % ("yes" if c and c.token else "no") +
           "builds:        %s\n" % ("yes, into " + h.store if getattr(c, "store", None)
                                    else "no") +
           "repo:          %s\n" % ROOT_DIR)


def get_banner(h, path, args):
    """this list"""
    lines = ["cardos server"]
    for method, pattern, fn, _ in ALL_ROUTES:
        doc = (fn.__doc__ or "").strip().split("\n")[0]
        lines.append("  %-4s %-20s %s" % (method, pattern, doc))
    h.text("\n".join(lines) + "\n")


STATIC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
_TYPES = {".png": "image/png", ".jpg": "image/jpeg", ".gif": "image/gif", ".css": "text/css"}


def get_static(h, path, args):
    """a file from server/static: pictures for an email, say -- public, since
    an email client fetches them with no cookie. Nothing secret goes there."""
    name = path[len("/dash/static/"):]
    full = os.path.normpath(os.path.join(STATIC, name))
    ext = os.path.splitext(full)[1].lower()
    if not full.startswith(STATIC + os.sep) or ext not in _TYPES or not os.path.isfile(full):
        h.send_error(404)
        return
    with open(full, "rb") as f:
        data = f.read()
    h.send_response(200)
    h.send_header("Content-Type", _TYPES[ext])
    h.send_header("Content-Length", str(len(data)))
    h.send_header("Cache-Control", "public, max-age=86400")
    h.end_headers()
    h.wfile.write(data)


SERVER_ROUTES = [
    ("GET", "/", get_banner, "open"),
    ("GET", "/status", get_status, "open"),
    ("GET", "/dash/static/*", get_static, "open"),
]


def _render_routes():
    """The renderer drags in Pillow's filters and the DevTools client; a
    server that cannot import them still serves everything else."""
    try:
        from .render import render
        return render.ROUTES
    except ImportError as e:
        sys.stderr.write("render: unavailable (%s)\n" % e)
        return []


def _screen_routes():
    """mss and numpy, for the desktop stream -- a PC-only feature."""
    try:
        from . import screen
        return screen.ROUTES
    except ImportError as e:
        sys.stderr.write("screen: unavailable (%s)\n" % e)
        return []


def _normalise(routes):
    return [r if len(r) == 4 else r + ("token",) for r in routes]


ALL_ROUTES = _normalise(SERVER_ROUTES + chat.ROUTES + agent.ROUTES + m5hub.ROUTES + updates.ROUTES +
                        voice.ROUTES + shots.ROUTES + tz.ROUTES + dash.ROUTES + dashapi.ROUTES +
                        google.ROUTES + files.ROUTES + toggl.ROUTES + notes.ROUTES + daily.ROUTES + images.ROUTES + photos.ROUTES + music.ROUTES + midi.ROUTES + talk.ROUTES + msg.ROUTES +
                        _render_routes() +
                        _screen_routes())


def find_route(method, path):
    """(fn, auth) for a request, or None. Exact paths first; then patterns
    ending in *, longest first, so a prefix never shadows a longer one."""
    for m, pattern, fn, auth in ALL_ROUTES:
        if m == method and pattern == path:
            return fn, auth
    prefixes = sorted((r for r in ALL_ROUTES if r[1].endswith("*") and r[0] == method),
                      key=lambda r: -len(r[1]))
    for m, pattern, fn, auth in prefixes:
        if path.startswith(pattern[:-1]):
            return fn, auth
    return None


class Handler(BaseHTTPRequestHandler):
    """One request. The services are class attributes, set once by main()."""
    chrome = None
    chat = None
    voice = None
    # --store DIR: /update serves what server/build.py published there, never
    # the build tree itself, which is half-written while a build runs.
    store = None

    # ---- helpers for route functions ---------------------------------------

    int_arg = staticmethod(int_arg)

    def text(self, body, code=200, headers=()):
        self._send(code, "text/plain; charset=utf-8", body, headers)

    def html(self, body, code=200, headers=()):
        """For the dashboard, the one thing here a browser reads."""
        self._send(code, "text/html; charset=utf-8", body,
                   (("Cache-Control", "no-store"),) + tuple(headers))

    def redirect(self, url, headers=()):
        self._send(303, "text/plain; charset=utf-8", "see %s\n" % url,
                   (("Location", url),) + tuple(headers))

    def _send(self, code, ctype, body, headers):
        data = body.encode("utf-8", "replace")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def file(self, path):
        """A whole file, with its length up front so the device's download
        knows when it is done rather than waiting for the socket to close."""
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self, limit):
        """The request body, refused rather than read if it is longer than
        the route could possibly want. Content-Length was trusted whole: one
        request claiming four gigabytes had the server trying to hold it."""
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ValueError("bad Content-Length")
        if n < 0 or n > limit:
            raise ValueError("body of %d bytes is more than %d" % (n, limit))
        return self.rfile.read(n) if n else b""

    def authorised(self):
        """A shared secret, if one was asked for.

        Off by default because the common case is a device and a laptop on one
        home network, and a token the device has no way to be told is a token
        nobody uses. On when it matters -- see the warning at startup."""
        if not self.chat or not self.chat.token:
            return True
        given_all = [self.headers.get("X-Token", ""), self.headers.get("Authorization", "")]
        if given_all[1].startswith("Bearer "):
            given_all[1] = given_all[1][7:]
        # With accounts, a device's token says whose it is (server/accounts.py).
        if accounts.enabled():
            for given in given_all:
                user = accounts.user_for_token(given)
                if user:
                    accounts.set_current(user)
                    return True
            self.text("unauthorised\n", 403)
            return False
        # Two spellings, because the device has only one. CardApi's http()
        # takes a bearer token and sends "Authorization: Bearer x" -- there is
        # no way to set an arbitrary header from an app -- while curl and a
        # browser console reach for X-Token. Both are the same string.
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            auth = auth[7:]
        for given in (self.headers.get("X-Token", ""), auth):
            if hmac.compare_digest(self.chat.token, given):
                return True
        self.text("unauthorised\n", 403)
        return False

    def device_or_dash(self):
        """The device's bearer or the dashboard's cookie; a 403 otherwise.

        Without accounts this takes only "Authorization: Bearer", not the
        X-Token spelling authorised() also reads -- as it always has."""
        if dash.logged_in(self):
            return True
        tok = self.chat.token if self.chat else None
        if not tok:
            return True
        if accounts.enabled():
            return self.authorised()               # says whose, or answers 403
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer ") and hmac.compare_digest(tok, auth[7:]):
            return True
        self.text("error signed out\n", 403)
        return False

    def dash_signed_in(self):
        """The dashboard's cookie, never the bearer. Refusals are JSON, which
        the page reads: a 403 sends it to sign in."""
        if not dash.password() or not dash._server_token(self):
            self.json({"error": "the dashboard needs --token and DASH_PASSWORD"}, 503)
            return False
        if not dash.logged_in(self):
            self.json({"error": "signed out"}, 403)
            return False
        return True

    def json(self, obj, code=200, headers=()):
        self._send(code, "application/json", json.dumps(obj),
                   (("Cache-Control", "no-store"),) + tuple(headers))

    def update_files(self, flavor=updates.DEFAULT_FLAVOR):
        """(firmware, apps_dir) that /update serves for a flavor."""
        if self.store:
            return updates.store_paths(self.store, flavor)
        return updates.firmware_path(flavor), updates.APPS_DIR

    # ---- dispatch ----------------------------------------------------------

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PATCH(self):
        self._dispatch("PATCH")

    def do_DELETE(self):
        self._dispatch("DELETE")

    def _dispatch(self, method):
        # An exception inside a route used to close the socket with no status
        # line at all: the device then waited out its whole timeout rather
        # than reading one line saying what went wrong. Everything is wrapped,
        # and the traceback goes to the terminal as before.
        try:
            q = urllib.parse.urlparse(self.path)
            found = find_route(method, q.path)
            if not found:
                self.send_error(404)
                return
            fn, auth = found
            accounts.set_current(None)          # nobody, until a token or cookie says
            if auth == "device_or_dash":
                if not self.device_or_dash():
                    return
            elif auth == "dash":
                if not self.dash_signed_in():
                    return
            elif auth != "open" and not self.authorised():
                return
            # The repo-editing agent and its builds are the owner's alone.
            if auth == "admin" and not accounts.is_admin():
                self.text("error that is the server owner's\n", 403)
                return
            fn(self, q.path, urllib.parse.parse_qs(q.query))
        except (BrokenPipeError, ConnectionResetError):
            pass                     # the device gave up first; nothing to tell
        except Exception as e:
            traceback.print_exc()
            try:
                self.text("error %s: %s\n" % (type(e).__name__, e), 500)
            except Exception:
                pass

    def log_message(self, *a):
        pass
