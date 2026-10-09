"""Toggl Track for the device, in lines it can read.

The same arrangement as server/google.py: the server holds the secret (a
Toggl API token, pasted into the dashboard), does the HTTPS and the JSON,
and the Toggl app (apps/toggl.c) reads plain lines over the HTTP it already
speaks to this server.

    GET  /toggl/status    the running entry, then what to start from:
                            running <tab> id <tab> start <tab> description <tab> project <tab> #colour <tab> seconds so far
                          or "idle", then up to RECENT lines
                            recent <tab> project id <tab> description <tab> project <tab> #colour
                          then every active project, by name:
                            project <tab> id <tab> name <tab> #colour
                          start is UTC epoch seconds; the device counts from it
    POST /toggl/start     body: description=..\\nproject=ID   -> a "running" line
                          (whatever was running is stopped first)
    POST /toggl/describe  body: description=..  -> the running line, or "idle"
    POST /toggl/stop      -> stopped <tab> seconds <tab> description, or "idle"
    GET  /toggl/targets?off=SECONDS
                          -> target <tab> project <tab> #colour <tab> week|total
                             <tab> seconds done <tab> seconds wanted <tab> since,
                             one a target. off is the device's offset from UTC,
                             so "this week" starts at its Monday
    POST /toggl/target    body: project=NAME\\nkind=week|total\\nhours=H[\\nsince=YYYY-MM-DD]
                          -> ok; hours=0 removes it
    GET  /toggl/today?from=RFC3339&to=RFC3339
                          -> start <tab> seconds <tab> description <tab> project,
                             a line an entry, then "total <tab> seconds"

Toggl counts requests against an hourly quota (small on the free plan), so
what changes rarely is kept: the workspace with the token, projects for ten
minutes, recent entries for a minute. A device that has the start time does
its own counting; it asks again only when something is done.

Auth is HTTP Basic with the token as the user and "api_token" as the
password, which is how Toggl's v9 API takes a personal token. The token is
in toggl.json in CARDOS_STATE, owner-only, beside google.json.
"""
import base64
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime

from . import accounts, dash, store, wire

API = "https://api.track.toggl.com/api/v9"
RECENT = 10
PROJECTS_MAX = 20
PROJECTS_FOR = 600
ENTRIES_FOR = 60

_lock = threading.Lock()         # the cache; toggl.json has store's lock for its path
_cache = {}                     # (user, key) -> (until, value)


class TogglError(Exception):
    def __init__(self, status, why):
        Exception.__init__(self, why)
        self.status = status
        self.why = why


# ---- the token ------------------------------------------------------------------

def path():
    return os.path.join(accounts.user_dir(), "toggl.json")


def load():
    return store.read_json(path())


def save(c):
    store.write_json(path(), c)
    _drop_mine()


def update(fn):
    """Change toggl.json in place under its lock: fn(c) edits the dict (an
    empty one if there is no file) and may raise to leave it as it was.
    toggl.json holds the targets beside the token, so a writer that replaces
    the whole file -- the dashboard saving a new token, once -- loses them."""
    c = store.update_json(path(), lambda c: fn(c) and None, {})
    _drop_mine()
    return c


def forget():
    with store.lock_for(path()):
        try:
            os.remove(path())
        except OSError:
            pass
    _drop_mine()


def _drop_mine():
    """The current person's cached answers, nobody else's."""
    me = accounts.current()
    with _lock:
        for k in [k for k in _cache if k[0] == me]:
            del _cache[k]


# ---- the network, in one place, so the tests can stand in for Toggl -------------

def http(method, url, body=None, token=None):
    """(status, parsed JSON or None). A 4xx/5xx is returned, not raised."""
    hdrs = {"Authorization": "Basic " + base64.b64encode(
        ("%s:api_token" % token).encode()).decode()}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        hdrs["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            status = r.status
    except urllib.error.HTTPError as e:
        raw, status = e.read(), e.code
    try:
        return status, json.loads(raw) if raw.strip() else None
    except ValueError:
        return status, raw.decode("utf-8", "replace")


def call(method, rel, body=None, token=None):
    c = load() if token is None else {"token": token}
    if not c or not c.get("token"):
        raise TogglError(401, "no Toggl token: paste one at /dash")
    status, j = http(method, API + rel, body, c["token"])
    if status in (401, 403):
        raise TogglError(401, "Toggl refused the token: paste a new one at /dash")
    if status in (402, 429):
        raise TogglError(429, "Toggl's hourly request limit is used up: try later")
    if status >= 400:
        raise TogglError(status, "Toggl said %d: %s" % (status, str(j)[:120]))
    return j


def cached(key, seconds, fn):
    now = time.time()
    with _lock:
        key = (accounts.current(), key)           # one person's answers are not another's
        hit = _cache.get(key)
        if hit and hit[0] > now:
            return hit[1]
    v = fn()
    with _lock:
        _cache[key] = (now + seconds, v)
    return v


def drop(*keys):
    """Forget the current person's answers for `keys` -- keyed as cached()
    keys them. Popping the bare key, as this did once accounts came, dropped
    nothing: a status right after a start was the cached "idle"."""
    with _lock:
        for k in keys:
            _cache.pop((accounts.current(), k), None)


# ---- the parts ----------------------------------------------------------------------

def check_token(token):
    """The account behind a token: (name, workspace id). For the dashboard."""
    me = call("GET", "/me", token=token)
    return me.get("fullname") or me.get("email") or "?", me.get("default_workspace_id")


def workspace():
    c = load() or {}
    if not c.get("workspace"):
        _, wid = check_token(c.get("token"))       # the network, outside the lock
        update(lambda c: c.__setitem__("workspace", wid))
        return wid
    return c["workspace"]


def _projects():
    def get():
        js = call("GET", "/workspaces/%s/projects?active=true" % workspace()) or []
        return {p["id"]: (p.get("name", ""), p.get("color") or "") for p in js if p.get("id")}
    return cached("projects", PROJECTS_FOR, get)


def projects():
    """id -> name, the workspace's active projects."""
    return {k: v[0] for k, v in _projects().items()}


def color(pid):
    """A project's colour as Toggl has it ("#0b83d9"), or "" -- the device
    draws each project's name in its own colour."""
    c = _projects().get(pid, ("", ""))[1]
    return c if len(c) == 7 and c.startswith("#") else ""


def epoch(s):
    """Toggl's '2026-10-01T16:15:56+00:00' (or a Z) as UTC epoch seconds."""
    return int(datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp())


def clean(s):
    return wire.flat(s, 60)


def current():
    return cached("current", 10, lambda: call("GET", "/me/time_entries/current"))


def recent_entries():
    def get():
        # Both dates or neither: Toggl refuses start_date alone. The end is a
        # day ahead so a running entry is never cut off by the clock's edge.
        fmt = "%Y-%m-%dT%H:%M:%SZ"
        since = time.strftime(fmt, time.gmtime(time.time() - 14 * 86400))
        until = time.strftime(fmt, time.gmtime(time.time() + 86400))
        return call("GET", "/me/time_entries?start_date=%s&end_date=%s"
                    % (urllib.parse.quote(since), urllib.parse.quote(until))) or []
    return cached("recent", ENTRIES_FOR, get)


def running_line(e, names):
    # The seconds so far, by this server's clock, last: the device counts on
    # from them with its own uptime. Counting from `start` with the device's
    # clock showed 0:00:00 whenever that clock was behind -- after a reboot
    # it restores the time it last saved and waits for the network.
    start = epoch(e["start"])
    return "running\t%s\t%d\t%s\t%s\t%s\t%d\n" % (e["id"], start,
                                                 clean(e.get("description")),
                                                 clean(names.get(e.get("project_id"), "")),
                                                 color(e.get("project_id")),
                                                 max(0, int(time.time()) - start))


def status_text():
    names = projects()
    cur = current()
    out = running_line(cur, names) if cur else "idle\n"
    seen = set()
    for e in sorted(recent_entries(), key=lambda e: e.get("start", ""), reverse=True):
        key = (clean(e.get("description")), e.get("project_id"))
        if key in seen or not (key[0] or key[1]):
            continue
        seen.add(key)
        out += "recent\t%s\t%s\t%s\t%s\n" % (key[1] or "", key[0], clean(names.get(key[1], "")),
                                            color(key[1]))
        if len(seen) >= RECENT:
            break
    # Every active project, to start one with no description and add it
    # later -- which is how a timer is usually started in a hurry.
    for pid, name in sorted(names.items(), key=lambda kv: kv[1].lower())[:PROJECTS_MAX]:
        out += "project\t%s\t%s\t%s\n" % (pid, clean(name), color(pid))
    return out


def describe(description):
    """Give the running entry a description; None when nothing runs."""
    cur = call("GET", "/me/time_entries/current")
    if not cur:
        return None
    e = call("PUT", "/workspaces/%s/time_entries/%s" % (cur["workspace_id"], cur["id"]),
             {"description": description})
    drop("current", "recent")
    return running_line(e, projects())


def stop_running():
    cur = call("GET", "/me/time_entries/current")
    if not cur:
        return None
    e = call("PATCH", "/workspaces/%s/time_entries/%s/stop" % (cur["workspace_id"], cur["id"]))
    drop("current", "recent")
    return e


def start(description, project):
    stop_running()
    body = {"description": description, "workspace_id": workspace(), "created_with": "CardOS",
            "start": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "duration": -1}
    if project:
        body["project_id"] = int(project)
    e = call("POST", "/workspaces/%s/time_entries" % workspace(), body)
    drop("current", "recent")
    return running_line(e, projects())


def today_text(frm, to):
    names = projects()
    es = call("GET", "/me/time_entries?start_date=%s&end_date=%s"
              % (urllib.parse.quote(frm), urllib.parse.quote(to))) or []
    now, total, out = int(time.time()), 0, ""
    for e in sorted(es, key=lambda e: e.get("start", "")):
        start_s = epoch(e["start"])
        secs = e.get("duration") or 0
        if secs < 0:                      # running: Toggl stores -start
            secs = now - start_s
        total += secs
        out += "%d\t%d\t%s\t%s\n" % (start_s, secs, clean(e.get("description")),
                                     clean(names.get(e.get("project_id"), "")))
    return out + "total\t%d\n" % total


# ---- targets: hours a project should get ----------------------------------------------
#
# Kept with the token in toggl.json: a list of
#   {"project": id, "kind": "week" | "total", "hours": 10, "since": "2026-10-01"}
# A weekly one counts Monday to now; a total one counts from `since` (when the
# contract was booked; moving it is re-booking). Toggl lists entries for about
# the last three months, which is where a total can look back to.

from datetime import timedelta, timezone


def targets():
    return (load() or {}).get("targets") or []


def set_target(project, kind, hours, since=None):
    """Add, change (same project and kind) or, with hours 0, remove a target."""
    if not load():
        raise TogglError(401, "no Toggl token: connect Toggl at /dash")
    names = projects()                  # may ask Toggl: before the lock, not under it
    pid = None
    if str(project).isdigit() and int(project) in names:
        pid = int(project)
    else:
        low = str(project).strip().lower()
        for k, v in names.items():
            if v.lower() == low or (pid is None and v.lower().startswith(low) and low):
                pid = k
                if v.lower() == low:
                    break
    if pid is None:
        raise ValueError("no project called %s" % project)
    if kind not in ("week", "total"):
        raise ValueError("kind is week or total")
    hours = float(hours)
    if hours < 0 or hours > 1000:
        raise ValueError("hours from 0 to 1000")
    if kind == "total":
        since = since or time.strftime("%Y-%m-%d")
        datetime.strptime(since, "%Y-%m-%d")         # a date, or ValueError

    def change(c):
        if not c.get("token"):
            raise TogglError(401, "no Toggl token: connect Toggl at /dash")
        ts = [t for t in c.get("targets") or [] if not (t["project"] == pid and t["kind"] == kind)]
        if hours > 0:
            t = {"project": pid, "kind": kind, "hours": hours}
            if kind == "total":
                t["since"] = since
            ts.append(t)
        c["targets"] = ts
    update(change)
    return pid


def _entries_since(start_epoch):
    """Entries from start_epoch to now, cached two minutes: a page of targets
    must not cost a request each."""
    def get():
        fmt = "%Y-%m-%dT%H:%M:%SZ"
        a = time.strftime(fmt, time.gmtime(start_epoch))
        b = time.strftime(fmt, time.gmtime(time.time() + 86400))
        return call("GET", "/me/time_entries?start_date=%s&end_date=%s"
                    % (urllib.parse.quote(a), urllib.parse.quote(b))) or []
    return cached("since:%d" % (start_epoch // 3600), 120, get)


def week_start(now, off):
    """This week's Monday 00:00, local to `off` seconds from UTC, as epoch."""
    local = datetime.fromtimestamp(now + off, timezone.utc)
    monday = (local - timedelta(days=local.weekday())).replace(hour=0, minute=0, second=0,
                                                               microsecond=0)
    return int(monday.timestamp()) - off


def progress(off=0):
    """[(target, seconds done)] for every target."""
    ts = targets()
    if not ts:
        return []
    now = int(time.time())
    starts = []
    for t in ts:
        if t["kind"] == "week":
            starts.append(week_start(now, off))
        else:
            d = datetime.strptime(t["since"], "%Y-%m-%d").replace(tzinfo=timezone.utc)
            starts.append(int(d.timestamp()) - off)
    entries = _entries_since(min(starts))
    out = []
    for t, start in zip(ts, starts):
        done = 0
        for e in entries:
            if e.get("project_id") != t["project"]:
                continue
            s = epoch(e["start"])
            if s < start:
                continue
            d = e.get("duration") or 0
            done += (now - s) if d < 0 else d
        out.append((t, done))
    return out


def targets_text(off=0):
    names = projects()
    lines = []
    for t, done in progress(off):
        lines.append("target\t%s\t%s\t%s\t%d\t%d\t%s\n" % (
            clean(names.get(t["project"], "?")), color(t["project"]), t["kind"], done,
            int(t["hours"] * 3600), t.get("since", "")))
    return "".join(lines)


# ---- routes ---------------------------------------------------------------------------

def _fields(body):
    f = {}
    for line in body.decode("utf-8", "replace").splitlines():
        k, _, v = line.partition("=")
        f[k.strip()] = v.strip()
    return f


def _route(fn):
    def wrapped(h, path, args):
        try:
            h.text(fn(h, args))
        except TogglError as e:
            sys.stderr.write("toggl: %s\n" % e.why)
            h.text("error %s\n" % e.why, e.status)
        except ValueError as e:
            h.text("error %s\n" % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


@_route
def get_status(h, args):
    """the running Toggl entry and what to start from"""
    return status_text()


@_route
def post_start(h, args):
    """start a Toggl entry (stops the running one)"""
    f = _fields(h.body(1024))
    return start(f.get("description", ""), f.get("project", ""))


@_route
def post_stop(h, args):
    """stop the running Toggl entry"""
    e = stop_running()
    if not e:
        return "idle\n"
    return "stopped\t%d\t%s\n" % (e.get("duration") or 0, clean(e.get("description")))


@_route
def post_describe(h, args):
    """describe the running Toggl entry"""
    f = _fields(h.body(1024))
    return describe(f.get("description", "")) or "idle\n"


@_route
def get_targets(h, args):
    """each target: project, how far, of how much"""
    try:
        off = int((args.get("off") or ["0"])[0])
    except ValueError:
        raise ValueError("off= is seconds")
    return targets_text(off)


@_route
def post_target(h, args):
    """set a target: project, week|total, hours (0 removes)"""
    f = _fields(h.body(1024))
    set_target(f.get("project", ""), f.get("kind", "week"), f.get("hours", "0"), f.get("since"))
    return "ok\n"


@_route
def get_today(h, args):
    """Toggl entries between from and to"""
    frm, to = (args.get("from") or [""])[0], (args.get("to") or [""])[0]
    if not frm or not to:
        raise ValueError("from= and to= are needed")
    return today_text(frm, to)


ROUTES = [
    ("GET", "/toggl/status", get_status),
    ("POST", "/toggl/start", post_start),
    ("POST", "/toggl/stop", post_stop),
    ("POST", "/toggl/describe", post_describe),
    ("GET", "/toggl/today", get_today),
    ("GET", "/toggl/targets", get_targets),
    ("POST", "/toggl/target", post_target),
]
