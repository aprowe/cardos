"""Chat's server: one conversation between the devices, kept here.

A server plugin (server/plugins.py): apps/chat is the app, this is what it
needs of the server. Its part of every device's /notify/poll is
notify_poll() below.

Two Cardputers cannot reach each other -- each is behind someone's router --
but both reach this server, so it is the room: a device posts a line and
asks every few seconds for what is newer than the last one it has. Kept in
CARDOS_STATE/chat.json, the last KEEP messages.

    GET  /msg?since=ID&max=N -> id <tab> unix time <tab> name <tab> text, oldest first:
                                the first N after ID (the device's buffer is small,
                                so it asks again for the rest); since=0 is the
                                last N
    POST /msg?name=NAME      body: the text -> the new id
    POST /share?app=APP[&name=NAME]  body: one line -> the new id; posted as
                                "[APP] line", for what an app's person chose to
                                send (a Wordle result)

Lines, not JSON, because the device parses them with no library. A tab or
a newline in a message becomes a space.
"""
import json
import os
import re
import threading
import time

from server import accounts, dash, kv, store, wire

KEEP = 500
SHOW = 40
TEXT_MAX = 400
NAME_MAX = 16

_lock = threading.Lock()


def _path():
    return os.path.join(dash.state_dir(), "chat.json")


def load():
    return store.read_json(_path(), [])


def _flat(s, n):
    return wire.flat(s, n)


def post(name, text):
    """The new message's id, or ValueError."""
    name, text = _flat(name, NAME_MAX), _flat(text, TEXT_MAX)
    if not name:
        raise ValueError("who is this?")
    if not text:
        raise ValueError("say something")
    mid = []

    def add(msgs):
        mid.append((msgs[-1]["id"] + 1) if msgs else 1)
        msgs.append({"id": mid[0], "t": int(time.time()), "name": name, "text": text})
        return msgs[-KEEP:]
    store.update_json(_path(), add, [])
    return mid[0]


def since(sid, most=SHOW):
    msgs = load()
    if sid <= 0:
        return msgs[-most:]
    return [m for m in msgs if m["id"] > sid][:most]


def get_msg(h, path, args):
    """the messages after ?since=ID"""
    try:
        sid = int((args.get("since") or ["0"])[0])
        most = max(1, min(SHOW, int((args.get("max") or [str(SHOW)])[0])))
    except ValueError:
        sid, most = 0, SHOW
    h.text("".join("%d\t%d\t%s\t%s\n" % (m["id"], m["t"], m["name"], m["text"]) for m in since(sid, most)))


def post_msg(h, path, args):
    """a message from ?name=, the text in the body"""
    text = h.body(TEXT_MAX * 4).decode("utf-8", "replace")
    try:
        mid = post((args.get("name") or [""])[0], text)
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    h.text("%d\n" % mid)


def post_share(h, path, args):
    """something you chose to send from an app: ?app=APP[&name=NAME], one
    line in the body, posted to Chat as "[APP] line" -- Chat's own poll
    makes the banner. NAME is the sender's Chat name; without it, the name
    their friends see (server/people.py)."""
    app = _flat((args.get("app") or [""])[0], 16)
    text = _flat(h.body(TEXT_MAX * 4).decode("utf-8", "replace"), TEXT_MAX - 20)
    name = (args.get("name") or [""])[0]
    if not name:
        user = accounts.current()
        d, _ = kv.store().people().get(user, (None, None)) if user else (None, None)
        name = d or user or "me"
    try:
        mid = post(name, "[%s] %s" % (app, text) if app else text)
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    h.text("%d\n" % mid)


# ":dog-mood:" in a message is a dog emoji (apps/chat/emoji.h): Chat draws
# it, a banner cannot, so the banner gets it in words -- "Arty: happy"
# when it is the whole message, "(Arty: happy)" among words.
_EMOJI = re.compile(r":(arty|marcy)-(happy|love|sad|sleepy|hungry|grumpy|zoomies|huh):")


def banner_text(text):
    words = lambda m: "%s: %s" % (m.group(1).title(), m.group(2))
    whole = _EMOJI.fullmatch(text.strip())
    if whole:
        return words(whole)
    return _EMOJI.sub(lambda m: "(%s)" % words(m), text)


POLL_MAX = 3


def notify_poll(args, user):
    """Chat's part of /notify/poll (server/msg.py): the last message's id,
    which the poll's first line carries, and -- for a device that says where
    it is (?chat=ID&me=NAME) -- up to three newer messages from others, as
    "chat <tab> name <tab> text". With no chat id (a device that has never
    asked) it is only told where the room is, so its first poll is not every
    message ever sent."""
    try:
        sid = int((args.get("chat") or ["-1"])[0])
    except ValueError:
        sid = -1
    me = _flat((args.get("me") or [""])[0], NAME_MAX).lower()
    msgs = load()
    last = msgs[-1]["id"] if msgs else 0
    lines = []
    if sid >= 0:
        new = [m for m in msgs if m["id"] > sid and m["name"].lower() != me][-POLL_MAX:]
        lines = ["chat\t%s\t%s\n" % (m["name"], banner_text(m["text"])) for m in new]
    return last, lines


ROUTES = [
    ("GET", "/msg", get_msg, "device_or_dash"),
    ("POST", "/msg", post_msg, "device_or_dash"),
    ("POST", "/share", post_share, "device_or_dash"),
]
