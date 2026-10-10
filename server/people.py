"""The server's people, as a friend sees them: a name, a display name, when
they were last here.

    GET  /people          -> one line a person, by name:
                             NAME <tab> DISPLAY <tab> LAST_SEEN\\n
                             DISPLAY is the name they chose (their account
                             name until they choose one); LAST_SEEN unix
                             seconds, 0 for never. Without accounts: empty.
    POST /people/name     body: a display name (one line, up to DISPLAY_MAX)
                          -> "ok\\n"; an empty body goes back to the account name

Nothing else about an account is shown: not who is an admin, not devices.
NAME is what /q/push?to= and /q/allow?who= take.

Last seen: app._dispatch calls saw() once a request is somebody's. It is
written to the store (kv.sqlite3, table people) at most once a SEEN_EVERY
seconds per person, so a device polling every few seconds costs a dict
lookup, not a write.
"""
import sqlite3
import sys
import threading
import time

from . import accounts, kv, wire
from .kv import kv_route

DISPLAY_MAX = 16
SEEN_EVERY = 60

_last = {}                   # (store path, user) -> when last written
_lock = threading.Lock()


def saw(user, now=None):
    """The person was here. Cheap: one write a minute at most."""
    if not user or not accounts.enabled():
        return
    now = time.time() if now is None else now
    k = (kv.path(), user)
    with _lock:
        if now - _last.get(k, 0) < SEEN_EVERY:
            return
        _last[k] = now
    try:
        kv.store().saw(user, now)
    except sqlite3.Error as e:                 # never the request's problem
        sys.stderr.write("people: %s\n" % e)


def listing():
    """[(name, display, seen)] for every account, by name."""
    if not accounts.enabled():
        return []
    known = kv.store().people()
    out = []
    for name in sorted(accounts.users()):
        display, seen = known.get(name, (None, None))
        out.append((name, display or name, int(seen or 0)))
    return out


def get_people(h, path, args):
    """everyone on this server: name, display name, last seen"""
    h.text("".join("%s\t%s\t%d\n" % row for row in listing()))


@kv_route
def post_name(h, args):
    """choose the name your friends see"""
    try:
        raw = h.body(1024)
    except ValueError as e:
        raise kv.TooBig(str(e))
    name = wire.flat(raw.decode("utf-8", "replace"), DISPLAY_MAX)
    kv.store().set_display(kv.me(), name or None)
    h.text("ok\n")


ROUTES = [
    ("GET", "/people", get_people, "device_or_dash"),
    ("POST", "/people/name", post_name, "device_or_dash"),
]
