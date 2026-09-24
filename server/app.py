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

A route is behind the token unless its tuple says "open", and an open route
checks for itself when it has something worth protecting -- /render shows the
banner to anyone but renders only for the device.

Run it with `python -m server`; see __main__.py for the flags.
"""
import hmac
import sys
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler

from . import chat, dash, shots, tz, updates, voice
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


SERVER_ROUTES = [
    ("GET", "/", get_banner, "open"),
    ("GET", "/status", get_status, "open"),
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


ALL_ROUTES = _normalise(SERVER_ROUTES + chat.ROUTES + updates.ROUTES +
                        voice.ROUTES + shots.ROUTES + tz.ROUTES + dash.ROUTES +
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

    def update_files(self):
        """(firmware, apps_dir) that /update serves."""
        if self.store:
            return updates.store_paths(self.store)
        return updates.FIRMWARE, updates.APPS_DIR

    # ---- dispatch ----------------------------------------------------------

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

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
            if auth != "open" and not self.authorised():
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
