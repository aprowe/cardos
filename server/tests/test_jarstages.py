"""The staged item pipeline (docs/superpowers/specs/2026-10-11-jar-item-
pipeline-design.md): the code's rolls, motion as parts, the record's bytes
215-217, the checks by stage, Tibbs's search, and the stages themselves --
with Claude stubbed.

python -m unittest server.tests.test_jarstages
"""
import json
import os
import random
import re
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import ask, jar, jarvm, kv, shopkeep, sign                     # noqa: E402
from server.tests.test_jar import (FIXTURES, raw_item, sprite, test_pub,   # noqa: E402
                                   test_signer)


def motion_raw(activity="still", travel="none", idle="none", liveliness="calm", **kw):
    """A combined item as the pipeline hands it to check_item."""
    raw = raw_item(**kw)
    raw.update(travel=travel, idle=idle, liveliness=liveliness,
               movement=jar.move_fallback(travel, idle))
    if activity in ("still", "reacts"):
        raw["speed"] = "slow"
    return raw


# ---- 1. the rolls and the rules ----------------------------------------------------

class Rolls(unittest.TestCase):

    def test_activities_come_in_their_proportions(self):
        rng = random.Random(7)
        n = 40000
        got = {}
        for _ in range(n):
            a = jar.roll_activity(rng)
            got[a] = got.get(a, 0) + 1
        want = dict(jar.ACTIVITIES)
        self.assertEqual(want, {"still": 35, "reacts": 30, "ambles": 25, "restless": 10})
        for a, pct in want.items():
            self.assertAlmostEqual(100.0 * got[a] / n, pct, delta=1.0, msg=a)

    def test_the_fallback_for_old_builds(self):
        f = jar.move_fallback
        self.assertEqual(f("crawl", "none"), "wanders")
        self.assertEqual(f("slide", "bob"), "wanders")
        self.assertEqual(f("hop", "none"), "hops")
        self.assertEqual(f("float", "pulse"), "floats")
        self.assertEqual(f("fly", "none"), "floats")
        self.assertEqual(f("none", "sway"), "sways")
        self.assertEqual(f("none", "flicker"), "sits")
        self.assertEqual(f("none", "none"), "sits")

    def test_the_motion_table(self):
        r = jar.MOTION_RULES
        self.assertEqual(r["still"]["travel"], ("none",))
        self.assertEqual(set(r["still"]["idle"]), {"none", "pulse", "flicker", "blink"})
        self.assertEqual(r["reacts"]["travel"], ("none",))
        self.assertEqual(r["reacts"]["idle"], ("none",))
        self.assertEqual(set(r["ambles"]["travel"]), set(jar.TRAVELS))
        self.assertEqual(r["ambles"]["speed"], ("slow",))
        self.assertNotIn("none", r["restless"]["travel"])
        self.assertNotIn("none", r["restless"]["idle"])
        self.assertEqual(set(r["restless"]["speed"]), {"medium", "fast"})


# ---- 2. the record ------------------------------------------------------------------

class Record(unittest.TestCase):

    def test_bytes_215_to_217(self):
        self.assertEqual(jar.TRAVELS, ("none", "crawl", "hop", "slide", "float", "fly"))
        self.assertEqual(jar.IDLES, ("none", "sway", "bob", "wobble", "spin", "pulse",
                                     "flicker", "blink"))
        self.assertEqual(jar.LIVELY, ("calm", "normal", "jittery"))
        raw = motion_raw("restless", "hop", "flicker", "jittery", speed="fast")
        it = jar.to_item(raw, 4444, 1791504000, "odd")
        self.assertEqual((it["travel"], it["idle"], it["lively"]), (2, 6, 2))
        self.assertEqual(it["move"], jar.enums()["moves"]["hops"])
        rec = jar.encode(it)
        self.assertEqual(rec[1], 224)                       # the longer header, for these alone
        self.assertEqual(rec[215:218], bytes([2, 6, 2]))
        self.assertEqual(rec[218:224], bytes(6))
        back = jar.decode(rec)
        self.assertEqual((back["travel"], back["idle"], back["lively"]), (2, 6, 2))
        self.assertEqual(jar.encode(back), rec)
        sealed = jar.seal(rec, test_signer)
        bad = bytearray(sealed)
        bad[216] = 1                                        # signed, like the rest of the header
        self.assertFalse(jar.verified(bytes(bad), test_pub()))

    def test_an_old_item_reads_as_zeros(self):
        rec = jar.encode(jar.to_item(raw_item(), 4445, 1791504000, "odd"))
        self.assertEqual(rec[1], 208)
        it = jar.decode(rec)
        self.assertEqual((it["travel"], it["idle"], it["lively"]), (0, 0, 0))
        out_of_range = bytearray(jar.encode(jar.to_item(
            motion_raw("ambles", "crawl", "sway", "normal"), 4446, 1791504000, "odd")))
        out_of_range[215], out_of_range[216], out_of_range[217] = 9, 99, 7
        it = jar.decode(bytes(out_of_range))
        self.assertEqual((it["travel"], it["idle"], it["lively"]), (0, 0, 0))

    def test_still_with_no_parts_keeps_the_short_header(self):
        raw = motion_raw("still")                           # none / none / calm
        rec = jar.encode(jar.to_item(raw, 4447, 1791504000, "odd"))
        self.assertEqual(rec[1], 208)
        self.assertEqual(rec[72], jar.enums()["moves"]["sits"])

    def test_the_shared_fixture(self):
        path = os.path.join(FIXTURES, "jar_item_motion.bin")
        with open(path, "rb") as f:
            rec = f.read()
        with open(os.path.join(FIXTURES, "jar_item_motion_message.bin"), "rb") as f:
            msg = f.read()
        self.assertTrue(jar.verified(rec, test_pub()))
        self.assertEqual(jar.message(rec), msg)
        it = jar.decode(rec)
        self.assertEqual((it["travel"], it["idle"], it["lively"]), (2, 6, 2))
        self.assertEqual(it["move"], jar.enums()["moves"]["hops"])
        self.assertEqual(len(it["frames"]), 2)


def write_motion_fixture():
    """test/fixtures/jar_item_motion.bin (+ _message): an item that hops,
    flickers and is jittery, signed with the test key, for test_jaritem.c."""
    raw = motion_raw("restless", "hop", "flicker", "jittery", name="Sparkbug", speed="fast",
                     line="It will not sit still")
    rec = jar.seal(jar.encode(jar.to_item(raw, 4545, 1791504000, "odd,light")), test_signer)
    for name, data in (("jar_item_motion.bin", rec),
                       ("jar_item_motion_message.bin", jar.message(rec))):
        with open(os.path.join(FIXTURES, name), "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (name, len(data)))


# ---- 3. the checks, by stage --------------------------------------------------------

class Checks(unittest.TestCase):

    def test_a_motion_outside_its_row_is_refused(self):
        bad = motion_raw("still", travel="crawl")
        why = jar.check_item(bad, activity="still")
        self.assertTrue(any("travel" in w for w in why), why)
        why = jar.check_item(motion_raw("still", idle="sway"), activity="still")
        self.assertTrue(any("idle" in w for w in why), why)
        why = jar.check_item(motion_raw("restless", "none", "bob", speed="medium"),
                             activity="restless")
        self.assertTrue(any("travel" in w for w in why), why)
        why = jar.check_item(motion_raw("ambles", "crawl", "bob", speed="fast"),
                             activity="ambles")
        self.assertTrue(any("speed" in w for w in why), why)
        self.assertEqual(jar.check_item(motion_raw("ambles", "crawl", "bob"), activity="ambles"),
                         [])
        self.assertEqual(jar.check_item(motion_raw("still", idle="flicker"), activity="still"),
                         [])

    def test_still_may_do_nothing_and_reacts_may_not(self):
        quiet = motion_raw("still", habits=[], bubbles=[])
        self.assertEqual(jar.check_item(quiet, activity="still"), [])
        why = jar.check_item(motion_raw("reacts", habits=[], bubbles=[]), activity="reacts")
        self.assertTrue(any("reacts" in w for w in why), why)
        with_script = motion_raw("reacts", habits=[], bubbles=["oh!"],
                                 script="on poke:\n  say 1\n")
        self.assertEqual(jar.check_item(with_script, activity="reacts"), [])
        with_habit = motion_raw("reacts", habits=[{"when": "poke", "do": "glow", "do_arg": 2}])
        self.assertEqual(jar.check_item(with_habit, activity="reacts"), [])

    def test_the_checks_are_split_by_stage(self):
        blank = ["0" * 16] * 16
        raw = motion_raw("still", frames=[blank],
                         habits=[{"when": "poke", "do": "say", "do_arg": 3}])
        parts = jar.check_parts(raw, activity="still")
        self.assertTrue(parts["art"] and all("frame" in w for w in parts["art"]), parts)
        self.assertTrue(parts["behaviour"] and "bubble" in parts["behaviour"][0], parts)
        self.assertEqual(parts["sheet"], [])
        taken = jar.check_parts(motion_raw("still", name="Glowcap"), {"glowcap"}, activity="still")
        self.assertTrue(taken["sheet"], taken)

    def test_without_an_activity_check_item_is_as_it_was(self):
        self.assertEqual(jar.check_item(raw_item()), [])
        bad = raw_item(travel="hop")                        # not a field of the one-call item
        self.assertTrue(jar.check_item(bad))


if __name__ == "__main__":
    if "--fixture" in sys.argv:
        write_motion_fixture()
    else:
        unittest.main()
