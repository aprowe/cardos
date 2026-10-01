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


def _body_json(h):
    try:
        return json.loads(h.body(8192) or b"{}")
    except ValueError:
        raise ValueError("not JSON")


# ---- the parts of the state --------------------------------------------------------

def session_state(h):
    exp = dash._cookie(h).split(".", 1)[0]
    return {"expires": int(exp) if exp.isdigit() else None}


def device_state():
    from . import files
    b = files.broker
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
def post_toggl_forget(h, args):
    """disconnect Toggl"""
    from . import toggl
    toggl.forget()
    _json(h, {"ok": True})


def post_logout(h, path, args):
    """dashboard sign-out"""
    _json(h, {"ok": True}, headers=[dash._set_cookie("", 0)])


ROUTES = [
    ("GET", "/dash/api/state", get_state, "open"),
    ("POST", "/dash/api/google/forget", post_google_forget, "open"),
    ("POST", "/dash/api/toggl/token", post_toggl_token, "open"),
    ("POST", "/dash/api/toggl/forget", post_toggl_forget, "open"),
    ("POST", "/dash/api/logout", post_logout, "open"),
]
