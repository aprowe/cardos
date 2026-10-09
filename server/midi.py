"""Songs for the MIDI sequencer, written by Claude.

The device's MIDI app (apps/midi.c) plays a small text format -- notes at
beats, controller changes and ramps, a tempo, a loop -- that apps/midiseq.h
parses. Asked "the first section of Fur Elise", Claude writes one. It is a
minute's work, so it is three short calls, as chat is: POST the request and
get an id, GET until the song is there.

    POST /midi/compose          body: what to write -> id
                                or: what to change, a line ---song---, the song
    GET  /midi/compose?id=ID    -> "pending" | "ok" then the song | "error why"

What comes back is checked by check() below, a reading of the same format
as midiseq.h; a song with a mistake goes back to Claude once with the line
and the reason, rather than to the device to fail there.
"""
import re
import secrets
import sys
import threading

from . import jobs


FORMAT = r"""You write songs for a tiny MIDI sequencer. Output ONLY the song, in this
exact text format, inside one ```song fenced block, and nothing else.

Format, one command a line:
  # Title                      first line: a short title
  tempo 72                     beats per minute, a whole number 20..400, once
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
_jobs = jobs.Table(1800)       # id -> (state, song or why); gone half an hour after


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
        elif cmd == "tempo" and not (w[1].isdigit() and 20 <= int(w[1]) <= 400):
            return i, "tempo is a whole number of beats a minute, 20..400"
        elif cmd == "ch" and not (w[1].isdigit() and 1 <= int(w[1]) <= 16):
            return i, "ch is 1..16"
    if "\nn " not in "\n" + song:
        return 1, "no notes at all"
    return None


def extract(text):
    m = re.search(r"```(?:song)?\s*\n(.*?)```", text or "", re.S)
    return (m.group(1) if m else (text or "")).strip() + "\n"


def _claude(chat, prompt):
    from .chat import ask_once
    return ask_once(chat, prompt, 240)[0]


SONG_MARK = "---song---"


def compose(chat, request):
    """The song for `request`, checked, with one chance to mend it. A request
    with a song after SONG_MARK is a change to that song."""
    if SONG_MARK in request:
        ask, old = request.split(SONG_MARK, 1)
        prompt = (FORMAT + "\nHere is a song:\n```song\n" + old.strip() + "\n```\n"
                  "Change it: " + ask.strip() + "\nKeep everything not asked about the same, "
                  "and return the whole song.")
    else:
        prompt = FORMAT + "\nWrite: " + request
    song = extract(_claude(chat, prompt))
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
    except Exception as e:                      # Claude, the check
        result = ("error", str(e))
    _jobs.replace(jid, result)
    sys.stderr.write("midi: %s %s\n" % (jid, result[0]))


def post_compose(h, path, args):
    """ask Claude for a song; the id to ask after"""
    request = h.body(16000).decode("utf-8", "replace").strip()
    if not request:
        h.text("error what should it write?\n", 400)
        return
    jid = secrets.token_hex(5)
    _jobs.put(jid, ("pending", ""))
    threading.Thread(target=_run, args=(jid, h.chat, request), daemon=True).start()
    h.text(jid + "\n")


def get_compose(h, path, args):
    """the song, once it is written"""
    jid = (args.get("id") or [""])[0]
    job = _jobs.get(jid)
    if not job:
        h.text("error no such request\n", 404)
        return
    state, body = job
    if state == "pending":
        h.text("pending\n")
    elif state == "ok":
        h.text("ok\n" + body)
    else:
        h.text("error %s\n" % body)


ROUTES = [
    ("POST", "/midi/compose", post_compose, "device_or_dash"),
    ("GET", "/midi/compose", get_compose, "device_or_dash"),
]
