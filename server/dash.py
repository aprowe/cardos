"""The dashboard: a private page on the droplet -- the device, the Google
login, the server -- and the way to the card's files (server/files.py).

The device has no browser, so Google's consent screen happens here, at
https://cardos.arowe.net/dash. The login stays on the server and never
leaves it: Calendar and Todo get their Google data through server/google.py
with it. (/google/creds once handed it to a device that asked -- `google
pull` -- and was removed on 2026-10-09, with the device's half.)

/dash... is a browser's, with a session cookie; the device's bearer does
not open it, and the cookie opens none of the device's routes. The
password is DASH_PASSWORD from the environment, or, with accounts, each
person's own (server/accounts.py).

The cookie is an expiry and an HMAC of it keyed by the token and the
password: a restart keeps you logged in, and changing either logs everyone
out. The server needs --token: the cookie is keyed by it.

Google will only redirect a web sign-in to HTTPS on a real domain, which is
why this lives behind nginx and certbot rather than on :8080. The Web client
it signs in with is configured in the environment (/etc/cardos/env on the
droplet), never in the repository:

  DASH_PASSWORD   the dashboard's password; with none set, it will not run
  GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET   the "Web application" OAuth client
  DASH_URL        https://cardos.arowe.net (the redirect is DASH_URL + CALLBACK)
  CARDOS_STATE    where google.json is kept; ~/.cardos by default

Design: docs/superpowers/specs/2026-09-24-dashboard-google-design.md.
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
PAGE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")

COOKIE = "cardos_dash"
SESSION_DAYS = 30
STATE_TTL = 600                      # a sign-in left open longer starts over


def client():
    return os.environ.get("GOOGLE_CLIENT_ID", ""), os.environ.get("GOOGLE_CLIENT_SECRET", "")


def base_url():
    return os.environ.get("DASH_URL", "https://cardos.arowe.net").rstrip("/")


def password():
    return os.environ.get("DASH_PASSWORD", "")


def state_dir():
    return os.environ.get("CARDOS_STATE") or os.path.expanduser("~/.cardos")


def creds_path():
    from . import accounts
    return os.path.join(accounts.user_dir(), "google.json")


# ---- the saved sign-in ------------------------------------------------------

_lock = threading.Lock()


def load_creds():
    from . import store
    return store.read_json(creds_path())


def save_creds(c):
    """Owner-only, and written whole: a half-written file is a lost login."""
    from . import store
    store.write_json(creds_path(), c, indent=1)


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


def _cookie_key(h):
    """Both secrets, so changing either one ends every session."""
    t, p = _server_token(h), password()
    return t + "\0" + p if t and p else None


def _cookie(h):
    for part in h.headers.get("Cookie", "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == COOKIE:
            return v
    return ""


def _user_key(h, name):
    """With accounts: the server's token and that user's password hash, so a
    new password ends that user's sessions and nobody else's."""
    from . import accounts
    t, pw = _server_token(h), accounts.password_hash(name)
    return t + "\0" + name + "\0" + pw if t and pw else None


def make_user_cookie(h, name, now=None):
    return name + ":" + make_cookie(_user_key(h, name), now)


def logged_in(h):
    """Signed in; with accounts, as whom (accounts.current())."""
    from . import accounts
    if accounts.enabled():
        name, _, rest = _cookie(h).partition(":")
        if name and rest and cookie_ok(_user_key(h, name), rest):
            accounts.set_current(name)
            return True
        return False
    return cookie_ok(_cookie_key(h), _cookie(h))


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
header{background:var(--card);border-bottom:1px solid var(--line)}
header .in{max-width:960px;margin:0 auto;padding:10px 16px;display:flex;align-items:center;gap:18px;flex-wrap:wrap}
header .brand{font-weight:700;font-size:17px;margin-right:6px}
header nav{display:flex;gap:4px;flex:1}
header nav a{color:var(--dim);text-decoration:none;padding:6px 12px;border-radius:6px}
header nav a.on{color:var(--ink);background:var(--bg);font-weight:600}
header nav a:hover{color:var(--ink)}
main.wide{max-width:960px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;align-items:start}
.grid section{margin:0}
.card-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}
.card-head h2{margin:0}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:1px;background:var(--dim)}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:4px}
.big{font-size:17px;font-weight:600;margin:0 0 4px}
details summary{cursor:pointer;margin-top:4px}
table.apps{width:100%;border-collapse:collapse;margin-top:8px;font-size:14px}
table.apps td{padding:4px 6px;border-top:1px solid var(--line)}
"""


PAGES = [("/dash", "Dashboard"), ("/dash/files", "Files")]


def _page(title, body, here=None):
    """A page; `here` (a path in PAGES) gives it the bar with the pages and
    log out, which only a signed-in page should have."""
    bar = ""
    if here:
        links = "".join("<a href=%s%s>%s</a>" % (u, " class=on" if u == here else "",
                                                  html.escape(n)) for u, n in PAGES)
        bar = ("<header><div class=in><span class=brand>CardOS</span><nav>%s</nav>"
               "<form method=post action=/dash/logout><button>Log out</button></form>"
               "</div></header>" % links)
    return ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta name=robots content=noindex><link rel=icon href='data:,'>"
            "<title>%s</title><style>%s</style></head>"
            "<body>%s<main%s>%s</main></body></html>"
            % (html.escape(title), STYLE, bar, " class=wide" if here else "", body))


def _when(t):
    if not t:
        return "never"
    return time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(t))


def login_page(msg=""):
    note = "<p class='msg bad'>%s</p>" % html.escape(msg) if msg else ""
    return _page("CardOS", "<h1>CardOS</h1>%s<section><h2>Sign in</h2>"
                 "<form method=post action=/dash/login style='display:block'>"
                 "%s<label>Password<input type=password name=password autofocus "
                 "autocomplete=current-password></label>"
                 "<button class=primary>Sign in</button></form></section>"
                 % (note, _name_field()))


def _name_field():
    from . import accounts
    if not accounts.enabled():
        return ""
    return ("<label>Name<input name=name autocomplete=username autocapitalize=none "
            "autofocus></label>")


def _google_check():
    """(ok, words): whether the server's login works right now. Uses the
    cached access token when there is one, so a page load is not a Google
    round trip every time."""
    from . import google                  # it imports this module
    try:
        google.access_token()
        return True, "working"
    except google.GoogleError as e:
        return False, e.why
    except Exception as e:                # the network, mostly
        return False, "could not check: %s" % e


# ---- routes -----------------------------------------------------------------
#
# The /dash routes are "open" to the bearer check and do their own, with the
# cookie: a browser has no bearer and the device has no cookie.

def _need_token(h):
    if _server_token(h) and password():
        return True
    h.html(_page("CardOS", "<h1>CardOS</h1><p class=bad>The dashboard needs the "
                 "server started with --token and DASH_PASSWORD set.</p>"), 503)
    return False


# Wrong passwords wait their turn: the server is threaded, and a sleep per
# request alone would let a hundred guesses run side by side.
_fail_lock = threading.Lock()


def get_dash(h, path, args):
    """the dashboard (browser, cookie)"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.html(login_page())
        return
    # One file of HTML and script; everything it shows comes from
    # /dash/api/state (server/dashapi.py) and /dash/files/*. Read each time,
    # so editing it needs no restart.
    try:
        with open(PAGE_FILE, encoding="utf-8") as f:
            h.html(f.read())
    except OSError as e:
        h.html(_page("CardOS", "<h1>CardOS</h1><p class=bad>%s is missing: %s</p>"
                     % (html.escape(PAGE_FILE), html.escape(str(e)))), 500)


def post_login(h, path, args):
    """dashboard sign-in"""
    if not _need_token(h):
        return
    from . import accounts
    form = _form(h)
    given = (form.get("password") or [""])[0]
    if accounts.enabled():
        name = accounts.login((form.get("name") or [""])[0], given)
        if not name:
            with _fail_lock:
                time.sleep(1)
            sys.stderr.write("dash: failed sign-in from %s\n"
                             % (h.headers.get("X-Real-IP") or h.client_address[0]))
            h.html(login_page("That name and password do not match."), 403)
            return
        h.redirect("/dash", [_set_cookie(make_user_cookie(h, name), SESSION_DAYS * 86400)])
        return
    if not hmac.compare_digest(password().encode(), given.encode()):
        with _fail_lock:
            time.sleep(1)                      # a guess a second, not a thousand
        sys.stderr.write("dash: wrong password from %s\n"
                         %(h.headers.get("X-Real-IP") or h.client_address[0]))
        h.html(login_page("Wrong password."), 403)
        return
    h.redirect("/dash", [_set_cookie(make_cookie(_cookie_key(h)), SESSION_DAYS * 86400)])


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
    save_creds({"client_id": cid, "client_secret": csec, "refresh_token": refresh,
                "email": email_from_id_token(tok.get("id_token", "")),
                "scope": tok.get("scope", ""), "issued_at": int(time.time())})
    # The login it replaces is NOT revoked. Google's revoke ends the app's
    # whole grant -- every token for this client and account -- so revoking
    # the old one here killed the one just issued, and each fresh sign-in
    # died minutes later with "Token has been expired or revoked"
    # (2026-10-01). A replaced token simply goes unused.
    from . import google
    google.forget_access()                       # the old login's access token
    sys.stderr.write("dash: google signed in\n")
    back("Signed in to Google. Calendar and Todo use this login.")


def post_google_forget(h, path, args):
    """sign out of Google and revoke"""
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return
    from . import google
    c = load_creds()
    forget_creds()
    google.forget_access()
    msg = "Signed out."
    if c and c.get("refresh_token"):
        try:
            revoke(c["refresh_token"])
            msg = "Signed out of Google and revoked. Calendar and Todo stop syncing."
        except Exception as e:
            msg = "Signed out here, but revoking failed (%s); revoke it at " \
                  "myaccount.google.com/permissions." % e
    h.redirect("/dash?msg=" + urllib.parse.quote(msg))


ROUTES = [
    ("GET", "/dash", get_dash, "open"),
    ("POST", "/dash/login", post_login, "open"),
    ("POST", "/dash/logout", post_logout, "open"),
    ("GET", "/dash/google/start", get_google_start, "open"),
    ("GET", CALLBACK, get_google_callback, "open"),
    ("POST", "/dash/google/forget", post_google_forget, "open"),
]
