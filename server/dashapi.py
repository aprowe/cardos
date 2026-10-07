"""The dashboard's data, as JSON for server/dashboard.html.

The page is one file of HTML and script, served whole at /dash to a signed-in
browser; everything it shows comes from here, and everything it does goes
through here or through server/files.py (the card). Behind the dashboard's
cookie, like every /dash route, never the device's bearer.

    GET  /dash/api/state         the session, device, Google, Toggl, the
                                 server's services, what is published
    POST /dash/api/google/forget sign out of Google (revokes)
    POST /dash/api/toggl/token   {"token": ...}: check it with Toggl, keep it
    POST /dash/api/toggl/forget
    POST /dash/api/logout

/state asks Toggl what is running, which counts against Toggl's hourly
quota -- the page fetches it on load and after an action, never on a timer.
The device's link, which does change by the second, is /dash/files/status.
"""
import json
import os
import sys
import time

from . import dash


def _json(h, obj, code=200, headers=()):
    h._send(code, "application/json", json.dumps(obj),
            (("Cache-Control", "no-store"),) + tuple(headers))


def _api(fn):
    """Signed in, or a 403 the page turns into the sign-in screen."""
    def wrapped(h, path, args):
        if not dash.password() or not dash._server_token(h):
            _json(h, {"error": "the dashboard needs --token and DASH_PASSWORD"}, 503)
            return
        if not dash.logged_in(h):
            _json(h, {"error": "signed out"}, 403)
            return
        fn(h, args)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _admin_api(fn):
    """Signed in as the server's owner: Claude's login is theirs."""
    def wrapped(h, args):
        from . import accounts
        if not accounts.is_admin():
            # owner_only: the page reads a plain 403 as "signed out"
            _json(h, {"error": "that is the server owner's", "owner_only": True}, 403)
            return
        fn(h, args)
    wrapped.__doc__ = fn.__doc__
    return _api(wrapped)


def _body_json(h):
    try:
        return json.loads(h.body(8192) or b"{}")
    except ValueError:
        raise ValueError("not JSON")


# ---- the parts of the state --------------------------------------------------------

def session_state(h):
    from . import accounts
    exp = dash._cookie(h).rpartition(":")[2].split(".", 1)[0]
    return {"expires": int(exp) if exp.isdigit() else None,
            "user": accounts.current(), "admin": accounts.is_admin(),
            "accounts": accounts.enabled()}


def device_state():
    from . import files
    b = files._broker()
    return {"connected": b.connected(), "last_seen": int(b.last_seen) if b.last_seen else None}


def google_check():
    """(ok, words): whether the server's login works right now. Uses the
    cached access token when there is one, so this is not a Google round
    trip on every page load."""
    from . import google                  # it imports dash, which imports nothing of ours
    try:
        google.access_token()
        return True, "working"
    except google.GoogleError as e:
        return False, e.why
    except Exception as e:                # the network, mostly
        return False, "could not check: %s" % e


def google_state():
    cid, csec = dash.client()
    c = dash.load_creds()
    st = {"configured": bool(cid and csec), "signed_in": bool(c), "ok": False, "why": "",
          "email": "", "calendar": False, "tasks": False, "signed_in_at": None,
          "client_mismatch": False}
    if not c:
        return st
    ok, why = google_check()
    scope = c.get("scope", "")
    st.update(ok=ok, why="" if ok else why, email=c.get("email") or "",
              calendar="calendar" in scope, tasks="tasks" in scope,
              signed_in_at=c.get("issued_at"), client_mismatch=c.get("client_id") != cid)
    return st


def toggl_state():
    from . import toggl
    c = toggl.load()
    st = {"connected": bool(c), "name": "", "since": None, "running": None}
    if not c:
        return st
    st.update(name=c.get("name") or "", since=c.get("saved_at"))
    try:
        cur = toggl.current()
        if cur:
            st["running"] = {"description": toggl.clean(cur.get("description")),
                             "project": toggl.clean(toggl.projects().get(cur.get("project_id"), "")),
                             "start": toggl.epoch(cur["start"])}
    except toggl.TogglError as e:
        st["error"] = e.why
    except Exception as e:                      # the network
        st["error"] = "could not ask Toggl: %s" % e
    return st


def server_state(h):
    from . import app
    c = h.chat
    voice = h.voice.ready() if h.voice else False
    render = any(r[1].startswith("/render") for r in app.ALL_ROUTES)
    store = getattr(c, "store", None)
    return {
        "claude": {"ok": bool(c and c.claude), "detail": str(c.claude) if c and c.claude
                   else "the claude command was not found"},
        "session": c.session_id if c and c.session_id else None,
        "builds": {"ok": bool(store), "detail": ("into %s" % h.store) if store
                   else "off: the server was started without --build"},
        "voice": {"ok": bool(voice), "detail": "whisper ready" if voice
                  else "whisper was not found"},
        "render": {"ok": render, "detail": "headless Chrome" if render
                   else "unavailable on this server"},
    }


def updates_state(h):
    """The /update manifest, both flavours, as data."""
    from . import updates
    fw, apps = {"debug": None, "release": None}, []
    for flavor in ("debug", "release"):
        try:
            firmware, apps_dir = h.update_files(flavor)
            man = updates.manifest(firmware=firmware,
                                   apps_dir=apps_dir if flavor == "release" else os.devnull)
        except Exception as e:
            sys.stderr.write("dash: manifest %s: %s\n" % (flavor, e))
            continue
        for line in man.splitlines():
            f = line.split()
            if len(f) >= 2 and f[0] == "firmware":
                fw[flavor] = f[1][:12]
            elif flavor == "release" and len(f) >= 4 and f[0] == "app":
                apps.append({"name": f[1], "folder": f[4] if len(f) > 4 else "",
                             "size": int(f[3]) if f[3].isdigit() else 0})
    return {"firmware": fw, "apps": apps}


# ---- routes ----------------------------------------------------------------------------

@_api
def get_state(h, args):
    """everything the dashboard shows"""
    _json(h, {"session": session_state(h), "device": device_state(),
              "google": google_state(), "toggl": toggl_state(),
              "server": server_state(h), "updates": updates_state(h)})


@_api
def post_google_forget(h, args):
    """sign out of Google and revoke"""
    c = dash.load_creds()
    dash.forget_creds()
    msg = "Signed out of Google."
    if c and c.get("refresh_token"):
        try:
            dash.revoke(c["refresh_token"])
            msg = "Signed out of Google and revoked. Calendar and Todo stop syncing."
        except Exception as e:
            msg = ("Signed out here, but revoking failed (%s); revoke it at "
                   "myaccount.google.com/permissions." % e)
    _json(h, {"ok": True, "msg": msg})


@_api
def post_toggl_token(h, args):
    """check a Toggl token and keep it"""
    from . import toggl
    try:
        token = (_body_json(h).get("token") or "").strip()
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    if not token:
        _json(h, {"error": "Paste a token first."}, 400)
        return
    try:
        name, wid = toggl.check_token(token)
    except toggl.TogglError as e:
        _json(h, {"error": e.why}, 400)
        return
    toggl.save({"token": token, "name": name, "workspace": wid, "saved_at": int(time.time())})
    sys.stderr.write("dash: toggl connected\n")
    _json(h, {"ok": True, "name": name})


@_api
def get_toggl_targets(h, args):
    """the Toggl targets, their progress, and the projects to pick from"""
    from . import toggl
    try:
        off = int((args.get("off") or ["0"])[0])
    except ValueError:
        off = 0
    try:
        names = toggl.projects()
        prog = toggl.progress(off)
    except toggl.TogglError as e:
        _json(h, {"error": e.why}, 400)
        return
    _json(h, {"projects": [{"id": k, "name": v, "color": toggl.color(k)}
                           for k, v in sorted(names.items(), key=lambda kv: kv[1].lower())],
              "targets": [{"project": t["project"], "name": names.get(t["project"], "?"),
                           "color": toggl.color(t["project"]), "kind": t["kind"],
                           "hours": t["hours"], "since": t.get("since"), "done": done}
                          for t, done in prog]})


@_api
def post_toggl_target(h, args):
    """set or (hours 0) remove a Toggl target"""
    from . import toggl
    try:
        b = _body_json(h)
        toggl.set_target(b.get("project"), b.get("kind", "week"), b.get("hours", 0),
                         b.get("since"))
    except toggl.TogglError as e:
        _json(h, {"error": e.why}, 400)
        return
    except (ValueError, TypeError) as e:
        _json(h, {"error": str(e)}, 400)
        return
    _json(h, {"ok": True})


@_api
def post_toggl_forget(h, args):
    """disconnect Toggl"""
    from . import toggl
    toggl.forget()
    _json(h, {"ok": True})


def post_logout(h, path, args):
    """dashboard sign-out"""
    _json(h, {"ok": True}, headers=[dash._set_cookie("", 0)])


# ---- Claude's login (server/claudeauth.py) ------------------------------------------

@_admin_api
def get_claude(h, args):
    """whether Claude answers, and with which login"""
    from . import claudeauth
    fresh = (args.get("fresh") or ["0"])[0] == "1"
    v = claudeauth.status(h.chat, fresh=fresh)
    v["signing_in"] = bool(claudeauth.SignIn.current())
    _json(h, v)


@_admin_api
def post_claude_start(h, args):
    """start `claude setup-token`; the link to sign in at"""
    from . import claudeauth
    c = h.chat
    if not c or not c.claude:
        _json(h, {"error": "this server runs without Claude"}, 400)
        return
    try:
        s = claudeauth.SignIn.begin(c.claude, c._child_env())
    except (RuntimeError, OSError, ImportError) as e:
        _json(h, {"error": str(e)}, 502)
        return
    _json(h, {"ok": True, "url": s.url})


@_admin_api
def post_claude_code(h, args):
    """the code claude.com showed: typed into the waiting sign-in"""
    from . import claudeauth
    s = claudeauth.SignIn.current()
    if not s:
        _json(h, {"error": "No sign-in is waiting; start again."}, 400)
        return
    try:
        s.finish(_body_json(h).get("code") or "")
    except (ValueError, RuntimeError, OSError) as e:
        _json(h, {"error": str(e)}, 400)
        return
    sys.stderr.write("dash: claude signed in\n")
    _json(h, {"ok": True, "status": claudeauth.status(h.chat, fresh=True)})


@_admin_api
def post_claude_token(h, args):
    """a token made elsewhere (claude setup-token), kept"""
    from . import claudeauth
    try:
        claudeauth.save_token(_body_json(h).get("token") or "", "pasted on the dashboard")
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    _json(h, {"ok": True, "status": claudeauth.status(h.chat, fresh=True)})


@_admin_api
def post_claude_forget(h, args):
    """forget the dashboard's token; the environment's is used again"""
    from . import claudeauth
    claudeauth.forget()
    _json(h, {"ok": True})


# ---- people and their devices (server/accounts.py) -------------------------------------

def _need_accounts(h):
    from . import accounts
    if not accounts.enabled():
        _json(h, {"error": "this server has one person; accounts start with the server's next restart"}, 400)
        return False
    return True


@_api
def get_me(h, args):
    """who is signed in, their devices, and (for an admin) everyone"""
    from . import accounts
    me = accounts.current()
    out = {"user": me, "admin": accounts.is_admin(), "accounts": accounts.enabled(),
           "devices": accounts.devices(me) if me else []}
    if accounts.enabled() and accounts.is_admin():
        out["users"] = [{"name": n, "admin": bool(u.get("admin")), "devices": len(accounts.devices(n))}
                        for n, u in sorted(accounts.users().items())]
    _json(h, out)


@_api
def post_device(h, args):
    """a new device: its token, shown this once"""
    from . import accounts
    if not _need_accounts(h):
        return
    body = _body_json(h)
    owner = accounts.current()
    if body.get("user") and accounts.is_admin():          # an admin may set one up for someone
        owner = body["user"]
    try:
        did, token = accounts.add_device(owner, body.get("label") or "device")
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    sys.stderr.write("dash: device %s for %s\n" % (did, owner))
    _json(h, {"ok": True, "id": did, "token": token, "user": owner})


@_api
def post_device_remove(h, args):
    """a device's token stops working"""
    from . import accounts
    if not _need_accounts(h):
        return
    try:
        accounts.remove_device(accounts.current(), _body_json(h).get("id") or "")
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    _json(h, {"ok": True})


@_api
def post_password(h, args):
    """a new password for whoever is signed in (and a fresh session)"""
    from . import accounts
    if not _need_accounts(h):
        return
    body, me = _body_json(h), accounts.current()
    if not accounts.login(me, body.get("old") or ""):
        _json(h, {"error": "the current password is not that"}, 403)
        return
    try:
        accounts.set_password(me, body.get("new") or "")
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    _json(h, {"ok": True}, headers=[dash._set_cookie(dash.make_user_cookie(h, me),
                                                     dash.SESSION_DAYS * 86400)])


@_admin_api
def post_user(h, args):
    """a new person"""
    from . import accounts
    if not _need_accounts(h):
        return
    body = _body_json(h)
    try:
        name = accounts.add_user(body.get("name") or "", body.get("password") or "",
                                 bool(body.get("admin")))
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    sys.stderr.write("dash: account %s made\n" % name)
    _json(h, {"ok": True, "name": name})


@_admin_api
def post_user_remove(h, args):
    """a person goes, with their devices; their files stay on the server"""
    from . import accounts
    if not _need_accounts(h):
        return
    name = _body_json(h).get("name") or ""
    if name == accounts.current():
        _json(h, {"error": "not yourself"}, 400)
        return
    try:
        accounts.remove_user(name)
    except ValueError as e:
        _json(h, {"error": str(e)}, 400)
        return
    _json(h, {"ok": True})


ROUTES = [
    ("GET", "/dash/api/me", get_me, "open"),
    ("POST", "/dash/api/device", post_device, "open"),
    ("POST", "/dash/api/device/remove", post_device_remove, "open"),
    ("POST", "/dash/api/password", post_password, "open"),
    ("POST", "/dash/api/user", post_user, "open"),
    ("POST", "/dash/api/user/remove", post_user_remove, "open"),
    ("GET", "/dash/api/claude", get_claude, "open"),
    ("POST", "/dash/api/claude/start", post_claude_start, "open"),
    ("POST", "/dash/api/claude/code", post_claude_code, "open"),
    ("POST", "/dash/api/claude/token", post_claude_token, "open"),
    ("POST", "/dash/api/claude/forget", post_claude_forget, "open"),
    ("GET", "/dash/api/state", get_state, "open"),
    ("POST", "/dash/api/google/forget", post_google_forget, "open"),
    ("POST", "/dash/api/toggl/token", post_toggl_token, "open"),
    ("POST", "/dash/api/toggl/forget", post_toggl_forget, "open"),
    ("GET", "/dash/api/toggl/targets", get_toggl_targets, "open"),
    ("POST", "/dash/api/toggl/target", post_toggl_target, "open"),
    ("POST", "/dash/api/logout", post_logout, "open"),
]
