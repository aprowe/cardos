"""A store any app can keep things in: keys, counters and queues.

Every service on this server grew its own module -- its own JSON file, its
own cache, its own routes -- and they drifted apart. A new device feature
(Jar Factory's daily stock, its friends and gifts; later, games) should not
need one: it keeps its data here, through routes every app shares, and a
server module is written only for what truly needs code on the server
(asking Claude, signing).

One sqlite3 database, CARDOS_STATE/kv.sqlite3 (0600, WAL), one connection
behind one lock: requests are few and short, and a lock is simpler than
a connection per thread.

Namespaces -- the `ns` argument -- and who sees them:

    me          the signed-in person's own (accounts.current())
    me/NAME     the person's data for one app, NAME
    app/NAME    one app's, shared by everyone: a game's world
    srv/NAME    written only by server code; anyone signed in may read it

NAME is [a-z0-9_-], 1 to 24. Without accounts the server is one person and
"me" is theirs. Keys are 1 to 128 of [A-Za-z0-9._:/@+-] -- no spaces or
tabs, so a listing is lines. Values are bytes, at most VALUE_MAX, with an
optional time to live in seconds; an expired value is gone.

Every value and every queued message counts against the person who wrote
it (key length plus value length): QUOTA bytes each. What server code
writes counts against nobody.

Counters are values holding a decimal integer: incr adds to one atomically
(a missing one is 0), and get reads it as text.

Queues: a person owns named queues (q, [a-z0-9][a-z0-9._-]{0,31}). Anyone
may push to their own; another person may push only if the owner has
allowed them on that queue (allow; "*" allows every signed-in person). The
allowlist is a value in the owner's "me" namespace, key q.allow/QUEUE, one
name a line. A message is bytes, at most VALUE_MAX, kept until the owner
acks it (or its TTL, if the pusher gave one, runs out); ids only grow.
A queue holds at most QUEUE_MAX messages.

The wire format (routes, all "device_or_dash"; writes are POST):

    GET  /kv/get?ns=NS&key=K        -> 200, the value's bytes exactly
                                       (application/octet-stream)
    POST /kv/put?ns=NS&key=K[&ttl=S]  body: the value's bytes -> "ok\\n"
    POST /kv/del?ns=NS&key=K        -> "ok 1\\n" (deleted) or "ok 0\\n" (was not there)
    GET  /kv/list?ns=NS[&prefix=P][&max=N]
                                    -> one line a key, in key order:
                                       KEY <tab> SIZE <tab> EXPIRES\\n
                                       EXPIRES is unix seconds, 0 for never.
                                       max default 200, at most 1000.
    POST /kv/incr?ns=NS&key=K[&by=N][&ttl=S]  -> the new value, "N\\n"
    POST /q/push?q=Q[&to=PERSON][&ttl=S]  body: the message -> "ok ID\\n"
                                       (to: whose queue; yourself if absent)
    GET  /q/peek?q=Q[&after=ID][&max=N]
                                    -> the oldest messages with id > after
                                       (default 0), max default 8, at most 50,
                                       and no more than PEEK_BYTES of bodies
                                       (always at least one). Each message is
                                       a header line and then its bytes:
                                       ID <tab> FROM <tab> AT <tab> SIZE\\n
                                       then SIZE bytes, then "\\n".
                                       FROM is the pusher's name ("-" for the
                                       server), AT unix seconds. Empty: no body.
    GET  /q/len?q=Q                 -> "N\\n", messages waiting
    POST /q/ack?q=Q&upto=ID         -> "ok N\\n": every message with id <= ID gone
    GET  /q/allow?q=Q               -> the allowed names, one a line
    POST /q/allow?q=Q&who=NAME[&off=1]  -> "ok\\n": NAME allowed (off=1: not)

A refusal is "error WHY\\n" with a status that says which: 400 a bad
argument, 403 not allowed (another's queue; srv/ from a device), 404 no
such key or person, 409 a counter that holds something that is not a
number, 413 too large, 507 over quota or the queue is full.
"""
import os
import re
import sqlite3
import threading
import time

from . import accounts, dash
from .routes import arg

VALUE_MAX = 16 * 1024
QUOTA = int(os.environ.get("CARDOS_KV_QUOTA") or 1024 * 1024)
QUEUE_MAX = 1000
PEEK_BYTES = 32 * 1024
LIST_MAX = 1000
PURGE_EVERY = 60

_NAME = re.compile(r"^[a-z0-9_-]{1,24}$")
_KEY = re.compile(r"^[A-Za-z0-9._:/@+-]{1,128}$")
_QUEUE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,31}$")
NOBODY = "-"                  # the one person of a server without accounts
SERVER = ""                   # what server code writes: no one's quota


class KVError(ValueError):
    code = 400


class NotAllowed(KVError):
    code = 403


class NotFound(KVError):
    code = 404


class NotANumber(KVError):
    code = 409


class TooBig(KVError):
    code = 413


class Full(KVError):
    code = 507


def me():
    """Who the request is: the account, or NOBODY without accounts."""
    return (accounts.current() if accounts.enabled() else None) or NOBODY


def resolve(ns, who, writing=False, server=False):
    """The stored namespace for a client's `ns`. srv/ is server code's to
    write; a device may only read it."""
    ns = ns or ""
    if ns == "me":
        return "u/" + who
    head, _, name = ns.partition("/")
    if not _NAME.match(name or ""):
        raise KVError("namespace is me, me/NAME, app/NAME or srv/NAME")
    if head == "me":
        return "u/%s/%s" % (who, name)
    if head == "app":
        return ns
    if head == "srv":
        if writing and not server:
            raise NotAllowed("srv/ is written by the server")
        return ns
    raise KVError("namespace is me, me/NAME, app/NAME or srv/NAME")


def check_key(key):
    if not _KEY.match(key or ""):
        raise KVError("a key is 1 to 128 of A-Z a-z 0-9 . _ : / @ + -")
    return key


def check_queue(q):
    if not _QUEUE.match(q or ""):
        raise KVError("a queue name is a-z 0-9 . _ -, up to 32")
    return q


def _allow_key(q):
    return "q.allow/" + q


class Store:
    """The database. Every method takes stored namespaces (resolve() makes
    them) and is safe from any thread. `clock` is for the tests."""

    def __init__(self, path, clock=time.time, quota=None):
        self.path = path
        self.clock = clock
        self.quota = QUOTA if quota is None else quota
        self.lock = threading.RLock()
        d = os.path.dirname(os.path.abspath(path))
        os.makedirs(d, mode=0o700, exist_ok=True)
        if not os.path.exists(path):
            os.close(os.open(path, os.O_CREAT | os.O_WRONLY, 0o600))
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        for side in ("-wal", "-shm"):          # sqlite makes these to the umask
            try:
                os.chmod(path + side, 0o600)
            except OSError:
                pass
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS kv (
                ns TEXT NOT NULL, key TEXT NOT NULL, value BLOB NOT NULL,
                expires REAL, owner TEXT NOT NULL,
                PRIMARY KEY (ns, key));
            CREATE INDEX IF NOT EXISTS kv_owner ON kv (owner);
            CREATE TABLE IF NOT EXISTS q (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                owner TEXT NOT NULL, queue TEXT NOT NULL, sender TEXT NOT NULL,
                at REAL NOT NULL, expires REAL, body BLOB NOT NULL);
            CREATE INDEX IF NOT EXISTS q_queue ON q (owner, queue, id);
            CREATE INDEX IF NOT EXISTS q_sender ON q (sender);
            CREATE TABLE IF NOT EXISTS people (
                user TEXT PRIMARY KEY, display TEXT, seen REAL);
        """)
        self._purged = 0

    def close(self):
        with self.lock:
            self.db.close()

    # ---- inside ---------------------------------------------------------------

    def _tx(self):
        """BEGIN IMMEDIATE .. COMMIT, or ROLLBACK on an exception."""
        store = self

        class Tx:
            def __enter__(self):
                store.db.execute("BEGIN IMMEDIATE")
                return store.db

            def __exit__(self, kind, *rest):
                store.db.execute("ROLLBACK" if kind else "COMMIT")
                return False
        return Tx()

    def _live(self):
        return "(expires IS NULL OR expires > %r)" % float(self.clock())

    def _purge(self):
        now = self.clock()
        if now - self._purged < PURGE_EVERY:
            return
        self._purged = now
        self.db.execute("DELETE FROM kv WHERE expires IS NOT NULL AND expires <= ?", (now,))
        self.db.execute("DELETE FROM q WHERE expires IS NOT NULL AND expires <= ?", (now,))

    def _expires(self, ttl):
        if ttl is None:
            return None
        ttl = float(ttl)
        if ttl <= 0:
            raise KVError("ttl is seconds, more than 0")
        return self.clock() + ttl

    def _usage(self, who):
        live = self._live()
        a = self.db.execute("SELECT COALESCE(SUM(LENGTH(key) + LENGTH(value)), 0) FROM kv "
                            "WHERE owner = ? AND " + live, (who,)).fetchone()[0]
        b = self.db.execute("SELECT COALESCE(SUM(LENGTH(body)), 0) FROM q "
                            "WHERE sender = ? AND " + live, (who,)).fetchone()[0]
        return a + b

    def _charge(self, who, adding, freeing=0):
        if who == SERVER or adding <= freeing:
            return
        if self._usage(who) - freeing + adding > self.quota:
            raise Full("over the quota of %d bytes" % self.quota)

    def _row(self, ns, key):
        return self.db.execute("SELECT value, owner FROM kv WHERE ns = ? AND key = ? AND "
                               + self._live(), (ns, key)).fetchone()

    def _write(self, ns, key, value, expires, who):
        old = self._row(ns, key)
        freeing = len(key) + len(old[0]) if old and old[1] == who else 0
        self._charge(who, len(key) + len(value), freeing)
        self.db.execute("INSERT OR REPLACE INTO kv (ns, key, value, expires, owner) "
                        "VALUES (?, ?, ?, ?, ?)", (ns, key, value, expires, who))

    # ---- values ---------------------------------------------------------------

    def get(self, ns, key):
        """The value's bytes, or None."""
        with self.lock:
            row = self._row(ns, key)
            return bytes(row[0]) if row else None

    def put(self, ns, key, value, ttl=None, who=SERVER):
        value = bytes(value)
        if len(value) > VALUE_MAX:
            raise TooBig("value of %d bytes is more than %d" % (len(value), VALUE_MAX))
        with self.lock, self._tx():
            self._purge()
            self._write(ns, key, value, self._expires(ttl), who)

    def delete(self, ns, key):
        """True if it was there."""
        with self.lock, self._tx():
            live = self._row(ns, key) is not None
            self.db.execute("DELETE FROM kv WHERE ns = ? AND key = ?", (ns, key))
            return live

    def list(self, ns, prefix="", limit=200):
        """[(key, size, expires or None)], in key order."""
        with self.lock:
            esc = prefix.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
            rows = self.db.execute(
                "SELECT key, LENGTH(value), expires FROM kv WHERE ns = ? AND key LIKE ? "
                "ESCAPE '\\' AND " + self._live() + " ORDER BY key LIMIT ?",
                (ns, esc + "%", int(limit))).fetchall()
            # LIKE folds ASCII case; a prefix is exact
            return [r for r in rows if r[0].startswith(prefix)]

    def incr(self, ns, key, by=1, ttl=None, who=SERVER):
        """The counter after adding `by`; a missing counter was 0. A ttl is
        set (or reset) when given, and an existing expiry kept when not."""
        with self.lock, self._tx():
            self._purge()
            row = self.db.execute("SELECT value, expires FROM kv WHERE ns = ? AND key = ? AND "
                                  + self._live(), (ns, key)).fetchone()
            n = 0
            expires = None
            if row:
                try:
                    n = int(bytes(row[0]).decode("ascii"))
                except ValueError:
                    raise NotANumber("%s is not a counter" % key)
                expires = row[1]
            n += int(by)
            if ttl is not None:
                expires = self._expires(ttl)
            self._write(ns, key, str(n).encode(), expires, who)
            return n

    def usage(self, who):
        """Bytes counted against a person."""
        with self.lock:
            return self._usage(who)

    # ---- queues ---------------------------------------------------------------

    def allowed(self, owner, q):
        """Who besides the owner may push to their queue q."""
        raw = self.get("u/" + owner, _allow_key(q)) or b""
        return [n for n in raw.decode("utf-8", "replace").split("\n") if n]

    def allow(self, owner, q, who, on=True):
        with self.lock:
            names = [n for n in self.allowed(owner, q) if n != who]
            if on:
                names.append(who)
            key = _allow_key(q)
            if names:
                self.put("u/" + owner, key, "\n".join(sorted(names)).encode(), who=owner)
            else:
                self.delete("u/" + owner, key)

    def may_push(self, owner, q, sender):
        if sender == SERVER or sender == owner:
            return True
        names = self.allowed(owner, q)
        return sender in names or "*" in names

    def push(self, owner, q, body, sender=SERVER, ttl=None):
        """The new message's id. sender SERVER is server code: always let in."""
        body = bytes(body)
        if len(body) > VALUE_MAX:
            raise TooBig("message of %d bytes is more than %d" % (len(body), VALUE_MAX))
        with self.lock:
            if not self.may_push(owner, q, sender):
                raise NotAllowed("%s does not take messages from you on %s" % (owner, q))
            with self._tx():
                self._purge()
                n = self.db.execute("SELECT COUNT(*) FROM q WHERE owner = ? AND queue = ? AND "
                                    + self._live(), (owner, q)).fetchone()[0]
                if n >= QUEUE_MAX:
                    raise Full("the queue is full")
                self._charge(sender, len(body))
                cur = self.db.execute(
                    "INSERT INTO q (owner, queue, sender, at, expires, body) VALUES (?, ?, ?, ?, ?, ?)",
                    (owner, q, sender, self.clock(), self._expires(ttl), body))
                return cur.lastrowid

    def peek(self, owner, q, after=0, limit=8, max_bytes=None):
        """[(id, sender, at, body)], oldest first, ids > after."""
        with self.lock:
            rows = self.db.execute(
                "SELECT id, sender, at, body FROM q WHERE owner = ? AND queue = ? AND id > ? AND "
                + self._live() + " ORDER BY id LIMIT ?", (owner, q, int(after), int(limit))).fetchall()
        out, total = [], 0
        for i, s, at, body in rows:
            total += len(body)
            if out and max_bytes is not None and total > max_bytes:
                break
            out.append((i, s, at, bytes(body)))
        return out

    def qlen(self, owner, q):
        with self.lock:
            return self.db.execute("SELECT COUNT(*) FROM q WHERE owner = ? AND queue = ? AND "
                                   + self._live(), (owner, q)).fetchone()[0]

    def ack(self, owner, q, upto):
        """Every message up to and including id `upto` gone: how many."""
        with self.lock, self._tx():
            return self.db.execute("DELETE FROM q WHERE owner = ? AND queue = ? AND id <= ?",
                                   (owner, q, int(upto))).rowcount

    def pop(self, owner, q):
        """The oldest message, taken: (id, sender, at, body), or None."""
        with self.lock:
            got = self.peek(owner, q, limit=1)
            if got:
                self.ack(owner, q, got[0][0])
            return got[0] if got else None

    # ---- people ---------------------------------------------------------------

    def saw(self, user, at=None):
        with self.lock:
            self.db.execute("INSERT INTO people (user, seen) VALUES (?, ?) "
                            "ON CONFLICT(user) DO UPDATE SET seen = excluded.seen",
                            (user, self.clock() if at is None else at))

    def set_display(self, user, display):
        with self.lock:
            self.db.execute("INSERT INTO people (user, display) VALUES (?, ?) "
                            "ON CONFLICT(user) DO UPDATE SET display = excluded.display",
                            (user, display))

    def people(self):
        """{user: (display or None, seen or None)}"""
        with self.lock:
            return {u: (d, s) for u, d, s in
                    self.db.execute("SELECT user, display, seen FROM people").fetchall()}


# ---- the server's one store -----------------------------------------------------

_stores = {}
_stores_lock = threading.Lock()


def path():
    return os.path.join(dash.state_dir(), "kv.sqlite3")


def store():
    """The store in CARDOS_STATE, opened once (the tests move CARDOS_STATE,
    so it is kept per path)."""
    p = os.path.abspath(path())
    with _stores_lock:
        s = _stores.get(p)
        if s is None or not os.path.exists(p):
            s = _stores[p] = Store(p)
        return s


def close_all():
    with _stores_lock:
        for s in _stores.values():
            s.close()
        _stores.clear()


# ---- routes -----------------------------------------------------------------------

def _num(args, name, default, lo=None, hi=None):
    raw = arg(args, name, "")
    if raw == "":
        return default
    try:
        v = float(raw) if name == "ttl" else int(raw)
    except ValueError:
        raise KVError("%s=%r is not a number" % (name, raw))
    if lo is not None and v < lo:
        raise KVError("%s is at least %s" % (name, lo))
    if hi is not None and v > hi:
        v = hi
    return v


def kv_route(fn):
    """fn(h, args) as a route: a KVError is one line, with its status."""
    def wrapped(h, path, args):
        try:
            fn(h, args)
        except KVError as e:
            h.text("error %s\n" % e, e.code)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _ns(args, writing=False):
    return resolve(arg(args, "ns"), me(), writing=writing), check_key(arg(args, "key"))


def _bytes(h, data, code=200):
    h.send_response(code)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(len(data)))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    h.wfile.write(data)


def _body(h):
    try:
        return h.body(VALUE_MAX)
    except ValueError as e:
        raise TooBig(str(e))


@kv_route
def get_kv(h, args):
    """a value, its bytes"""
    ns, key = _ns(args)
    v = store().get(ns, key)
    if v is None:
        raise NotFound("no such key")
    _bytes(h, v)


@kv_route
def put_kv(h, args):
    """keep a value: the body"""
    ns, key = _ns(args, writing=True)
    ttl = _num(args, "ttl", None)
    store().put(ns, key, _body(h), ttl=ttl, who=me())
    h.text("ok\n")


@kv_route
def del_kv(h, args):
    """delete a value"""
    ns, key = _ns(args, writing=True)
    h.text("ok %d\n" % store().delete(ns, key))


@kv_route
def list_kv(h, args):
    """keys by prefix: key, size, expires"""
    ns = resolve(arg(args, "ns"), me())
    prefix = arg(args, "prefix")
    if prefix and not _KEY.match(prefix):
        raise KVError("a prefix is made of what keys are")
    n = _num(args, "max", 200, lo=1, hi=LIST_MAX)
    rows = store().list(ns, prefix, n)
    h.text("".join("%s\t%d\t%d\n" % (k, size, int(exp or 0)) for k, size, exp in rows))


@kv_route
def incr_kv(h, args):
    """add to a counter; the new value"""
    ns, key = _ns(args, writing=True)
    by = _num(args, "by", 1)
    ttl = _num(args, "ttl", None)
    h.text("%d\n" % store().incr(ns, key, by, ttl=ttl, who=me()))


def _person(name):
    """A person who exists: an account, or the one person without accounts."""
    if not accounts.enabled():
        if name not in ("", NOBODY):
            raise NotFound("no such person")
        return NOBODY
    if name not in accounts.users():
        raise NotFound("no such person")
    return name


@kv_route
def push_q(h, args):
    """send a message to a queue: the body"""
    q = check_queue(arg(args, "q"))
    who = me()
    to = _person(arg(args, "to") or who)
    ttl = _num(args, "ttl", None)
    body = _body(h)
    h.text("ok %d\n" % store().push(to, q, body, sender=who, ttl=ttl))


@kv_route
def peek_q(h, args):
    """the messages waiting on one of your queues"""
    q = check_queue(arg(args, "q"))
    after = _num(args, "after", 0, lo=0)
    n = _num(args, "max", 8, lo=1, hi=50)
    out = []
    for i, sender, at, body in store().peek(me(), q, after, n, PEEK_BYTES):
        out.append(("%d\t%s\t%d\t%d\n" % (i, sender or NOBODY, int(at), len(body))).encode()
                   + body + b"\n")
    _bytes(h, b"".join(out))


@kv_route
def len_q(h, args):
    """how many messages wait on one of your queues"""
    q = check_queue(arg(args, "q"))
    h.text("%d\n" % store().qlen(me(), q))


@kv_route
def ack_q(h, args):
    """done with your messages up to an id"""
    q = check_queue(arg(args, "q"))
    if not arg(args, "upto"):
        raise KVError("upto= is the last id read")
    upto = _num(args, "upto", 0, lo=0)
    h.text("ok %d\n" % store().ack(me(), q, upto))


@kv_route
def get_allow(h, args):
    """who may send to one of your queues"""
    q = check_queue(arg(args, "q"))
    h.text("".join(n + "\n" for n in store().allowed(me(), q)))


@kv_route
def post_allow(h, args):
    """let someone send to one of your queues (off=1: stop)"""
    q = check_queue(arg(args, "q"))
    who = arg(args, "who")
    if who != "*":
        who = _person(who)
    store().allow(me(), q, who, on=arg(args, "off") not in ("1", "yes", "true"))
    h.text("ok\n")


ROUTES = [
    ("GET", "/kv/get", get_kv, "device_or_dash"),
    ("POST", "/kv/put", put_kv, "device_or_dash"),
    ("POST", "/kv/del", del_kv, "device_or_dash"),
    ("GET", "/kv/list", list_kv, "device_or_dash"),
    ("POST", "/kv/incr", incr_kv, "device_or_dash"),
    ("POST", "/q/push", push_q, "device_or_dash"),
    ("GET", "/q/peek", peek_q, "device_or_dash"),
    ("GET", "/q/len", len_q, "device_or_dash"),
    ("POST", "/q/ack", ack_q, "device_or_dash"),
    ("GET", "/q/allow", get_allow, "device_or_dash"),
    ("POST", "/q/allow", post_allow, "device_or_dash"),
]
