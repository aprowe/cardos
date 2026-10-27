"""Crosswords for the device: a library of puzzles kept here, served in the
compact line format apps/crossword/xwcore.h reads.

    GET /crossword/list[?from=&to=&weekday=&order=&after=&limit=]
        one puzzle a line: id <tab> date <tab> weekday <tab> WxH <tab> title
        Newest first (order=old for the other way), so the app can page back
        through thousands of old puzzles without one reply holding them all.
          from=, to=   a date, whole or part: 1993, 1993-11, 1993-11-21; from
                       means the first day it names, to the last (inclusive)
          weekday=     mon..sun or 0..6 (0 is Monday), several with commas:
                       sat,sun
          after=ID     the last id of the page before; the next page starts
                       after it in the order asked for
          limit=       lines to a page, 50 unless asked, 80 at most (each line
                       is at most about 75 bytes; the device's buffer is 8 KB)
        An empty reply is the end. A puzzle's id is its print date.
    GET /crossword/puzzle?id=ID
        the puzzle itself, in the format below

The puzzle text is lines (the device parses it a line at a time, with no
JSON, so this is what it gets):

    xw 1                    the format and its version; first
    title TEXT              optional; so are by, date and note. Text to the end
    by TEXT                   of the line, ASCII (the font has nothing else)
    date 1993-11-21
    note TEXT
    size W H                at most 21 by 21
    row CELLS               H of them, W characters each: . is a black square,
                            A-Z a white one with its answer, a-z the same in
                            a circled square
    A N TEXT               the across clues, in number order, then the down
    D N TEXT                 ones; N is the clue's number, which is the usual
                             one: squares that start a word of two or more
                             squares, numbered in reading order
    end                     last; a text without it was cut off

Every square is one letter. A rebus square keeps its first letter (the device
has no way to type more), and clue text has its markup taken out and its
accents and symbols made plain. `parse_text` refuses what the device's own
loader refuses -- more than 21 by 21, more than 240 entries, more than 8 KB
of clue text, a square in no word, a clue that is not the grid's next --
so nothing is stored that the device could not open.

The NYT Midi is fetched by the server itself, from nytmidi.org, every hour
it runs: today's and the six days before, as DATE-midi, older ones deleted
(`midi_refresh`; `python -m server.crossword midi` runs it once).

Puzzles are kept as CARDOS_STATE/crosswords/ID.xw, that very text. They are
shared by everyone who can reach the server, not one person's. `add` stores
one; `convert` reads the two kinds of file puzzles are found in:

  .puz   Across Lite's binary file. Clues are kept in number order, across
         before down; circled squares come from the GEXT section. Checksums
         are not checked. A scrambled one (its solution locked) is refused.
  .json  as the NYT's own site serves it (a "body" holding "cells" and
         "clues"), or as XWord Info and the archives made from it write it
         ("grid", "size", "clues": {"across": ["1. Clue"], "down": [...]},
         "circles"). The clue numbers are checked against the grid's.

A puzzle has to say when it was printed, since that is its id: in a field
of the file, in a title like "NY Times, Sunday, November 21, 1993", or in
the file's name (1993-11-21.json, 1993/11/21.json, Nov2193.puz). The
weekday is checked as well: a file that says it is a Sunday (a "dow" field,
or "Sunday" in a title like that one) but whose date was not one is refused,
because a date wrongly read from a file name would otherwise put a Monday
puzzle among the Saturdays, and the app lists by weekday. `import_file` and
`import_bytes` do one file: read, convert, store, and say whether the puzzle
was new or already here (a puzzle already here is left alone).

Putting the old puzzles in. Nothing here fetches them -- the Times' puzzles
are the Times' to give, so use files you are entitled to have, such as the
.puz downloads of a subscription's archive -- and the agent that wrote this
cannot download them either. So: copy them, as they came, onto the machine
the server runs on, into any folder (subfolders and .zip archives are read
too, and the dates may be in the files or in their names below that folder),
then, as the user the server runs as and with the server's CARDOS_STATE
(`--state DIR` says it), run

    python -m server.crossword import ~/nyt-puzzles --dry-run     look first
    python -m server.crossword import ~/nyt-puzzles
    python tools/crossword_import.py ~/nyt-puzzles                the same, from a checkout

Each puzzle goes into CARDOS_STATE/crosswords under its print date; one
already there is skipped (--replace puts the new one over it), a file that is
not a puzzle is listed with the reason and the rest carry on, and the exit
status is 1 if any could not be read. The running server notices new files
by itself; it need not be restarted, and the device sees them at its next
sync of the list.
"""
import calendar
import collections
import datetime
import html
import json
import os
import re
import struct
import sys
import tempfile
import threading
import zipfile
import zlib

from . import dash, wiki, wire
from .routes import arg, text_route

# What the device can hold: apps/crossword/xwcore.h, same numbers.
MAX_W = MAX_H = 21
MAX_CLUES = 240
POOL_MAX = 8192                 # bytes of clue text, a NUL after each
CLUE_MAX = 200
TITLE_MAX, BY_MAX, NOTE_MAX = 63, 47, 159

MAX_IN = 2 << 20                # the biggest file taken for a puzzle
LIST_DEFAULT, LIST_MAX = 50, 80
LIST_TITLE = 40                 # a title is cut to this in the list
DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
PUZ_MAGIC = b"ACROSS&DOWN\x00"

_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,39}$")
_ISO = re.compile(r"^\d{4}-\d{2}-\d{2}$")


# ---- text the device can draw -------------------------------------------------------

_MARKUP = re.compile(r"</?(?:i|b|u|em|strong|sup|sub|br|span|font|p|a)\b[^>]*>", re.I)

_GREEK = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
          "omicron pi rho sigma tau upsilon phi chi psi omega").split()
_SYMBOLS = {
    "♠": "spade", "♣": "club", "♥": "heart", "♦": "diamond",
    "→": "->", "←": "<-", "↑": "up arrow", "↓": "down arrow", "↔": "<->",
    "∞": "infinity", "√": "root", "≠": "!=", "≤": "<=", "≥": ">=", "÷": "/",
    "™": "TM", "©": "(c)", "®": "(R)", "☺": ":)", "☹": ":(", "★": "star", "☆": "star",
    "♪": "note", "♫": "notes", "¢": "cents", "§": "section", "¶": "paragraph",
    "†": "dagger", "‡": "double dagger", "ς": "sigma",
}
_SYMBOLS.update(zip("αβγδεζηθικλμνξοπρστυφχψω", _GREEK))
_SYMBOLS.update(zip("ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ", (n.capitalize() for n in _GREEK)))


def plain(s, n):
    """One line of ASCII for the device, at most n characters: markup taken out
    (the NYT's clues carry <i>), entities undone, every kind of space a
    space, symbols given their names, accents taken off."""
    s = _MARKUP.sub("", str(s if s is not None else ""))
    s = _MARKUP.sub("", html.unescape(s))      # an escaped tag is a tag too
    s = "".join(" " if c.isspace() else _SYMBOLS.get(c, c) for c in s)
    return wiki.ascii_text(s)[:n].rstrip()


def clean_clue(s):
    return plain(s, CLUE_MAX) or "(no clue)"


# ---- dates ------------------------------------------------------------------------------

_MONTHS = {m: i + 1 for i, m in enumerate(("jan", "feb", "mar", "apr", "may", "jun",
                                           "jul", "aug", "sep", "oct", "nov", "dec"))}
_WORDY = re.compile(r"\b(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\.?\s+"
                    r"(\d{1,2})(?:st|nd|rd|th)?,?\s+(\d{4})\b", re.I)


def _iso(y, m, d):
    """YYYY-MM-DD for a real date from 1900 to 2200, else ''."""
    if not 1900 <= y <= 2200:
        return ""
    try:
        datetime.date(y, m, d)
    except ValueError:
        return ""
    return "%04d-%02d-%02d" % (y, m, d)


def parse_date(s):
    """YYYY-MM-DD from the ways a puzzle says its date -- 1993-11-21 (a time
    after it is ignored), 11/21/1993, "Sunday, November 21, 1993" -- or ''."""
    s = str(s if s is not None else "").strip()
    m = re.match(r"(\d{4})-(\d{1,2})-(\d{1,2})(?!\d)", s)
    if m:
        return _iso(*(int(g) for g in m.groups()))
    m = re.match(r"(\d{1,2})/(\d{1,2})/(\d{4})$", s)
    if m:
        return _iso(int(m.group(3)), int(m.group(1)), int(m.group(2)))
    m = _WORDY.search(s)
    if m:
        return _iso(int(m.group(3)), _MONTHS[m.group(1).lower()], int(m.group(2)))
    return ""


def date_from_name(name):
    """A date in a file's path: 1993-11-21.json, 1993/11/21.json, 19931121.puz,
    Nov2193.puz (the NYT's own file names: month, day, two-digit year). The
    last one in the path, since a folder may be dated and a file more so."""
    s = str(name or "").replace("\\", "/")
    found = ""
    for m in re.finditer(r"(?<!\d)(\d{4})[-_./]?(\d{2})[-_./]?(\d{2})(?!\d)", s):
        found = _iso(int(m.group(1)), int(m.group(2)), int(m.group(3))) or found
    if found:
        return found
    for m in re.finditer(r"([A-Za-z]{3})(\d{2})(\d{2})(?!\d)", s):
        mon = _MONTHS.get(m.group(1).lower())
        if mon:
            yy = int(m.group(3))
            found = _iso(1900 + yy if yy >= 42 else 2000 + yy, mon, int(m.group(2))) or found
    return found


_WHO = r"(?:(?:the\s+)?new\s+york\s+times|n\.?\s?y\.?\s?times|nyt)\b"
_BOILER = re.compile(
    r"^\s*" + _WHO + r"[\s,:;.\-]*"
    r"(?:(?:mon|tue|wed|thu|fri|sat|sun)[a-z]*\.?[\s,:;\-]*)?"
    r"(?:(?:jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\.?\s+\d{1,2}"
    r"(?:st|nd|rd|th)?,?\s+\d{4}|\d{1,2}/\d{1,2}/\d{2,4}|\d{4}-\d{2}-\d{2})?[\s,:;.\-]*", re.I)

FULL_DAYS = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
_DAYWORD = (r"(mon(?:day)?|tue(?:s(?:day)?)?|wed(?:nesday)?|thu(?:r(?:s(?:day)?)?)?|"
            r"fri(?:day)?|sat(?:urday)?|sun(?:day)?)\b")
_DOW_FIELD = re.compile(r"^\s*" + _DAYWORD + r"\.?\s*$", re.I)
_DOW_TITLE = re.compile(r"^\s*" + _WHO + r"[\s,:;.\-]*" + _DAYWORD, re.I)


def stated_weekday(title="", dow=""):
    """The weekday a file says its puzzle ran on -- 0 Monday .. 6 Sunday -- or
    None. From a "dow" field ("Sunday", "Sun") or a title that begins the way
    the Times' do ("NY Times, Sunday, November 21, 1993"); a title that merely
    contains a day's name ("Sunday Challenge") says nothing."""
    m = _DOW_FIELD.match(str(dow or "")) or _DOW_TITLE.match(str(title or ""))
    return [d.lower() for d in DAYS].index(m.group(1)[:3].lower()) if m else None


def clean_title(raw):
    """A title for the list. The date and weekday are columns of their own, so
    "NY Times, Sunday, November 21, 1993" is no title at all, and "... - Pair
    Dealing" is just Pair Dealing."""
    t = _BOILER.sub("", plain(raw, 400), count=1)
    return t.strip(" -:,;.")[:TITLE_MAX].rstrip()


# ---- the grid ------------------------------------------------------------------------------

def entries(rows):
    """(across, down): the entries of a grid, each (number, row, col, length),
    in number order -- the usual numbering, as apps/crossword/xwcore.h does it.
    ValueError if it cannot be a crossword (no white squares, a white square
    in no entry, more entries than the device has room for)."""
    h, w = len(rows), len(rows[0])

    def black(r, c):
        return r < 0 or c < 0 or r >= h or c >= w or rows[r][c] == "."

    across, down, num = [], [], 0
    used = [[False] * w for _ in range(h)]
    for r in range(h):
        for c in range(w):
            if black(r, c):
                continue
            sa = black(r, c - 1) and not black(r, c + 1)
            sd = black(r - 1, c) and not black(r + 1, c)
            if sa or sd:
                num += 1
            if sa:
                n = 1
                while not black(r, c + n):
                    n += 1
                across.append((num, r, c, n))
                for k in range(n):
                    used[r][c + k] = True
            if sd:
                n = 1
                while not black(r + n, c):
                    n += 1
                down.append((num, r, c, n))
                for k in range(n):
                    used[r + k][c] = True
    if not any(ch != "." for row in rows for ch in row):
        raise ValueError("the grid has no white squares")
    for r in range(h):
        for c in range(w):
            if not black(r, c) and not used[r][c]:
                raise ValueError("the square at row %d, column %d is in no word" % (r + 1, c + 1))
    if len(across) + len(down) > MAX_CLUES:
        raise ValueError("%d entries; the device has room for %d" % (len(across) + len(down), MAX_CLUES))
    return across, down


def _check_rows(rows):
    if not rows or not rows[0]:
        raise ValueError("the grid is empty")
    w = len(rows[0])
    if len(rows) > MAX_H or w > MAX_W:
        raise ValueError("the grid is %dx%d; the device takes at most %dx%d" % (w, len(rows), MAX_W, MAX_H))
    for r, row in enumerate(rows):
        if len(row) != w:
            raise ValueError("row %d is %d squares, not %d" % (r + 1, len(row), w))
        for ch in row:
            if ch != "." and not (ch.isascii() and ch.isalpha()):
                raise ValueError("a square whose answer is %r" % ch)


def make_puzzle(rows, across, down, title="", by="", note="", date="", dow=None):
    """A puzzle from its pieces, cleaned and checked to fit the device.

    rows are the grid in the text format ('.' black, a letter white, lower
    case circled); across and down are the clue texts in entry order (the
    first across entry's clue first). dow is the weekday the file says the
    puzzle ran on (0 Monday), kept in the dict for convert to check the date
    against; it is not part of the puzzle text. ValueError saying what is
    wrong."""
    rows = [str(r) for r in rows]
    _check_rows(rows)
    a, d = entries([r.upper() for r in rows])
    if len(across) != len(a) or len(down) != len(d):
        raise ValueError("%d across and %d down clues for a grid with %d and %d entries"
                         % (len(across), len(down), len(a), len(d)))
    pz = {"title": clean_title(title), "by": plain(by, BY_MAX), "note": plain(note, NOTE_MAX),
          "date": parse_date(date), "w": len(rows[0]), "h": len(rows), "rows": rows,
          "across": [(e[0], clean_clue(t)) for e, t in zip(a, across)],
          "down": [(e[0], clean_clue(t)) for e, t in zip(d, down)]}
    if dow is not None:
        pz["dow"] = dow
    parse_text(to_text(pz))               # it fits: 8 KB of clue text, say
    return pz


# ---- the text format -----------------------------------------------------------------------------

def to_text(pz):
    """The puzzle in the compact line format. ASCII, a line a field."""
    out = ["xw 1"]
    for key, field, n in (("title", "title", TITLE_MAX), ("by", "by", BY_MAX)):
        if pz.get(field):
            out.append("%s %s" % (key, wire.flat(pz[field], n, ascii=True)))
    if pz.get("date") and _ISO.match(pz["date"]):
        out.append("date " + pz["date"])
    if pz.get("note"):
        out.append("note " + wire.flat(pz["note"], NOTE_MAX, ascii=True))
    out.append("size %d %d" % (pz["w"], pz["h"]))
    out.extend("row " + r for r in pz["rows"])
    for key, clues in (("A", pz["across"]), ("D", pz["down"])):
        for num, text in clues:
            text = wire.flat(text, CLUE_MAX, ascii=True)
            out.append("%s %d%s" % (key, num, " " + text if text else ""))
    out.append("end")
    return "\n".join(out) + "\n"


def parse_text(text):
    """A puzzle's text as a dict, refusing whatever the device would. The
    dict is what make_puzzle makes: title, by, note, date, w, h, rows,
    across and down as lists of (number, text)."""
    pz = {"title": "", "by": "", "note": "", "date": "", "w": 0, "h": 0, "rows": [],
          "across": [], "down": []}
    limits = {"title": TITLE_MAX, "by": BY_MAX, "note": NOTE_MAX}
    stage = 0                   # 0 no header yet, 1 header, 2 rows, 3 clues, 4 done
    ents = None
    got = {"A": 0, "D": 0}
    pool = 0

    def numbered():
        a, d = entries([r.upper() for r in pz["rows"]])
        return {"A": a, "D": d}

    for raw in text.split("\n"):
        line = raw.rstrip(" \r")
        if not line:
            continue
        key, _, rest = line.partition(" ")
        if stage == 0:
            if key != "xw" or rest.strip() != "1":
                raise ValueError("not a puzzle: it starts %r, not 'xw 1'" % line[:20])
            stage = 1
        elif key in limits:
            pz[key] = rest[:limits[key]]
        elif key == "date":
            pz["date"] = rest.strip() if _ISO.match(rest.strip()) and parse_date(rest) else ""
        elif key == "size":
            m = re.match(r"(\d+) (\d+)$", rest.strip())
            if stage != 1 or not m or not (1 <= int(m.group(1)) <= MAX_W and 1 <= int(m.group(2)) <= MAX_H):
                raise ValueError("a bad size line %r (once, at most %dx%d)" % (line[:20], MAX_W, MAX_H))
            pz["w"], pz["h"] = int(m.group(1)), int(m.group(2))
            stage = 2
        elif key == "row":
            if stage == 1:
                raise ValueError("a row before the size")
            if stage != 2 or len(pz["rows"]) >= pz["h"] or len(rest) != pz["w"]:
                raise ValueError("row %d: %r is not a row of %d squares" % (len(pz["rows"]) + 1, rest[:30], pz["w"]))
            if any(ch != "." and not (ch.isascii() and ch.isalpha()) for ch in rest):
                raise ValueError("row %d has a square that is not a letter or '.'" % (len(pz["rows"]) + 1))
            pz["rows"].append(rest)
        elif key in ("A", "D", "end"):
            if stage == 1:
                raise ValueError("clues before the size and the grid")
            if stage == 2:
                if len(pz["rows"]) != pz["h"]:
                    raise ValueError("the grid has %d rows, not %d" % (len(pz["rows"]), pz["h"]))
                ents = numbered()
                stage = 3
            if key == "end":
                if got["A"] != len(ents["A"]) or got["D"] != len(ents["D"]):
                    raise ValueError("%d across and %d down clues; the grid has %d and %d entries"
                                     % (got["A"], got["D"], len(ents["A"]), len(ents["D"])))
                stage = 4
                break
            m = re.match(r"(\d{1,9})(?: (.*))?$", rest)
            if not m:
                raise ValueError("a clue line %r with no number" % line[:20])
            num, clue = int(m.group(1)), m.group(2) or ""
            if got[key] >= len(ents[key]):
                raise ValueError("more %s clues than the grid has entries" % ("across" if key == "A" else "down"))
            if ents[key][got[key]][0] != num:
                raise ValueError("clue %d-%s is not the next one: the grid's next %s entry is %d"
                                 % (num, "Across" if key == "A" else "Down",
                                    "across" if key == "A" else "down", ents[key][got[key]][0]))
            pool += min(len(clue), CLUE_MAX) + 1
            if pool > POOL_MAX:
                raise ValueError("more than %d bytes of clue text" % POOL_MAX)
            pz["across" if key == "A" else "down"].append((num, clue[:CLUE_MAX]))
            got[key] += 1
        # anything else is a line a later version added
    if stage == 0:
        raise ValueError("not a puzzle: it has no 'xw 1' line")
    if stage != 4:
        raise ValueError("puzzle cut short: no end line")
    return pz


# ---- reading the files puzzles come in --------------------------------------------------------------

def _decode(b):
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("cp1252", "replace")


def _grid_cell(ch):
    """A square's answer as one letter, or '.' for black."""
    ch = (str(ch) if ch is not None else "")[:1]
    if ch in ("", ".", "#"):
        return "."
    if ch.isascii() and ch.isalpha():
        return ch.upper()
    raise ValueError("a square whose answer is %r" % ch)


def from_puz(data):
    """A puzzle from an Across Lite .puz file's bytes."""
    i = data.find(PUZ_MAGIC, 0, 64)
    if i < 2:
        raise ValueError("not a .puz file")
    base = i - 2
    if len(data) < base + 0x34:
        raise ValueError("a .puz file cut short")
    w, h = data[base + 0x2C], data[base + 0x2D]
    nclues, _, scrambled = struct.unpack_from("<HHH", data, base + 0x2E)
    if scrambled:
        raise ValueError("a scrambled .puz (its solution is locked)")
    if not w or not h:
        raise ValueError("a .puz with no grid")
    n = w * h
    start = base + 0x34
    if len(data) < start + 2 * n:
        raise ValueError("a .puz file cut short")
    pos = [start + 2 * n]                     # past the solution and the player's state

    def take():
        end = data.find(b"\x00", pos[0])
        if end < 0:
            raise ValueError("a .puz file cut short")
        s = _decode(data[pos[0]:end])
        pos[0] = end + 1
        return s

    title, author, _copyright = take(), take(), take()
    texts = [take() for _ in range(nclues)]
    notes = take() if pos[0] < len(data) else ""
    extras = {}
    while pos[0] + 8 <= len(data):            # GRBS, RTBL, LTIM, GEXT, ...
        name = data[pos[0]:pos[0] + 4]
        size = struct.unpack_from("<H", data, pos[0] + 4)[0]
        body = data[pos[0] + 8:pos[0] + 8 + size]
        if len(body) < size:
            break
        extras[name] = body
        pos[0] += 8 + size + 1
    gext = extras.get(b"GEXT", b"")
    rows = []
    for r in range(h):
        row = []
        for c in range(w):
            ch = _grid_cell(chr(data[start + r * w + c]))
            if ch != "." and len(gext) == n and gext[r * w + c] & 0x80:
                ch = ch.lower()               # circled
            row.append(ch)
        rows.append("".join(row))
    a, d = entries([r.upper() for r in rows])
    if len(a) + len(d) != nclues:
        raise ValueError("%d clues for a grid with %d entries" % (nclues, len(a) + len(d)))
    # Clues are kept by number, across before down.
    slots = sorted([(e[0], 0, k) for k, e in enumerate(a)] + [(e[0], 1, k) for k, e in enumerate(d)])
    across, down = [None] * len(a), [None] * len(d)
    for (_, dirn, k), text in zip(slots, texts):
        (down if dirn else across)[k] = text
    return make_puzzle(rows, across, down, title=title, by=author, note=notes,
                       date=parse_date(title), dow=stated_weekday(title))


def _numbered(items):
    """[(number or None, text)] from a list of "12. text" strings, [12, text]
    pairs or {"number": 12, "clue": text} objects."""
    out = []
    for it in items or []:
        if isinstance(it, str):
            m = re.match(r"\s*(\d+)\s*[.)]\s*(.*)$", it, re.S)
            out.append((int(m.group(1)), m.group(2)) if m else (None, it))
        elif isinstance(it, (list, tuple)) and len(it) == 2:
            out.append((int(it[0]), str(it[1])))
        elif isinstance(it, dict):
            num = it.get("number", it.get("num", it.get("label")))
            out.append((int(num) if num not in (None, "") else None,
                        str(it.get("clue", it.get("text", "")))))
        else:
            raise ValueError("a clue that is not text")
    return out


def _clue_texts(items, ents, label):
    """The texts for a list of entries, from numbered or unnumbered clues."""
    items = _numbered(items)
    if all(n is not None for n, _ in items) and items:
        by_num = {}
        for n, t in items:
            by_num[n] = t
        want = {e[0] for e in ents}
        for n in by_num:
            if n not in want:
                raise ValueError("%d-%s is not an entry of the grid" % (n, label))
        texts = []
        for e in ents:
            if e[0] not in by_num:
                raise ValueError("no clue for %d-%s" % (e[0], label))
            texts.append(by_num[e[0]])
        return texts
    if any(n is not None for n, _ in items):
        raise ValueError("%s clues are numbered only in part" % label)
    if len(items) != len(ents):
        raise ValueError("%d %s clues for %d entries" % (len(items), label, len(ents)))
    return [t for _, t in items]


def _flat_grid(grid, w, h):
    """The grid's squares in reading order, from a flat list, a list of
    rows that are lists, or a list of row strings."""
    if not isinstance(grid, list) or not grid:
        raise ValueError("the JSON has no grid")
    if len(grid) == w * h:
        return [c for c in grid]
    if len(grid) == h and all(len(r) == w for r in grid):
        return [c for r in grid for c in r]
    raise ValueError("a grid of %d squares for a %dx%d puzzle" % (len(grid), w, h))


def _from_xword_json(j):
    """XWord Info's JSON, and the archives made from it."""
    size = j.get("size") or {}
    w, h = int(size.get("cols") or 0), int(size.get("rows") or 0)
    if not w or not h:
        raise ValueError("the JSON gives no size")
    cells = _flat_grid(j["grid"], w, h)
    circles = j.get("circles") or []
    if circles and len(circles) != w * h:
        circles = []
    rows = []
    for r in range(h):
        row = []
        for c in range(w):
            ch = _grid_cell(cells[r * w + c])
            if ch != "." and circles and circles[r * w + c]:
                ch = ch.lower()
            row.append(ch)
        rows.append("".join(row))
    a, d = entries([r.upper() for r in rows])
    clues = j.get("clues") or {}
    return make_puzzle(rows, _clue_texts(clues.get("across"), a, "Across"),
                       _clue_texts(clues.get("down"), d, "Down"),
                       title=j.get("title") or "", by=j.get("author") or "",
                       note=j.get("notepad") or "",
                       date=parse_date(j.get("date")) or parse_date(j.get("title")),
                       dow=stated_weekday(j.get("title"), j.get("dow")))


def _nyt_text(t):
    if isinstance(t, list):
        return "".join(_nyt_text(p) for p in t)
    if isinstance(t, dict):
        return str(t.get("plain") or t.get("formatted") or t.get("text") or "")
    return str(t if t is not None else "")


def _from_nyt_json(j):
    """The JSON the New York Times' own crossword pages are made from."""
    body = j["body"][0]
    dim = body["dimensions"]
    w, h = int(dim["width"]), int(dim["height"])
    cells = body["cells"]
    if len(cells) != w * h:
        raise ValueError("%d cells for a %dx%d puzzle" % (len(cells), w, h))
    rows = []
    for r in range(h):
        row = []
        for c in range(w):
            cell = cells[r * w + c] or {}
            ch = _grid_cell(cell.get("answer"))
            if ch != "." and cell.get("type") == 2:
                ch = ch.lower()                # a circle
            row.append(ch)
        rows.append("".join(row))
    a, d = entries([r.upper() for r in rows])
    lists = body.get("clueLists") or []
    by_dir = {"across": {}, "down": {}}
    for c in body["clues"]:
        direction = c.get("direction")
        if not direction and isinstance(c.get("list"), int) and c["list"] < len(lists):
            direction = lists[c["list"]].get("name")
        direction = str(direction or "").lower()
        if direction not in by_dir:
            raise ValueError("a clue that is neither across nor down")
        by_dir[direction][int(c["label"])] = _nyt_text(c.get("text"))
    across = _clue_texts(sorted(by_dir["across"].items()), a, "Across")
    down = _clue_texts(sorted(by_dir["down"].items()), d, "Down")
    notes = j.get("notes") or body.get("notes") or ""
    if isinstance(notes, list):
        notes = " ".join(_nyt_text(n) for n in notes)
    by = j.get("constructors") or j.get("author") or ""
    if isinstance(by, list):
        by = ", ".join(str(b) for b in by)
    return make_puzzle(rows, across, down, title=j.get("title") or "", by=by, note=notes,
                       date=parse_date(j.get("publicationDate")) or parse_date(j.get("title")),
                       dow=stated_weekday(j.get("title"), j.get("dayOfWeek") or j.get("dow")))


def from_json(j):
    """A puzzle from parsed JSON of either shape."""
    if not isinstance(j, dict):
        raise ValueError("a JSON crossword is an object")
    if isinstance(j.get("body"), list) and j["body"]:
        return _from_nyt_json(j)
    if "grid" in j:
        return _from_xword_json(j)
    raise ValueError("JSON that is not a crossword this server reads (no body or grid)")


def convert(data, name="", date=""):
    """A puzzle from a file's bytes -- .puz or JSON, told by what is in
    them, not the name -- with its print date: `date` if given, else the
    file's own, else one in the name. A date that was not given but found
    must agree with any weekday the file states. ValueError saying why not."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    if len(data) > MAX_IN:
        raise ValueError("a file of %d KB is not a crossword" % (len(data) // 1024))
    if data.find(PUZ_MAGIC, 0, 64) >= 2:
        pz = from_puz(data)
    else:
        if data.lstrip(b"\xef\xbb\xbf \t\r\n")[:1] != b"{":
            raise ValueError("neither a .puz file nor a crossword in JSON")
        try:
            obj = json.loads(data.decode("utf-8-sig"))
        except ValueError as e:
            raise ValueError("JSON that does not parse (%s)" % e)
        try:
            pz = from_json(obj)
        except (KeyError, TypeError, AttributeError, IndexError) as e:
            raise ValueError("JSON this server cannot read as a crossword (%s: %s)" % (type(e).__name__, e))
    asked = parse_date(date)
    day = asked or pz["date"] or date_from_name(name)
    if not day:
        raise ValueError("no print date in the file, its title or its name "
                         "(name it for the date, like 1993-11-21.json)")
    # A date asked for is the caller's word. Any other can be wrong -- a name
    # read the wrong way, say -- and the file may know better.
    said = pz.get("dow")
    if said is not None and not asked:
        actual = datetime.date.fromisoformat(day).weekday()
        if actual != said:
            raise ValueError("dated %s, which was a %s, but the file says %s"
                             % (day, FULL_DAYS[actual], FULL_DAYS[said]))
    pz["date"] = day
    return pz


# ---- the library ------------------------------------------------------------------------------------

_lock = threading.Lock()
_cache = {"stamp": None, "entries": []}


def root_dir():
    return os.path.join(dash.state_dir(), "crosswords")


def puzzle_path(pid):
    if not _ID.match(pid or ""):
        raise ValueError("%r is not a puzzle id" % (pid or "")[:40])
    return os.path.join(root_dir(), pid + ".xw")


def _invalidate():
    with _lock:
        _cache["stamp"] = None


def _write(path, text):
    d = os.path.dirname(path)
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def add(pz, replace=False, dry=False, pid=None):
    """Keep a puzzle, under its print date (or pid, for a series beside the
    dailies: the Midi's are DATE-midi). 'added', 'replaced', or
    'duplicate' -- a puzzle already there is left as it was unless replace.
    (A file there that is not a puzzle is no puzzle to keep: it is replaced.)
    With dry, say which and write nothing."""
    day = pz.get("date") or ""
    if not _ISO.match(day) or not parse_date(day):
        raise ValueError("a puzzle needs a print date to be kept under")
    text = to_text(pz)
    parse_text(text)                       # never keep what the device would refuse
    pid = pid or day
    path = puzzle_path(pid)
    with _lock:
        had = os.path.exists(path)
        if had and not replace and _head(path, pid):
            return "duplicate"
        if not dry:
            _write(path, text)
            _cache["stamp"] = None
    return "replaced" if had else "added"


def remove(pid):
    """Forget a puzzle. True if there was one."""
    path = puzzle_path(pid)
    with _lock:
        try:
            os.remove(path)
        except FileNotFoundError:
            return False
        _cache["stamp"] = None
    return True


def load(pid):
    """A puzzle's text, or None if there is none. A stored file that is not a
    puzzle is the server's fault, not the caller's: RuntimeError."""
    path = puzzle_path(pid)
    try:
        with open(path, encoding="ascii", newline="") as f:
            text = f.read()
    except FileNotFoundError:
        return None
    except UnicodeDecodeError:
        text = "\xff"
    try:
        parse_text(text)
    except ValueError as e:
        raise RuntimeError("puzzle %s is damaged: %s" % (pid, e))
    return text


def _head(path, pid):
    """A stored puzzle's list entry from the top of its file, or None."""
    try:
        with open(path, "rb") as f:
            raw = f.read(2048)
    except OSError:
        return None
    e = {"id": pid, "title": "", "date": "", "weekday": -1, "w": 0, "h": 0}
    first = True
    for line in raw.decode("ascii", "replace").split("\n"):
        line = line.rstrip(" \r")
        if not line:
            continue
        if first:
            if line != "xw 1":
                return None
            first = False
            continue
        key, _, rest = line.partition(" ")
        if key == "title":
            e["title"] = rest
        elif key == "date" and _ISO.match(rest) and parse_date(rest):
            e["date"] = rest
        elif key == "size":
            m = re.match(r"(\d+) (\d+)$", rest)
            if not m:
                return None
            e["w"], e["h"] = int(m.group(1)), int(m.group(2))
            break
    if not e["w"]:
        return None
    if e["date"]:
        e["weekday"] = datetime.date.fromisoformat(e["date"]).weekday()
    return e


def index():
    """Every stored puzzle, oldest id first: dicts of id, title, date,
    weekday (0 Monday, -1 for none), w and h. Read from the files' tops and
    kept until the folder changes, so a puzzle copied in by hand shows up."""
    root = root_dir()
    try:
        names = os.listdir(root)
        stamp = (root, os.stat(root).st_mtime_ns, len(names))
    except OSError:
        return []
    with _lock:
        if _cache["stamp"] == stamp:
            return _cache["entries"]
    found = []
    for name in names:
        if name.endswith(".xw") and _ID.match(name[:-3]):
            e = _head(os.path.join(root, name), name[:-3])
            if e:
                found.append(e)
    found.sort(key=lambda e: e["id"])
    with _lock:
        _cache["stamp"] = stamp
        _cache["entries"] = found
    return found


def select(weekdays=None, lo="", hi="", after="", order="new", limit=LIST_DEFAULT):
    """The puzzles a list page holds. weekdays a set of 0-6; lo and hi dates
    (inclusive, YYYY-MM-DD) -- a puzzle with no date is in no range; after an
    id, the page before's last; order 'new' or 'old'."""
    out = index()
    if weekdays is not None:
        out = [e for e in out if e["weekday"] in weekdays]
    if lo or hi:
        out = [e for e in out if e["date"] and (not lo or e["date"] >= lo) and (not hi or e["date"] <= hi)]
    if order == "new":
        out = out[::-1]
    if after:
        out = [e for e in out if (e["id"] < after if order == "new" else e["id"] > after)]
    return out[:max(1, min(int(limit), LIST_MAX))]


def import_bytes(data, name="", date="", replace=False, dry=False):
    """Read, convert and keep one file's puzzle: (id, 'added' | 'replaced' |
    'duplicate'). ValueError, saying why, for a file that is no puzzle."""
    pz = convert(data, name, date)
    return pz["date"], add(pz, replace, dry)


def _read_file(path):
    """A file's bytes, or ValueError saying why not -- including that it is
    too big to be a puzzle, which is said before it is read."""
    try:
        size = os.path.getsize(path)
        if size > MAX_IN:
            raise ValueError("a file of %d KB is not a crossword" % (size // 1024))
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise ValueError("cannot read it: %s" % (e.strerror or e))


def import_file(path, date="", replace=False, dry=False):
    """import_bytes for a file on this machine; the path is the name, so a
    date in it counts."""
    return import_bytes(_read_file(path), path, date, replace, dry)


# ---- a folder of them -------------------------------------------------------------------------------

Row = collections.namedtuple("Row", "status id weekday size file why")
RECAP_MAX = 100                 # failures listed at the end of a run
PROGRESS_EVERY = 500            # files between "N files..." lines


class Report(object):
    """What a bulk import has done so far. Rows are in the order the files
    were tried; status is added, replaced, duplicate or failed (with dry_run,
    what would be). ignored counts files that were not tried at all."""

    def __init__(self):
        self.rows = []
        self.counts = {"added": 0, "replaced": 0, "duplicate": 0, "failed": 0}
        self.ignored = 0

    def add(self, row):
        self.rows.append(row)
        self.counts[row.status] += 1
        return row

    def failed(self):
        return [r for r in self.rows if r.status == "failed"]


def _skippable(name):
    """What a Mac leaves in a zip or beside a file (__MACOSX/, ._name): never
    a puzzle, and not worth a line in a report."""
    return any(p == "__MACOSX" or p.startswith("._") for p in name.replace("\\", "/").split("/"))


def _ext(name):
    return os.path.splitext(name)[1].lower()


def _failing(why):
    def read():
        raise ValueError(why)
    return read


def _zip_reader(zf, info):
    def read():
        if info.file_size > MAX_IN:
            raise ValueError("a file of %d KB is not a crossword" % (info.file_size // 1024))
        try:
            with zf.open(info) as f:
                data = f.read(MAX_IN + 1)       # what it says it holds is not trusted
        except (zipfile.BadZipFile, OSError, EOFError, zlib.error, NotImplementedError, RuntimeError) as e:
            raise ValueError("cannot read it from the zip: %s" % e)
        if len(data) > MAX_IN:
            raise ValueError("a file of more than %d KB is not a crossword" % (MAX_IN // 1024))
        return data
    return read


def _from_zip(path, report):
    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as e:
        yield path, os.path.basename(path), _failing("not a readable zip file: %s" % e)
        return
    with zf:
        for info in sorted(zf.infolist(), key=lambda i: i.filename):
            if info.is_dir():
                continue
            if _skippable(info.filename) or _ext(info.filename) not in (".puz", ".json"):
                report.ignored += 1
                continue
            yield "%s:%s" % (path, info.filename), info.filename, _zip_reader(zf, info)


def _from_file(full, rel, report):
    if _skippable(rel):
        report.ignored += 1
    elif _ext(rel) in (".puz", ".json"):
        yield full, rel, (lambda p=full: _read_file(p))
    elif _ext(rel) == ".zip":
        for item in _from_zip(full, report):
            yield item
    else:
        report.ignored += 1


def _walk(paths, report):
    """Every file under the paths worth trying as a puzzle, in a fixed order:
    (shown, name, read). `name` is the path below the folder that was given
    (below the zip, for a member) -- what a date in a file's name is looked
    for in, and not the folders above it, which are the user's own business:
    a download folder called 20240101 is no print date. `read()` gives the
    bytes or ValueError. A zip is open while its members are being yielded,
    so each is to be read before the next is asked for."""
    for top in paths:
        if os.path.isdir(top):
            for dirpath, dirnames, filenames in os.walk(top):
                dirnames.sort()
                for fn in sorted(filenames):
                    full = os.path.join(dirpath, fn)
                    for item in _from_file(full, os.path.relpath(full, top).replace(os.sep, "/"), report):
                        yield item
        elif _ext(top) == ".zip":
            for item in _from_zip(top, report):
                yield item
        else:
            # Named outright, so tried whatever it is called: what is in it decides.
            yield top, os.path.basename(top), (lambda p=top: _read_file(p))


def bulk_import(paths, replace=False, dry_run=False, date="", report=None, log=None):
    """Import every .puz and .json under the paths (folders all the way down,
    and .zip archives), each under its print date. A file that is not a
    puzzle, or whose date contradicts its weekday, is a failed row with the
    reason, and the rest carry on. With dry_run, nothing is written, and the
    rows say what would happen -- including a second file for a date that a
    first one would have taken. log(row) is called as each file is done.
    Returns the Report (pass one in to keep it if this is interrupted)."""
    report = report if report is not None else Report()
    would = set()
    for shown, name, read in _walk(paths, report):
        try:
            pz = convert(read(), name, date)
            pid = pz["date"]
            status = add(pz, replace, dry=dry_run)
            if dry_run and status == "added" and pid in would:
                status = "replaced" if replace else "duplicate"
            would.add(pid)
            row = Row(status, pid, DAYS[datetime.date.fromisoformat(pid).weekday()],
                      "%dx%d" % (pz["w"], pz["h"]), shown, "")
        except ValueError as e:
            row = Row("failed", "", "", "", shown, str(e))
        except Exception as e:                 # a bug in a reader must not end a run of ten thousand
            row = Row("failed", "", "", "", shown, "unexpected %s: %s" % (type(e).__name__, e))
        report.add(row)
        if log:
            log(row)
    return report


_LABELS = {"added": "added", "replaced": "replaced", "duplicate": "duplicate", "failed": "FAILED"}
_DRY_LABELS = {"added": "would add", "replaced": "would replace", "duplicate": "duplicate",
               "failed": "FAILED"}


def format_row(row, dry_run=False):
    """A report line: what happened, the id, its weekday and size, the file."""
    label = (_DRY_LABELS if dry_run else _LABELS)[row.status]
    if row.status == "failed":
        return "%-13s %s: %s" % (label, row.file, row.why)
    return "%-13s %s %-3s %-5s %s" % (label, row.id, row.weekday, row.size, row.file)


def format_summary(report, dry_run=False):
    c = report.counts
    parts = ["%d %s" % (c["added"], "would be added" if dry_run else "added")]
    if c["replaced"]:
        parts.append("%d %s" % (c["replaced"], "would be replaced" if dry_run else "replaced"))
    parts.append("%d already there" % c["duplicate"])
    parts.append("%d could not be read" % c["failed"])
    s = ", ".join(parts)
    if report.ignored:
        s += " (%d other files ignored)" % report.ignored
    return s


def import_main(argv=None, out=None, err=None, prog="python -m server.crossword import"):
    """The import command. Returns the exit status: 0, or 1 if any file could
    not be read, or 2 for a command that could not be run at all (no such
    folder, nothing in it to import). A mistake in the arguments is argparse's
    own usage message and exit 2."""
    import argparse
    out = out or sys.stdout
    err = err or sys.stderr
    ap = argparse.ArgumentParser(
        prog=prog, description="Add downloaded crosswords -- .puz files or JSON, in folders (read all "
        "the way down) or .zip archives -- to the library, each under its print date.")
    ap.add_argument("paths", nargs="+", metavar="PATH", help="a folder, a .zip or a file")
    ap.add_argument("--dry-run", action="store_true", help="say what would happen; write nothing")
    ap.add_argument("--replace", action="store_true",
                    help="put a puzzle over one already there (otherwise it is skipped)")
    ap.add_argument("--date", metavar="YYYY-MM-DD",
                    help="the print date, for one file that neither says it nor has it in its name")
    ap.add_argument("--state", metavar="DIR",
                    help="the server's CARDOS_STATE (default: the environment's, else ~/.cardos)")
    loud = ap.add_mutually_exclusive_group()
    loud.add_argument("-v", "--verbose", action="store_true", help="a line for every file")
    loud.add_argument("-q", "--quiet", action="store_true", help="only the summary")
    args = ap.parse_args(argv)

    for p in args.paths:
        if not os.path.exists(p):
            print("no such file or folder: %s" % p, file=err)
            return 2
    date = ""
    if args.date:
        date = parse_date(args.date)
        if not date:
            print("--date is a date, like 1993-11-21; not %r" % args.date, file=err)
            return 2
        if len(args.paths) != 1 or os.path.isdir(args.paths[0]) or _ext(args.paths[0]) == ".zip":
            print("--date is for one file; a folder's puzzles each have a date of their own", file=err)
            return 2
    if args.state:
        os.environ["CARDOS_STATE"] = os.path.expanduser(args.state)

    if not args.quiet:
        print("library: %s (%d puzzles)" % (root_dir(), len(index())), file=out)
    report = Report()
    tried = [0]

    def log(row):
        tried[0] += 1
        if args.verbose:
            print(format_row(row, args.dry_run), file=out)
        elif not args.quiet and tried[0] % PROGRESS_EVERY == 0:
            print("%d files..." % tried[0], file=out)

    interrupted = False
    try:
        bulk_import(args.paths, args.replace, args.dry_run, date, report, log)
    except KeyboardInterrupt:
        interrupted = True
    if not report.rows and not interrupted:
        print("no .puz or .json files under %s%s" % (
            " or ".join(args.paths), " (%d other files ignored)" % report.ignored if report.ignored else ""),
            file=err)
        return 2
    if interrupted:
        print("interrupted; what was done stays done:", file=out)
    print(format_summary(report, args.dry_run), file=out)
    failed = report.failed()
    if failed and not args.quiet:
        print("could not be read:", file=out)
        for r in failed[:RECAP_MAX]:
            print("  %s: %s" % (r.file, r.why), file=out)
        if len(failed) > RECAP_MAX:
            print("  ... and %d more" % (len(failed) - RECAP_MAX), file=out)
    if not args.dry_run and not args.quiet:
        print("library: %d puzzles" % len(index()), file=out)
    return 130 if interrupted else 1 if failed else 0


def main(argv=None, out=None, err=None):
    """python -m server.crossword import PATH...; see the top of this file."""
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["import"]:
        return import_main(argv[1:], out, err)
    if argv == ["midi"]:
        midi_refresh(log=lambda s: print(s, file=out or sys.stdout))
        return 0
    usage = ("usage: python -m server.crossword import PATH [PATH ...] "
             "[--dry-run] [--replace] [--date D] [--state DIR] [-v | -q]\n"
             "       python -m server.crossword midi     (this week's NYT Midi, now)")
    if argv in (["-h"], ["--help"]):
        print(usage, file=out or sys.stdout)
        return 0
    print(usage, file=err or sys.stderr)
    return 2


# ---- the NYT Midi, fetched every day -----------------------------------------------------------------------
#
# nytmidi.org serves the Times' Midi (between the Mini and the daily) as the
# Times' own JSON: /puzzle/today.json, and /puzzle/DATE.json for days gone.
# The owner asked for each day's added and a week of them kept, rolling. The
# week is counted from the date today.json says, not this machine's clock, so
# the server's zone and the Times' do not have to agree. Kept as DATE-midi,
# which sorts just after a daily of the same date and never takes its place;
# the title says Midi, since the app's list shows only date and title.

MIDI_URL = "https://www.nytmidi.org/puzzle/"
MIDI_KEEP = 7                   # days kept, today among them
MIDI_EVERY = 3600               # seconds between looks
MIDI_SUFFIX = "-midi"


def _fetch_json(url):
    import urllib.request
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (CardOS server)"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read(MAX_IN + 1)
    if len(data) > MAX_IN:
        raise ValueError("%s is too big to be a crossword" % url)
    return json.loads(data.decode("utf-8"))


def _midi_puzzle(j):
    pz = from_json(j)
    if not pz.get("date"):
        raise ValueError("a Midi with no publicationDate")
    title = pz.get("title") or ""
    pz["title"] = plain("Midi: " + title if title else "Midi", TITLE_MAX)
    return pz


def midi_refresh(fetch=None, log=None):
    """Today's Midi and any of the last week's missing, then forget what is
    older than a week. Returns (added ids, removed ids). One day that fails
    is logged and skipped; today.json failing raises."""
    fetch = fetch or _fetch_json
    log = log or (lambda s: print("crossword: " + s))
    pz = _midi_puzzle(fetch(MIDI_URL + "today.json"))
    today = datetime.date.fromisoformat(pz["date"])
    oldest = (today - datetime.timedelta(days=MIDI_KEEP - 1)).isoformat()
    added = []
    if add(pz, pid=pz["date"] + MIDI_SUFFIX) == "added":
        added.append(pz["date"] + MIDI_SUFFIX)
    for back in range(1, MIDI_KEEP):
        day = (today - datetime.timedelta(days=back)).isoformat()
        pid = day + MIDI_SUFFIX
        if os.path.exists(puzzle_path(pid)):
            continue
        try:
            old = _midi_puzzle(fetch(MIDI_URL + day + ".json"))
            if old["date"] != day:
                raise ValueError("asked for %s, given %s" % (day, old["date"]))
            if add(old, pid=pid) == "added":
                added.append(pid)
        except Exception as e:             # a day the site lacks is not the week's end
            log("midi %s: %s" % (day, e))
    removed = [e["id"] for e in index()
               if e["id"].endswith(MIDI_SUFFIX) and e["id"][:-len(MIDI_SUFFIX)] < oldest]
    for pid in removed:
        remove(pid)
    if added or removed:
        log("midi: added %s; removed %s" % (" ".join(added) or "none", " ".join(removed) or "none"))
    return added, removed


def start_midi(every=MIDI_EVERY):
    """Look for a new Midi now and every hour after, on a thread of its own."""
    def loop():
        import time
        while True:
            try:
                midi_refresh()
            except Exception as e:
                print("crossword: midi: %s" % e)
            time.sleep(every)
    threading.Thread(target=loop, name="midi", daemon=True).start()


# ---- routes ----------------------------------------------------------------------------------------------

def _limit(args):
    raw = arg(args, "limit")
    if not raw:
        return LIST_DEFAULT
    try:
        n = int(raw)
    except ValueError:
        raise ValueError("limit= is a number")
    return max(1, min(n, LIST_MAX))


def _weekdays(spec):
    """A set of weekdays (0 Monday) from "sat,sun" or "5,6"; None for no filter."""
    if not spec:
        return None
    names = [d.lower() for d in DAYS]
    out = set()
    for tok in spec.lower().split(","):
        tok = tok.strip()
        if tok.isdigit() and 0 <= int(tok) <= 6:
            out.add(int(tok))
        elif len(tok) >= 3 and tok[:3] in names:
            out.add(names.index(tok[:3]))
        else:
            raise ValueError("weekday= is mon to sun, or 0 to 6 from Monday; several with commas")
    return out


def _bound(spec, end):
    """A date as far as it goes -- 1993, 1993-11, 1993-11-21 -- as the first
    day it names, or with end the last."""
    if not spec:
        return ""
    m = re.match(r"(\d{4})(?:-(\d{2})(?:-(\d{2}))?)?$", spec)
    if not m:
        raise ValueError("from= and to= are a date: 1993, 1993-11 or 1993-11-21")
    y = int(m.group(1))
    mo = int(m.group(2)) if m.group(2) else (12 if end else 1)
    try:
        if m.group(3):
            d = int(m.group(3))
        elif end:
            d = calendar.monthrange(y, mo)[1]
        else:
            d = 1
        return datetime.date(y, mo, d).isoformat()
    except ValueError:
        raise ValueError("%s is not a date" % spec)


@text_route
def get_list(h, args):
    """puzzles, newest first, a line each: id, date, weekday, size, title"""
    order = arg(args, "order") or "new"
    if order not in ("new", "old"):
        raise ValueError("order= is new or old")
    after = arg(args, "after")
    if after and not _ID.match(after):
        raise ValueError("after= is a puzzle id")
    page = select(_weekdays(arg(args, "weekday")), _bound(arg(args, "from"), False),
                  _bound(arg(args, "to"), True), after, order, _limit(args))
    h.text("".join("%s\t%s\t%s\t%dx%d\t%s\n" % (
        e["id"], e["date"], DAYS[e["weekday"]] if e["weekday"] >= 0 else "",
        e["w"], e["h"], wire.flat(e["title"], LIST_TITLE, ascii=True)) for e in page))


@text_route
def get_puzzle(h, args):
    """one puzzle in the compact line format"""
    pid = arg(args, "id")
    if not pid:
        raise ValueError("id= is the puzzle's id, from the list")
    text = load(pid)
    if text is None:
        h.text("error no such puzzle\n", 404)
        return
    h.text(text)


ROUTES = [
    ("GET", "/crossword/list", get_list),
    ("GET", "/crossword/puzzle", get_puzzle),
]


if __name__ == "__main__":
    sys.exit(main())
