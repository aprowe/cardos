"""Remote Files: the device's SD card, browsed from the dashboard.

The droplet cannot reach the device -- it is behind somebody's router -- so
the device reaches out instead, the way Claude's terminal does. While the
Remote Files app is open on it (apps/rfiles.c), it asks this server for work
over and over:

    POST /files/poll?id=N     body: the answer to job N (none on the first)
                              -> the next job, or "idle" after a short wait

One request carries an answer one way and the next job the other, so a
transfer is one round trip a chunk, not two. A job is a line and maybe data:

    ID <tab> OP <tab> PATH <tab> ARG <newline> [base64]

    list  PATH            -> "ok" and a line per entry: d|f <tab> size <tab> name
    stat  PATH            -> "ok d|f SIZE"
    read  PATH  OFF,LEN   -> "ok" and the bytes, base64
    write PATH  OFF       data: base64. Into PATH.part; OFF 0 starts it afresh,
                          any other must be the part's length so far
    commit PATH           PATH.part becomes PATH (the old one removed first)
    mkdir PATH
    rm    PATH            a file, or an empty folder
    mv    PATH  NEWPATH
    sh    /     -     data: one console line -> "ok" (or "ok refused") and what
                          it printed. The terminal on the dashboard; the device
                          answers when the command is done, so this one waits
                          up to SH_TIMEOUT -- `update os` takes minutes.

and an answer is "ok ..." or "error why" on its first line. Base64 because
the device's HTTP layer carries text -- the reply is copied with %s -- and
CHUNK is what keeps a job inside the app's buffer.

The browser side is behind the dashboard's sign-in (server/dash.py): the page
at /dash/files and the calls it makes. nginx already passes /dash* through
over HTTPS, and the cookie is SameSite=Lax, so another site cannot post here
with it. Nothing is kept on the server: a download streams to the browser as
the chunks arrive, an upload is held only while it is being sent on.
"""
import base64
import json
import threading
import time
import urllib.parse
from collections import deque

from . import accounts, dash

CHUNK = 3072                 # bytes a job carries: 4096 of base64
POLL_WAIT = 1.5              # how long an idle poll is held open
SEEN = 6.0                   # a device not heard from in this long is gone
ASK_TIMEOUT = 20.0           # a job not answered in this long failed
UPLOAD_MAX = 16 * 1024 * 1024
SH_TIMEOUT = 300.0           # a console line may run this long (an update)
LINE_MAX = 200


class DeviceGone(Exception):
    pass


class Broker:
    """Jobs for the device, and the answers coming back. One device: the
    first to poll is the one asked, which is the only case there is."""

    def __init__(self):
        self.cond = threading.Condition()
        self.jobs = deque()
        self.answers = {}            # id -> answer text, or None while waiting
        self.next_id = 1
        self.last_seen = 0.0

    def connected(self):
        return time.time() - self.last_seen < SEEN

    def ask(self, op, path, arg="", data="", timeout=ASK_TIMEOUT):
        """Queue a job and wait for its answer: (ok, first line, the rest)."""
        with self.cond:
            if not self.connected():
                raise DeviceGone("the device is not connected: open Remote Files on it")
            jid = self.next_id
            self.next_id += 1
            self.answers[jid] = None
            self.jobs.append("%d\t%s\t%s\t%s\n%s" % (jid, op, path, arg, data))
            self.cond.notify_all()
            end = time.time() + timeout
            while self.answers[jid] is None:
                left = end - time.time()
                if left <= 0:
                    del self.answers[jid]
                    self.jobs = deque(j for j in self.jobs
                                      if not j.startswith("%d\t" % jid))
                    raise DeviceGone("the device did not answer")
                self.cond.wait(left)
            ans = self.answers.pop(jid)
        head, _, rest = ans.partition("\n")
        if head.startswith("ok"):
            return head[2:].strip(), rest
        raise IOError(head[6:].strip() if head.startswith("error") else head or "no answer")

    def poll(self, jid, answer, wait=POLL_WAIT):
        """The device's side: hand in an answer, take the next job."""
        with self.cond:
            self.last_seen = time.time()
            if jid in self.answers and self.answers[jid] is None:
                self.answers[jid] = answer
                self.cond.notify_all()
            end = time.time() + wait
            while not self.jobs:
                left = end - time.time()
                if left <= 0:
                    return "idle\n"
                self.cond.wait(left)
            self.last_seen = time.time()
            return self.jobs.popleft()


broker = Broker()             # without accounts, the one there is
_brokers = {}                 # with them: one per person -- each their own device


def _broker():
    u = accounts.current()
    return broker if u is None else _brokers.setdefault(u, Broker())


# ---- what the page asks, in terms of jobs ------------------------------------

def clean_path(p):
    """An absolute card path, or ValueError. The device checks too."""
    p = (p or "").strip()
    if not p.startswith("/") or "\t" in p or "\n" in p:
        raise ValueError("not a path: %r" % p)
    parts = [x for x in p.split("/") if x not in ("", ".")]
    if ".." in parts:
        raise ValueError("no .. in paths")
    return "/" + "/".join(parts)


def list_dir(path):
    _, rest = _broker().ask("list", path)
    out = []
    for line in rest.splitlines():
        f = line.split("\t", 2)
        if len(f) == 3 and f[0] in ("d", "f"):
            out.append({"name": f[2], "dir": f[0] == "d", "size": int(f[1] or 0)})
    out.sort(key=lambda e: (not e["dir"], e["name"].lower()))
    return out


def stat(path):
    head, _ = _broker().ask("stat", path)
    kind, _, size = head.partition(" ")
    return kind == "d", int(size or 0)


def read_chunks(path, size):
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        _, rest = _broker().ask("read", path, "%d,%d" % (off, n))
        data = base64.b64decode(rest.strip() or b"")
        if not data:
            raise IOError("the device read nothing at %d" % off)
        yield data
        off += len(data)


def write_file(path, data):
    off = 0
    while True:
        part = data[off:off + CHUNK]
        _broker().ask("write", path, str(off), base64.b64encode(part).decode())
        off += len(part)
        if off >= len(data):
            break
    _broker().ask("commit", path)


# ---- routes ----------------------------------------------------------------------

def _json(h, obj, code=200):
    h._send(code, "application/json", json.dumps(obj), (("Cache-Control", "no-store"),))


def _arg(args, k):
    return (args.get(k) or [""])[0]


def _browser(fn):
    """Behind the dashboard's cookie; the device's errors as JSON."""
    def wrapped(h, path, args):
        if not dash.logged_in(h):
            if path == "/dash/files":
                h.redirect("/dash")
            else:
                _json(h, {"error": "signed out: open /dash"}, 403)
            return
        try:
            fn(h, args)
        except DeviceGone as e:
            _json(h, {"error": str(e), "gone": True}, 503)
        except (IOError, ValueError) as e:
            _json(h, {"error": str(e)}, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def get_page(h, path, args):
    """the card: the dashboard's Files view"""
    h.redirect("/dash#/files")


@_browser
def get_status(h, args):
    """whether Remote Files is open on the device"""
    _json(h, {"connected": _broker().connected(),
              "last_seen": int(_broker().last_seen) if _broker().last_seen else None})


@_browser
def get_ls(h, args):
    """a folder on the card"""
    p = clean_path(_arg(args, "path") or "/")
    _json(h, {"path": p, "entries": list_dir(p)})


@_browser
def get_file(h, args):
    """a file from the card, streamed as it comes"""
    p = clean_path(_arg(args, "path"))
    is_dir, size = stat(p)
    if is_dir:
        raise ValueError("%s is a folder" % p)
    name = p.rsplit("/", 1)[-1]
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(size))
    h.send_header("Content-Disposition",
                  "attachment; filename*=UTF-8''%s" % urllib.parse.quote(name))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    try:
        for data in read_chunks(p, size):
            h.wfile.write(data)
    except (DeviceGone, IOError):
        h.close_connection = True       # short, and the browser says so


@_browser
def post_put(h, args):
    """a file onto the card (the body is the file)"""
    p = clean_path(_arg(args, "path"))
    data = h.body(UPLOAD_MAX)
    write_file(p, data)
    _json(h, {"ok": True, "size": len(data)})


@_browser
def post_mkdir(h, args):
    """a new folder"""
    _broker().ask("mkdir", clean_path(_arg(args, "path")))
    _json(h, {"ok": True})


@_browser
def post_rm(h, args):
    """delete a file or an empty folder"""
    _broker().ask("rm", clean_path(_arg(args, "path")))
    _json(h, {"ok": True})


@_browser
def post_mv(h, args):
    """rename or move"""
    _broker().ask("mv", clean_path(_arg(args, "from")), clean_path(_arg(args, "to")))
    _json(h, {"ok": True})


def run_line(line):
    """A console line on the device: (refused, output)."""
    line = " ".join((line or "").replace("\t", " ").split())
    if not line:
        raise ValueError("an empty line")
    if len(line) > LINE_MAX:
        raise ValueError("a line of at most %d characters" % LINE_MAX)
    head, rest = _broker().ask("sh", "/", "", line, timeout=SH_TIMEOUT)
    return head == "refused", rest


@_browser
def post_term(h, args):
    """a console line on the device, and what it printed"""
    try:
        line = json.loads(h.body(4096) or b"{}").get("line", "")
    except ValueError:
        raise ValueError("not JSON")
    refused, out = run_line(line)
    _json(h, {"ok": True, "refused": refused, "output": out})


def post_poll(h, path, args):
    """the device: an answer in, the next job out (Remote Files)"""
    try:
        jid = int(_arg(args, "id") or 0)
    except ValueError:
        jid = 0
    answer = h.body(64 * 1024).decode("utf-8", "replace")
    h.text(_broker().poll(jid, answer))


ROUTES = [
    ("GET", "/dash/files", get_page, "open"),
    ("GET", "/dash/files/status", get_status, "open"),
    ("GET", "/dash/files/ls", get_ls, "open"),
    ("GET", "/dash/files/get", get_file, "open"),
    ("POST", "/dash/files/put", post_put, "open"),
    ("POST", "/dash/files/mkdir", post_mkdir, "open"),
    ("POST", "/dash/files/rm", post_rm, "open"),
    ("POST", "/dash/files/mv", post_mv, "open"),
    ("POST", "/dash/term", post_term, "open"),
    ("POST", "/files/poll", post_poll),
]
