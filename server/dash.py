"""The dashboard: a private page on the droplet, and Google sign-in on it.

The device has no browser, so Google's consent screen has to happen somewhere
else. It used to be tools/google_auth.py on a PC, which then typed the result
down a serial cable. Now it happens here, at https://cardos.arowe.net/dash,
and the device fetches the result over the network with `google pull`.

Two doors, and neither opens the other:

  /dash...        a browser, with a session cookie. The password is the
                  server's --token, so there is no user list to keep.
  /google/creds   the device, with its bearer token (/config/claude.token),
                  as for every other route. A cookie does not open it and
                  the bearer does not open /dash.

The cookie is an expiry and an HMAC of it keyed by the token: a restart keeps
you logged in, and changing the token logs everyone out.

Google will only redirect a web sign-in to HTTPS on a real domain, which is
why this lives behind nginx and certbot rather than on :8080. The Web client
it signs in with is configured in the environment (/etc/cardos/env on the
droplet), never in the repository:

  GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET   the "Web application" OAuth client
  DASH_URL        https://cardos.arowe.net (the redirect is DASH_URL + CALLBACK)
  CARDOS_STATE    where google.json is kept; ~/.cardos by default

A refresh token works only with the client that issued it, so the device is
handed all three values, not just the token. Design:
docs/superpowers/specs/2026-09-24-dashboard-google-design.md.
"""
import base64
import hashlib
import hmac
import html
import json
import os
import secrets
import sys
import threading
import time
import urllib.parse
import urllib.request

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
REVOKE_URL = "https://oauth2.googleapis.com/revoke"
# What Todo and Calendar use, plus openid/email so the page can say whose
# account it is.
SCOPES = ["openid", "email",
          "https://www.googleapis.com/auth/tasks",
          "https://www.googleapis.com/auth/calendar.events"]
CALLBACK = "/dash/google/callback"

COOKIE = "cardos_dash"
SESSION_DAYS = 30
STATE_TTL = 600                      # a sign-in left open longer starts over


def client():
    return os.environ.get("GOOGLE_CLIENT_ID", ""), os.environ.get("GOOGLE_CLIENT_SECRET", "")


def base_url():
    return os.environ.get("DASH_URL", "https://cardos.arowe.net").rstrip("/")


def state_dir():
    return os.environ.get("CARDOS_STATE") or os.path.expanduser("~/.cardos")


def creds_path():
    return os.path.join(state_dir(), "google.json")


# ---- the saved sign-in ------------------------------------------------------

_lock = threading.Lock()


def load_creds():
    try:
        with open(creds_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def save_creds(c):
    """Owner-only, and written whole: a half-written file is a lost login."""
    os.makedirs(state_dir(), mode=0o700, exist_ok=True)
    tmp = creds_path() + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(c, f, indent=1)
    os.replace(tmp, creds_path())


def forget_creds():
    try:
        os.remove(creds_path())
    except FileNotFoundError:
        pass


# ---- Google, replaceable in the tests ---------------------------------------

def _post_form(url, fields):
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=data, headers={
        "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read() or b"{}")


def exchange_code(code, client_id, client_secret, redirect_uri):
    """The authorisation code for {refresh_token, id_token, ...}."""
    return _post_form(TOKEN_URL, {
        "code": code, "client_id": client_id, "client_secret": client_secret,
        "redirect_uri": redirect_uri, "grant_type": "authorization_code"})


def revoke(token):
    _post_form(REVOKE_URL, {"token": token})


def email_from_id_token(id_token):
    """Unverified on purpose: it came straight from Google's token endpoint
    over TLS, and it only decides what the page prints."""
    try:
        payload = id_token.split(".")[1]
        payload += "=" * (-len(payload) % 4)
        return json.loads(base64.urlsafe_b64decode(payload)).get("email", "")
    except Exception:
        return ""


# ---- the session ------------------------------------------------------------

def _sign(token, msg):
    return hmac.new(token.encode(), ("dash|" + msg).encode(), hashlib.sha256).hexdigest()


def make_cookie(token, now=None):
    exp = str(int((now or time.time()) + SESSION_DAYS * 86400))
    return exp + "." + _sign(token, exp)


def cookie_ok(token, value, now=None):
    if not token or not value or "." not in value:
        return False
    exp, sig = value.split(".", 1)
    if not hmac.compare_digest(_sign(token, exp), sig):
        return False
    try:
        return int(exp) > (now or time.time())
    except ValueError:
        return False


def _server_token(h):
    return h.chat.token if h.chat else None


def _cookie(h):
    for part in h.headers.get("Cookie", "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == COOKIE:
            return v
    return ""


def logged_in(h):
    return cookie_ok(_server_token(h), _cookie(h))


def _set_cookie(value, max_age):
    return ("Set-Cookie", "%s=%s; Path=/dash; Max-Age=%d; HttpOnly; Secure; SameSite=Lax"
            % (COOKIE, value, max_age))


# Sign-ins in flight: state -> when it was issued. In memory, because a
# restart in the middle of one is rare and starting again is the fix.
_states = {}


def _new_state():
    now = time.time()
    with _lock:
        for s, t in list(_states.items()):
            if now - t > STATE_TTL:
                del _states[s]
        s = secrets.token_urlsafe(24)
        _states[s] = now
    return s


def _take_state(s):
    with _lock:
        t = _states.pop(s, None)
    return t is not None and time.time() - t <= STATE_TTL


def _form(h):
    return urllib.parse.parse_qs(h.body(4096).decode("utf-8", "replace"))


# ---- the page ---------------------------------------------------------------

STYLE = """
:root{--bg:#f6f5f2;--card:#fff;--ink:#1c1c1a;--dim:#6b6a66;--line:#e2e0da;
--acc:#1f6feb;--ok:#1a7f37;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--bg:#141413;--card:#1e1e1c;--ink:#ecebe6;
--dim:#9a9891;--line:#33322f;--acc:#58a6ff;--ok:#3fb950;--bad:#f47067}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);
font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:720px;margin:0 auto;padding:24px 16px}
h1{font-size:20px;margin:0 0 20px;display:flex;justify-content:space-between;align-items:center}
h2{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--dim);margin:0 0 12px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px;margin-bottom:16px}
dl{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px;margin:0 0 14px}
dt{color:var(--dim)}dd{margin:0;overflow-wrap:anywhere}
pre{margin:0;font:12px/1.5 ui-monospace,Consolas,monospace;overflow-x:auto;white-space:pre}
button,.btn{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--ink);
border-radius:6px;padding:6px 14px;cursor:pointer;text-decoration:none;display:inline-block}
.primary{background:var(--acc);border-color:var(--acc);color:#fff}
input{font:inherit;padding:6px 10px;border:1px solid var(--line);border-radius:6px;
background:var(--bg);color:var(--ink);width:100%;margin:8px 0 12px}
.ok{color:var(--ok)}.bad{color:var(--bad)}.dim{color:var(--dim)}
form{display:inline}.msg{margin:0 0 16px;padding:10px 14px;border-radius:6px;border:1px solid var(--line)}
"""


def _page(title, body):
    return ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta name=robots content=noindex><title>%s</title><style>%s</style></head>"
            "<body><main>%s</main></body></html>" % (html.escape(title), STYLE, body))


def _when(t):
    if not t:
        return "never"
    return time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(t))


def login_page(msg=""):
    note = "<p class='msg bad'>%s</p>" % html.escape(msg) if msg else ""
    return _page("CardOS", "<h1>CardOS</h1>%s<section><h2>Sign in</h2>"
                 "<form method=post action=/dash/login style='display:block'>"
                 "<label>Server token<input type=password name=password autofocus "
                 "autocomplete=current-password></label>"
                 "<button class=primary>Sign in</button></form></section>" % note)


def google_card():
    cid, csec = client()
    c = load_creds()
    if not cid or not csec:
        return ("<section><h2>Google</h2><p class=bad>No Web client configured.</p>"
                "<p class=dim>Set GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET in the "
                "server's environment. The client's redirect URI must be "
                "<code>%s</code>.</p></section>" % html.escape(base_url() + CALLBACK))
    if not c:
        return ("<section><h2>Google</h2><p>Not signed in. Todo and Calendar on the "
                "device need this.</p><a class='btn primary' href=/dash/google/start>"
                "Sign in with Google</a></section>")
    stale = c.get("client_id") != cid
    return ("<section><h2>Google</h2><dl>"
            "<dt>Account</dt><dd>%s</dd>"
            "<dt>Signed in</dt><dd>%s</dd>"
            "<dt>Device pulled</dt><dd>%s</dd>"
            "<dt>Client</dt><dd class=dim>%s%s</dd></dl>"
            "<p class=dim>On the device: <code>google pull</code></p>"
            "<a class=btn href=/dash/google/start>Sign in again</a> "
            "<form method=post action=/dash/google/forget>"
            "<button>Sign out and revoke</button></form></section>"
            % (html.escape(c.get("email") or "unknown"), _when(c.get("issued_at")),
               _when(c.get("pulled_at")), html.escape(c.get("client_id", "")[:24] + "..."),
               " <span class=bad>(not the configured client)</span>" if stale else ""))


def status_card(h):
    from . import app, updates                 # app imports this module
    firmware, apps_dir = h.update_files()
    try:
        man = updates.manifest(firmware=firmware, apps_dir=apps_dir)
    except Exception as e:
        man = "error %s: %s\n" % (type(e).__name__, e)
    c = h.chat
    render = any(r[1].startswith("/render") for r in app.ALL_ROUTES)
    voice = h.voice.ready() if h.voice else False

    def yn(ok, good, bad):
        return "<span class=%s>%s</span>" % ("ok" if ok else "bad", good if ok else bad)
    return ("<section><h2>Server</h2><dl>"
            "<dt>Claude</dt><dd>%s</dd>"
            "<dt>Session</dt><dd class=dim>%s</dd>"
            "<dt>Builds</dt><dd>%s</dd>"
            "<dt>Voice</dt><dd>%s</dd>"
            "<dt>Render</dt><dd>%s</dd></dl>"
            "<h2>Published for update</h2><pre>%s</pre></section>"
            % (yn(c and c.claude, html.escape(str(c.claude if c else "")), "not found"),
               html.escape(c.session_id if c and c.session_id else "none yet"),
               yn(getattr(c, "store", None), "yes, into " + html.escape(str(h.store)), "no"),
               yn(voice, "whisper ready", "not found"),
               yn(render, "available", "unavailable"),
               html.escape(man)))


# ---- routes -----------------------------------------------------------------
#
# The /dash routes are "open" to the bearer check and do their own, with the
# cookie: a browser has no bearer and the device has no cookie.

def _need_token(h):
    if _server_token(h):
        return True
    h.html(_page("CardOS", "<h1>CardOS</h1><p class=bad>The dashboard needs the "
                 "server started with --token; that is its password.</p>"), 503)
    return False


def get_dash(h, path, args):
    """the dashboard (browser, cookie)"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.html(login_page())
        return
    msg = (args.get("msg") or [""])[0]
    note = "<p class=msg>%s</p>" % html.escape(msg) if msg else ""
    h.html(_page("CardOS", "<h1>CardOS <form method=post action=/dash/logout>"
                 "<button>Log out</button></form></h1>" + note +
                 google_card() + status_card(h)))


def post_login(h, path, args):
    """dashboard sign-in"""
    if not _need_token(h):
        return
    given = (_form(h).get("password") or [""])[0]
    if not hmac.compare_digest(_server_token(h), given):
        time.sleep(1)                          # a guess a second, not a thousand
        h.html(login_page("That is not the token."), 403)
        return
    h.redirect("/dash", [_set_cookie(make_cookie(_server_token(h)), SESSION_DAYS * 86400)])


def post_logout(h, path, args):
    """dashboard sign-out"""
    h.redirect("/dash", [_set_cookie("", 0)])


def get_google_start(h, path, args):
    """begin Google sign-in"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return
    cid, _ = client()
    if not cid:
        h.redirect("/dash?msg=" + urllib.parse.quote("No Web client configured."))
        return
    q = urllib.parse.urlencode({
        "client_id": cid, "redirect_uri": base_url() + CALLBACK,
        "response_type": "code", "scope": " ".join(SCOPES),
        "access_type": "offline",
        # consent, every time: without it a second sign-in comes back with no
        # refresh token at all, and that is the one thing this is for.
        "prompt": "consent", "state": _new_state()})
    h.redirect(AUTH_URL + "?" + q)


def get_google_callback(h, path, args):
    """Google sends the browser back here"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return

    def back(msg):
        h.redirect("/dash?msg=" + urllib.parse.quote(msg))

    if not _take_state((args.get("state") or [""])[0]):
        back("That sign-in had expired or was not started here. Try again.")
        return
    if args.get("error"):
        back("Google said: " + args["error"][0])
        return
    code = (args.get("code") or [""])[0]
    if not code:
        back("Google sent no code.")
        return
    cid, csec = client()
    tok = exchange_code(code, cid, csec, base_url() + CALLBACK)
    refresh = tok.get("refresh_token")
    if not refresh:
        back("Google sent no refresh token: %s" % (tok.get("error_description")
                                                   or tok.get("error") or "no reason given"))
        return
    old = load_creds()
    save_creds({"client_id": cid, "client_secret": csec, "refresh_token": refresh,
                "email": email_from_id_token(tok.get("id_token", "")),
                "scope": tok.get("scope", ""), "issued_at": int(time.time()),
                "pulled_at": None})
    # The token it replaces is still live at Google until revoked.
    if old and old.get("refresh_token") and old["refresh_token"] != refresh:
        try:
            revoke(old["refresh_token"])
        except Exception as e:
            sys.stderr.write("dash: revoking the old token: %s\n" % e)
    sys.stderr.write("dash: google signed in\n")
    back("Signed in. On the device: google pull")


def post_google_forget(h, path, args):
    """sign out of Google and revoke"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return
    c = load_creds()
    forget_creds()
    msg = "Signed out."
    if c and c.get("refresh_token"):
        try:
            revoke(c["refresh_token"])
            msg = "Signed out and revoked. The device's copy no longer works."
        except Exception as e:
            msg = "Signed out here, but revoking failed (%s); revoke it at " \
                  "myaccount.google.com/permissions." % e
    h.redirect("/dash?msg=" + urllib.parse.quote(msg))


def get_creds(h, path, args):
    """Google client id, secret, refresh token, a line each"""
    # Every other route is open when there is no token; this one never is.
    if not _server_token(h):
        h.text("error the server has no --token; it will not hand out credentials\n", 503)
        return
    c = load_creds()
    if not c:
        h.text("error not signed in: sign in at %s/dash\n" % base_url(), 404)
        return
    with _lock:
        c["pulled_at"] = int(time.time())
        save_creds(c)
    h.text("%s\n%s\n%s\n" % (c["client_id"], c["client_secret"], c["refresh_token"]))
    sys.stderr.write("dash: device pulled google credentials\n")


ROUTES = [
    ("GET", "/dash", get_dash, "open"),
    ("POST", "/dash/login", post_login, "open"),
    ("POST", "/dash/logout", post_logout, "open"),
    ("GET", "/dash/google/start", get_google_start, "open"),
    ("GET", CALLBACK, get_google_callback, "open"),
    ("POST", "/dash/google/forget", post_google_forget, "open"),
    ("GET", "/google/creds", get_creds),
]
