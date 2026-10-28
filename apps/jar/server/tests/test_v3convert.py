"""Moving held items to JarScript 3 (apps/jar/server/v3convert.py), and back: each
original kept, the owner's device told to fetch it, a shelf's copy replaced
quietly, a failure leaving the old record, undo putting every byte back.
And the pipeline's v3 switch: the tier, the pool's retirement, the schema.

    python -m apps.jar.server.tests.test_v3convert
"""
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))))

from server import kv, sign                                  # noqa: E402
from apps.jar import server as jar
from apps.jar.server import jarstages, jarvm3, shopkeep, v3convert     # noqa: E402
from apps.jar.server.tests.test_jar import raw_item, test_signer as signer  # noqa: E402

SCRIPT = """state rest:
  wait 2s
  walk random(-10, 10)
end

on poke:
  say "hm?"
end
"""


class Convert(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        sign.forget_cached()
        for p in (mock.patch("server.sign.sign", signer),
                  mock.patch.object(jarstages, "_ask", self.ask)):
            p.start()
            self.addCleanup(p.stop)
        self.asked = []
        self.answer = SCRIPT

    def tearDown(self):
        kv.close_all()
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def ask(self, chat, prompt, schema, person, effort, st):
        self.asked.append(prompt)
        return {"script": self.answer}

    def make(self, item_id, who=b"alex", name="Owl"):
        st = kv.store()
        it = jar.to_item(raw_item(name), item_id, 1791504000, "shiny")
        rec = jar.seal(jar.encode(it), signer)
        st.put(jar.NS, "rec/%d" % item_id, rec)
        if who:
            st.put(jar.NS, "own/%d" % item_id, who)
        return st, rec

    def test_an_owned_item_is_converted_kept_and_its_owner_told(self):
        st, old = self.make(5001)
        out = v3convert.convert(object(), store=st, log=lambda s: None)
        self.assertEqual(out["done"], [5001])
        new = st.get(jar.NS, "rec/5001")
        self.assertTrue(jar.is_v3(new))
        self.assertEqual(st.get(jar.NS, "v2rec/5001"), old)          # the original, kept
        a, b = jar.decode(old), jar.decode(new)
        for k in ("name", "line", "price", "frames", "pal", "tags", "habits", "bubbles"):
            self.assertEqual(a[k], b[k], k)                          # only the script changed
        self.assertEqual(b["mem"], [0] * jar.MEM)
        self.assertIn({"id": 1, "op": "remake", "arg": 5001}, shopkeep.txs("alex", st))
        self.assertIn("Keep what it does", self.asked[0])
        self.assertIn("when poke", self.asked[0])                    # its habits, spelled out
        # done once: a second run skips it
        self.assertEqual(v3convert.convert(object(), store=st, log=lambda s: None)["skipped"], 1)

    def test_undo_puts_every_byte_back_and_tells_the_device(self):
        st, old = self.make(5002)
        v3convert.convert(object(), store=st, log=lambda s: None)
        self.assertEqual(v3convert.undo(store=st, log=lambda s: None), 1)
        self.assertEqual(st.get(jar.NS, "rec/5002"), old)
        self.assertIsNone(st.get(jar.NS, "v2rec/5002"))
        self.assertEqual([t["op"] for t in shopkeep.txs("alex", st)], ["remake", "remake"])
        # and it can be converted again after
        self.assertEqual(v3convert.convert(object(), store=st, log=lambda s: None)["done"], [5002])

    def test_a_shelf_item_is_replaced_quietly_and_its_buyer_told(self):
        st, old = self.make(5003)
        jar._set_shown(st, "alex", [5003])
        v3convert.convert(object(), store=st, log=lambda s: None)
        self.assertEqual(shopkeep.txs("alex", st), [])               # nothing to fetch yet
        self.assertEqual(jar.settle_bought(None, "alex", [5003], st), [5003])
        self.assertEqual([t["op"] for t in shopkeep.txs("alex", st)], ["remake"])

    def test_a_script_that_will_not_compile_keeps_the_old_record(self):
        st, old = self.make(5004)
        self.answer = "state rest:\n  frobnicate\nend\n"
        out = v3convert.convert(object(), store=st, log=lambda s: None)
        self.assertEqual(out["done"], [])
        self.assertEqual(out["failed"][0][0], 5004)
        self.assertEqual(st.get(jar.NS, "rec/5004"), old)
        self.assertIsNone(st.get(jar.NS, "v2rec/5004"))
        self.assertEqual(len(self.asked), 2)                         # told why, once

    def test_the_pool_and_nobodys_items_are_left_alone(self):
        st, _ = self.make(5005, who=None)
        jar._set_pool(st, [5005])
        out = v3convert.convert(object(), store=st, log=lambda s: None)
        self.assertEqual(out["done"], [])
        self.assertFalse(jar.is_v3(st.get(jar.NS, "rec/5005")))

    def test_dry_says_what_it_would_do_and_does_nothing(self):
        st, old = self.make(5006)
        said = []
        out = v3convert.convert(None, dry=True, store=st, log=said.append)
        self.assertEqual(out["done"], [5006])
        self.assertIn("would convert 5006", said[0])
        self.assertEqual(st.get(jar.NS, "rec/5006"), old)
        self.assertEqual(self.asked, [])


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_code_rolls_the_script_tier(self):
        self.assertEqual(jarstages.script_tier("critter", None, "still"), "full")
        self.assertEqual(jarstages.script_tier("floor", None, "still"), "small")   # lively
        self.assertEqual(jarstages.script_tier("floor", None, "still", "rare"), "full")
        self.assertEqual(jarstages.script_tier("floor", None, "reacts"), "small")
        self.assertEqual(jarstages.script_tier("hanging", "furniture", "restless"), "small")

    def test_a_v3_item_passes_the_checks_and_compiles_to_its_tier(self):
        raw = dict(raw_item(), script=SCRIPT, lang=3, tier="small", habits=[])
        self.assertEqual(jar.check_item(raw, activity=None), [])
        rec = jar.encode(jar.to_item(raw, 7001, 1791504000, "shiny"))
        self.assertTrue(jar.is_v3(rec))
        self.assertLessEqual(len(jar.decode(rec)["script"]), jarvm3.tier_limit("small"))

    def test_with_v3_on_the_pools_v2_items_are_retired(self):
        st = kv.store()
        v2 = jar.seal(jar.encode(jar.to_item(raw_item("Old"), 7101, 1791504000, "")),
                      signer)
        v3 = jar.seal(jar.encode(jar.to_item(dict(raw_item("New"), script=SCRIPT, lang=3,
                                                  tier="small"), 7102, 1791504000, "")),
                      signer)
        st.put(jar.NS, "rec/7101", v2)
        st.put(jar.NS, "rec/7102", v3)
        jar._set_pool(st, [7101, 7102])
        with mock.patch.object(jar, "V3", False):
            self.assertFalse(jar.retired(v2))
        with mock.patch.object(jar, "V3", True):
            self.assertTrue(jar.retired(v2))
            self.assertFalse(jar.retired(v3))
            got = jar.pick_from_pool("alex", {"owned": []}, 2, st)
        self.assertEqual(got, [7102])
        self.assertIn(7101, jar.pool_ids(st))                    # still there, for turning it off


if __name__ == "__main__":
    unittest.main()
