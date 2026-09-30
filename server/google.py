"""Google Calendar and Tasks for the device, in lines it can read.

The device used to talk to Google itself: TLS to googleapis.com (about 35 KB
of heap while a request is open), JSON replies trimmed with fields= to fit a
6 KB buffer, a refresh token and its expiry in the kernel. That made Calendar
and Todo the two largest apps on a 512 KB machine, and every Google call the
moment most likely to fail for memory. Here all of that is easy: the server
holds the login, does the HTTPS and the JSON, and hands the device plain
lines over the plain HTTP it already speaks to this server.

    GET  /calendar/events?from=RFC3339&to=RFC3339
         -> one event a line: id <tab> start <tab> end <tab> summary
            start and end exactly as Google gives them -- "2026-09-29T13:00:00
            -07:00" or, all day, "2026-09-29" -- so the app parses what it did
    POST /calendar/event[?id=ID]   body: start=..\\nend=..\\nsummary=..
         -> the event's id; with ?id= it changes that one (PATCH)
    GET  /todo/lists               -> id <tab> title, a list a line
    POST /todo/lists               body: the title   -> the new list's id
    GET  /todo/tasks?list=ID       -> id <tab> 0|1 <tab> title   (1 is done)
    POST /todo/task?list=ID        body: the title   -> the new task's id
    PATCH /todo/task?list=ID&id=ID body: done=0|1 and/or title=..
    DELETE /todo/task?list=ID&id=ID
    POST /google/credentials       body: client id, secret, refresh token
    GET  /google/status

Every page is fetched, so a long list is whole rather than cut at the
device's buffer. A tab or newline in a title becomes a space. Google's own
status passes through -- a 404 here is a 404 there -- so the apps' handling
of a task deleted in a browser still works. All behind the server's token.

The login is the dashboard's (server/dash.py): signing in to Google at /dash
leaves google.json in CARDOS_STATE, and this reads the same file -- one login
on the server, used here for the device's data and handed out by
/google/creds as before. /google/credentials is the other way in, for a
login that already exists on a device (`google push` in its console).
"""
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from . import dash

TOKEN_URL = "https://oauth2.googleapis.com/token"
CAL = "https://www.googleapis.com/calendar/v3/calendars/primary/events"
TASKS = "https://tasks.googleapis.com/tasks/v1"


_lock = threading.Lock()
_access = {"token": None, "until": 0.0}


class GoogleError(Exception):
    def __init__(self, status, why):
        Exception.__init__(self, why)
        self.status = status
        self.why = why


# ---- the network, in one place, so the tests can stand in for Google ---------

def http(method, url, body=None, headers=None, form=False):
    """(status, parsed JSON or None). A 4xx/5xx is returned, not raised."""
    data = None
    hdrs = dict(headers or {})
    if body is not None:
        if form:
            data = urllib.parse.urlencode(body).encode()
            hdrs["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


# ---- the login -----------------------------------------------------------------

def load_creds():
    c = dash.load_creds()
    return c if c and c.get("refresh_token") else None


def save_creds(client_id, secret, refresh):
    """The dashboard's file, owner-only and written whole (dash.save_creds)."""
    dash.save_creds({"client_id": client_id, "client_secret": secret,
                     "refresh_token": refresh, "issued_at": int(time.time()),
                     "email": "", "scope": "pushed from the device"})
    with _lock:
        _access["token"] = None


def access_token():
    """A current access token, refreshed a minute before it runs out."""
    with _lock:
        if _access["token"] and time.time() < _access["until"]:
            return _access["token"]
    c = load_creds()
    if not c:
        raise GoogleError(401, "the server has no Google login: sign in at /dash")
    status, j = http("POST", TOKEN_URL, form=True, body={
        "client_id": c["client_id"], "client_secret": c["client_secret"],
        "refresh_token": c["refresh_token"], "grant_type": "refresh_token"})
    if status != 200 or not j or "access_token" not in j:
        why = (j or {}).get("error", "status %d" % status)
        # invalid_grant is the one people meet: a Testing-mode consent screen
        # expires refresh tokens after seven days. Say what fixes it.
        if why == "invalid_grant":
            why = "invalid_grant: sign in again at /dash"
        raise GoogleError(401, "google refused the login: %s" % why)
    with _lock:
        _access["token"] = j["access_token"]
        _access["until"] = time.time() + int(j.get("expires_in", 3600)) - 60
    return j["access_token"]


def call(method, url, body=None):
    """A Google API call with the login; raises GoogleError on its failures."""
    status, j = http(method, url, body=body,
                     headers={"Authorization": "Bearer " + access_token()})
    if status == 401:                       # expired early: once more, fresh
        with _lock:
            _access["token"] = None
        status, j = http(method, url, body=body,
                         headers={"Authorization": "Bearer " + access_token()})
    if status >= 400:
        msg = ((j or {}).get("error") or {})
        msg = msg.get("message") if isinstance(msg, dict) else msg
        raise GoogleError(status, "google said %d: %s" % (status, msg or "error"))
    return j or {}


def pages(url, key="items"):
    """Every page of a list call."""
    out, token = [], None
    while True:
        u = url + ("&" if "?" in url else "?") + ("pageToken=" + urllib.parse.quote(token)
                                                  if token else "")
        j = call("GET", u.rstrip("?&"))
        out.extend(j.get(key) or [])
        token = j.get("nextPageToken")
        if not token or len(out) > 2000:
            return out


def clean(s):
    return " ".join((s or "").replace("\t", " ").split())


# ---- routes ----------------------------------------------------------------------

def _arg(args, name):
    return (args.get(name) or [""])[0]


def _fields(body):
    """key=value lines."""
    out = {}
    for line in body.decode("utf-8", "replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def _route(fn):
    """GoogleError into its status and one line; everything else is the route's."""
    def wrapped(h, path, args):
        try:
            h.text(fn(h, args))
        except GoogleError as e:
            sys.stderr.write("google: %s\n" % e.why)
            h.text("error %s\n" % e.why, e.status if e.status >= 400 else 502)
        except ValueError as e:
            h.text("error %s\n" % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


@_route
def get_events(h, args):
    """calendar events between from and to, one a line"""
    q = {"singleEvents": "true", "orderBy": "startTime", "maxResults": "250",
         "fields": "nextPageToken,items(id,summary,start,end)"}
    for k in ("from", "to"):
        v = _arg(args, k)
        if not v:
            raise ValueError("%s= is needed" % k)
        q["timeMin" if k == "from" else "timeMax"] = v
    lines = []
    for e in pages(CAL + "?" + urllib.parse.urlencode(q)):
        s, en = e.get("start") or {}, e.get("end") or {}
        start = s.get("dateTime") or s.get("date") or ""
        end = en.get("dateTime") or en.get("date") or ""
        if e.get("id") and start:
            lines.append("%s\t%s\t%s\t%s" % (e["id"], start, end,
                                             clean(e.get("summary")) or "(no title)"))
    return "".join(l + "\n" for l in lines)


def _when(v):
    return {"date": v} if len(v) == 10 else {"dateTime": v}


@_route
def post_event(h, args):
    """add a calendar event, or change one (?id=)"""
    f = _fields(h.body(4096))
    if not f.get("start") or not f.get("end"):
        raise ValueError("start= and end= are needed")
    ev = {"summary": f.get("summary", ""), "start": _when(f["start"]), "end": _when(f["end"])}
    eid = _arg(args, "id")
    if eid:
        j = call("PATCH", CAL + "/" + urllib.parse.quote(eid), ev)
    else:
        j = call("POST", CAL, ev)
    return "%s\n" % j.get("id", "")


@_route
def get_lists(h, args):
    """every task list, id and title"""
    return "".join("%s\t%s\n" % (l["id"], clean(l.get("title")))
                   for l in pages(TASKS + "/users/@me/lists?maxResults=100&fields="
                                  "nextPageToken,items(id,title)") if l.get("id"))


@_route
def post_list(h, args):
    """make a task list"""
    title = clean(h.body(1024).decode("utf-8", "replace"))
    if not title:
        raise ValueError("a list needs a title")
    return "%s\n" % call("POST", TASKS + "/users/@me/lists", {"title": title}).get("id", "")


def _list(args):
    lid = _arg(args, "list")
    if not lid:
        raise ValueError("list= is needed")
    return TASKS + "/lists/" + urllib.parse.quote(lid)


@_route
def get_tasks(h, args):
    """a list's tasks, id, done and title"""
    items = pages(_list(args) + "/tasks?showCompleted=true&showHidden=false&maxResults=100"
                  "&fields=nextPageToken,items(id,title,status)")
    items = [t for t in items if t.get("id") and clean(t.get("title"))]
    # What the device can hold: max= tasks, the open ones first (they are what
    # the list is for), and titles cut at 60. Its reply buffer is sized to that.
    cap = h.int_arg(args, "max", 0)
    if cap > 0:
        items = ([t for t in items if t.get("status") != "completed"] +
                 [t for t in items if t.get("status") == "completed"])[:cap]
    return "".join("%s\t%d\t%s\n" % (t["id"], 1 if t.get("status") == "completed" else 0,
                                     clean(t.get("title"))[:60])
                   for t in items)


@_route
def post_task(h, args):
    """add a task"""
    title = clean(h.body(1024).decode("utf-8", "replace"))
    if not title:
        raise ValueError("a task needs a title")
    return "%s\n" % call("POST", _list(args) + "/tasks", {"title": title}).get("id", "")


@_route
def patch_task(h, args):
    """tick, untick or rename a task"""
    tid = _arg(args, "id")
    f = _fields(h.body(1024))
    body = {}
    if "done" in f:
        body["status"] = "completed" if f["done"] == "1" else "needsAction"
    if f.get("title"):
        body["title"] = clean(f["title"])
    if not tid or not body:
        raise ValueError("id= and done= or title= are needed")
    call("PATCH", _list(args) + "/tasks/" + urllib.parse.quote(tid), body)
    return "ok\n"


@_route
def delete_task(h, args):
    """delete a task"""
    tid = _arg(args, "id")
    if not tid:
        raise ValueError("id= is needed")
    call("DELETE", _list(args) + "/tasks/" + urllib.parse.quote(tid))
    return "ok\n"


@_route
def post_credentials(h, args):
    """the Google login, from the device"""
    lines = [l.strip() for l in h.body(4096).decode("utf-8", "replace").splitlines()]
    if len(lines) < 3 or not all(lines[:3]):
        raise ValueError("three lines: client id, client secret, refresh token")
    save_creds(lines[0], lines[1], lines[2])
    access_token()                          # proves it, now, rather than at the first sync
    return "ok google signed in\n"


@_route
def get_google_status(h, args):
    """whether the server can reach Google"""
    access_token()
    return "ok\n"


ROUTES = [
    ("GET", "/calendar/events", get_events),
    ("POST", "/calendar/event", post_event),
    ("GET", "/todo/lists", get_lists),
    ("POST", "/todo/lists", post_list),
    ("GET", "/todo/tasks", get_tasks),
    ("POST", "/todo/task", post_task),
    ("PATCH", "/todo/task", patch_task),
    ("DELETE", "/todo/task", delete_task),
    ("POST", "/google/credentials", post_credentials),
    ("GET", "/google/status", get_google_status),
]
