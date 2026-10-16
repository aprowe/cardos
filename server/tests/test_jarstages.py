"""The staged item pipeline (docs/superpowers/specs/2026-10-11-jar-item-
pipeline-design.md): the code's rolls, motion as parts, the record's bytes
215-217, the checks by stage, Tibbs's search, and the stages themselves --
with Claude stubbed.

python -m unittest server.tests.test_jarstages
"""
import json
from server.jar import jarstages
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

from server import ask, jar, kv, sign
from server.jar import jarvm, shopkeep                     # noqa: E402
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
        self.assertEqual(want, {"still": 20, "reacts": 45, "ambles": 25, "restless": 10})
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

    def test_a_still_thing_is_never_all_zero(self):
        # all zero means "an old item: go by byte 72" on the device, which would
        # animate a sitter; so none/none is written with liveliness normal
        raw = motion_raw("still")                           # none / none / calm
        rec = jar.encode(jar.to_item(raw, 4447, 1791504000, "odd"))
        self.assertEqual(rec[1], 224)
        self.assertEqual(rec[215:218], bytes([0, 0, 1]))
        self.assertEqual(rec[72], jar.enums()["moves"]["sits"])
        flick = jar.encode(jar.to_item(motion_raw("still", idle="flicker"), 4448, 1791504000, "x"))
        self.assertEqual(flick[215:218], bytes([0, 6, 0]))      # calm, as the sheet said

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

    def test_only_a_critter_travels(self):
        # the device moves only critters about; decor and furniture keep their place
        for kind in ("floor", "hanging"):
            why = jar.check_item(motion_raw("ambles", "crawl", "bob", kind=kind),
                                 activity="ambles")
            self.assertTrue(any("travel" in w for w in why), (kind, why))
            self.assertEqual(jar.check_item(motion_raw("restless", "none", "spin", kind=kind,
                                                       speed="fast"), activity="restless"), [])
            self.assertEqual(jar.motion_rules("restless", kind)["travel"], ("none",))
        self.assertEqual(jar.check_item(motion_raw("ambles", "crawl", "bob"), activity="ambles"),
                         [])
        from server.jar import jarstages
        s = jarstages.fitter_schema("floor", None, "restless")
        self.assertEqual(s["properties"]["travel"]["enum"], ["none"])
        self.assertNotIn("none", s["properties"]["idle"]["enum"])

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



# ---- 4. Tibbs's search ----------------------------------------------------------------

class Search(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        self.said = []
        self.answers = []

        def fake_turn(chat, text, user, store=None, **kw):
            self.said.append(text)
            if self.answers:
                a = self.answers.pop(0)
                if isinstance(a, Exception):
                    raise a
                return a
            return json.dumps({"line": "Back from the canal.", "picks": [],
                               "search": "I went down to the old canal locks, after anything "
                                         "the barges dropped."})
        p = mock.patch.object(shopkeep, "turn", fake_turn)
        p.start()
        self.addCleanup(p.stop)

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_he_is_asked_where_he_went_only_on_an_errand(self):
        # A stock's finds come from his latest chapter; only a commission is
        # an errand of its own, with its own search.
        line, picks, search = shopkeep.stock_turn(None, "alex", [], 0, 4)
        self.assertEqual(line, "Back from the canal.")
        self.assertEqual(search, "")
        self.assertNotIn("Where did you go", self.said[-1])
        line, picks, search = shopkeep.stock_turn(None, "alex", [], 0, 4, commission="")
        self.assertIn("old canal locks", search)
        asked = self.said[-1]
        self.assertIn("Where did you go", asked)
        self.assertNotIn("sparks", asked)                   # no random words
        self.assertNotIn("by chance", asked)
        self.assertNotIn("brief", asked.lower())
        # next time he is not shown where he has been: he would hold on to it
        shopkeep.stock_turn(None, "alex", [], 0, 4, commission="")
        self.assertNotIn("old canal locks", self.said[-1])

    def test_a_commission_is_in_the_question(self):
        shopkeep.stock_turn(None, "alex", [], 0, 8, commission="brass birds")
        self.assertIn("brass birds", self.said[-1])

    def test_no_answer_is_no_search(self):
        self.answers = [RuntimeError("no Claude")]
        logs = []
        line, picks, search = shopkeep.stock_turn(None, "alex", [], 0, 4,
                                                  log=logs.append)
        self.assertEqual(search, "")
        self.assertTrue(any("note on the door" in s for s in logs), logs)

# ---- 5-7. the stages --------------------------------------------------------------------

class StageStub:
    """ask.ask_shape for the stages, answering each by its schema: the DM
    ("concept"), the fitter ("look"), the artist ("frames"), the behaviour
    writer ("habits"). Queues per stage override the defaults; an Exception
    in a queue is raised."""

    def __init__(self):
        self.lock = threading.Lock()
        self.prompts = {"dm": [], "fitter": [], "artist": [], "behaviour": [], "items": []}
        self.kw = []
        self.queue = {"dm": [], "fitter": [], "artist": [], "behaviour": []}
        self.n = 0
        self.travel = None

    def stage(self, schema):
        req = schema.get("required", [])
        if "concept" in req:
            return "dm"
        if "look" in req:
            return "fitter"
        if "frames" in req and "palette" in req and "name" not in req:
            return "artist"
        if "habits" in req and "script" in req and "name" not in req:
            return "behaviour"
        return "items"

    def __call__(self, chat, prompt, schema, **kw):
        with self.lock:
            s = self.stage(schema)
            self.prompts[s].append(prompt)
            self.kw.append((s, kw))
            q = self.queue.get(s)
            if q:
                a = q.pop(0)
                if isinstance(a, Exception):
                    raise a
                return a
            self.n += 1
            n = self.n
        if s == "dm":
            return {"concept": "A brass thimble that rings faintly when rain is coming, found "
                               "wedged in a canal lock gate. Its dents spell a name. (%d)" % n}
        if s == "fitter":
            p = schema["properties"]
            sheet = {"name": "Thing %d" % n, "line": "A small odd thing",
                     "kind": p["kind"]["enum"][0], "zone": "garden",
                     "travel": self.travel or p["travel"]["enum"][-1],
                     "idle": p["idle"]["enum"][-1],
                     "liveliness": "normal", "speed": p["speed"]["enum"][0],
                     "bubbles": ["ting"], "frames": p["frames"]["minimum"],
                     "look": "a dented brass thimble, gold with dark outline",
                     "intent": "rings when poked", "note": ""}
            if "traits" in p:
                sheet.update(traits=["shiny"], voice="none")
            if "surface" in p:
                sheet["surface"] = 4
            return sheet
        if s == "artist":
            m = schema["properties"]["frames"]["minItems"]
            return {"palette": raw_item()["palette"], "frames": [sprite(i) for i in range(m)]}
        if s == "behaviour":
            return {"habits": [{"when": "poke", "do": "say", "do_arg": 0}], "script": ""}
        raise AssertionError("the one-call path ran: %s" % prompt[:60])


class Stages(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        sign.forget_cached()
        self.stub = StageStub()
        for target, value in (("server.ask.ask_shape", self.stub),
                              ("server.sign.sign", test_signer)):
            p = mock.patch(target, value)
            p.start()
            self.addCleanup(p.stop)
        self.logs = []

    def tearDown(self):
        kv.close_all()
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def gen(self, want=1, search="I went down to the old canal locks.", rng=None, **kw):
        return jar.generate(None, "alex", {"owned": ["Moth Lamp"]}, None, want=want,
                            search=search, rng=rng or random.Random(3), log=self.logs.append,
                            **kw)

    def test_a_find_through_every_stage(self):
        st = kv.store()
        recs = self.gen()
        self.assertEqual(len(recs), 1)
        it = jar.decode(recs[0])
        self.assertTrue(it["name"].startswith("Thing"))
        self.assertEqual(it["hdr"], 224)
        self.assertTrue(it["travel"] or it["idle"] or it["lively"])     # never all zero
        for s in ("dm", "fitter", "artist", "behaviour"):
            self.assertEqual(len(self.stub.prompts[s]), 1, s)
        self.assertEqual(self.stub.prompts["items"], [])
        self.assertTrue(all(kw.get("limit", "x") is None for _, kw in self.stub.kw))
        c = json.loads(st.get(jar.NS, "concept/%d" % it["id"]))
        self.assertIn("thimble", c["concept"])
        self.assertIn("canal locks", c["search"])
        self.assertEqual(st.get(jar.NS, "own/%d" % it["id"]), b"alex")
        self.assertTrue(jar.verified(recs[0], test_pub()))

    def test_each_stage_is_told_what_it_should_and_not_what_it_must_not(self):
        self.gen()
        dm = self.stub.prompts["dm"][0]
        self.assertIn("old canal locks", dm)
        for word in ("Moth Lamp", "palette", "frames", "16x16", "script", "habits", "zone",
                     "floor decor", "travel", "liveliness", "alex"):
            self.assertNotIn(word, dm)
        # told the slot in plain words, and no place: a named place steers it
        self.assertIn("Invent one thing for the terrarium:", dm)
        self.assertNotIn("found it at", dm)
        fit = self.stub.prompts["fitter"][0]
        self.assertIn("thimble", fit)                       # the concept
        self.assertNotIn("palette", fit)
        self.assertNotIn(jarvm.LANGUAGE_GUIDE_V1[:60], fit)
        art = self.stub.prompts["artist"][0]
        self.assertIn("dented brass thimble", art)          # the look
        self.assertNotIn(jarvm.LANGUAGE_GUIDE_V1[:60], art)
        self.assertNotIn("rings when poked", art)           # not the intent
        beh = self.stub.prompts["behaviour"][0]
        self.assertIn("rings when poked", beh)
        self.assertNotIn("dented brass thimble", beh)       # never the look
        self.assertIn(jarvm.LANGUAGE_GUIDE_V1[:60], beh)

    def test_a_critter_is_alive_and_no_place_is_rolled_for_it(self):
        # 2026-10-13: a critter slot was told "he found it at a model village
        # gift shop -- let that shape what it is" and came out "a painted tin
        # hedgehog"; 21 of 22 finds were made objects. A list of places is
        # examples the model holds on to, so there is none.
        from server.jar import jarstages
        self.assertFalse(hasattr(jarstages, "SOURCES"))
        dm = jarstages.dm_prompt("A day at the clockmaker's attic.", "ambles", "common",
                                 kind="critter")
        self.assertIn("It is alive", dm)
        self.assertNotIn("trinket", dm)                 # the tier is rarity, not a thing
        self.assertNotIn("found it at", dm)
        self.assertNotIn("let that shape", dm)
        dm = jarstages.dm_prompt("A day at the clockmaker's attic.", "still", "common",
                                 kind="floor")
        self.assertNotIn("It is alive", dm)
        # a part's slot is not a critter's, whatever the kind
        dm = jarstages.dm_prompt("", "still", "common", kind="critter", form="part")
        self.assertNotIn("It is alive", dm)

    def test_the_dm_decides_what_it_is_before_what_it_is_made_of(self):
        from server.jar import jarstages
        dm = jarstages.dm_prompt("Up the clockmaker's stairs, hoping for springs.", "still",
                                 "common", earlier=("a brass watch key",))
        self.assertIn("what it is first", dm)
        self.assertIn("setting", dm)                    # his day, not a shopping list
        # nothing about materials used today: they do not steer the next one
        self.assertNotIn("made of:", dm)

    def test_the_fitter_is_held_to_the_activity_row(self):
        for activity in jar.MOTION_RULES:
            from server.jar import jarstages
            s = jarstages.fitter_schema("critter", None, activity)
            self.assertEqual(s["properties"]["travel"]["enum"],
                             list(jar.MOTION_RULES[activity]["travel"]))
            self.assertEqual(s["properties"]["idle"]["enum"],
                             list(jar.MOTION_RULES[activity]["idle"]))

    def test_the_concepts_of_a_stock_are_not_repeated(self):
        self.gen(want=3)
        self.assertIn("brass thimble", self.stub.prompts["dm"][2])     # told the earlier ones

    def test_a_still_thing_may_do_nothing(self):
        from server.jar import jarstages
        st = kv.store()
        self.stub.queue["behaviour"] = [{"habits": [], "script": ""}]
        raw, concept, sheet = jarstages.make_find(None, "alex", "floor", jar.TIERS[0][:1] +
                                                  (40, "low", jar.TIERS[0][4]), None, "still",
                                                  "", [], set(), st)
        self.assertEqual(raw["habits"], [])
        self.assertEqual((raw["travel"], raw["movement"]), ("none", "sits"))

    def test_a_reacting_thing_that_does_nothing_is_sent_back_then_dropped(self):
        from server.jar import jarstages
        st = kv.store()
        nothing = {"habits": [], "script": ""}
        self.stub.queue["behaviour"] = [nothing, nothing]
        tier = ("common", 40, "low", jar.TIERS[0][4])
        with self.assertRaises(jarstages.Drop):
            jarstages.make_find(None, "alex", "floor", tier, None, "reacts", "", [], set(), st)
        self.assertEqual(len(self.stub.prompts["behaviour"]), 2)
        self.assertIn("must react", self.stub.prompts["behaviour"][1])
        self.assertEqual(len(self.stub.prompts["artist"]), 1)          # the art was fine

    def test_a_refusal_goes_back_to_the_stage_at_fault(self):
        from server.jar import jarstages
        st = kv.store()
        blank = ["0" * 16] * 16
        self.stub.queue["artist"] = [{"palette": raw_item()["palette"], "frames": [blank]}]
        tier = ("common", 40, "low", jar.TIERS[0][4])
        raw, _, _ = jarstages.make_find(None, "alex", "floor", tier, None, "still", "", [],
                                        set(), st)
        self.assertEqual(len(self.stub.prompts["artist"]), 2)
        self.assertIn("blank", self.stub.prompts["artist"][1])
        self.assertEqual(len(self.stub.prompts["behaviour"]), 1)
        self.assertEqual(len(self.stub.prompts["fitter"]), 1)
        # a script that does not compile goes back to the behaviour writer
        self.stub.queue["behaviour"] = [{"habits": [], "script": "on poke:\n  fly to the moon\n"}]
        raw, _, _ = jarstages.make_find(None, "alex", "floor", tier, None, "reacts", "", [],
                                        set(), st)
        self.assertIn("script does not work", self.stub.prompts["behaviour"][-1])
        self.assertTrue(raw["habits"])

    def test_a_dm_that_fails_drops_the_find_not_the_stock(self):
        self.stub.queue["dm"] = [RuntimeError("no Claude")]
        recs = self.gen(want=2)
        self.assertEqual(len(recs), 1)
        self.assertTrue(any("find 1 dropped: DM" in s for s in self.logs), self.logs)

    def test_an_artist_that_fails_once_is_asked_again(self):
        self.stub.queue["artist"] = [RuntimeError("timeout")]
        self.assertEqual(len(self.gen()), 1)
        self.assertEqual(len(self.stub.prompts["artist"]), 2)

    def test_no_search_says_so_and_goes_on(self):
        self.gen(search="")
        self.assertNotIn("Tibbs's day", self.stub.prompts["dm"][0])
        self.assertIn("Invent one thing for the terrarium:", self.stub.prompts["dm"][0])
        self.assertTrue(any("no search" in s for s in self.logs), self.logs)

    def test_a_stock_is_made_from_his_chapter(self):
        from server.tests.test_jar import Stub
        tibbs = {"line": "Back from the canal.", "picks": []}
        chapter = {"chapter": "Down the canal locks today, after anything the barges dropped."}

        def turn(chat, text, *a, **k):
            return json.dumps(chapter if "next chapter of your story" in text else tibbs)
        with mock.patch.object(shopkeep, "turn", turn), \
                mock.patch.object(jarstages, "CHAPTER_CHANCE", 1.0):
            jar.start_day(None, "alex", {"owned": []})
            for _ in range(300):
                d = jar.day_state("alex")
                if d and d.get("state") != "pending":
                    break
                time.sleep(0.02)
        self.assertEqual(d["state"], "ok")
        self.assertIn("barges dropped", self.stub.prompts["dm"][0])
        self.assertEqual(self.stub.prompts["items"], [])

    def test_a_chapter_is_borrowed_from_now_and_then(self):
        # 2026-10-15: ten finds from one chapter were 4 windmills and 7 clovers
        recs = self.gen(want=8, search="Out at Windmill Common.", chapter_chance=0.35,
                        rng=random.Random(5))
        told = [p for p in self.stub.prompts["dm"] if "Windmill Common" in p]
        self.assertTrue(0 < len(told) < len(recs), (len(told), len(recs)))
        self.assertIn("at most one", told[0])

    def test_recent_finds_are_shown_and_their_names_taken(self):
        first = jar.decode(self.gen()[0])
        self.gen()
        dm = self.stub.prompts["dm"][-1]
        self.assertIn("Already found lately", dm)
        self.assertIn("A brass thimble that rings", dm)  # the first find's opening sentence
        names, said = jarstages.recent_finds(kv.store())
        self.assertIn(first["name"], names)


# ---- lively (2026-10-15) -----------------------------------------------------------------

class Lively(unittest.TestCase):

    def test_every_v3_find_has_a_script_and_critters_are_never_still(self):
        self.assertEqual(jarstages.script_tier("floor", None, "still"), "small")
        self.assertEqual(jarstages.script_tier("hanging", None, "still", "treasure"), "full")
        rng = random.Random(1)
        self.assertNotIn("still", {jar.roll_activity(rng, "critter") for _ in range(500)})

    def test_the_code_rolls_a_life(self):
        rng = random.Random(2)
        seen = set()
        for _ in range(300):
            life = jarstages.roll_life(rng, "critter", None)
            self.assertIn(life["role"], jarstages.ROLES["critter"])
            self.assertIn(life["poke"], jarstages.POKES["critter"])
            seen.add(life["role"])
        self.assertEqual(seen, set(jarstages.ROLES["critter"]))
        self.assertEqual(jarstages.roll_life(rng, "floor", "furniture")["role"], "stage")
        self.assertIn(jarstages.roll_life(rng, "hanging", None)["poke"], jarstages.POKES["hanging"])
        # it notices what really lives there, mostly
        got = {jarstages.roll_life(rng, "critter", None, [("Rolly", ["shell"])])["notices"]
               for _ in range(50)}
        self.assertIn("shell", got)

    def test_the_prompts_carry_it(self):
        life = jarstages.roll_life(random.Random(4), "critter", None)
        dm = jarstages.dm_prompt("", "ambles", "common", kind="critter", life=life)
        self.assertIn(life["poke"], dm)
        self.assertIn(life["said"], dm)
        sheet = {"name": "Nib", "line": "a nibbler", "intent": "nibbles", "zone": "garden",
                 "travel": "crawl", "traits": ["critter"]}
        beh = jarstages.behaviour_prompt_v3(sheet, "critter", "ambles", 2, "full", None, life,
                                            [("Rolly", ["bug", "shell"]), ("Sapling", [])])
        for want in (life["how"], "on poke", "startle", "landed", "Rolly -- bug, shell",
                     "Nothing moves it but its script", "WHOLE picture"):
            self.assertIn(want, beh)
        fit = jarstages.fitter_prompt("A nibbler.", "critter", None, "ambles",
                                      jar.TIERS[0][:1] + (50, "low", ""), life)
        self.assertIn("traits must include %s" % life["trait"], fit)

    def test_the_role_trait_is_worn(self):
        sheet = {"traits": ["cosy", "soft", "sleepy", "plant"]}
        jarstages._wear_role(sheet, {"trait": "hunter"})
        self.assertEqual(sheet["traits"], ["hunter", "cosy", "soft", "sleepy"])

    def test_a_script_must_be_lively(self):
        e = jarstages.lively_error
        ok = "state a:\n  walk 10\nend\non poke: go a end\non near bug: say \"!\" end\n"
        self.assertIsNone(e(ok, "critter", None))
        self.assertIn("on poke", e("state a:\n  walk 10\nend\non near bug: go a end\n",
                                   "critter", None))
        self.assertIn("answers nothing", e("state a:\n  walk 10\nend\non poke: go a end\n",
                                           "critter", None))
        still = "state a:\n  frame 1\nend\non poke: go a end\non time night: go a end\n"
        self.assertIn("never moves", e(still, "critter", None))
        self.assertIsNone(e(still, "floor", None))

    def test_residents_come_with_their_traits(self):
        d = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = d
        try:
            st = kv.store()
            rec = raw_item("Rolly")
            it = jar.to_item(dict(rec, traits=["bug", "shell"]), 150, 0, "", None, 50, None)
            it["tags"] = "bug,shell,common"
            st.put(jar.NS, "rec/150", jar.encode(it))
            got = jarstages.residents_of({"injar": [150, 999], "jar": ["Rolly", "Sapling"]}, st)
            self.assertEqual(got, [("Rolly", ["bug", "shell"]), ("Sapling", [])])
        finally:
            kv.close_all()
            os.environ.pop("CARDOS_STATE", None)
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    if "--fixture" in sys.argv:
        write_motion_fixture()
    else:
        unittest.main()
