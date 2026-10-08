"""Chat between the devices: one conversation, kept here.

Two Cardputers cannot reach each other -- each is behind someone's router --
but both reach this server, so it is the room: a device posts a line and
asks every few seconds for what is newer than the last one it has. Kept in
CARDOS_STATE/chat.json, the last KEEP messages.

    GET  /msg?since=ID&max=N -> id <tab> unix time <tab> name <tab> text, oldest first:
                                the first N after ID (the device's buffer is small,
                                so it asks again for the rest); since=0 is the
                                last N
    POST /msg?name=NAME      body: the text -> the new id

Lines, not JSON, because the device parses them with no library. A tab or
a newline in a message becomes a space.
"""
import json
import os
import threading
import time

from . import accounts, dash, notes

KEEP = 500
SHOW = 40
TEXT_MAX = 400
NAME_MAX = 16

_lock = threading.Lock()


def _path():
    return os.path.join(dash.state_dir(), "chat.json")


def load():
    try:
        with open(_path(), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def _save(msgs):
    os.makedirs(dash.state_dir(), exist_ok=True)
    tmp = _path() + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(msgs, f)
    os.replace(tmp, _path())


def _flat(s, n):
    return " ".join(s.replace("\t", " ").split())[:n]


def post(name, text):
    """The new message's id, or ValueError."""
    name, text = _flat(name, NAME_MAX), _flat(text, TEXT_MAX)
    if not name:
        raise ValueError("who is this?")
    if not text:
        raise ValueError("say something")
    with _lock:
        msgs = load()
        mid = (msgs[-1]["id"] + 1) if msgs else 1
        msgs.append({"id": mid, "t": int(time.time()), "name": name, "text": text})
        _save(msgs[-KEEP:])
    return mid


def since(sid, most=SHOW):
    msgs = load()
    if sid <= 0:
        return msgs[-most:]
    return [m for m in msgs if m["id"] > sid][:most]


def get_msg(h, path, args):
    """the messages after ?since=ID"""
    if not notes._allowed(h):
        return
    try:
        sid = int((args.get("since") or ["0"])[0])
        most = max(1, min(SHOW, int((args.get("max") or [str(SHOW)])[0])))
    except ValueError:
        sid, most = 0, SHOW
    h.text("".join("%d\t%d\t%s\t%s\n" % (m["id"], m["t"], m["name"], m["text"]) for m in since(sid, most)))


def post_msg(h, path, args):
    """a message from ?name=, the text in the body"""
    if not notes._allowed(h):
        return
    text = h.body(TEXT_MAX * 4).decode("utf-8", "replace")
    try:
        mid = post((args.get("name") or [""])[0], text)
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    h.text("%d\n" % mid)


POLL_MAX = 3
NOTES_KEEP = 20

# Things to tell a person's devices about, from the server's own work -- a
# Build that finished while its app was shut. Per person, in memory: a
# restart loses what was not yet fetched, which is a notification, not data.
_notes = {}             # user -> [{"id", "app", "title", "text"}]
_note_ids = {}          # user -> the last id given


def notify_push(user, app, title, text):
    """A note for every device of `user` (None: the one person a server
    without accounts has), fetched at their next /notify/poll."""
    with _lock:
        n = _note_ids.get(user, 0) + 1
        _note_ids[user] = n
        q = _notes.setdefault(user, [])
        q.append({"id": n, "app": _flat(app, 12), "title": _flat(title, 30),
                  "text": _flat(text, 70)})
        del q[:-NOTES_KEEP]


def notes_since(user, nid):
    with _lock:
        return [x for x in _notes.get(user, []) if x["id"] > nid], _note_ids.get(user, 0)


def get_notify_poll(h, path, args):
    """what a device should tell its owner about, while Chat is closed:
    ?chat=ID&me=NAME -> "ok LAST" and up to three newer messages from others
    &note=ID         -> "ok LAST NOTELAST", and the server's own news after
                        ID: "note <tab> app <tab> title <tab> text" (a Build done)

    The device (kernel/sys/notify.c) asks every half minute. With no chat id
    -- a device that has never asked -- it is only told where the room is,
    so its first poll is not every message ever sent. Other sources can add
    their own lines here later; the device ignores kinds it does not know."""
    if not notes._allowed(h):
        return
    try:
        sid = int((args.get("chat") or ["-1"])[0])
        nid = int((args.get("note") or ["-1"])[0])
    except ValueError:
        sid, nid = -1, -1
    me = _flat((args.get("me") or [""])[0], NAME_MAX).lower()
    msgs = load()
    last = msgs[-1]["id"] if msgs else 0
    user = accounts.current()
    _, note_last = notes_since(user, 0)
    if nid > note_last:
        nid = 0                 # the server restarted: its ids did too
    news, _ = notes_since(user, max(nid, 0))
    # The second number only for a device that asks for notes; one that
    # does not reads the first and stops there either way.
    out = ["ok %d %d\n" % (last, note_last) if nid >= 0 or "note" in args else "ok %d\n" % last]
    if sid >= 0:
        new = [m for m in msgs if m["id"] > sid and m["name"].lower() != me][-POLL_MAX:]
        out += ["chat\t%s\t%s\n" % (m["name"], m["text"]) for m in new]
    if nid >= 0:
        out += ["note\t%s\t%s\t%s\n" % (x["app"], x["title"], x["text"]) for x in news[-POLL_MAX:]]
    h.text("".join(out))


ROUTES = [
    ("GET", "/notify/poll", get_notify_poll, "open"),
    ("GET", "/msg", get_msg, "open"),
    ("POST", "/msg", post_msg, "open"),
]
