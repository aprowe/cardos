"""Crosswords through the server: the compact text the device reads, the two
kinds of file puzzles come in, the library, and the list the app pages
through.

What the device relies on: the text parses exactly as apps/crossword/xwcore.h
parses it (the same fixture as test/test_crossword.c is used here), nothing is
kept that the device would refuse, a puzzle's id is its print date, and the
list is newest first in pages that can be followed to the end.

    python -m server.tests.test_crossword
"""
import datetime
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, crossword
from server import chat as chatmod

TOKEN = "tok"

# The fixture of test/test_crossword.c: a 5x5 word square with two black
# corners and a circled U.
FIX_ROWS = ["HEAR.", "EMBER", "ABuSE", "RESIN", ".REND"]
FIX_ACROSS = ["Catch with the ears", "Smoldering bit", "Mistreat", "Pine product", "Tear, as a seam"]
FIX_DOWN = ["Listen up", "Fire remnant", "Misuse", "Sticky stuff", "Rip apart"]
FIX = ("xw 1\ntitle Mini Heart\nby A. Setter\ndate 1993-11-21\nnote Circled letter: U\n"
       "size 5 5\nrow HEAR.\nrow EMBER\nrow ABuSE\nrow RESIN\nrow .REND\n"
       "A 1 Catch with the ears\nA 5 Smoldering bit\nA 7 Mistreat\nA 8 Pine product\n"
       "A 9 Tear, as a seam\n"
       "D 1 Listen up\nD 2 Fire remnant\nD 3 Misuse\nD 4 Sticky stuff\nD 6 Rip apart\n"
       "end\n")
# The clues in the order a .puz keeps them: by number, across first.
FIX_PUZ_ORDER = ["Catch with the ears", "Listen up", "Fire remnant", "Misuse", "Sticky stuff",
                 "Smoldering bit", "Rip apart", "Mistreat", "Pine product", "Tear, as a seam"]

# ... and the ring of four words round a black middle.
FRAME_ROWS = ["SLATE", "T...A", "A...R", "R...L", "SEALS"]


def upper(rows):
    return [r.upper() for r in rows]


def small(date, title=None):
    """A 2x2 puzzle with no black squares, for the library tests."""
    return crossword.make_puzzle(["AB", "CD"], ["a1", "a3"], ["d1", "d2"],
                                 title="Puzzle " + date[-2:] if title is None else title,
                                 date=date)


# ---- files, made the way the real ones are ---------------------------------------------

def make_puz(rows, clues, title="", author="", copyright="", notes="", circled=(),
             version=b"1.3\x00", scrambled=0, encoding="cp1252", lead=b""):
    """An Across Lite file, checksums left at zero (the server reads none)."""
    h, w = len(rows), len(rows[0])
    cells = "".join(rows).encode("ascii")
    sol = cells.upper()
    state = bytes(46 if b == 46 else 45 for b in sol)
    head = (struct.pack("<H", 0) + b"ACROSS&DOWN\x00" + struct.pack("<H", 0) + bytes(8) +
            version + bytes(2) + struct.pack("<H", 0) + bytes(12) + bytes([w, h]) +
            struct.pack("<HHH", len(clues), 1, scrambled))
    assert len(head) == 0x34
    body = sol + state
    for s in [title, author, copyright] + list(clues) + [notes]:
        body += s.encode(encoding) + b"\x00"
    if circled:
        flags = bytes(0x80 if i in circled else 0 for i in range(w * h))
        body += b"GEXT" + struct.pack("<HH", len(flags), 0) + flags + b"\x00"
    return lead + head + body


def xword_json(rows, across, down, **kw):
    """XWord Info's shape: a flat grid, "N. text" clues."""
    a, d = crossword.entries(upper(rows))
    j = {"title": "Mini Heart", "author": "A. Setter", "date": "11/21/1993", "dow": "Sunday",
         "notepad": "Circled letter: U", "editor": "Will Shortz",
         "size": {"rows": len(rows), "cols": len(rows[0])},
         "grid": [ch for r in upper(rows) for ch in r],
         "circles": [1 if ch.islower() else 0 for r in rows for ch in r],
         "clues": {"across": ["%d. %s" % (e[0], t) for e, t in zip(a, across)],
                   "down": ["%d. %s" % (e[0], t) for e, t in zip(d, down)]}}
    j.update(kw)
    return j


def nyt_json(rows, across, down, **kw):
    """The NYT's own shape: cells, clues with a direction and a label."""
    a, d = crossword.entries(upper(rows))
    cells = []
    for r in rows:
        for ch in r:
            cells.append({} if ch == "." else
                         {"answer": ch.upper(), "type": 2 if ch.islower() else 1, "clues": [0, 0]})
    clues = []
    for name, ents, texts in (("Across", a, across), ("Down", d, down)):
        for e, t in zip(ents, texts):
            clues.append({"direction": name, "label": str(e[0]), "list": 0 if name == "Across" else 1,
                          "text": [{"plain": t}]})
    j = {"publicationDate": "1993-11-21", "title": "Mini Heart", "constructors": ["A. Setter"],
         "notes": [{"text": "Circled letter: U"}],
         "body": [{"dimensions": {"width": len(rows[0]), "height": len(rows)}, "cells": cells,
                   "clues": clues, "clueLists": [{"name": "Across", "clues": []},
                                                 {"name": "Down", "clues": []}]}]}
    j.update(kw)
    return j


# ---- the text format --------------------------------------------------------------------------

class TextFormat(unittest.TestCase):
    def test_the_fixture_parses(self):
        pz = crossword.parse_text(FIX)
        self.assertEqual((pz["w"], pz["h"]), (5, 5))
        self.assertEqual(pz["rows"], FIX_ROWS)
        self.assertEqual((pz["title"], pz["by"], pz["note"], pz["date"]),
                         ("Mini Heart", "A. Setter", "Circled letter: U", "1993-11-21"))
        self.assertEqual(pz["across"], list(zip([1, 5, 7, 8, 9], FIX_ACROSS)))
        self.assertEqual(pz["down"], list(zip([1, 2, 3, 4, 6], FIX_DOWN)))

    def test_it_writes_back_what_it_read(self):
        self.assertEqual(crossword.to_text(crossword.parse_text(FIX)), FIX)

    def test_entries_are_numbered_the_way_the_device_numbers_them(self):
        across, down = crossword.entries(upper(FIX_ROWS))
        self.assertEqual(across, [(1, 0, 0, 4), (5, 1, 0, 5), (7, 2, 0, 5), (8, 3, 0, 5), (9, 4, 1, 4)])
        self.assertEqual(down, [(1, 0, 0, 4), (2, 0, 1, 5), (3, 0, 2, 5), (4, 0, 3, 5), (6, 1, 4, 4)])
        across, down = crossword.entries(FRAME_ROWS)
        self.assertEqual(across, [(1, 0, 0, 5), (3, 4, 0, 5)])
        self.assertEqual(down, [(1, 0, 0, 5), (2, 0, 4, 5)])
        # No black squares: along the top, then down the side.
        across, down = crossword.entries(["AAAAA"] * 5)
        self.assertEqual([e[0] for e in across], [1, 6, 7, 8, 9])
        self.assertEqual([e[0] for e in down], [1, 2, 3, 4, 5])
        # One square wide.
        across, down = crossword.entries(["A", "B", "C"])
        self.assertEqual((across, down), ([], [(1, 0, 0, 3)]))

    def test_a_grid_that_cannot_be_a_crossword(self):
        with self.assertRaisesRegex(ValueError, "in no word"):
            crossword.entries(["A.B"])
        with self.assertRaisesRegex(ValueError, "no white squares"):
            crossword.entries(["..", ".."])
        # Two-square words in every row and column, each row shifted along: 280 entries.
        rows = ["".join("." if (r + c) % 3 == 2 else "A" for c in range(21)) for r in range(21)]
        with self.assertRaisesRegex(ValueError, "280 entries"):
            crossword.entries(rows)

    def test_crlf_blank_lines_and_lines_it_does_not_know(self):
        pz = crossword.parse_text(FIX.replace("\n", "\r\n"))
        self.assertEqual(pz["title"], "Mini Heart")
        self.assertEqual(pz["down"][4], (6, "Rip apart"))
        pz = crossword.parse_text(FIX.replace("size 5 5\n", "\n   \nfuture 7\nsize 5 5\n"))
        self.assertEqual(pz["rows"], FIX_ROWS)
        # Nothing after `end` is read.
        pz = crossword.parse_text(FIX + "A 99 junk\nrow nonsense\n")
        self.assertEqual(len(pz["across"]), 5)
        # An empty clue is allowed.
        pz = crossword.parse_text("xw 1\nsize 2 1\nrow AB\nA 1\nend\n")
        self.assertEqual(pz["across"], [(1, "")])

    def test_what_the_device_cuts_is_cut_here_too(self):
        pz = crossword.parse_text(FIX.replace("title Mini Heart", "title " + "t" * 100))
        self.assertEqual(len(pz["title"]), crossword.TITLE_MAX)
        pz = crossword.parse_text(FIX.replace("Rip apart", "r" * 300))
        self.assertEqual(len(pz["down"][4][1]), crossword.CLUE_MAX)

    def test_a_bad_date_is_no_date(self):
        for line in ("date soon", "date 1993-13-40", "date 11/21/1993"):
            pz = crossword.parse_text(FIX.replace("date 1993-11-21", line))
            self.assertEqual(pz["date"], "", line)

    def test_what_the_device_would_refuse_is_refused(self):
        bad = [
            "size 5 5\n", "xw 2\n", "xw\n", "", "xw 1\n",
            "xw 1\nsize 22 5\n", "xw 1\nsize 5 22\n", "xw 1\nsize 0 5\n", "xw 1\nsize 5\n",
            "xw 1\nsize 2 1\nsize 2 1\n",
            "xw 1\nrow AB\n", "xw 1\nA 1 x\n",
            "xw 1\nsize 2 1\nrow ABC\n", "xw 1\nsize 2 1\nrow \n", "xw 1\nsize 2 1\nrow A4\n",
            "xw 1\nsize 2 1\nrow AB\nrow AB\n",
            "xw 1\nsize 2 2\nrow AB\nA 1 x\n", "xw 1\nsize 2 2\nrow AB\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA 1 x\nrow AB\n",
            "xw 1\nsize 3 1\nrow A.B\nend\n", "xw 1\nsize 2 1\nrow ..\nend\n",
            "xw 1\nsize 2 1\nrow AB\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA 2 x\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA 1 x\nA 2 y\nend\n",
            "xw 1\nsize 2 1\nrow AB\nD 1 x\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA x\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA 1x y\nend\n",
            "xw 1\nsize 2 1\nrow AB\nA 1 x\n",                     # no end
            FIX[:FIX.index("end\n")],                              # cut off
            FIX.replace("A 5 Smoldering bit\n", ""),               # a number missing
            FIX.replace("D 6 Rip apart\n", ""),                    # a clue missing
            FIX.replace("end\n", "A 10 extra\nend\n"),             # one too many
            FIX.replace("A 7 Mistreat", "A 6 Mistreat"),           # not the grid's number
        ]
        for text in bad:
            with self.subTest(text=text[:60]):
                with self.assertRaises(ValueError):
                    crossword.parse_text(text)
        with self.assertRaisesRegex(ValueError, "cut short"):
            crossword.parse_text(FIX[:FIX.index("end\n")])
        with self.assertRaisesRegex(ValueError, "not a puzzle"):
            crossword.parse_text("")

    def test_the_biggest_grid_fits_and_too_much_does_not(self):
        rows = ["A" * 21] * 21
        pz = crossword.make_puzzle(rows, ["xxxxxxxx"] * 21, ["xxxxxxxx"] * 21)
        self.assertEqual((pz["w"], pz["h"]), (21, 21))
        self.assertEqual([n for n, _ in pz["across"]], [1] + list(range(22, 42)))
        self.assertEqual([n for n, _ in pz["down"]], list(range(1, 22)))
        self.assertEqual(crossword.parse_text(crossword.to_text(pz))["rows"], rows)
        # 42 clues of 200 characters is 8442 bytes against room for 8192.
        with self.assertRaisesRegex(ValueError, "clue text"):
            crossword.make_puzzle(rows, ["x" * 200] * 21, ["x" * 200] * 21)
        with self.assertRaisesRegex(ValueError, "21x21"):
            crossword.make_puzzle(["A" * 22] * 3, ["a"] * 3, ["a"] * 22)
        with self.assertRaisesRegex(ValueError, "21x21"):
            crossword.make_puzzle(["AB"] * 22, ["a"] * 22, ["a"] * 2)

    def test_pieces_that_do_not_fit_the_grid_are_refused(self):
        with self.assertRaisesRegex(ValueError, "clues for a grid"):
            crossword.make_puzzle(["AB", "CD"], ["a1"], ["d1", "d2"])
        with self.assertRaisesRegex(ValueError, "row 2"):
            crossword.make_puzzle(["AB", "C"], ["a1", "a2"], ["d1"])
        with self.assertRaisesRegex(ValueError, "answer is '1'"):
            crossword.make_puzzle(["AB", "C1"], ["a1", "a3"], ["d1", "d2"])
        with self.assertRaisesRegex(ValueError, "empty"):
            crossword.make_puzzle([], [], [])


# ---- text cleaning and dates --------------------------------------------------------------------

class Cleaning(unittest.TestCase):
    def test_markup_entities_and_symbols_become_plain_text(self):
        self.assertEqual(crossword.plain("<i>Star Wars</i> &amp; <b>Co</b>", 200), "Star Wars & Co")
        self.assertEqual(crossword.plain("&lt;i&gt;Hi&lt;/i&gt; there", 50), "Hi there")
        self.assertEqual(crossword.plain("Is 3 < 5?", 50), "Is 3 < 5?")          # not a tag
        self.assertEqual(crossword.plain("Caf\u00e9 \u2014 \u201cna\u00efve\u201d \u2026 \u00bd", 80),
                         "Cafe - \"naive\" ... 1/2")
        self.assertEqual(crossword.plain("a\u00a0b\tc\nd", 50), "a b c d")
        self.assertEqual(crossword.plain("\u2665 or \u2660", 80), "heart or spade")
        self.assertEqual(crossword.plain("\u0394 and \u03c0", 80), "Delta and pi")
        self.assertEqual(crossword.plain(None, 10), "")
        self.assertEqual(crossword.plain("x" * 50, 10), "x" * 10)

    def test_a_clue_is_never_empty_or_long(self):
        self.assertEqual(crossword.clean_clue(""), "(no clue)")
        self.assertEqual(crossword.clean_clue("\u4e2d\u6587"), "(no clue)")
        self.assertLessEqual(len(crossword.clean_clue("word " * 300)), crossword.CLUE_MAX)

    def test_a_title_that_is_only_the_date_is_no_title(self):
        t = crossword.clean_title
        self.assertEqual(t("NY Times, Sunday, November 21, 1993"), "")
        self.assertEqual(t("NY Times, Sun, Nov 21, 1993 - Pair Dealing"), "Pair Dealing")
        self.assertEqual(t("N.Y. Times, Monday, Jan 1, 1990"), "")
        self.assertEqual(t("The New York Times, Monday, 1/1/1990: Hello"), "Hello")
        self.assertEqual(t("NYT: Hello Dolly"), "Hello Dolly")
        self.assertEqual(t("Sunday Challenge"), "Sunday Challenge")
        self.assertEqual(len(t("t" * 100)), crossword.TITLE_MAX)

    def test_dates_in_the_ways_puzzles_give_them(self):
        d = crossword.parse_date
        self.assertEqual(d("1993-11-21"), "1993-11-21")
        self.assertEqual(d("1993-11-21T00:00:00Z"), "1993-11-21")
        self.assertEqual(d("1993-1-2"), "1993-01-02")
        self.assertEqual(d("11/21/1993"), "1993-11-21")
        self.assertEqual(d("November 21, 1993"), "1993-11-21")
        self.assertEqual(d("Sunday, Nov 21st, 1993"), "1993-11-21")
        self.assertEqual(d("NY Times, Sunday, September 5, 2010"), "2010-09-05")
        for bad in ("", None, "soon", "1993-02-30", "13/1/1993", "1800-01-01", "Feb 30, 1993"):
            self.assertEqual(d(bad), "", bad)

    def test_a_date_in_a_file_name(self):
        n = crossword.date_from_name
        self.assertEqual(n("1993-11-21.json"), "1993-11-21")
        self.assertEqual(n("dump/1993/11/21.json"), "1993-11-21")
        self.assertEqual(n("C:\\puzzles\\19931121.puz"), "1993-11-21")
        self.assertEqual(n("nyt_1993_11_21.puz"), "1993-11-21")
        self.assertEqual(n("Nov2193.puz"), "1993-11-21")
        self.assertEqual(n("Jan0100.puz"), "2000-01-01")
        self.assertEqual(n("Feb1542.puz"), "1942-02-15")
        self.assertEqual(n("/data/20240101/xyz.json"), "2024-01-01")
        self.assertEqual(n("/data/2024-01-01/1993-11-21.json"), "1993-11-21")   # the file's own
        for bad in ("", "puzzle.puz", "Xyz2193.puz", "12345678.puz", "1993-13-45.json"):
            self.assertEqual(n(bad), "", bad)

    def test_the_weekday_a_file_states(self):
        s = crossword.stated_weekday
        self.assertEqual(s("NY Times, Sunday, November 21, 1993"), 6)
        self.assertEqual(s("NY Times, Sun, Nov 21, 1993"), 6)
        self.assertEqual(s("The New York Times, Monday, Jan 1, 1990"), 0)
        self.assertEqual(s("NYT: Tues"), 1)
        self.assertEqual(s("", "Saturday"), 5)
        self.assertEqual(s("", "Sat."), 5)
        self.assertEqual(s("", " thurs "), 3)
        self.assertEqual(s("Wednesday", "Fri"), 4)                       # the field first
        # A title that only mentions a day, or a day's name that is not one, says nothing.
        for title, dow in (("Sunday Challenge", ""), ("NYT Sundays", ""), ("NY Times, Monkeys", ""),
                           ("", "Sunday Challenge"), ("", ""), (None, None), ("Mini Heart", "")):
            self.assertIsNone(s(title, dow), (title, dow))


# ---- reading files ---------------------------------------------------------------------------------

class PuzFiles(unittest.TestCase):
    def fix_puz(self, **kw):
        args = dict(title="Mini Heart", author="A. Setter", notes="Circled letter: U", circled={12})
        args.update(kw)
        return make_puz(upper(FIX_ROWS), FIX_PUZ_ORDER, **args)

    def test_a_puz_becomes_the_fixture(self):
        pz = crossword.convert(self.fix_puz(), "nyt/1993-11-21.puz")
        self.assertEqual(crossword.to_text(pz), FIX)
        self.assertEqual(pz["rows"][2], "ABuSE")         # the circle, from the GEXT section

    def test_clues_are_matched_by_number_with_across_first(self):
        pz = crossword.convert(self.fix_puz(), "1993-11-21.puz")
        self.assertEqual(pz["across"][1], (5, "Smoldering bit"))
        self.assertEqual(pz["down"][0], (1, "Listen up"))
        self.assertEqual(pz["down"][4], (6, "Rip apart"))

    def test_the_date_in_the_title_wins_and_the_title_is_then_only_a_date(self):
        puz = self.fix_puz(title="NY Times, Sunday, November 21, 1993")
        pz = crossword.convert(puz, "elsewhere/2000-01-01.puz")
        self.assertEqual(pz["date"], "1993-11-21")
        self.assertEqual(pz["title"], "")
        self.assertNotIn("\ntitle ", crossword.to_text(pz))
        # A date asked for beats them both.
        self.assertEqual(crossword.convert(puz, "x.puz", date="1942-02-15")["date"], "1942-02-15")

    def test_no_date_anywhere_is_said(self):
        with self.assertRaisesRegex(ValueError, "print date"):
            crossword.convert(self.fix_puz(), "x.puz")

    def test_a_date_that_is_not_the_weekday_the_file_names_is_refused(self):
        # 1993-11-22 was a Monday; this title says Sunday.
        puz = self.fix_puz(title="NY Times, Sunday, November 22, 1993")
        with self.assertRaisesRegex(ValueError, "1993-11-22, which was a Monday, but the file says Sunday"):
            crossword.convert(puz, "x.puz")
        # The caller's own date is taken as it is.
        self.assertEqual(crossword.convert(puz, "x.puz", date="1993-11-21")["date"], "1993-11-21")
        # A name read the wrong way is caught by a file that knows better: Nov2293 is a Monday.
        puz = self.fix_puz(title="NY Times, Sunday")
        with self.assertRaisesRegex(ValueError, "which was a Monday"):
            crossword.convert(puz, "dump/Nov2293.puz")
        self.assertEqual(crossword.convert(puz, "dump/Nov2193.puz")["date"], "1993-11-21")

    def test_text_in_either_encoding(self):
        order = list(FIX_PUZ_ORDER)
        order[0] = "Don\u2019t stop"
        pz = crossword.convert(make_puz(upper(FIX_ROWS), order), "1993-11-21.puz")     # cp1252
        self.assertEqual(pz["across"][0][1], "Don't stop")
        order[0] = "Caf\u00e9 au lait"
        pz = crossword.convert(make_puz(upper(FIX_ROWS), order, version=b"2.0\x00", encoding="utf-8"),
                               "1993-11-21.puz")
        self.assertEqual(pz["across"][0][1], "Cafe au lait")

    def test_junk_before_the_header_is_skipped(self):
        pz = crossword.convert(self.fix_puz(lead=b"\x00" * 8), "1993-11-21.puz")
        self.assertEqual(pz["rows"], FIX_ROWS)

    def test_a_file_that_is_not_right_is_refused_in_words(self):
        puz = self.fix_puz()
        with self.assertRaisesRegex(ValueError, "scrambled"):
            crossword.convert(self.fix_puz(scrambled=4), "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "cut short"):
            crossword.convert(puz[:0x34 + 30], "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "cut short"):
            crossword.convert(puz[:puz.rfind(b"Tear") + 2], "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "10 entries"):
            crossword.convert(make_puz(upper(FIX_ROWS), FIX_PUZ_ORDER[:-1]), "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "answer is '1'"):
            crossword.convert(make_puz(["AB", "C1"], ["a", "b", "c", "d"]), "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "neither"):
            crossword.convert(b"hello", "1993-11-21.puz")
        with self.assertRaisesRegex(ValueError, "21x21"):
            crossword.convert(make_puz(["A" * 22] * 2, ["a"] * 24), "1993-11-21.puz")


class JsonFiles(unittest.TestCase):
    def convert(self, j, name="x.json"):
        return crossword.convert(json.dumps(j).encode("utf-8"), name)

    def test_xword_json_becomes_the_fixture(self):
        pz = self.convert(xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN))
        self.assertEqual(crossword.to_text(pz), FIX)

    def test_nyt_json_becomes_the_fixture(self):
        pz = self.convert(nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN))
        self.assertEqual(crossword.to_text(pz), FIX)

    def test_xword_json_in_its_other_dresses(self):
        # Rows of strings, rows of lists, a rebus square, clues with no numbers.
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        flat = j["grid"]
        j["grid"] = ["".join(flat[i:i + 5]) for i in range(0, 25, 5)]
        self.assertEqual(crossword.to_text(self.convert(j)), FIX)
        j["grid"] = [flat[i:i + 5] for i in range(0, 25, 5)]
        self.assertEqual(crossword.to_text(self.convert(j)), FIX)
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["grid"][0] = "HEART"                      # a rebus square keeps its first letter
        self.assertEqual(crossword.to_text(self.convert(j)), FIX)
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["clues"] = {"across": FIX_ACROSS, "down": FIX_DOWN}
        self.assertEqual(crossword.to_text(self.convert(j)), FIX)
        j["circles"] = None
        self.assertNotIn("ABuSE", crossword.to_text(self.convert(j)))
        # The date from the title when the file has none of its own.
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN, title="NY Times, Sunday, November 21, 1993")
        del j["date"]
        self.assertEqual(self.convert(j)["date"], "1993-11-21")

    def test_a_dow_that_disagrees_with_the_date_is_refused(self):
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN, date="11/22/1993")      # a Monday; dow says Sunday
        with self.assertRaisesRegex(ValueError, "Monday.*Sunday"):
            self.convert(j)
        self.assertEqual(self.convert(dict(j, dow="Monday"))["date"], "1993-11-22")
        self.assertEqual(self.convert(dict(j, dow=""))["date"], "1993-11-22")  # no claim, no check
        self.assertEqual(crossword.convert(json.dumps(j).encode(), "x.json", date="1993-11-21")["date"],
                         "1993-11-21")

    def test_the_date_comes_from_the_name_when_the_file_has_none(self):
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        del j["date"]
        self.assertEqual(self.convert(j, "dump/1993/11/21.json")["date"], "1993-11-21")
        with self.assertRaisesRegex(ValueError, "print date"):
            self.convert(j, "puzzle.json")

    def test_clue_numbers_are_checked_against_the_grid(self):
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["clues"]["across"][1] = "2. Smoldering bit"
        with self.assertRaisesRegex(ValueError, "2-Across is not an entry"):
            self.convert(j)
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        del j["clues"]["across"][2]
        with self.assertRaisesRegex(ValueError, "no clue for 7-Across"):
            self.convert(j)
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["clues"]["down"] = j["clues"]["down"][:-1] + ["Rip apart"]      # one unnumbered
        with self.assertRaisesRegex(ValueError, "numbered only in part"):
            self.convert(j)
        j = xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["clues"] = {"across": FIX_ACROSS[:-1], "down": FIX_DOWN}
        with self.assertRaisesRegex(ValueError, "4 Across clues for 5 entries"):
            self.convert(j)

    def test_nyt_json_is_checked_too(self):
        j = nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        del j["body"][0]["clues"][3]
        with self.assertRaises(ValueError):
            self.convert(j)
        j = nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["body"][0]["clues"][0]["direction"] = "Sideways"
        with self.assertRaisesRegex(ValueError, "neither across nor down"):
            self.convert(j)
        j = nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["body"][0]["cells"] = j["body"][0]["cells"][:-1]
        with self.assertRaisesRegex(ValueError, "24 cells"):
            self.convert(j)
        # A clue's text may be a plain string, or styled.
        j = nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)
        j["body"][0]["clues"][0]["text"] = "Catch with the ears"
        j["body"][0]["clues"][1]["text"] = [{"formatted": "<i>Smoldering</i> bit"}]
        self.assertEqual(crossword.to_text(self.convert(j)), FIX)

    def test_what_is_not_a_crossword_is_said_so(self):
        for data, why in ((b"{oops", "does not parse"), (b'{"hello": 1}', "not a crossword"),
                          (b"[1, 2]", "neither"), (b"", "neither"), (b"hello", "neither"),
                          (b'{"grid": 5}', "no size"), (b'{"body": [1]}', "cannot read"),
                          (json.dumps(xword_json(["A" * 22] * 2, ["a"] * 2, ["a"] * 22,
                                                 date="1993-11-21")).encode(), "21x21")):
            with self.subTest(data=data[:20]):
                with self.assertRaisesRegex(ValueError, why):
                    crossword.convert(data, "1993-11-21.json")
        with self.assertRaisesRegex(ValueError, "not a crossword"):
            crossword.convert(b" " * (crossword.MAX_IN + 1), "x.json")

    def test_a_byte_order_mark_is_forgiven(self):
        data = b"\xef\xbb\xbf" + json.dumps(xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)).encode()
        self.assertEqual(crossword.to_text(crossword.convert(data, "x.json")), FIX)


# ---- the library ---------------------------------------------------------------------------------------

class Library(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        crossword._invalidate()

    def tearDown(self):
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def fill(self, n, first="2020-01-01"):
        day = datetime.date.fromisoformat(first)
        for i in range(n):
            crossword.add(small((day + datetime.timedelta(days=i)).isoformat()))

    def test_a_puzzle_is_kept_under_its_print_date(self):
        pz = crossword.parse_text(FIX)
        self.assertEqual(crossword.add(pz), "added")
        path = os.path.join(self.dir, "crosswords", "1993-11-21.xw")
        with open(path, encoding="ascii") as f:
            self.assertEqual(f.read(), FIX)
        if os.name == "posix":
            self.assertEqual(os.stat(path).st_mode & 0o077, 0)        # nobody else's business
        self.assertEqual(crossword.load("1993-11-21"), FIX)
        self.assertIsNone(crossword.load("1993-11-22"))
        self.assertEqual([n for n in os.listdir(os.path.dirname(path)) if n.endswith(".tmp")], [])

    def test_a_second_copy_is_left_alone_unless_asked(self):
        crossword.add(small("2020-01-05", title="First"))
        self.assertEqual(crossword.add(small("2020-01-05", title="Second")), "duplicate")
        self.assertIn("title First", crossword.load("2020-01-05"))
        self.assertEqual(crossword.add(small("2020-01-05", title="Second"), replace=True), "replaced")
        self.assertIn("title Second", crossword.load("2020-01-05"))
        self.assertEqual([e["title"] for e in crossword.index()], ["Second"])

    def test_a_dry_run_says_what_would_happen_and_writes_nothing(self):
        pz = small("2020-01-05")
        self.assertEqual(crossword.add(pz, dry=True), "added")
        self.assertEqual(crossword.index(), [])
        self.assertFalse(os.path.exists(os.path.join(self.dir, "crosswords")))
        crossword.add(pz)
        self.assertEqual(crossword.add(pz, dry=True), "duplicate")
        self.assertEqual(crossword.add(pz, replace=True, dry=True), "replaced")
        self.assertEqual(crossword.import_bytes(make_puz(upper(FIX_ROWS), FIX_PUZ_ORDER), "1993-11-21.puz",
                                                dry=True), ("1993-11-21", "added"))
        self.assertEqual([e["id"] for e in crossword.index()], ["2020-01-05"])

    def test_a_damaged_file_is_no_duplicate(self):
        root = os.path.join(self.dir, "crosswords")
        os.makedirs(root)
        with open(os.path.join(root, "2020-01-05.xw"), "w") as f:
            f.write("")
        self.assertEqual(crossword.add(small("2020-01-05"), dry=True), "replaced")
        self.assertEqual(crossword.add(small("2020-01-05")), "replaced")
        self.assertIn("title Puzzle 05", crossword.load("2020-01-05"))
        self.assertEqual(crossword.add(small("2020-01-05")), "duplicate")

    def test_a_puzzle_without_a_date_cannot_be_kept(self):
        pz = small("2020-01-05")
        pz["date"] = ""
        with self.assertRaisesRegex(ValueError, "print date"):
            crossword.add(pz)
        pz["date"] = "05/01/2020"
        with self.assertRaises(ValueError):
            crossword.add(pz)
        self.assertEqual(crossword.index(), [])

    def test_what_the_device_would_refuse_is_not_kept(self):
        pz = small("2020-01-05")
        pz["across"] = [(1, "a1")]                       # a clue short
        with self.assertRaises(ValueError):
            crossword.add(pz)
        self.assertEqual(crossword.index(), [])

    def test_ids_cannot_leave_the_folder(self):
        for bad in ("", "../x", "a/b", "a\\b", ".hidden", "x" * 41, "a b"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    crossword.puzzle_path(bad)
                with self.assertRaises(ValueError):
                    crossword.load(bad)

    def test_removing(self):
        self.fill(2)
        self.assertTrue(crossword.remove("2020-01-01"))
        self.assertFalse(crossword.remove("2020-01-01"))
        self.assertEqual([e["id"] for e in crossword.index()], ["2020-01-02"])

    def test_the_index_has_what_a_list_shows(self):
        crossword.add(crossword.parse_text(FIX))
        crossword.add(small("2026-10-27"))
        got = crossword.index()
        self.assertEqual([e["id"] for e in got], ["1993-11-21", "2026-10-27"])
        e = got[0]
        self.assertEqual((e["title"], e["date"], e["weekday"], e["w"], e["h"]),
                         ("Mini Heart", "1993-11-21", 6, 5, 5))
        self.assertEqual(got[1]["weekday"], 1)                        # a Tuesday

    def test_a_puzzle_copied_in_by_hand_appears_and_a_damaged_one_does_not(self):
        self.fill(1)
        self.assertEqual(len(crossword.index()), 1)                   # now cached
        root = os.path.join(self.dir, "crosswords")
        with open(os.path.join(root, "1999-12-31.xw"), "w") as f:
            f.write(FIX)
        with open(os.path.join(root, "2000-01-01.xw"), "w") as f:
            f.write("garbage\n")
        with open(os.path.join(root, "notes.txt"), "w") as f:
            f.write(FIX)
        self.assertEqual([e["id"] for e in crossword.index()], ["1999-12-31", "2020-01-01"])
        with self.assertRaisesRegex(RuntimeError, "damaged"):
            crossword.load("2000-01-01")

    def test_an_empty_library_is_an_empty_list(self):
        self.assertEqual(crossword.index(), [])
        self.assertEqual(crossword.select(), [])

    def test_select_filters_orders_and_pages(self):
        self.fill(20)                                                  # Jan 1 (a Wednesday) to Jan 20
        ids = lambda **kw: [e["id"] for e in crossword.select(**kw)]
        newest = ids()
        self.assertEqual(newest[0], "2020-01-20")
        self.assertEqual(newest[-1], "2020-01-01")
        self.assertEqual(len(newest), 20)
        self.assertEqual(ids(order="old", limit=3), ["2020-01-01", "2020-01-02", "2020-01-03"])
        self.assertEqual(ids(weekdays={5}), ["2020-01-18", "2020-01-11", "2020-01-04"])      # Saturdays
        self.assertEqual(ids(weekdays={5, 6}, limit=3), ["2020-01-19", "2020-01-18", "2020-01-12"])
        self.assertEqual(ids(lo="2020-01-10", hi="2020-01-12"), ["2020-01-12", "2020-01-11", "2020-01-10"])
        self.assertEqual(ids(lo="2020-01-19"), ["2020-01-20", "2020-01-19"])
        self.assertEqual(ids(hi="2020-01-02"), ["2020-01-02", "2020-01-01"])
        self.assertEqual(ids(lo="2021-01-01"), [])
        # Following the pages to the end meets everything once.
        seen, after = [], ""
        while True:
            page = ids(limit=7, after=after)
            if not page:
                break
            seen += page
            after = page[-1]
        self.assertEqual(seen, newest)
        seen, after = [], ""
        while True:
            page = ids(limit=6, after=after, order="old")
            if not page:
                break
            seen += page
            after = page[-1]
        self.assertEqual(seen, newest[::-1])
        # A cursor need not be an id that is there.
        self.assertEqual(ids(after="2020-01-10", limit=2), ["2020-01-09", "2020-01-08"])
        self.assertEqual(ids(after="2020-01-10x", limit=1), ["2020-01-10"])

    def test_a_page_is_never_longer_than_the_limit(self):
        self.fill(crossword.LIST_MAX + 10)
        self.assertEqual(len(crossword.select(limit=10 ** 6)), crossword.LIST_MAX)
        self.assertEqual(len(crossword.select(limit=0)), 1)

    def test_import_does_the_whole_job(self):
        puz = make_puz(upper(FIX_ROWS), FIX_PUZ_ORDER, title="Mini Heart", author="A. Setter",
                       notes="Circled letter: U", circled={12})
        self.assertEqual(crossword.import_bytes(puz, "nyt/1993-11-21.puz"), ("1993-11-21", "added"))
        self.assertEqual(crossword.load("1993-11-21"), FIX)
        self.assertEqual(crossword.import_bytes(puz, "nyt/1993-11-21.puz"), ("1993-11-21", "duplicate"))
        j = json.dumps(xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN)).encode()
        self.assertEqual(crossword.import_bytes(j, "x.json"), ("1993-11-21", "duplicate"))
        self.assertEqual(crossword.import_bytes(j, "x.json", replace=True), ("1993-11-21", "replaced"))
        j2 = json.dumps(nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN, publicationDate="1993-11-28")).encode()
        self.assertEqual(crossword.import_bytes(j2, "y.json"), ("1993-11-28", "added"))
        self.assertEqual(crossword.import_bytes(j2, "z.json", date="1993-12-05"), ("1993-12-05", "added"))
        self.assertEqual(len(crossword.index()), 3)

    def test_import_a_file_on_disk(self):
        folder = tempfile.mkdtemp()
        try:
            good = os.path.join(folder, "1993-11-21.json")
            with open(good, "w", encoding="utf-8") as f:
                json.dump(xword_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN), f)
            self.assertEqual(crossword.import_file(good), ("1993-11-21", "added"))
            bad = os.path.join(folder, "1993-11-22.json")
            with open(bad, "w") as f:
                f.write("{not json")
            with self.assertRaisesRegex(ValueError, "does not parse"):
                crossword.import_file(bad)
            with self.assertRaisesRegex(ValueError, "cannot read it"):
                crossword.import_file(os.path.join(folder, "nothing.puz"))
            self.assertEqual([e["id"] for e in crossword.index()], ["1993-11-21"])
        finally:
            shutil.rmtree(folder, ignore_errors=True)


# ---- over HTTP ----------------------------------------------------------------------------------------------

class Midi(unittest.TestCase):
    """The NYT Midi from nytmidi.org: today's and the week's, kept as DATE-midi."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        crossword._invalidate()
        self.asked = []

    def tearDown(self):
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def site(self, today, missing=()):
        def fetch(url):
            self.asked.append(url)
            name = url[len(crossword.MIDI_URL):-len(".json")]
            day = today if name == "today" else name
            if day in missing:
                raise urllib.error.HTTPError(url, 404, "Not Found", {}, None)
            return nyt_json(FIX_ROWS, FIX_ACROSS, FIX_DOWN, publicationDate=day, title="Heart")
        return fetch

    def ids(self):
        return [e["id"] for e in crossword.index()]

    def test_today_and_the_week_before_are_kept_beside_the_dailies(self):
        crossword.add(small("2026-10-27"))
        added, removed = crossword.midi_refresh(self.site("2026-10-27"), log=lambda s: None)
        self.assertEqual(len(added), 7)
        self.assertEqual(removed, [])
        self.assertEqual(self.ids(), ["2026-10-21-midi", "2026-10-22-midi", "2026-10-23-midi",
                                      "2026-10-24-midi", "2026-10-25-midi", "2026-10-26-midi",
                                      "2026-10-27", "2026-10-27-midi"])
        self.assertIn("title Midi: Heart\n", crossword.load("2026-10-27-midi"))
        self.assertNotIn("Midi", crossword.load("2026-10-27"))    # the daily is untouched

    def test_the_next_day_adds_one_and_drops_the_oldest(self):
        crossword.midi_refresh(self.site("2026-10-27"), log=lambda s: None)
        self.asked.clear()
        added, removed = crossword.midi_refresh(self.site("2026-10-28"), log=lambda s: None)
        self.assertEqual(added, ["2026-10-28-midi"])
        self.assertEqual(removed, ["2026-10-21-midi"])
        self.assertEqual(self.asked, [crossword.MIDI_URL + "today.json"])  # nothing fetched twice
        self.assertEqual(len(self.ids()), 7)

    def test_a_day_the_site_lacks_is_skipped_and_asked_again_later(self):
        said = []
        added, _ = crossword.midi_refresh(self.site("2026-10-27", missing={"2026-10-24"}), log=said.append)
        self.assertNotIn("2026-10-24-midi", added)
        self.assertEqual(len(added), 6)
        self.assertTrue(any("2026-10-24" in s for s in said))
        added, _ = crossword.midi_refresh(self.site("2026-10-27"), log=lambda s: None)
        self.assertEqual(added, ["2026-10-24-midi"])

    def test_only_midis_are_dropped(self):
        crossword.add(small("2020-01-01"))
        crossword.midi_refresh(self.site("2026-10-27"), log=lambda s: None)
        self.assertIn("2020-01-01", self.ids())


def wordhint_page(rows, across, down, day="October 27, 2026", by="A. Setter", answers=None):
    """A page in wordhint.net's markup: a button a white square, its letter in
    the last span, and the Across and Down tables of number, clue, answer."""
    w = len(rows[0])
    cells = []
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == ".":
                cells.append('<div class="bg-slate-800"></div>')
            else:
                cells.append('<button type="button" aria-label="Reveal letter at row %d, column %d" class="x">'
                             '<span aria-hidden="true">1</span><span class="opacity-0">%s</span></button>'
                             % (r + 1, c + 1, ch.upper()))
    a, d = crossword.entries(upper(rows))

    def table(name, ents, texts, dr, dc):
        out = ['<h3 class="mb-2">%s</h3><table class="w-full"><thead><tr><th>#</th><th>Clue</th>'
               '<th>Answer</th></tr></thead><tbody>' % name]
        for (n, r, c, ln), t in zip(ents, texts):
            ans = (answers or {}).get((name, n)) or "".join(upper(rows)[r + k * dr][c + k * dc] for k in range(ln))
            out.append('<tr class="b"><td class="p">%d</td><td class="p"><a href="/x/">%s</a></td>'
                       '<td class="p"><strong class="f">%s</strong></td></tr>' % (n, t.replace('"', "&quot;"), ans))
        return "".join(out) + "</tbody></table>"

    return ('<html><head><title>NYT Crossword Answers Today</title></head><body>'
            '<p class="byline">By %s</p>'
            '<div class="grid" style="grid-template-columns:repeat(%d, minmax(0, 1fr))">%s</div>'
            '<p>The full solution for today\'s puzzle (%s) is listed.</p>%s%s</body></html>'
            % (by, w, "".join(cells), day, table("Across", a, across, 0, 1), table("Down", d, down, 1, 0)))


class Wordhint(unittest.TestCase):
    """wordhint.net's NYT answers page: the grid and clues from its markup."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        crossword._invalidate()

    def tearDown(self):
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_page_is_the_puzzle(self):
        pz = crossword.from_wordhint(wordhint_page(FIX_ROWS, FIX_ACROSS, FIX_DOWN))
        self.assertEqual(pz["rows"], upper(FIX_ROWS))
        self.assertEqual(pz["date"], "2026-10-27")
        self.assertEqual(pz["by"], "A. Setter")
        self.assertEqual(pz["title"], "Daily: A. Setter")
        self.assertEqual([t for _, t in pz["across"]], FIX_ACROSS)
        self.assertEqual([t for _, t in pz["down"]], FIX_DOWN)

    def test_a_challenge_page_is_refused(self):
        with self.assertRaises(ValueError):
            crossword.from_wordhint("<html><title>Just a moment...</title></html>")

    def test_a_grid_that_disagrees_with_its_answers_is_refused(self):
        page = wordhint_page(FIX_ROWS, FIX_ACROSS, FIX_DOWN, answers={("Across", 1): "HEAT"})
        with self.assertRaisesRegex(ValueError, "1-Across"):
            crossword.from_wordhint(page)

    def test_put_keeps_a_week_of_the_series_only(self):
        crossword.add(small("2026-10-01"))
        for day in ("2026-10-20", "2026-10-21", "2026-10-27"):
            pz = crossword.from_wordhint(wordhint_page(FIX_ROWS, FIX_ACROSS, FIX_DOWN,
                                                       day=datetime.date.fromisoformat(day).strftime("%B %d, %Y")))
            status, removed = crossword.put_text(crossword.to_text(pz), day + "-nyt", keep=7)
        self.assertEqual(status, "added")
        self.assertEqual(removed, ["2026-10-20-nyt"])
        self.assertEqual([e["id"] for e in crossword.index()],
                         ["2026-10-01", "2026-10-21-nyt", "2026-10-27-nyt"])


class Routes(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        crossword._invalidate()
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        day = datetime.date(2020, 1, 1)
        for i in range(20):
            crossword.add(small((day + datetime.timedelta(days=i)).isoformat()))

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def req(self, path, token=TOKEN):
        headers = {"Authorization": "Bearer " + token} if token else {}
        r = urllib.request.Request(self.base + path, headers=headers)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def ids(self, query):
        s, text = self.req("/crossword/list" + query)
        self.assertEqual(s, 200, text)
        return [line.split("\t")[0] for line in text.splitlines()]

    def test_the_list_is_a_puzzle_a_line_newest_first(self):
        s, text = self.req("/crossword/list")
        self.assertEqual(s, 200)
        lines = text.splitlines()
        self.assertEqual(len(lines), 20)
        self.assertEqual(lines[0], "2020-01-20\t2020-01-20\tMon\t2x2\tPuzzle 20")
        self.assertEqual(lines[-1], "2020-01-01\t2020-01-01\tWed\t2x2\tPuzzle 01")
        self.assertEqual(text.count("\t"), 4 * 20)

    def test_a_long_title_is_cut_in_the_list_not_in_the_puzzle(self):
        crossword.add(small("2021-05-05", title="Q" * 60))
        s, text = self.req("/crossword/list?limit=1")
        self.assertEqual(text.splitlines()[0].split("\t")[4], "Q" * crossword.LIST_TITLE)
        s, puzzle = self.req("/crossword/puzzle?id=2021-05-05")
        self.assertIn("title " + "Q" * 60 + "\n", puzzle)

    def test_the_list_can_be_filtered_and_paged(self):
        self.assertEqual(self.ids("?weekday=sat"), ["2020-01-18", "2020-01-11", "2020-01-04"])
        self.assertEqual(self.ids("?weekday=5"), ["2020-01-18", "2020-01-11", "2020-01-04"])
        self.assertEqual(self.ids("?weekday=Saturday,sun&limit=2"), ["2020-01-19", "2020-01-18"])
        self.assertEqual(self.ids("?from=2020-01-10&to=2020-01-12"),
                         ["2020-01-12", "2020-01-11", "2020-01-10"])
        self.assertEqual(len(self.ids("?from=2020-01&to=2020-01")), 20)           # the whole month
        self.assertEqual(self.ids("?from=2020-02"), [])
        self.assertEqual(len(self.ids("?to=2020")), 20)                           # to the year's end
        self.assertEqual(self.ids("?to=2019-12"), [])
        self.assertEqual(self.ids("?order=old&limit=2"), ["2020-01-01", "2020-01-02"])
        first = self.ids("?limit=5")
        self.assertEqual(first, ["2020-01-20", "2020-01-19", "2020-01-18", "2020-01-17", "2020-01-16"])
        self.assertEqual(self.ids("?limit=5&after=" + first[-1]),
                         ["2020-01-15", "2020-01-14", "2020-01-13", "2020-01-12", "2020-01-11"])
        self.assertEqual(self.ids("?limit=5&order=old&after=2020-01-19"), ["2020-01-20"])
        self.assertEqual(self.ids("?after=2020-01-01"), [])                       # the end is an empty reply
        self.assertEqual(self.req("/crossword/list?from=2021")[1], "")

    def test_a_page_of_the_biggest_size_fits_the_devices_buffer(self):
        day = datetime.date(2019, 1, 1)
        for i in range(90):
            crossword.add(small((day + datetime.timedelta(days=i)).isoformat(), title="T" * 63))
        s, text = self.req("/crossword/list?limit=999")
        self.assertEqual(len(text.splitlines()), crossword.LIST_MAX)
        self.assertLess(len(text), 6000)

    def test_a_bad_question_is_a_400_in_words(self):
        for q, why in (("?limit=x", "limit="), ("?weekday=xyz", "weekday="), ("?weekday=7", "weekday="),
                       ("?from=19", "from="), ("?to=1993-1", "to="), ("?from=1993-13", "not a date"),
                       ("?order=sideways", "order="), ("?after=../x", "after=")):
            with self.subTest(q=q):
                s, text = self.req("/crossword/list" + q)
                self.assertEqual(s, 400, text)
                self.assertTrue(text.startswith("error "), text)
                self.assertIn(why, text)

    def test_a_puzzle_is_the_text_the_device_parses(self):
        crossword.add(crossword.parse_text(FIX))
        s, text = self.req("/crossword/puzzle?id=1993-11-21")
        self.assertEqual((s, text), (200, FIX))
        self.assertEqual(crossword.parse_text(text)["rows"], FIX_ROWS)

    def test_no_such_puzzle_and_bad_ids(self):
        self.assertEqual(self.req("/crossword/puzzle?id=1993-11-21"), (404, "error no such puzzle\n"))
        s, text = self.req("/crossword/puzzle")
        self.assertEqual(s, 400)
        self.assertIn("id=", text)
        self.assertEqual(self.req("/crossword/puzzle?id=..%2Fx")[0], 400)
        self.assertEqual(self.req("/crossword/puzzle?id=a%2Fb")[0], 400)

    def test_the_library_is_behind_the_token(self):
        self.assertEqual(self.req("/crossword/list", token=None)[0], 403)
        self.assertEqual(self.req("/crossword/puzzle?id=2020-01-01", token="wrong")[0], 403)

    def test_the_server_lists_the_routes(self):
        s, text = self.req("/", token=None)
        self.assertEqual(s, 200)
        self.assertIn("/crossword/list", text)
        self.assertIn("/crossword/puzzle", text)


if __name__ == "__main__":
    unittest.main()
