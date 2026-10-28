"""Friends: who on this server a person plays with and shares with.

A friendship is mutual. One person asks, the other asks back (or accepts on
the dashboard, which is the same request), and from then on any app may
show one friend's public data to the other (`pub/APP` in server/kv.py),
start a room with them (server/rooms.py) or send them something. There is
no feed and nothing is posted by itself: the brief was "not social media".

Kept in the store, srv/friends, key of/NAME: one line a person,
`OTHER <space> STATE`, STATE one of

    friend    both said yes
    asked     NAME asked OTHER, who has not answered
    asking    OTHER asked NAME

Both sides are written under the store's lock, so they cannot disagree.
Without accounts there is one person and no friends.

    GET  /friends               -> one line a person you have any state with:
                                   NAME <tab> DISPLAY <tab> STATE <tab> LAST_SEEN
                                   friends first, then asks, each by name
    POST /friends/ask?who=NAME  -> "ok asked\\n", or "ok friend\\n" when NAME had
                                   asked you; NAME is notified (app Friends)
    POST /friends/drop?who=NAME -> "ok\\n": the friendship, or an ask either
                                   way, gone from both sides

and the same three under /dash/ for the dashboard, whose cookie goes
nowhere else (people.py has /dash/people for the same reason).
"""
from . import accounts, kv, msg, people
from .kv import kv_route
from .routes import arg

NS = "srv/friends"
STATES = ("friend", "asked", "asking")


def _key(name):
    return "of/" + name


def _read(st, name):
    raw = (st.get(NS, _key(name)) or b"").decode("utf-8", "replace")
    out = {}
    for line in raw.splitlines():
        other, _, state = line.partition(" ")
        if other and state in STATES:
            out[other] = state
    return out


def _write(st, name, rel):
    if rel:
        body = "".join("%s %s\n" % (o, rel[o]) for o in sorted(rel)).encode()
        st.put(NS, _key(name), body)
    else:
        st.delete(NS, _key(name))


def relations(name):
    """{other: state} for one person."""
    if not name or not accounts.enabled():
        return {}
    return _read(kv.store(), name)


def friends_of(name):
    """The names of a person's friends, sorted."""
    return sorted(o for o, s in relations(name).items() if s == "friend")


def are_friends(a, b):
    return bool(a) and bool(b) and a != b and relations(a).get(b) == "friend"


def ask(me, who):
    """me asks who: 'asked', or 'friend' when who had already asked."""
    if not accounts.enabled():
        raise kv.NotFound("this server has one person")
    if who == me:
        raise kv.KVError("you cannot be your own friend")
    if who not in accounts.users():
        raise kv.NotFound("no such person")
    st = kv.store()
    with st.lock:
        mine, theirs = _read(st, me), _read(st, who)
        if mine.get(who) == "friend":
            return "friend"
        if mine.get(who) == "asking":
            mine[who], theirs[me] = "friend", "friend"
            state = "friend"
        else:
            mine[who], theirs[me] = "asked", "asking"
            state = "asked"
        _write(st, me, mine)
        _write(st, who, theirs)
    name = display(me)
    if state == "friend":
        msg.notify_push(who, "Friends", name, "is your friend now")
    else:
        msg.notify_push(who, "Friends", name, "wants to be friends")
    return state


def drop(me, who):
    st = kv.store()
    with st.lock:
        mine, theirs = _read(st, me), _read(st, who)
        mine.pop(who, None)
        theirs.pop(me, None)
        _write(st, me, mine)
        _write(st, who, theirs)


def display(name):
    """The name a person chose to be seen by, or their account name."""
    d, _ = kv.store().people().get(name, (None, None))
    return d or name


def listing(me):
    """[(name, display, state, seen)]: friends first, then asks, by name."""
    rel = relations(me)
    seen = {n: (d, s) for n, d, s in people.listing()}
    order = sorted(rel, key=lambda o: (STATES.index(rel[o]), o))
    return [(o, seen.get(o, (o, 0))[0], rel[o], seen.get(o, (o, 0))[1]) for o in order]


# ---- routes -----------------------------------------------------------------------

def get_friends(h, path, args):
    """your friends and asks: name, display name, state, last seen"""
    h.text("".join("%s\t%s\t%s\t%d\n" % row for row in listing(kv.me())))


@kv_route
def post_ask(h, args):
    """ask someone to be friends (or say yes to their ask)"""
    h.text("ok %s\n" % ask(kv.me(), arg(args, "who")))


@kv_route
def post_drop(h, args):
    """stop being friends, or take back an ask"""
    drop(kv.me(), arg(args, "who"))
    h.text("ok\n")


ROUTES = [
    ("GET", "/friends", get_friends, "device_or_dash"),
    ("POST", "/friends/ask", post_ask, "device_or_dash"),
    ("POST", "/friends/drop", post_drop, "device_or_dash"),
    # The same for the dashboard's Friends panel: its cookie's Path is /dash,
    # so a browser sends it to nothing outside (server/dash.py).
    ("GET", "/dash/friends", get_friends, "dash"),
    ("POST", "/dash/friends/ask", post_ask, "dash"),
    ("POST", "/dash/friends/drop", post_drop, "dash"),
]
