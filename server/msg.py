"""Notifications from the server to a person's devices.

Two sources reach a device through one poll, /notify/poll, which every
device asks every half minute while WiFi is up (kernel/sys/notify.c):

  - the server's own notes (notify_push): a Build done, a Claude reply, an
    update to install, a friend's invite or move (server/friends.py,
    server/rooms.py) -- kept per person in the store;
  - each app plugin's part (server/plugins.py, notify_poll): Chat's new
    messages, the one app with lines of its own here.
"""
import json
import threading

from . import accounts, kv, plugins, wire

_lock = threading.Lock()


def _flat(s, n):
    return wire.flat(s, n)


POLL_MAX = 3                 # notes a poll brings at most
NOTES_KEEP = 20

# Things to tell a person's devices about, from the server's own work -- a
# Build that finished while its app was shut, an update to install. Per
# person, in the store (server/kv.py), not memory (2026-10-13): a deploy
# publishes from its own process (`python -m server.build deploy`) and then
# restarts the service, so an "Update available" kept in memory was never
# seen by anyone, while Chat, on disk, always was.
NOTES_NS = "srv/notify"


def _who(user):
    return user or "-"


def notify_push(user, app, title, text):
    """A note for every device of `user` (None: the one person a server
    without accounts has), fetched at their next /notify/poll. From any
    process on the server."""
    st = kv.store()
    with _lock:
        n = st.incr(NOTES_NS, "id/" + _who(user))
        q = json.loads((st.get(NOTES_NS, "q/" + _who(user)) or b"[]").decode())
        q.append({"id": n, "app": _flat(app, 12), "title": _flat(title, 30),
                  "text": _flat(text, 70)})
        st.put(NOTES_NS, "q/" + _who(user), json.dumps(q[-NOTES_KEEP:]).encode())


def notes_since(user, nid):
    st = kv.store()
    with _lock:
        q = json.loads((st.get(NOTES_NS, "q/" + _who(user)) or b"[]").decode())
        last = st.incr(NOTES_NS, "id/" + _who(user), 0)
    return [x for x in q if x["id"] > nid], last


def get_notify_poll(h, path, args):
    """what a device should tell its owner about:
    ?chat=ID&me=NAME -> "ok LAST" (LAST: Chat's last message id) and Chat's
                        newer messages from others (apps/chat/server)
    &note=ID         -> "ok LAST NOTELAST", and the server's own news after
                        ID: "note <tab> app <tab> title <tab> text"
                        (a Build done, a Claude reply, an update to install)

    The device ignores kinds of line it does not know, so a plugin may add
    its own."""
    try:
        nid = int((args.get("note") or ["-1"])[0])
    except ValueError:
        nid = -1
    user = accounts.current()
    last, lines = plugins.notify_parts(args, user)
    last = last or 0
    _, note_last = notes_since(user, 0)
    if nid > note_last:
        nid = 0                 # the server restarted: its ids did too
    news, _ = notes_since(user, max(nid, 0))
    # The second number only for a device that asks for notes; one that
    # does not reads the first and stops there either way.
    out = ["ok %d %d\n" % (last, note_last) if nid >= 0 or "note" in args else "ok %d\n" % last]
    out += lines
    if nid >= 0:
        out += ["note\t%s\t%s\t%s\n" % (x["app"], x["title"], x["text"]) for x in news[-POLL_MAX:]]
    h.text("".join(out))


ROUTES = [
    ("GET", "/notify/poll", get_notify_poll, "device_or_dash"),
]
