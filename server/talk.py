"""Talking to Claude about one document, from whichever app has it open.

Edit's ctrl-k, MIDI's `c`: the app hands its file to the Claude app, which
uploads it here and then carries a conversation about it. Claude may answer
in words, or with the whole document revised; a revision is kept here and
the device fetches it only when the person says to save it. Nothing in this
conversation can touch the repository -- it is Claude with no tools, unlike
/chat.

    POST /talk/start?name=NAME&kind=KIND   body: the document -> session id
    POST /talk/say?s=ID                    body: what was said -> "ok"
    GET  /talk/poll?s=ID                   -> "pending" | "reply" or "reply rev",
                                              then the words | "error why"
    GET  /talk/doc?s=ID                    -> the latest revision

`kind` is `song` for MIDI's songs (apps/midiseq.h), whose revisions are
checked like server/midi.py's and sent back to be mended once; anything
else is text.

Short calls, as everywhere the device is concerned: the shell is one loop,
and a minute's wait is a frozen screen.
"""
import json
import re
import secrets
import subprocess
import sys
import threading

from . import jobs, midi

DOC_MAX = 24000
SAY_MAX = 2000
IDLE_S = 3 * 3600

PROMPT = """You are helping someone with one document on a tiny handheld computer:
a 40-column screen and a small keyboard. They will ask about it or ask for
changes.

Answer in plain sentences, short -- a few lines that fit the screen. No
markdown headings, tables or bold.

When they ask you to change the document, say in one or two sentences what
you changed, then give the WHOLE revised document in one fenced block that
starts with ```doc and ends with ```. Do not abbreviate it or leave anything
out: it replaces the file. When they only ask a question, do not include the
document.
"""

SONG = """
The document is a song for a small MIDI sequencer, in this format:
""" + midi.FORMAT.split("Example:")[0].split("\n", 2)[2]

_sessions = jobs.Table(IDLE_S)  # id -> Session, gone after IDLE_S untouched
_lock = threading.Lock()         # a Session's fields, between a request and its thread
_FENCE = re.compile(r"```doc[^\n]*\n(.*?)```", re.S)


class Session:
    def __init__(self, name, kind, doc):
        self.name = name
        self.kind = kind
        self.doc = doc
        self.revision = None          # the latest revised document
        self.sid = None               # Claude Code's session, once there is one
        self.state = "idle"           # idle | pending | reply | error
        self.answer = ""
        self.rev_new = False


def _claude(chat, prompt, sid):
    if not chat or not chat.claude:
        raise RuntimeError("this server runs without Claude")
    cmd = [chat.claude, "-p", prompt, "--output-format", "json",
           "--allowed-tools", "", "--permission-mode", "dontAsk"]
    if sid:
        cmd += ["--resume", sid]
    r = subprocess.run(cmd, cwd=chat.cwd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=300, env=chat._child_env())
    j = json.loads(r.stdout or "{}")
    if j.get("is_error") or not j.get("result"):
        raise RuntimeError((j.get("result") or r.stderr or "Claude did not answer")[:160])
    return j["result"], j.get("session_id") or sid


def split(answer):
    """(words, revision or None): the fenced document taken out of the
    answer, and a line in its place."""
    m = _FENCE.search(answer or "")
    if not m:
        return (answer or "").strip(), None
    words = (answer[:m.start()] + answer[m.end():]).strip()
    return words, m.group(1).rstrip("\n") + "\n"


def turn(chat, s, said):
    """One exchange: what Claude said, and the revision if it made one."""
    if s.sid:
        prompt = said
    else:
        prompt = (PROMPT + (SONG if s.kind == "song" else "") +
                  "\nThe document, %s:\n```\n%s```\n\n%s" % (s.name, s.doc, said))
    answer, s.sid = _claude(chat, prompt, s.sid)
    words, rev = split(answer)
    if rev is not None and s.kind == "song":
        bad = midi.check(rev)
        if bad:
            answer, s.sid = _claude(chat, "That song has a mistake on line %d (%s). "
                                    "Give the whole song again, corrected, in a ```doc block." % bad, s.sid)
            _, rev = split(answer)
            if rev is None or midi.check(rev):
                return words + "\n(the revision had a mistake, so it was not kept)", None
    return words, rev


def _run(chat, s, said):
    try:
        words, rev = turn(chat, s, said)
        with _lock:
            if rev is not None:
                s.revision = rev
                s.doc = rev
            s.rev_new = rev is not None
            s.answer = words or ("(revised)" if rev else "(no answer)")
            s.state = "reply"
    except Exception as e:                         # subprocess, JSON
        with _lock:
            s.answer = str(e)
            s.state = "error"
    sys.stderr.write("talk: %s %s\n" % (s.name, s.state))


def _session(args):
    return _sessions.get((args.get("s") or [""])[0], touch=True)


def post_start(h, path, args):
    """a document to talk about; the session id"""
    doc = h.body(DOC_MAX).decode("utf-8", "replace")
    name = ((args.get("name") or ["document"])[0] or "document")[:80]
    kind = (args.get("kind") or ["text"])[0]
    sid = secrets.token_hex(5)
    _sessions.put(sid, Session(name, kind, doc))
    h.text(sid + "\n")


def post_say(h, path, args):
    """something said about the document; poll for the answer"""
    s = _session(args)
    if not s:
        h.text("error that conversation is over\n", 404)
        return
    said = h.body(SAY_MAX).decode("utf-8", "replace").strip()
    if not said:
        h.text("error say something\n", 400)
        return
    with _lock:
        if s.state == "pending":
            h.text("error still answering the last one\n", 409)
            return
        s.state = "pending"
    threading.Thread(target=_run, args=(h.chat, s, said), daemon=True).start()
    h.text("ok\n")


def get_poll(h, path, args):
    """the answer, once there is one"""
    s = _session(args)
    if not s:
        h.text("error that conversation is over\n", 404)
        return
    with _lock:
        if s.state == "pending":
            h.text("pending\n")
        elif s.state == "reply":
            h.text(("reply rev\n" if s.rev_new else "reply\n") + s.answer + "\n")
            s.state = "idle"
        elif s.state == "error":
            h.text("error %s\n" % s.answer.replace("\n", " "))
            s.state = "idle"
        else:
            h.text("idle\n")


def get_doc(h, path, args):
    """the latest revision of the document"""
    s = _session(args)
    if not s or s.revision is None:
        h.text("error no revision\n", 404)
        return
    h.text(s.revision)


ROUTES = [
    ("POST", "/talk/start", post_start, "device_or_dash"),
    ("POST", "/talk/say", post_say, "device_or_dash"),
    ("GET", "/talk/poll", get_poll, "device_or_dash"),
    ("GET", "/talk/doc", get_doc, "device_or_dash"),
]
