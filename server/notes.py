"""Notes, kept on the server: written on the dashboard or the device.

Each note is a JSON file in CARDOS_STATE/notes, named by its id. The title
is its first line, so nothing else needs editing. The device (apps/notes.c)
keeps each as a .md file on its card and syncs by hash: FNV-1a 32 of the
UTF-8 text, the same function the device computes, so a note whose hash
is unchanged on both sides needs no transfer at all.

    device (bearer) and dashboard (cookie) alike:
    GET    /notes               id <tab> hash <tab> updated <tab> title, a line a note
    GET    /notes/note?id=      the text
    POST   /notes/note[?id=]    body: the text -> id <tab> hash (no id: a new note)
    DELETE /notes/note?id=
    POST   /notes/audio         body: a WAV -> id <tab> hash <tab> title, transcribed
                                (a voice memo made into a note)
    GET    /dash/notes          the same list as JSON, for the dashboard

A save with no text deletes nothing -- an empty note is still a note. Last
write wins: the device keeps a conflicting copy of its own rather than
asking the server to merge.
"""
import hmac
import json
import os
import re
import secrets
import sys
import threading
import time

from . import dash

MAX_TEXT = 64 * 1024
_lock = threading.Lock()


def notes_dir():
    return os.path.join(dash.state_dir(), "notes")


def fnv(text):
    h = 2166136261
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return "%08x" % h


def title_of(text):
    for line in text.splitlines():
        t = line.strip().lstrip("#").strip()
        if t:
            return " ".join(t.split())[:60]
    return "Untitled"


def _path(nid):
    if not nid or not all(c.isalnum() for c in nid) or len(nid) > 16:
        raise ValueError("not a note id")
    return os.path.join(notes_dir(), nid + ".json")


def load(nid):
    try:
        with open(_path(nid), encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        return None


def save(nid, text):
    os.makedirs(notes_dir(), mode=0o700, exist_ok=True)
    now = int(time.time())
    with _lock:
        old = load(nid) if nid else None
        if not nid:
            nid = secrets.token_hex(5)
        n = {"id": nid, "text": text, "title": title_of(text), "hash": fnv(text),
             "updated": now, "created": (old or {}).get("created", now)}
        tmp = _path(nid) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(n, f)
        os.replace(tmp, _path(nid))
    return n


def delete(nid):
    try:
        os.remove(_path(nid))
        return True
    except OSError:
        return False


def all_notes():
    out = []
    try:
        names = os.listdir(notes_dir())
    except OSError:
        return out
    for name in names:
        if name.endswith(".json"):
            n = load(name[:-5])
            if n:
                out.append(n)
    out.sort(key=lambda n: -n["updated"])
    return out


# ---- routes ------------------------------------------------------------------------

def _allowed(h):
    """The device's bearer or the dashboard's cookie; a 403 otherwise."""
    if dash.logged_in(h):
        return True
    tok = h.chat.token if h.chat else None
    if not tok:
        return True
    auth = h.headers.get("Authorization", "")
    if auth.startswith("Bearer ") and hmac.compare_digest(tok, auth[7:]):
        return True
    h.text("error signed out\n", 403)
    return False


def _route(fn):
    def wrapped(h, path, args):
        if not _allowed(h):
            return
        try:
            fn(h, args)
        except ValueError as e:
            h.text("error %s\n" % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _id(args):
    return (args.get("id") or [""])[0]


@_route
def get_list(h, args):
    """every note: id, hash, updated, title"""
    h.text("".join("%s\t%s\t%d\t%s\n" % (n["id"], n["hash"], n["updated"], n["title"])
                   for n in all_notes()))


@_route
def get_note(h, args):
    """one note's text"""
    n = load(_id(args))
    if not n:
        h.text("error no such note\n", 404)
        return
    h.text(n["text"])


@_route
def post_note(h, args):
    """save a note's text; no id makes a new one"""
    text = h.body(MAX_TEXT).decode("utf-8", "replace")
    nid = _id(args)
    if nid and not load(nid):
        h.text("error no such note\n", 404)
        return
    n = save(nid or None, text)
    h.text("%s\t%s\n" % (n["id"], n["hash"]))


@_route
def delete_note(h, args):
    """delete a note"""
    if not delete(_id(args)):
        h.text("error no such note\n", 404)
        return
    h.text("ok\n")


MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()


def memo_title(name):
    """"Voice memo, 1 Oct 16:48" from Memo's file name (MMDD-HHMMSS.wav, the
    device's own clock), or "Voice memo, NAME" for one named by count."""
    stem = os.path.basename(name or "").rsplit(".", 1)[0]
    m = re.fullmatch(r"(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)?", stem)
    if m and 1 <= int(m.group(1)) <= 12:
        return "Voice memo, %d %s %s:%s" % (int(m.group(2)), MONTHS[int(m.group(1)) - 1],
                                            m.group(3), m.group(4))
    return "Voice memo, " + stem if stem else "Voice memo"


@_route
def post_audio(h, args):
    """a voice memo, transcribed into a new note"""
    wav = h.body(16 << 20)
    if len(wav) <= 44:
        raise ValueError("nothing recorded")
    if not h.voice:
        h.text("error this server has no transcription\n", 503)
        return
    text, err = h.voice.transcribe(wav)
    if err:
        h.text("error %s\n" % err, 502)
        return
    text = (text or "").strip() or "(nothing heard)"
    # A heading, then the words. The words alone made a one-line note, and a
    # note's first line is its title: a sentence of memo arrived as a title
    # over an empty page.
    n = save(None, "# %s\n\n%s\n" % (memo_title((args.get("name") or [""])[0]), text))
    sys.stderr.write("notes: memo -> %s\n" % n["id"])
    h.text("%s\t%s\t%s\n" % (n["id"], n["hash"], n["title"]))


def get_dash_list(h, path, args):
    """every note, as JSON, for the dashboard"""
    if not dash.logged_in(h):
        h._send(403, "application/json", '{"error": "signed out"}', ())
        return
    h._send(200, "application/json", json.dumps(
        [{"id": n["id"], "title": n["title"], "updated": n["updated"]} for n in all_notes()]),
        (("Cache-Control", "no-store"),))


ROUTES = [
    ("GET", "/notes", get_list, "open"),
    ("GET", "/notes/note", get_note, "open"),
    ("POST", "/notes/note", post_note, "open"),
    ("DELETE", "/notes/note", delete_note, "open"),
    ("POST", "/notes/audio", post_audio, "open"),
    ("GET", "/dash/notes", get_dash_list, "open"),
]
