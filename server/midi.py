"""Songs for the MIDI sequencer, written by Claude.

The device's MIDI app (apps/midi.c) plays a small text format -- notes at
beats, controller changes and ramps, a tempo, a loop -- that apps/midiseq.h
parses. Asked "the first section of Fur Elise", Claude writes one. It is a
minute's work, so it is three short calls, as chat is: POST the request and
get an id, GET until the song is there.

    POST /midi/compose          body: what to write -> id
    GET  /midi/compose?id=ID    -> "pending" | "ok" then the song | "error why"

What comes back is checked by check() below, a reading of the same format
as midiseq.h; a song with a mistake goes back to Claude once with the line
and the reason, rather than to the device to fail there.
"""
import json
import re
import secrets
import subprocess
import sys
import threading
import time

from . import notes

FORMAT = r"""You write songs for a tiny MIDI sequencer. Output ONLY the song, in this
exact text format, inside one ```song fenced block, and nothing else.

Format, one command a line:
  # Title                      first line: a short title
  tempo 72                     beats per minute (20..400), once
  ch 1                         channel 1..16 for the lines that follow (drums: 10)
  prog 0                       General MIDI program 0..127 on the current channel (optional: prog 0 at BEAT)
  n BEAT NOTE LEN [VEL]        a note: BEAT and LEN in beats (decimals like 0.25, or fractions like 1/3), NOTE
                               a name (C4 = middle C, sharps #, flats b, octaves -1..9) or a MIDI number;
                               chords as NOTE,NOTE,NOTE; VEL 1..127, default 90
  cc BEAT CTRL VALUE           a controller change (0..127): 7 volume, 10 pan, 11 expression, 64 sustain
                               (127 on, 0 off), 1 modulation, 74 brightness
  ramp FROM TO CTRL START END  a controller moving smoothly from START to END between beats FROM and TO
  bend BEAT VALUE              pitch bend, -8192..8191 (0 is centre)
  loop                         repeat the whole song (or `loop BEATS` for a length)
  ; a comment                  anything after ; is ignored

Rules: BEAT is the absolute position from the start of the song in beats, not
relative to the previous note. A quarter note is 1 beat; an eighth 0.5; a
sixteenth 0.25; a triplet eighth 1/3. In 3/8 time, a bar of three eighths is
1.5 beats. Write every note out explicitly -- no repeats or variables. Keep it
musically correct: the right notes, rhythm and both hands where the piece has
them (use separate channels or the same channel with chords). At most 350
notes. Prefer a piano (prog 0) unless asked otherwise.

Example:
```song
# C major arpeggio
tempo 100
prog 0
cc 0 7 100
n 0 C4 0.5
n 0.5 E4 0.5
n 1 G4 0.5
n 1.5 C5 1.5 100
n 0 C3,G3 2 70
loop
```
"""

COMMANDS = {"tempo", "ch", "prog", "n", "cc", "ramp", "bend", "loop"}
_PITCH = re.compile(r"^(?:\d{1,3}|[A-Ga-g][#b]?-?\d)$")
_BEAT = re.compile(r"^\d+(?:\.\d+)?(?:/\d+)?$")
_jobs = {}
_lock = threading.Lock()


def check(song):
    """(line, why) for the first thing apps/midiseq.h would refuse, or None."""
    for i, raw in enumerate(song.splitlines(), 1):
        line = raw.split(";", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        w = line.split()
        cmd = w[0]
        if cmd not in COMMANDS:
            return i, "unknown command %r" % cmd
        if cmd == "n":
            if len(w) < 4:
                return i, "n needs BEAT NOTE LEN"
            if not _BEAT.match(w[1]) or not _BEAT.match(w[3]):
                return i, "BEAT and LEN are beats like 0.5 or 1/3"
            for p in w[2].split(","):
                if not _PITCH.match(p):
                    return i, "what note is %r" % p
        elif cmd in ("cc", "ramp", "bend", "prog", "tempo", "ch") and len(w) < 2:
            return i, "%s needs values" % cmd
    if "\nn " not in "\n" + song:
        return 1, "no notes at all"
    return None


def extract(text):
    m = re.search(r"```(?:song)?\s*\n(.*?)```", text or "", re.S)
    return (m.group(1) if m else (text or "")).strip() + "\n"


def _claude(chat, prompt):
    if not chat or not chat.claude:
        raise RuntimeError("this server runs without Claude")
    cmd = [chat.claude, "-p", prompt, "--output-format", "json",
           "--allowed-tools", "", "--permission-mode", "dontAsk"]
    r = subprocess.run(cmd, cwd=chat.cwd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=240, env=chat._child_env())
    j = json.loads(r.stdout or "{}")
    if j.get("is_error") or not j.get("result"):
        raise RuntimeError((j.get("result") or r.stderr or "Claude did not answer")[:160])
    return j["result"]


def compose(chat, request):
    """The song for `request`, checked, with one chance to mend it."""
    song = extract(_claude(chat, FORMAT + "\nWrite: " + request))
    bad = check(song)
    if bad:
        song = extract(_claude(chat, FORMAT + "\nThis song has a mistake on line %d (%s). "
                               "Return the whole song corrected.\n```song\n%s```" % (bad[0], bad[1], song)))
        bad = check(song)
        if bad:
            raise RuntimeError("line %d: %s" % bad)
    return song


def _run(jid, chat, request):
    try:
        song = compose(chat, request)
        result = ("ok", song)
    except Exception as e:                      # subprocess, JSON, the check
        result = ("error", str(e))
    with _lock:
        _jobs[jid] = (time.time(),) + result
    sys.stderr.write("midi: %s %s\n" % (jid, result[0]))


def post_compose(h, path, args):
    """ask Claude for a song; the id to ask after"""
    if not notes._allowed(h):
        return
    request = h.body(2000).decode("utf-8", "replace").strip()
    if not request:
        h.text("error what should it write?\n", 400)
        return
    jid = secrets.token_hex(5)
    now = time.time()
    with _lock:
        for k in [k for k, v in _jobs.items() if now - v[0] > 1800]:
            del _jobs[k]
        _jobs[jid] = (now, "pending", "")
    threading.Thread(target=_run, args=(jid, h.chat, request), daemon=True).start()
    h.text(jid + "\n")


def get_compose(h, path, args):
    """the song, once it is written"""
    if not notes._allowed(h):
        return
    jid = (args.get("id") or [""])[0]
    with _lock:
        job = _jobs.get(jid)
    if not job:
        h.text("error no such request\n", 404)
        return
    _, state, body = job
    if state == "pending":
        h.text("pending\n")
    elif state == "ok":
        h.text("ok\n" + body)
    else:
        h.text("error %s\n" % body)


ROUTES = [
    ("POST", "/midi/compose", post_compose, "open"),
    ("GET", "/midi/compose", get_compose, "open"),
]
