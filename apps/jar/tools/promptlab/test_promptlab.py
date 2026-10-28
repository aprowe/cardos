"""The prompt lab's API in --dry mode: no Claude, no network.

    python -m unittest discover -s apps/jar/tools/promptlab -p "test_*.py"
    python apps/jar/tools/promptlab/test_promptlab.py
"""
import http.client
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REAL_BEFORE = os.environ.get("CARDOS_STATE")            # what the lab must not use
sys.path.insert(0, HERE)
import serve  # noqa: E402 - imports lab, which moves CARDOS_STATE first
import lab    # noqa: E402

lab.configure(dry=True, dry_delay=0)


class Api(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exp_dir = tempfile.mkdtemp(prefix="promptlab-exp-")
        cls.old_exp = lab.EXP_DIR
        lab.EXP_DIR = cls.exp_dir
        cls.srv = serve.make_server(0)
        cls.port = cls.srv.server_address[1]
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        lab.EXP_DIR = cls.old_exp
        shutil.rmtree(cls.exp_dir, ignore_errors=True)

    def call(self, method, path, body=None, host=None, ctype="application/json"):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        headers = {"Host": host or "127.0.0.1:%d" % self.port}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = ctype
        c.request(method, path, data, headers)
        r = c.getresponse()
        raw = r.read()
        c.close()
        try:
            return r.status, json.loads(raw.decode())
        except ValueError:
            return r.status, raw

    def post(self, path, body):
        status, out = self.call("POST", path, body)
        self.assertEqual(status, 200, out)
        return out

    def inputs(self, **rolls):
        r = {"kind": "critter", "tier": "everyday", "price": 135, "form": "", "activity": "reacts"}
        r.update(rolls)
        return {"search": "I went down to the canal lock at low water, after old tins.", "rolls": r}

    # ---- the scratch state
    def test_scratch_state_is_not_the_real_one(self):
        self.assertEqual(os.environ["CARDOS_STATE"], lab.STATE)
        self.assertTrue(os.path.isdir(lab.STATE))
        self.assertNotEqual(os.path.realpath(lab.STATE),
                            os.path.realpath(os.path.expanduser("~/.cardos")))
        if REAL_BEFORE:
            self.assertNotEqual(os.path.realpath(lab.STATE), os.path.realpath(REAL_BEFORE))
        self.assertTrue(os.path.basename(lab.STATE).startswith("promptlab-state-"))
        self.assertEqual(lab.shopkeep.home(), os.path.join(lab.STATE, "tibbs"))

    def test_nothing_signed_or_stored(self):
        out = self.post("/api/run_find", {"inputs": self.inputs(), "wait": True})["result"]
        self.assertIsNone(out["dropped"])
        self.assertFalse(os.path.exists(os.path.join(lab.STATE, "sign_key.pem")))
        st = lab.jar.kv.store() if hasattr(lab.jar, "kv") else None
        if st is not None:                              # no pool, no records, no ledger
            self.assertIsNone(st.get(lab.jar.NS, "pool"))
            self.assertIsNone(st.get(lab.jar.NS, "rec/0"))
            self.assertIsNone(st.get(lab.jar.NS, "next_id"))
        self.assertTrue(out["item"]["signed"])          # sealed, but with the dummy signer
        self.assertEqual(out["item"]["id"], 0)

    # ---- defaults and the dice
    def test_defaults(self):
        status, d = self.call("GET", "/api/defaults")
        self.assertEqual(status, 200)
        self.assertTrue(d["dry"])
        for s in lab.STAGES:
            self.assertIn("template", d["stages"][s], s)
            self.assertIn("schema", d["stages"][s], s)
        self.assertEqual(d["dice"]["kind_plan"], list(lab.jar.KIND_PLAN))
        self.assertEqual([a["name"] for a in d["dice"]["activities"]],
                         [a for a, _ in lab.jar.ACTIVITIES])
        self.assertEqual(len(d["dice"]["tiers"]), len(lab.jar.TIERS))
        self.assertIn("{{search}}", d["stages"]["dm"]["template"] + "{{search}}")
        self.assertIn("{{reference}}", d["stages"]["behaviour_js3"]["template"])
        self.assertIn("Jarscript 3", d["stages"]["behaviour_js3"]["default"])
        self.assertIn("move to", d["stages"]["behaviour_js3"]["default"])   # the reference itself

    def test_templates_give_the_servers_prompts_back(self):
        for v2 in (False, True):
            for seed in range(12):
                inp = dict(self.inputs(), rolls=lab.roll(seed, seed, v2=v2))
                inp["earlier"] = ["A rusty key moth.", "A velvet sock mole."] if seed % 2 else []
                for s in lab.STAGES:
                    c = lab.stage_ctx(s, inp, v2)
                    text, missing = lab.render(c["template"], c["values"])
                    self.assertEqual(text, c["default"], (v2, seed, s))
                    self.assertEqual(missing, [])
        c = lab.stage_ctx("dm", self.inputs())
        self.assertIn("{{search}}", c["template"])
        self.assertEqual(c["default"], lab.jarstages.dm_prompt(
            self.inputs()["search"], "reacts", "everyday", [], "critter", None, "",
            [lab.describe(c) for c in lab.default_residents()], ""))

    def test_residents_are_read_from_their_scripts(self):
        tree, fence, rolly, bird = lab.default_residents()
        self.assertEqual((tree["perches"], tree["solid"]), (5, True))
        self.assertEqual((tree["sends"], tree["hears"]), (["startle"], ["landed"]))
        self.assertEqual(rolly["notices"], ["bird"])
        self.assertEqual(bird["visits"], ["tree", "fence", "rim"])
        self.assertEqual((bird["sends"], bird["hears"]), (["landed"], ["startle"]))
        c = lab.stage_ctx("behaviour_js3", dict(self.inputs(), rolls=dict(
            self.inputs()["rolls"], tie="naps on the Sapling")))
        self.assertIn("answers signal landed", c["default"])
        self.assertIn("Its story with them: it naps on the Sapling.", c["default"])
        self.assertIn("on signal startle:", c["default"])         # the worked examples

    def test_roll_matches_generate(self):
        r = self.post("/api/roll", {"seed": 1234, "k": 2, "v2": True})
        import random
        rng = random.Random(1234)
        kind = lab.jar.KIND_PLAN[2]
        tier = lab.jar.pick_tier(rng)
        form = lab.jar.roll_form(kind, rng)
        activity = lab.jar.roll_activity(rng)
        if form in ("scenery", "furniture") and activity in ("ambles", "restless"):
            activity = "still"
        self.assertEqual((r["kind"], r["tier"], r["price"], r["form"] or None, r["activity"]),
                         (kind, tier[0], tier[1], form, activity))
        self.assertEqual(r, self.post("/api/roll", {"seed": 1234, "k": 2, "v2": True}))
        for seed in range(300):                         # a backdrop or a shelf never wanders
            x = lab.roll(seed, 1, v2=True)
            if x["form"] in ("scenery", "furniture"):
                self.assertIn(x["activity"], ("still", "reacts"))
        self.assertEqual(lab.roll(5, 0, v2=False)["form"], "")

    # ---- running
    def test_run_find_returns_every_intermediate(self):
        for variant in ("jarvm", "js3"):
            out = self.post("/api/run_find", {"inputs": self.inputs(), "variant": variant,
                                              "ask_tibbs": True, "wait": True})
            self.assertEqual(out["state"], "done", out.get("error"))
            r = out["result"]
            self.assertIsNone(r["dropped"])
            self.assertEqual(r["search_from"], "tibbs")
            self.assertTrue(r["search_used"])
            for s in ("tibbs", "dm", "fitter", "artist", "behaviour"):
                st = r["stages"][s]
                self.assertTrue(st["ok"], (variant, s, st.get("errors")))
                for k in ("prompt", "sent", "raw", "value", "attempts", "secs"):
                    self.assertIn(k, st, (s, k))
            self.assertEqual(r["stages"]["behaviour"]["stage"], lab.VARIANTS[variant])
            self.assertEqual(r["behaviour_stage"], lab.VARIANTS[variant])
            self.assertTrue(r["concept"])
            self.assertIn("note", r["sheet"])
            self.assertIn("fitter_note", r)
            self.assertEqual(len(r["art"]["frames"]), r["sheet"]["frames"])
            self.assertTrue(r["checks"] and r["checks"][-1]["ok"])
            self.assertEqual(set(r["checks"][0]["parts"]), {"sheet", "art", "behaviour"})
            self.assertEqual(r["item"]["name"], r["raw"]["name"])
            sb = r["sandbox"]
            self.assertEqual(len(sb["frames"]), len(r["art"]["frames"]))
            self.assertEqual(sb["palette"], r["art"]["palette"])
            if variant == "js3":
                self.assertEqual(sb["script_source"], "js3")
                self.assertEqual(sb["script"], r["behaviour"]["script"])
                if lab.CFG.node:
                    self.assertTrue(r["checks"][-1]["js3"]["ok"], r["checks"][-1]["js3"])
            else:
                self.assertEqual(sb["script_source"], "motion")

    def test_refusal_goes_back_to_the_behaviour_writer(self):
        out = self.post("/api/run_find", {"inputs": self.inputs(activity="reacts"),
                                          "variant": "jarvm", "dry_fault": "behaviour",
                                          "wait": True})["result"]
        self.assertIsNone(out["dropped"])
        self.assertEqual([x["stage"] for x in out["redo"]], ["behaviour"])
        self.assertIn("must react", out["redo"][0]["reasons"][0])
        self.assertIn("The last answer was refused", out["redo"][0]["result"]["prompt"])
        self.assertFalse(out["checks"][0]["ok"])
        self.assertTrue(out["checks"][1]["ok"])
        if lab.CFG.node:
            out = self.post("/api/run_find", {"inputs": self.inputs(), "variant": "js3",
                                              "dry_fault": "behaviour", "wait": True})["result"]
            self.assertEqual([x["stage"] for x in out["redo"]], ["behaviour_js3"])
            self.assertIn("does not compile", out["redo"][0]["reasons"][0])
            self.assertEqual(out["sandbox"]["script_source"], "js3")

    def test_overrides_change_what_is_sent(self):
        tpl = "MY OWN DM PROMPT. Tibbs went: {{search}}. Invent one {{tier_words}}."
        out = self.post("/api/run_find", {"inputs": self.inputs(),
                                          "overrides": {"dm": tpl, "fitter": "FITTER: {{concept}}"},
                                          "wait": True})["result"]
        dm = out["stages"]["dm"]
        expect = "MY OWN DM PROMPT. Tibbs went: %s. Invent one %s." % (
            self.inputs()["search"], lab.jarstages.TIER_WORDS["everyday"])
        self.assertEqual(dm["prompt"], expect)
        self.assertTrue(dm["sent"].startswith(expect))
        self.assertIn("JSON Schema", dm["sent"])          # appended as ask_shape does
        self.assertTrue(dm["edited"])
        self.assertEqual(out["stages"]["fitter"]["prompt"], "FITTER: " + out["concept"])
        self.assertFalse(out["stages"]["artist"]["edited"])
        # no override: exactly the server's prompt
        out = self.post("/api/run_find", {"inputs": self.inputs(), "wait": True})["result"]
        self.assertEqual(out["stages"]["dm"]["prompt"], lab.jarstages.dm_prompt(
            self.inputs()["search"], "reacts", "everyday", [], "critter", None, "",
            [lab.describe(c) for c in lab.default_residents()], ""))

    def test_run_stage_and_missing_placeholders(self):
        out = self.post("/api/run_stage", {"stage": "dm", "inputs": self.inputs(),
                                           "template": "Go: {{search}} {{nonsense}}", "wait": True})
        r = out["result"]
        self.assertTrue(r["ok"])
        self.assertEqual(r["missing"], ["nonsense"])
        self.assertIn("{{nonsense}}", r["prompt"])
        self.assertIn("concept", r["value"])
        # a stage behind a stand-in sheet says so
        r = self.post("/api/run_stage", {"stage": "artist", "inputs": self.inputs(),
                                         "wait": True})["result"]
        self.assertEqual(r["stand_in"], ["sheet"])
        self.assertTrue(r["ok"])

    def test_job_polling(self):
        jid = self.post("/api/run_find", {"inputs": self.inputs()})["job"]
        for _ in range(200):
            status, j = self.call("GET", "/api/job?id=" + jid)
            self.assertEqual(status, 200)
            if j["state"] != "pending":
                break
            threading.Event().wait(0.05)
        self.assertEqual(j["state"], "done")
        self.assertTrue(j["result"]["raw"]["name"])
        status, h = self.call("GET", "/api/history")
        self.assertIn(jid, [x["id"] for x in h["history"]])

    def test_tibbs(self):
        r = self.post("/api/tibbs", {"inputs": {"commission": "something that glows"},
                                     "wait": True})["result"]
        self.assertTrue(r["ok"])
        self.assertTrue(r["search"])
        self.assertIn("something that glows", r["prompt"])
        self.assertFalse(r["sent"].endswith("}"))       # Tibbs is not sent the schema appended
        r = self.post("/api/tibbs", {"inputs": {}, "template": "[Just say where. {{commission}}]",
                                     "wait": True})["result"]
        self.assertEqual(r["prompt"], "[Just say where. ]")

    def test_batch(self):
        r = self.post("/api/batch", {"n": 3, "seed": 7, "inputs": {"search": "a beach"},
                                     "variant": "js3", "wait": True})["result"]
        self.assertEqual(len(r["cards"]), 3)
        self.assertEqual([c["rolls"]["kind"] for c in r["cards"]], list(lab.jar.KIND_PLAN[:3]))
        names = [c["name"] for c in r["cards"]]
        self.assertEqual(len(set(n.lower() for n in names)), 3)
        dm2 = r["finds"][2]["stages"]["dm"]["prompt"]   # the DM was told the earlier finds
        self.assertIn("Already found today", dm2)

    def test_check_and_js3check(self):
        out = self.post("/api/run_find", {"inputs": self.inputs(), "variant": "js3",
                                          "wait": True})["result"]
        inp = dict(self.inputs(), sheet=out["sheet"], art=out["art"], behaviour=out["behaviour"])
        r = self.post("/api/check", {"inputs": inp, "variant": "js3"})
        self.assertTrue(r["check"]["ok"], r["check"])
        self.assertEqual(r["item"]["name"], out["sheet"]["name"])
        bad = dict(inp, art={"palette": out["art"]["palette"], "frames": [["0" * 16] * 16]})
        r = self.post("/api/check", {"inputs": bad, "variant": "js3"})
        self.assertFalse(r["check"]["ok"])
        self.assertTrue(r["check"]["parts"]["art"])
        if lab.CFG.node:
            r = self.post("/api/js3check", {"script": "state a:\n  frame f2\nend", "frames": 2})
            self.assertTrue(r["ok"], r)
            r = self.post("/api/js3check", {"script": "on poke:\n  wait 1\nend", "frames": 1})
            self.assertFalse(r["ok"])
            self.assertEqual(r["line"], 2)

    def test_motion_scripts_compile(self):
        if not lab.CFG.node:
            self.skipTest("no node")
        for travel in lab.jar.TRAVELS:
            for idle in lab.jar.IDLES:
                for form in (None, "big", "scenery", "furniture", "part"):
                    sheet = {"travel": travel, "idle": idle, "liveliness": "jittery",
                             "speed": "fast", "frames": 2, "bubbles": ['say "hi"'],
                             "traits": ["shiny"], "surface": 9}
                    src = lab.motion_script(sheet, "critter", form, "restless")
                    chk = lab.js3_check(src, 2)
                    self.assertTrue(chk["ok"], (travel, idle, form, chk, src))

    # ---- experiments
    def test_experiments(self):
        self.post("/api/experiment", {"name": "dm terse", "overrides": {"dm": "x {{search}}"},
                                      "note": "a try"})
        status, lst = self.call("GET", "/api/experiments")
        self.assertIn("dm terse", [x["name"] for x in lst["experiments"]])
        status, x = self.call("GET", "/api/experiment?name=dm%20terse")
        self.assertEqual(x["overrides"], {"dm": "x {{search}}"})
        self.assertTrue(os.path.exists(os.path.join(self.exp_dir, "dm terse.json")))
        status, bad = self.call("POST", "/api/experiment", {"name": "../escape"})
        self.assertEqual(status, 400)
        self.assertEqual(self.post("/api/experiment/delete", {"name": "dm terse"}), {"deleted": True})

    # ---- local only
    def test_foreign_host_and_content_type_refused(self):
        status, _ = self.call("GET", "/api/defaults", host="evil.example:%d" % self.port)
        self.assertEqual(status, 403)
        status, _ = self.call("POST", "/api/roll", {"seed": 1}, ctype="text/plain")
        self.assertEqual(status, 400)
        self.assertEqual(self.srv.server_address[0], "127.0.0.1")
        with self.assertRaises(ValueError):
            serve.make_server(0, host="0.0.0.0")

    def test_pages_are_served(self):
        for path in ("/", "/jarsandbox/", "/jarsandbox/jarscript3.js", "/jarsandbox/jarscript3.md"):
            status, body = self.call("GET", path)
            self.assertEqual(status, 200, path)


if __name__ == "__main__":
    unittest.main()
