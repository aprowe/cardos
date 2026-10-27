"""Loading a folder of downloaded crosswords: `python -m server.crossword
import` and tools/crossword_import.py.

What a person running it relies on: every .puz and JSON file under the folder
(or in a .zip) is filed under its print date, a puzzle already there is
skipped, a file that is not a puzzle is listed with the reason while the rest
carry on, the folders above the one given are never mistaken for a date, a
date that contradicts the weekday the file states is refused, a dry run
writes nothing, and the exit status says whether anything could not be read.

    python -m server.tests.test_crossword_import
"""
import contextlib
import datetime
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from server import crossword
from server.tests import test_crossword as fx

MONTHS = ("January", "February", "March", "April", "May", "June", "July", "August",
          "September", "October", "November", "December")


def title_for(day):
    """The title the Times' own .puz files carry: NY Times, Sunday, November 21, 1993."""
    d = datetime.date.fromisoformat(day)
    return "NY Times, %s, %s %d, %d" % (crossword.FULL_DAYS[d.weekday()], MONTHS[d.month - 1],
                                        d.day, d.year)


def json_for(day, **kw):
    """The fixture puzzle as XWord Info's JSON, dated, with the weekday it says right."""
    d = datetime.date.fromisoformat(day)
    j = fx.xword_json(fx.FIX_ROWS, fx.FIX_ACROSS, fx.FIX_DOWN, date=day,
                      dow=crossword.FULL_DAYS[d.weekday()])
    j.update(kw)
    return j


def undated_json():
    """... with no date in it, and no weekday said."""
    j = fx.xword_json(fx.FIX_ROWS, fx.FIX_ACROSS, fx.FIX_DOWN, dow="")
    del j["date"]
    return j


def puz_for(title="Mini Heart", **kw):
    """The fixture puzzle as a .puz."""
    return fx.make_puz(fx.upper(fx.FIX_ROWS), fx.FIX_PUZ_ORDER, title=title, author="A. Setter",
                       notes="Circled letter: U", circled={12}, **kw)


class Folder(unittest.TestCase):
    """A folder of downloads and a library, both in temporary places."""

    def setUp(self):
        self.state = tempfile.mkdtemp()
        self.src = tempfile.mkdtemp()
        self._env = os.environ.get("CARDOS_STATE")
        os.environ["CARDOS_STATE"] = self.state
        crossword._invalidate()

    def tearDown(self):
        if self._env is None:
            os.environ.pop("CARDOS_STATE", None)
        else:
            os.environ["CARDOS_STATE"] = self._env
        shutil.rmtree(self.state, ignore_errors=True)
        shutil.rmtree(self.src, ignore_errors=True)

    def put(self, rel, data):
        path = os.path.join(self.src, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def put_json(self, rel, j):
        return self.put(rel, json.dumps(j).encode("utf-8"))

    def ids(self):
        return [e["id"] for e in crossword.index()]


class BulkImport(Folder):
    def test_a_folder_is_read_all_the_way_down_and_each_puzzle_filed_by_its_date(self):
        self.put_json("a/1993-11-21.json", json_for("1993-11-21"))
        self.put("b/Nov2893.puz", puz_for())                                    # the name has the date
        self.put("c/monday.puz", puz_for(title=title_for("1993-11-22")))        # the title has it
        self.put("README.txt", b"hello")
        self.put(".DS_Store", b"\x00")
        self.put("__MACOSX/a/._1993-11-21.json", b"junk")
        self.put("c/._monday.puz", b"junk")
        r = crossword.bulk_import([self.src])
        self.assertEqual(r.counts, {"added": 3, "replaced": 0, "duplicate": 0, "failed": 0})
        self.assertEqual(r.ignored, 4)
        self.assertEqual({row.id: (row.weekday, row.size) for row in r.rows},
                         {"1993-11-21": ("Sun", "5x5"), "1993-11-28": ("Sun", "5x5"),
                          "1993-11-22": ("Mon", "5x5")})
        self.assertEqual(self.ids(), ["1993-11-21", "1993-11-22", "1993-11-28"])
        self.assertIn("date 1993-11-28\n", crossword.load("1993-11-28"))

    def test_only_the_path_below_the_folder_given_is_looked_in_for_a_date(self):
        # The download folder is called 20200101, which is not when the puzzle ran.
        self.put_json("20200101/inner/x.json", undated_json())
        r = crossword.bulk_import([os.path.join(self.src, "20200101", "inner")])
        self.assertEqual(r.counts["failed"], 1)
        self.assertIn("print date", r.rows[0].why)
        self.assertEqual(self.ids(), [])
        # From above it, the folder is part of the puzzle's own path.
        r = crossword.bulk_import([self.src])
        self.assertEqual([(row.status, row.id, row.weekday) for row in r.rows],
                         [("added", "2020-01-01", "Wed")])

    def test_duplicates_are_skipped_and_replaced_on_request(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put("1993-11-21.puz", puz_for())                      # the same puzzle, the other way
        self.put_json("1993-11-28.json", json_for("1993-11-28"))
        r = crossword.bulk_import([self.src])
        self.assertEqual((r.counts["added"], r.counts["duplicate"]), (2, 1))
        self.assertEqual([row.status for row in r.rows], ["added", "duplicate", "added"])
        again = crossword.bulk_import([self.src])
        self.assertEqual(again.counts, {"added": 0, "replaced": 0, "duplicate": 3, "failed": 0})
        over = crossword.bulk_import([self.src], replace=True)
        self.assertEqual(over.counts, {"added": 0, "replaced": 3, "duplicate": 0, "failed": 0})
        self.assertEqual(self.ids(), ["1993-11-21", "1993-11-28"])

    def test_what_cannot_be_read_is_reported_with_the_reason_and_the_rest_goes_on(self):
        self.put_json("good/1993-11-21.json", json_for("1993-11-21"))
        self.put("bad/broken.json", b"{not json")
        self.put("bad/garbage.puz", b"this is not a puzzle at all")
        self.put("bad/empty.json", b"")
        self.put_json("bad/nodate.json", undated_json())
        self.put_json("bad/1993-11-22.json", json_for("1993-11-22", dow="Sunday"))   # a Monday
        self.put("bad/1993-11-23.puz", puz_for(scrambled=4))
        self.put_json("bad/notxword.json", {"hello": 1})
        r = crossword.bulk_import([self.src])
        self.assertEqual(r.counts, {"added": 1, "replaced": 0, "duplicate": 0, "failed": 7})
        why = {os.path.basename(row.file): row.why for row in r.failed()}
        self.assertIn("does not parse", why["broken.json"])
        self.assertIn("neither", why["garbage.puz"])
        self.assertIn("neither", why["empty.json"])
        self.assertIn("print date", why["nodate.json"])
        self.assertIn("which was a Monday, but the file says Sunday", why["1993-11-22.json"])
        self.assertIn("scrambled", why["1993-11-23.puz"])
        self.assertIn("not a crossword", why["notxword.json"])
        self.assertEqual(self.ids(), ["1993-11-21"])

    def test_a_bug_in_one_reader_does_not_end_the_run(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put_json("1993-11-28.json", json_for("1993-11-28"))
        real = crossword.convert
        calls = []

        def broken(data, name="", date=""):
            calls.append(name)
            if len(calls) == 1:
                raise TypeError("surprise")
            return real(data, name, date)

        with mock.patch.object(crossword, "convert", broken):
            r = crossword.bulk_import([self.src])
        self.assertEqual([row.status for row in r.rows], ["failed", "added"])
        self.assertIn("unexpected TypeError: surprise", r.rows[0].why)

    def test_a_dry_run_writes_nothing_and_says_what_would_happen(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put("1993-11-21.puz", puz_for())
        self.put_json("1993-11-28.json", json_for("1993-11-28"))
        crossword.add(fx.small("1993-11-28"))
        r = crossword.bulk_import([self.src], dry_run=True)
        self.assertEqual([row.status for row in r.rows], ["added", "duplicate", "duplicate"])
        self.assertEqual(self.ids(), ["1993-11-28"])                           # still only the one
        self.assertNotIn("1993-11-21.xw", os.listdir(os.path.join(self.state, "crosswords")))
        r = crossword.bulk_import([self.src], dry_run=True, replace=True)
        self.assertEqual([row.status for row in r.rows], ["added", "replaced", "replaced"])
        self.assertEqual(self.ids(), ["1993-11-28"])
        self.assertIn("title Puzzle 28", crossword.load("1993-11-28"))         # not replaced

    def test_zip_archives_are_read_in_place(self):
        z = os.path.join(self.src, "dl", "archive.zip")
        os.makedirs(os.path.dirname(z))
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("1993/", "")                                           # a folder entry
            zf.writestr("1993/11/21.json", json.dumps(undated_json()))         # the date is in the path
            zf.writestr("1993/11/28.json", json.dumps(json_for("1993-11-28")))
            zf.writestr("__MACOSX/1993/11/._21.json", "junk")
            zf.writestr("readme.txt", "hi")
            zf.writestr("nested.zip", b"PK")
        r = crossword.bulk_import([z])
        self.assertEqual([(row.status, row.id) for row in r.rows],
                         [("added", "1993-11-21"), ("added", "1993-11-28")])
        self.assertTrue(r.rows[0].file.endswith("archive.zip:1993/11/21.json"), r.rows[0].file)
        self.assertEqual(r.ignored, 3)
        # Found by walking a folder, it is read the same, and now they are there.
        again = crossword.bulk_import([self.src])
        self.assertEqual(again.counts["duplicate"], 2)

    def test_a_zip_that_is_not_one_and_a_member_too_big_are_failures_not_crashes(self):
        self.put("bad.zip", b"this is not a zip file")
        z = os.path.join(self.src, "big.zip")
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("1993-11-21.json", b" " * (crossword.MAX_IN + 1))      # small to store, big to hold
            zf.writestr("1993-11-28.json", json.dumps(json_for("1993-11-28")))
        r = crossword.bulk_import([self.src])
        by_file = {os.path.basename(row.file): row for row in r.rows}
        self.assertEqual(by_file["bad.zip"].status, "failed")
        self.assertIn("not a readable zip file", by_file["bad.zip"].why)
        self.assertEqual(by_file["big.zip:1993-11-21.json"].status, "failed")
        self.assertIn("not a crossword", by_file["big.zip:1993-11-21.json"].why)
        self.assertEqual(by_file["big.zip:1993-11-28.json"].status, "added")

    def test_a_file_named_outright_is_tried_whatever_it_is_called(self):
        named = self.put("download (3)", puz_for(title=title_for("1993-11-21")))
        r = crossword.bulk_import([named])
        self.assertEqual([(row.status, row.id) for row in r.rows], [("added", "1993-11-21")])
        # But in a folder that is walked, a file of that name is not even looked at.
        self.put("sub/download (4)", puz_for(title=title_for("1993-11-28")))
        r = crossword.bulk_import([os.path.join(self.src, "sub")])
        self.assertEqual((r.rows, r.ignored), ([], 1))

    def test_a_date_asked_for_is_the_files_date(self):
        f = self.put("one.puz", puz_for(title=title_for("1993-11-21")))
        r = crossword.bulk_import([f], date="1993-12-05")
        self.assertEqual([(row.status, row.id, row.weekday) for row in r.rows],
                         [("added", "1993-12-05", "Sun")])

    def test_each_file_is_logged_as_it_is_done_and_an_interruption_keeps_the_report(self):
        for day in ("1993-11-21", "1993-11-28", "1993-12-05"):
            self.put_json(day + ".json", json_for(day))
        seen = []
        r = crossword.bulk_import([self.src], log=seen.append)
        self.assertEqual(seen, r.rows)
        shutil.rmtree(os.path.join(self.state, "crosswords"))
        crossword._invalidate()
        real = crossword.convert
        calls = []

        def interrupted(data, name="", date=""):
            calls.append(name)
            if len(calls) == 3:
                raise KeyboardInterrupt
            return real(data, name, date)

        rep = crossword.Report()
        with mock.patch.object(crossword, "convert", interrupted):
            with self.assertRaises(KeyboardInterrupt):
                crossword.bulk_import([self.src], report=rep)
        self.assertEqual([row.id for row in rep.rows], ["1993-11-21", "1993-11-28"])
        self.assertEqual(self.ids(), ["1993-11-21", "1993-11-28"])             # what was done stays done


class Reporting(unittest.TestCase):
    def test_lines_and_summaries(self):
        added = crossword.Row("added", "1993-11-21", "Sun", "15x15", "dump/a.puz", "")
        failed = crossword.Row("failed", "", "", "", "dump/b.json", "no print date")
        self.assertEqual(crossword.format_row(added).split(), ["added", "1993-11-21", "Sun", "15x15", "dump/a.puz"])
        self.assertEqual(crossword.format_row(added, dry_run=True).split()[:2], ["would", "add"])
        self.assertEqual(crossword.format_row(failed), "FAILED        dump/b.json: no print date")
        r = crossword.Report()
        r.add(added)
        r.add(failed)
        r.add(crossword.Row("duplicate", "1993-11-22", "Mon", "15x15", "c", ""))
        self.assertEqual(crossword.format_summary(r), "1 added, 1 already there, 1 could not be read")
        self.assertEqual(crossword.format_summary(r, dry_run=True),
                         "1 would be added, 1 already there, 1 could not be read")
        r.add(crossword.Row("replaced", "1993-11-23", "Tue", "15x15", "d", ""))
        r.ignored = 4
        self.assertEqual(crossword.format_summary(r),
                         "1 added, 1 replaced, 1 already there, 1 could not be read (4 other files ignored)")
        self.assertEqual([x.file for x in r.failed()], ["dump/b.json"])


class Cli(Folder):
    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        code = crossword.main(["import"] + list(argv), out, err)
        return code, out.getvalue(), err.getvalue()

    def lib(self):
        return os.path.join(self.state, "crosswords")

    def test_a_run_says_where_it_put_them_and_how_many(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put_json("1993-11-28.json", json_for("1993-11-28"))
        code, out, err = self.run_cli(self.src)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out.splitlines(), ["library: %s (0 puzzles)" % self.lib(),
                                            "2 added, 0 already there, 0 could not be read",
                                            "library: 2 puzzles"])
        code, out, err = self.run_cli(self.src)                                 # again: nothing new
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[1], "0 added, 2 already there, 0 could not be read")

    def test_verbose_names_every_file_by_date_and_weekday(self):
        f = self.put_json("1993-11-21.json", json_for("1993-11-21"))
        code, out, _ = self.run_cli(self.src, "-v")
        line = [ln for ln in out.splitlines() if f in ln][0]
        self.assertEqual(line.split(), ["added", "1993-11-21", "Sun", "5x5", f])

    def test_quiet_is_the_summary_alone(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put("bad.json", b"{no")
        code, out, _ = self.run_cli(self.src, "-q")
        self.assertEqual(code, 1)
        self.assertEqual(out.splitlines(), ["1 added, 0 already there, 1 could not be read"])

    def test_what_could_not_be_read_is_listed_at_the_end_and_the_status_says_so(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        bad = self.put("bad.json", b"{no")
        code, out, _ = self.run_cli(self.src)
        self.assertEqual(code, 1)
        lines = out.splitlines()
        self.assertIn("1 added, 0 already there, 1 could not be read", lines)
        self.assertIn("could not be read:", lines)
        self.assertTrue(any(ln.startswith("  " + bad + ": JSON that does not parse") for ln in lines), lines)
        self.assertEqual(lines[-1], "library: 1 puzzles")

    def test_the_list_of_failures_is_capped(self):
        for i in range(3):
            self.put("bad%d.json" % i, b"{no")
        with mock.patch.object(crossword, "RECAP_MAX", 2):
            code, out, _ = self.run_cli(self.src)
        self.assertEqual(code, 1)
        self.assertEqual(sum(1 for ln in out.splitlines() if ln.startswith("  ") and "bad" in ln), 2)
        self.assertIn("  ... and 1 more", out.splitlines())

    def test_a_dry_run(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        code, out, _ = self.run_cli(self.src, "--dry-run")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["library: %s (0 puzzles)" % self.lib(),
                                            "1 would be added, 0 already there, 0 could not be read"])
        self.assertEqual(self.ids(), [])
        code, out, _ = self.run_cli(self.src, "--dry-run", "-v")
        self.assertIn("would add", out)

    def test_replace(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.run_cli(self.src)
        code, out, _ = self.run_cli(self.src, "--replace")
        self.assertEqual(out.splitlines()[1], "0 added, 1 replaced, 0 already there, 0 could not be read")

    def test_progress_is_shown_every_so_many_files(self):
        for i in range(5):                                         # five Sundays
            day = (datetime.date(1993, 11, 7) + datetime.timedelta(days=7 * i)).isoformat()
            self.put_json(day + ".json", json_for(day))
        with mock.patch.object(crossword, "PROGRESS_EVERY", 2):
            code, out, _ = self.run_cli(self.src)
        self.assertEqual(code, 0)
        self.assertIn("2 files...", out.splitlines())
        self.assertIn("4 files...", out.splitlines())
        self.assertNotIn("5 files...", out.splitlines())

    def test_nothing_to_import_and_no_such_place_are_status_2(self):
        code, out, err = self.run_cli(os.path.join(self.src, "nowhere"))
        self.assertEqual(code, 2)
        self.assertIn("no such file or folder", err)
        code, out, err = self.run_cli(self.src)                                 # empty
        self.assertEqual(code, 2)
        self.assertIn("no .puz or .json files under " + self.src, err)
        self.put("README.txt", b"hi")
        code, out, err = self.run_cli(self.src)
        self.assertEqual(code, 2)
        self.assertIn("(1 other files ignored)", err)
        self.assertEqual(self.ids(), [])

    def test_the_date_option_is_for_one_file(self):
        f = self.put("one.puz", puz_for())
        code, out, err = self.run_cli(f, "--date", "1993-12-05")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.ids(), ["1993-12-05"])
        code, out, err = self.run_cli(self.src, "--date", "1993-12-05")
        self.assertEqual(code, 2)
        self.assertIn("one file", err)
        code, out, err = self.run_cli(f, "--date", "soon")
        self.assertEqual(code, 2)
        self.assertIn("--date is a date", err)
        code, out, err = self.run_cli(f, f, "--date", "1993-12-05")
        self.assertEqual(code, 2)

    def test_state_says_which_library(self):
        elsewhere = tempfile.mkdtemp()
        try:
            self.put_json("1993-11-21.json", json_for("1993-11-21"))
            code, out, _ = self.run_cli(self.src, "--state", elsewhere)
            self.assertEqual(code, 0)
            self.assertEqual(out.splitlines()[0], "library: %s (0 puzzles)" % os.path.join(elsewhere, "crosswords"))
            self.assertTrue(os.path.exists(os.path.join(elsewhere, "crosswords", "1993-11-21.xw")))
            self.assertFalse(os.path.exists(self.lib()))
        finally:
            shutil.rmtree(elsewhere, ignore_errors=True)

    def test_an_interruption_reports_what_was_done(self):
        for day in ("1993-11-21", "1993-11-28"):
            self.put_json(day + ".json", json_for(day))
        real = crossword.convert
        calls = []

        def interrupted(data, name="", date=""):
            calls.append(name)
            if len(calls) == 2:
                raise KeyboardInterrupt
            return real(data, name, date)

        with mock.patch.object(crossword, "convert", interrupted):
            code, out, _ = self.run_cli(self.src)
        self.assertEqual(code, 130)
        self.assertIn("interrupted; what was done stays done:", out.splitlines())
        self.assertIn("1 added, 0 already there, 0 could not be read", out.splitlines())

    def test_mistakes_in_the_arguments_are_argparses_usage(self):
        for argv in ([], ["-v", "-q", "x"], ["--nonsense"]):
            with self.subTest(argv=argv):
                with contextlib.redirect_stderr(io.StringIO()) as e:
                    with self.assertRaises(SystemExit) as cm:
                        crossword.main(["import"] + argv, io.StringIO(), io.StringIO())
                self.assertEqual(cm.exception.code, 2)
                self.assertIn("usage:", e.getvalue())

    def test_the_command_names(self):
        out, err = io.StringIO(), io.StringIO()
        self.assertEqual(crossword.main([], out, err), 2)
        self.assertIn("usage: python -m server.crossword import PATH", err.getvalue())
        self.assertEqual(crossword.main(["frobnicate"], out, err), 2)
        out, err = io.StringIO(), io.StringIO()
        self.assertEqual(crossword.main(["--help"], out, err), 0)
        self.assertIn("usage: python -m server.crossword import PATH", out.getvalue())
        self.assertEqual(err.getvalue(), "")


class EntryPoints(Folder):
    """The two ways to run it, as processes: the relative imports of a module
    run with -m, and the path a script finds the server by."""

    def run_proc(self, argv, cwd):
        env = dict(os.environ)
        env.pop("CARDOS_STATE", None)
        return subprocess.run([sys.executable] + argv, cwd=cwd, env=env, capture_output=True,
                              text=True, timeout=120)

    def test_python_dash_m(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        p = self.run_proc(["-m", "server.crossword", "import", self.src, "--state", self.state], ROOT)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("1 added, 0 already there, 0 could not be read", p.stdout)
        self.assertEqual(self.ids(), ["1993-11-21"])

    def test_the_tool_from_anywhere(self):
        self.put_json("1993-11-21.json", json_for("1993-11-21"))
        self.put("bad.json", b"{no")
        elsewhere = tempfile.mkdtemp()
        try:
            p = self.run_proc([os.path.join(ROOT, "tools", "crossword_import.py"), self.src,
                               "--state", self.state], elsewhere)
        finally:
            shutil.rmtree(elsewhere, ignore_errors=True)
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("1 added, 0 already there, 1 could not be read", p.stdout)
        self.assertEqual(self.ids(), ["1993-11-21"])

    def test_the_tool_is_its_own_usage(self):
        p = self.run_proc([os.path.join(ROOT, "tools", "crossword_import.py"), "--help"], ROOT)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("usage: python tools/crossword_import.py", p.stdout)
        self.assertIn("--dry-run", p.stdout)

    def test_no_command_is_status_2(self):
        p = self.run_proc(["-m", "server.crossword"], ROOT)
        self.assertEqual(p.returncode, 2)
        self.assertIn("usage: python -m server.crossword import", p.stderr)


if __name__ == "__main__":
    unittest.main()
