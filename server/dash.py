"""The dashboard: a private page on the droplet -- the device, the Google
login, the server -- and the way to the card's files (server/files.py).

The device has no browser, so Google's consent screen happens here, at
https://cardos.arowe.net/dash. The login stays on the server: Calendar and
Todo get their Google data through server/google.py with it. /google/creds
still hands it to a device that asks (`google pull`), which nothing needs
since 2026-09-29.

Two doors, and neither opens the other:

  /dash...        a browser, with a session cookie. The password is
                  DASH_PASSWORD from the environment; there is no user list.
  /google/creds   the device, with its bearer token (/config/claude.token),
                  as for every other route. A cookie does not open it and
                  the bearer does not open /dash.

The cookie is an expiry and an HMAC of it keyed by the token and the
password: a restart keeps you logged in, and changing either logs everyone
out. The server still needs --token: it is what keeps /google/creds shut.

Google will only redirect a web sign-in to HTTPS on a real domain, which is
why this lives behind nginx and certbot rather than on :8080. The Web client
it signs in with is configured in the environment (/etc/cardos/env on the
droplet), never in the repository:

  DASH_PASSWORD   the dashboard's password; with none set, it will not run
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


def password():
    return os.environ.get("DASH_PASSWORD", "")


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


def logged_in(h):
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
                 "<label>Password<input type=password name=password autofocus "
                 "autocomplete=current-password></label>"
                 "<button class=primary>Sign in</button></form></section>" % note)


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


def google_card():
    cid, csec = client()
    c = load_creds()
    head = "<div class=card-head><h2>Google</h2></div>"
    if not cid or not csec:
        return ("<section>%s<p class=bad>No Web client configured.</p>"
                "<p class=dim>Set GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET in the "
                "server's environment. The client's redirect URI must be "
                "<code>%s</code>.</p></section>" % (head, html.escape(base_url() + CALLBACK)))
    if not c:
        return ("<section>%s<p class=big><span class='dot bad'></span>Not signed in</p>"
                "<p class=dim>Calendar and Todo on the device get their Google data "
                "through this server, with this login.</p>"
                "<div class=actions><a class='btn primary' href=/dash/google/start>"
                "Sign in with Google</a></div></section>" % head)
    ok, why = _google_check()
    scope = c.get("scope", "")
    has = lambda s: ("<span class=ok>yes</span>" if s in scope
                     else "<span class=bad>no &mdash; sign in again and allow it</span>")
    stale = c.get("client_id") != cid
    return ("<section>%s<p class=big><span class='dot %s'></span>%s</p>"
            "<p class=dim>%s</p><dl>"
            "<dt>Account</dt><dd>%s</dd>"
            "<dt>Calendar</dt><dd>%s</dd>"
            "<dt>Tasks</dt><dd>%s</dd>"
            "<dt>Signed in</dt><dd>%s</dd>%s</dl>"
            "<div class=actions><a class=btn href=/dash/google/start>Sign in again</a>"
            "<form method=post action=/dash/google/forget>"
            "<button>Sign out</button></form></div></section>"
            % (head, "ok" if ok else "bad",
               "Working" if ok else "Not working",
               "Calendar and Todo on the device get their Google data through this "
               "server, with this login." if ok else html.escape(why),
               html.escape(c.get("email") or "unknown"),
               has("calendar"), has("tasks"), _when(c.get("issued_at")),
               "<dt>Client</dt><dd class=bad>not the configured client: sign in again</dd>"
               if stale else ""))


def toggl_card():
    """The Toggl token: who it belongs to, or a box to paste one into. Not
    checked against Toggl on every page load -- its hourly quota is small,
    and the device needs it more."""
    from . import toggl
    c = toggl.load()
    head = "<div class=card-head><h2>Toggl</h2></div>"
    if not c:
        return ("<section>%s<p class=big><span class=dot></span>Not connected</p>"
                "<p class=dim>Paste your API token from the bottom of "
                "<a href='https://track.toggl.com/profile' target=_blank rel=noopener>"
                "your Toggl profile</a>. The Toggl app on the device then starts and "
                "stops timers through this server.</p>"
                "<form method=post action=/dash/toggl/token style='display:block'>"
                "<input name=token autocomplete=off placeholder='API token'>"
                "<button class=primary>Connect</button></form></section>" % head)
    return ("<section>%s<p class=big><span class='dot ok'></span>Connected</p>"
            "<dl><dt>Account</dt><dd>%s</dd><dt>Connected</dt><dd>%s</dd></dl>"
            "<div class=actions><form method=post action=/dash/toggl/forget>"
            "<button>Disconnect</button></form></div></section>"
            % (head, html.escape(c.get("name") or "?"), _when(c.get("saved_at"))))


def device_card():
    """Whether Remote Files is open on the device, and the way to the card."""
    from . import files
    b = files.broker
    on = b.connected()
    seen = ("never this run" if not b.last_seen else
            "just now" if on else _ago(b.last_seen))
    return ("<section><div class=card-head><h2>Device</h2></div>"
            "<p class=big><span class='dot %s'></span>%s</p><p class=dim>%s</p>"
            "<dl><dt>Last heard</dt><dd>%s</dd></dl>"
            "<div class=actions><a class='btn primary' href=/dash/files>Open files</a></div>"
            "</section>"
            % ("ok" if on else "", "Card reachable" if on else "Card not reachable",
               "Browse, upload, download and edit what is on the SD card." if on else
               "Open <b>Remote Files</b> (Net folder) on the device to browse its card "
               "from here.", seen))


def _ago(t):
    s = int(time.time() - t)
    if s < 90:
        return "%d s ago" % s
    if s < 5400:
        return "%d min ago" % (s // 60)
    if s < 129600:
        return "%d h ago" % (s // 3600)
    return _when(t)


def published_card(man):
    """The /update manifest as a summary and a table, rather than the raw
    lines the device reads."""
    fw, apps, other = [], [], []
    for line in man.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0] == "firmware":
            fw.append((f[1], f[2][:12]))
        elif len(f) >= 4 and f[0] == "app":
            apps.append((f[1], f[4] if len(f) > 4 else "", int(f[3]) if f[3].isdigit() else 0))
        elif line.strip():
            other.append(line)
    rows = "".join("<tr><td>%s</td><td class=dim>%s</td><td class=dim>%.1f KB</td></tr>"
                   % (html.escape(n), html.escape(folder or "top"), size / 1024.0)
                   for n, folder, size in apps)
    firm = "".join("<dt>Firmware %s</dt><dd><code>%s</code></dd>"
                   % (html.escape(k), html.escape(s)) for k, s in fw)
    return ("<section><div class=card-head><h2>Published for update</h2></div>"
            "<p class=dim>What <code>update</code> on the device compares itself with.</p>"
            "<dl>%s<dt>Apps</dt><dd>%d</dd></dl>%s"
            "<details><summary class=dim>Every app</summary>"
            "<table class=apps>%s</table></details></section>"
            % (firm, len(apps),
               "".join("<p class=bad>%s</p>" % html.escape(o) for o in other), rows))


def status_card(h):
    from . import app, updates                 # app imports this module
    firmware, apps_dir = h.update_files("release")
    try:
        man = updates.manifest(firmware=firmware, apps_dir=apps_dir)
        man = man.replace("firmware ", "firmware release ", 1)
        debug_fw, _ = h.update_files("debug")
        # Only its firmware line: the apps are the same for both.
        debug = updates.manifest(firmware=debug_fw, apps_dir=os.devnull)
        man = debug.replace("firmware ", "firmware debug ", 1) + man
    except Exception as e:
        man = "error %s: %s\n" % (type(e).__name__, e)
    c = h.chat
    render = any(r[1].startswith("/render") for r in app.ALL_ROUTES)
    voice = h.voice.ready() if h.voice else False

    def yn(ok, good, bad):
        return "<span class=%s>%s</span>" % ("ok" if ok else "bad", good if ok else bad)
    return ("<section><div class=card-head><h2>Server</h2></div><dl>"
            "<dt>Claude</dt><dd>%s</dd>"
            "<dt>Session</dt><dd class=dim>%s</dd>"
            "<dt>Builds</dt><dd>%s</dd>"
            "<dt>Voice</dt><dd>%s</dd>"
            "<dt>Render</dt><dd>%s</dd></dl></section>"
            "%s"
            % (yn(c and c.claude, html.escape(str(c.claude if c else "")), "not found"),
               html.escape(c.session_id if c and c.session_id else "none yet"),
               yn(getattr(c, "store", None), "yes, into " + html.escape(str(h.store)), "no"),
               yn(voice, "whisper ready", "not found"),
               yn(render, "available", "unavailable"),
               published_card(man)))


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
    msg = (args.get("msg") or [""])[0]
    note = "<p class=msg>%s</p>" % html.escape(msg) if msg else ""
    h.html(_page("CardOS", note + "<div class=grid>" + device_card() + google_card() + toggl_card() +
                 status_card(h) + "</div>", here="/dash"))


def post_login(h, path, args):
    """dashboard sign-in"""
    if not _need_token(h):
        return
    given = (_form(h).get("password") or [""])[0]
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
                "scope": tok.get("scope", ""), "issued_at": int(time.time()),
                "pulled_at": None})
    # The login it replaces is NOT revoked. Google's revoke ends the app's
    # whole grant -- every token for this client and account -- so revoking
    # the old one here killed the one just issued, and each fresh sign-in
    # died minutes later with "Token has been expired or revoked"
    # (2026-10-01). A replaced token simply goes unused.
    from . import google
    google._access.update(token=None, until=0)   # the old login's access token
    sys.stderr.write("dash: google signed in\n")
    back("Signed in to Google. Calendar and Todo use this login.")


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
            msg = "Signed out of Google and revoked. Calendar and Todo stop syncing."
        except Exception as e:
            msg = "Signed out here, but revoking failed (%s); revoke it at " \
                  "myaccount.google.com/permissions." % e
    h.redirect("/dash?msg=" + urllib.parse.quote(msg))


def post_toggl_token(h, path, args):
    """connect Toggl: check a token and keep it"""
    from . import toggl
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return
    token = (_form(h).get("token") or [""])[0].strip()
    if not token:
        h.redirect("/dash?msg=" + urllib.parse.quote("Paste a token first."))
        return
    try:
        name, wid = toggl.check_token(token)
    except toggl.TogglError as e:
        h.redirect("/dash?msg=" + urllib.parse.quote(e.why))
        return
    toggl.save({"token": token, "name": name, "workspace": wid, "saved_at": int(time.time())})
    sys.stderr.write("dash: toggl connected\n")
    h.redirect("/dash?msg=" + urllib.parse.quote("Toggl connected as %s." % name))


def post_toggl_forget(h, path, args):
    """disconnect Toggl"""
    from . import toggl
    if not _need_token(h):
        return
    if not logged_in(h):
        h.redirect("/dash")
        return
    toggl.forget()
    h.redirect("/dash?msg=" + urllib.parse.quote("Toggl disconnected."))


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
    ("POST", "/dash/toggl/token", post_toggl_token, "open"),
    ("POST", "/dash/toggl/forget", post_toggl_forget, "open"),
    ("GET", "/google/creds", get_creds),
]
