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

from . import dash, notes

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


ROUTES = [
    ("GET", "/msg", get_msg, "open"),
    ("POST", "/msg", post_msg, "open"),
]
