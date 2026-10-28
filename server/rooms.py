"""Rooms: a game, or anything else, that two or three friends share.

A room belongs to one app and to its members -- its creator and up to seven
of the creator's friends (server/friends.py). It has a small shared state
with a version, and a log of moves that only grows. Playing at once and
playing hours apart are the same thing: a device reads the moves after the
last one it has seen, every couple of seconds while the room is on screen,
or once when it is opened again. Two people typing into one crossword never
undo each other, because nobody overwrites anything: they append.

The server checks who is in a room, never a game's rules -- this is a
friends' server, and an app that needs rules kept writes a plugin.

In the store's database (server/kv.py: tables rooms and moves). A room's
moves count against whoever sent them and its state against its creator,
in the same quota as everything else they keep.

    POST /room/new?app=APP&with=A[,B..][&title=T]   body: the first state
                                -> "ok ID\\n"; each other member is told
                                   (app APP, "NAME: T")
    GET  /room/list?app=APP     -> the rooms you are in, latest first:
                                   ID <tab> TITLE <tab> MEMBERS <tab> SEQ
                                   <tab> UPDATED <tab> CREATOR\\n
                                   MEMBERS comma-separated; SEQ the last move
    GET  /room/get?id=ID        -> "ID <tab> VERSION <tab> SEQ <tab> MEMBERS\\n"
                                   then the state's bytes
    POST /room/state?id=ID[&if=VERSION]   body: the new state
                                -> "ok VERSION\\n"; when if= is given and is
                                   not the current version, 409 and
                                   "error changed VERSION\\n" (read, merge, retry)
    POST /room/move?id=ID[&say=TEXT]      body: the move
                                -> "ok SEQ\\n"; say= tells the other members
                                   (a turn game's "your move"); without it
                                   nobody is told -- a crossword's letters
    GET  /room/moves?id=ID[&after=SEQ][&max=N]
                                -> moves after SEQ, oldest first, at most N
                                   (default 50, at most 200) and PEEK_BYTES:
                                   SEQ <tab> FROM <tab> AT <tab> SIZE\\n then
                                   SIZE bytes then "\\n"
    POST /room/leave?id=ID      -> "ok\\n"; the room goes with its last member

A refusal is "error WHY\\n": 400, 403 (not a member, not a friend), 404, 409,
413, 507 as in server/kv.py.
"""
import time

from . import accounts, friends, kv, msg, wire
from .kv import KVError, NotAllowed, NotFound, TooBig, Full, kv_route
from .routes import arg

MAX_MEMBERS = 8
MAX_MOVES = 20000
IDLE_DAYS = 90
TITLE_MAX = 40
MOVES_MAX = 200


class Conflict(KVError):
    code = 409


def _members(row):
    return row[0].split(",")


def _room(st, rid, who):
    """(members, version, seq) of a room `who` is in."""
    row = st.db.execute("SELECT members, version, seq, app, title FROM rooms WHERE id = ?",
                        (int(rid),)).fetchone()
    if row is None:
        raise NotFound("no such room")
    if who not in _members(row):
        raise NotAllowed("you are not in that room")
    return row


def _purge(st):
    st.db.execute("DELETE FROM moves WHERE room IN (SELECT id FROM rooms WHERE updated < ?)",
                  (st.clock() - IDLE_DAYS * 86400,))
    st.db.execute("DELETE FROM rooms WHERE updated < ?", (st.clock() - IDLE_DAYS * 86400,))


def _check_app(app):
    if not kv._NAME.match(app or ""):
        raise KVError("app= is the app's name, a-z 0-9 _ -")
    return app


def create(who, app, others, state=b"", title=""):
    """The new room's id."""
    _check_app(app)
    others = [o for o in dict.fromkeys(others) if o and o != who]
    if not others:
        raise KVError("with= names at least one friend")
    if len(others) + 1 > MAX_MEMBERS:
        raise KVError("a room has at most %d people" % MAX_MEMBERS)
    for o in others:
        if not friends.are_friends(who, o):
            raise NotAllowed("%s is not your friend" % o)
    if len(state) > kv.VALUE_MAX:
        raise TooBig("state of %d bytes is more than %d" % (len(state), kv.VALUE_MAX))
    title = wire.flat(title, TITLE_MAX)
    st = kv.store()
    with st.lock, st._tx():
        _purge(st)
        st._charge(who, len(state))
        now = st.clock()
        rid = st.db.execute(
            "INSERT INTO rooms (app, title, creator, members, state, version, seq, created, updated) "
            "VALUES (?, ?, ?, ?, ?, 1, 0, ?, ?)",
            (app, title, who, ",".join([who] + others), bytes(state), now, now)).lastrowid
    name = friends.display(who)
    for o in others:
        msg.notify_push(o, app, name, title or "started something with you")
    return rid


def rooms_of(who, app):
    """[(id, title, members, seq, updated, creator)], latest first."""
    _check_app(app)
    st = kv.store()
    with st.lock:
        rows = st.db.execute("SELECT id, title, members, seq, updated, creator FROM rooms "
                             "WHERE app = ? ORDER BY updated DESC, id DESC", (app,)).fetchall()
    return [r for r in rows if who in r[2].split(",")]


def get(who, rid):
    """(version, seq, members, state)"""
    st = kv.store()
    with st.lock:
        _room(st, rid, who)
        v, seq, members, state = st.db.execute(
            "SELECT version, seq, members, state FROM rooms WHERE id = ?", (int(rid),)).fetchone()
    return v, seq, members.split(","), bytes(state)


def set_state(who, rid, state, if_version=None):
    """The new version; Conflict when if_version is not the current one."""
    state = bytes(state)
    if len(state) > kv.VALUE_MAX:
        raise TooBig("state of %d bytes is more than %d" % (len(state), kv.VALUE_MAX))
    st = kv.store()
    with st.lock, st._tx():
        row = _room(st, rid, who)
        if if_version is not None and int(if_version) != row[1]:
            raise Conflict("changed %d" % row[1])
        creator, old = st.db.execute("SELECT creator, LENGTH(state) FROM rooms WHERE id = ?",
                                     (int(rid),)).fetchone()
        st._charge(creator, len(state), old)
        st.db.execute("UPDATE rooms SET state = ?, version = version + 1, updated = ? WHERE id = ?",
                      (state, st.clock(), int(rid)))
        return row[1] + 1


def move(who, rid, body, say=""):
    """The move's sequence number."""
    body = bytes(body)
    if len(body) > kv.VALUE_MAX:
        raise TooBig("move of %d bytes is more than %d" % (len(body), kv.VALUE_MAX))
    st = kv.store()
    with st.lock, st._tx():
        row = _room(st, rid, who)
        if row[2] >= MAX_MOVES:
            raise Full("the room has %d moves, the most it keeps" % MAX_MOVES)
        st._charge(who, len(body))
        seq = row[2] + 1
        now = st.clock()
        st.db.execute("INSERT INTO moves (room, seq, sender, at, body) VALUES (?, ?, ?, ?, ?)",
                      (int(rid), seq, who, now, body))
        st.db.execute("UPDATE rooms SET seq = ?, updated = ? WHERE id = ?", (seq, now, int(rid)))
        members, app, title = _members(row), row[3], row[4]
    if say:
        name = friends.display(who)
        for m in members:
            if m != who:
                msg.notify_push(m, app, name, wire.flat(say, 60))
    return seq


def moves(who, rid, after=0, limit=50, max_bytes=kv.PEEK_BYTES):
    """[(seq, sender, at, body)] after `after`, oldest first."""
    st = kv.store()
    with st.lock:
        _room(st, rid, who)
        rows = st.db.execute("SELECT seq, sender, at, body FROM moves WHERE room = ? AND seq > ? "
                             "ORDER BY seq LIMIT ?", (int(rid), int(after), int(limit))).fetchall()
    out, total = [], 0
    for seq, s, at, body in rows:
        total += len(body)
        if out and total > max_bytes:
            break
        out.append((seq, s, at, bytes(body)))
    return out


def leave(who, rid):
    st = kv.store()
    with st.lock, st._tx():
        row = _room(st, rid, who)
        left = [m for m in _members(row) if m != who]
        if left:
            st.db.execute("UPDATE rooms SET members = ? WHERE id = ?", (",".join(left), int(rid)))
        else:
            st.db.execute("DELETE FROM moves WHERE room = ?", (int(rid),))
            st.db.execute("DELETE FROM rooms WHERE id = ?", (int(rid),))


# ---- routes -----------------------------------------------------------------------

def _id(args):
    raw = arg(args, "id")
    if not raw.isdigit():
        raise KVError("id= is a room's number")
    return int(raw)


def _int(args, name, default, hi=None):
    raw = arg(args, name)
    if raw == "":
        return default
    if not raw.lstrip("-").isdigit():
        raise KVError("%s= is a number" % name)
    v = max(0, int(raw))
    return min(v, hi) if hi is not None else v


def _body(h):
    try:
        return h.body(kv.VALUE_MAX)
    except ValueError as e:
        raise TooBig(str(e))


def _me():
    if not accounts.enabled():
        raise NotFound("this server has one person, and rooms are for friends")
    return kv.me()


@kv_route
def post_new(h, args):
    """start a room with friends: the body is its first state"""
    others = [o.strip() for o in arg(args, "with").split(",")]
    rid = create(_me(), arg(args, "app"), others, _body(h), arg(args, "title"))
    h.text("ok %d\n" % rid)


@kv_route
def get_list(h, args):
    """the rooms of one app you are in"""
    rows = rooms_of(_me(), arg(args, "app"))
    h.text("".join("%d\t%s\t%s\t%d\t%d\t%s\n" % (r[0], r[1], r[2], r[3], int(r[4]), r[5])
                   for r in rows))


@kv_route
def get_room(h, args):
    """a room's state, after a line saying its version"""
    rid = _id(args)
    v, seq, members, state = get(_me(), rid)
    kv._bytes(h, ("%d\t%d\t%d\t%s\n" % (rid, v, seq, ",".join(members))).encode() + state)


@kv_route
def post_state(h, args):
    """replace a room's state (if=VERSION: only if nobody changed it)"""
    iv = arg(args, "if")
    v = set_state(_me(), _id(args), _body(h), int(iv) if iv.isdigit() else None)
    h.text("ok %d\n" % v)


@kv_route
def post_move(h, args):
    """add a move (say=TEXT tells the others)"""
    h.text("ok %d\n" % move(_me(), _id(args), _body(h), arg(args, "say")))


@kv_route
def get_moves(h, args):
    """the moves after one you have"""
    rows = moves(_me(), _id(args), _int(args, "after", 0), _int(args, "max", 50, MOVES_MAX) or 1)
    kv._bytes(h, b"".join(("%d\t%s\t%d\t%d\n" % (seq, s, int(at), len(b))).encode() + b + b"\n"
                          for seq, s, at, b in rows))


@kv_route
def post_leave(h, args):
    """leave a room"""
    leave(_me(), _id(args))
    h.text("ok\n")


ROUTES = [
    ("POST", "/room/new", post_new, "device_or_dash"),
    ("GET", "/room/list", get_list, "device_or_dash"),
    ("GET", "/room/get", get_room, "device_or_dash"),
    ("POST", "/room/state", post_state, "device_or_dash"),
    ("POST", "/room/move", post_move, "device_or_dash"),
    ("GET", "/room/moves", get_moves, "device_or_dash"),
    ("POST", "/room/leave", post_leave, "device_or_dash"),
]
