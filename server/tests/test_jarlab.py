"""server/jarlab.py: Jar Lab, the bench for item generation and behaviour.

    python -m server.tests.test_jarlab

No network and no Claude: ask.ask_shape is stubbed, as in test_jar.py.
"""
import base64
import http.client
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import accounts, app, ask, dash, jar, jarlab, jarvm, kv, people, sign
from server import chat as chatmod

PASSWORD = "dash-password"


class StubAsk:
    """ask.ask_shape, standing in: answers with jarlab's own stub items and
    remembers what it was asked."""

    def __init__(self):
        self.calls = []

    def __call__(self, chat, prompt, schema, **kw):
        self.calls.append((prompt, schema, kw))
        return jarlab.stub_answer(schema, serial=len(self.calls))


class Core(unittest.TestCase):
    """The functions behind the routes."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_prompt_is_jars_own(self):
        tier = jarlab.tier_for("rare", 300)
        for v2 in (False, True):
            p = jarlab.build_prompt({"kind": "floor", "tier": "rare", "price": 300,
                                     "form": "furniture", "brief": "a brass stool", "v2": v2})
            with jarlab.version(v2):
                want = jar.prompt_for("a brass stool", [], [], "floor", tier, "furniture")
                schema = jar.batch_schema(1, "floor", "furniture")
            self.assertEqual(p["prompt"], want)
            self.assertEqual(p["schema"], schema)
            self.assertIn("a brass stool", p["asked"])
            self.assertEqual(p["tier"]["price"], 300)
        self.assertNotEqual(jarlab.build_prompt({"v2": True})["prompt"],
                            jarlab.build_prompt({"v2": False})["prompt"])

    def test_the_version_flip_puts_it_back(self):
        before = jarvm.V2
        with jarlab.version(not before):
            self.assertEqual(jarvm.V2, not before)
        self.assertEqual(jarvm.V2, before)

    def test_bad_specs_are_value_errors(self):
        for bad in ({"kind": "dragon"}, {"tier": "legendary"}, {"form": "huge"}):
            with self.assertRaises(ValueError):
                jarlab.build_prompt(bad)

    def test_defaults(self):
        d = jarlab.defaults()
        self.assertEqual([t["name"] for t in d["tiers"]], [t[0] for t in jar.TIERS])
        self.assertIn("furniture", [f["name"] for f in d["forms"]])
        self.assertEqual(d["sparks"]["things"], [])               # no sparks since the stages
        self.assertIn("JAR SCRIPT", d["guide_v1"])
        self.assertIn("schema", d)

    def test_roll(self):
        a, b = jarlab.roll(7, v2=True), jarlab.roll(7, v2=True)
        self.assertEqual(a, b)                                  # the same seed, the same dice
        self.assertIn(a["tier"], [t[0] for t in jar.TIERS])
        self.assertEqual(jarlab.roll(7, v2=False)["form"], "")  # no forms under v1

    def test_a_dry_generate_is_a_decodable_unsigned_item(self):
        for v2 in (False, True):
            r = jarlab.generate({"kind": "critter", "tier": "rare", "dry": True, "v2": v2, "n": 2})
            self.assertEqual(len(r["runs"]), 2)
            for run in r["runs"]:
                self.assertEqual(run["source"], "stub")
                it = run["items"][0]
                self.assertEqual(it["errors"], [])
                self.assertEqual(it["stage"], "packed")
                rec = base64.b64decode(it["record"]["b64"])
                d = jar.decode(rec)
                self.assertEqual(d["sig"], b"")                 # never signed
                self.assertEqual(d["price"], 300)
                view = it["item"]
                self.assertEqual(view["kind"], "critter")
                self.assertEqual(len(view["frames"]), 2)
                self.assertEqual(len(view["frames"][0]), 16)
                self.assertTrue(all(len(row) == 16 for row in view["frames"][0]))
                self.assertIsNone(view["palette"][0])
                self.assertRegex(view["palette"][1], r"^#[0-9a-f]{6}$")
                self.assertFalse(view["signed"])
                self.assertTrue(it["script"]["ok"])
                self.assertTrue(it["script"]["trace"])
                self.assertGreater(view["script_len"], 0)
                if v2:
                    self.assertTrue(view["traits"])
        # nothing reached the store
        st = kv.store()
        self.assertEqual(st.list(jar.NS), [])

    def test_a_refused_item_says_why(self):
        schema = jar.batch_schema(1, "hanging")
        raw = jarlab.stub_answer(schema)["items"][0]
        raw["habits"] = [{"when": "poke", "do": "hop", "do_arg": 4}]
        raw["frames"] = [["0" * 16] * 16]
        r = jarlab.examine(raw, "hanging")
        self.assertFalse(r["ok"])
        self.assertEqual(r["stage"], "refused")
        self.assertTrue(any("blank" in e for e in r["errors"]))
        self.assertTrue(any("hanging decor cannot hop" in e for e in r["errors"]))

    def test_a_pasted_answer(self):
        schema = jar.batch_schema(1, "floor")
        ans = json.dumps(jarlab.stub_answer(schema))
        r = jarlab.generate({"kind": "floor", "answer": ans})
        self.assertEqual(len(r["runs"]), 1)
        self.assertEqual(r["runs"][0]["source"], "pasted")
        self.assertTrue(r["runs"][0]["items"][0]["ok"])

    def test_simulate(self):
        good = jarlab.simulate("on poke:\n  say 1; hop\nend", nbub=1, nframes=1, v2=False)
        self.assertTrue(good["ok"])
        self.assertIn("ACTK say", good["disassembly"])
        self.assertTrue(any(t["ev"] == "poke" for t in good["trace"]))
        self.assertEqual(sum(good["report"]["per_hour"]),
                         sum(len(t["acts"]) for t in good["trace"]))
        bad = jarlab.simulate("on poke:\n  say 3\nend", nbub=1, nframes=1, v2=False)
        self.assertFalse(bad["ok"])
        self.assertEqual(bad["line"], 2)
        self.assertNotIn("report", bad)
        newer = jarlab.simulate("on poke: sound ding; end", 1, 1, v2=False)
        self.assertFalse(newer["ok"])
        self.assertEqual(newer.get("hint"), "it compiles as version 2")
        quiet = jarlab.simulate("on poke: wait 1; end", 1, 1, v2=False)
        self.assertFalse(quiet["ok"])                           # compiles, fails its day
        self.assertIn("never did anything visible", quiet["error"])

    def test_the_trace_hook_changes_nothing(self):
        code = jarvm.compile_script(jarlab.STUB_SCRIPT_V1, 2, 2, v2=False)
        plain = jarvm.simulate_day(code, 2, 2)
        seen = []
        traced = jarvm.simulate_day(code, 2, 2, trace=seen.append)
        self.assertEqual((plain.ok, plain.runs, plain.actions, plain.mem),
                         (traced.ok, traced.runs, traced.actions, traced.mem))
        self.assertEqual(len(seen), traced.runs)

    def test_experiments(self):
        jarlab.save_experiment({"name": "moth b", "note": "shorter", "prompt": "P"})
        self.assertEqual([e["name"] for e in jarlab.list_experiments()], ["moth b"])
        self.assertEqual(jarlab.load_experiment("moth b")["prompt"], "P")
        with self.assertRaises(ValueError):
            jarlab.save_experiment({"name": "../escape"})
        jarlab.delete_experiment("moth b")
        self.assertEqual(jarlab.list_experiments(), [])
        self.assertTrue(os.path.isdir(os.path.join(self.dir, "jarlab")))


class Routes(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        os.environ["DASH_PASSWORD"] = PASSWORD
        sign.forget_cached()
        accounts.migrate("alex", "alex-password", "alex-token")
        accounts.add_user("sam", "sam-password")
        _, self.sam_tok = accounts.add_device("sam", "hers")
        people._last.clear()
        self.ask = StubAsk()
        p = mock.patch("server.ask.ask_shape", self.ask)
        p.start()
        self.addCleanup(p.stop)
        app.Handler.chat = chatmod.ChatService(claude="stub", token="alex-token")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.alex = self.login("alex", "alex-password")
        self.sam = self.login("sam", "sam-password")

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        kv.close_all()
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        os.environ.pop("DASH_PASSWORD", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def raw(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, dict(r.getheaders()), data

    def login(self, name, pw):
        s, hdrs, _ = self.raw("POST", "/dash/login",
                              ("name=%s&password=%s" % (name, pw)).encode(),
                              {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(s, 303)
        return hdrs["Set-Cookie"].split(";")[0]

    def api(self, method, path, body=None, cookie=None):
        h = {"Content-Type": "application/json"}
        if cookie is not False:
            h["Cookie"] = cookie or self.alex
        s, _, data = self.raw(method, "/dash/jarlab/api/" + path, body, h)
        return s, json.loads(data.decode() or "null")

    def test_the_page(self):
        s, _, data = self.raw("GET", "/dash/jarlab", headers={"Cookie": self.alex})
        self.assertEqual(s, 200)
        self.assertIn(b"Jar Lab", data)
        s, hdrs, _ = self.raw("GET", "/dash/jarlab")            # signed out: to the sign-in
        self.assertEqual((s, hdrs.get("Location")), (303, "/dash"))
        self.assertEqual(self.raw("GET", "/dash/jarlab", headers={"Cookie": self.sam})[0], 403)
        s, hdrs, _ = self.raw("GET", "/jarlab")
        self.assertEqual((s, hdrs.get("Location")), (303, "/dash/jarlab"))

    def test_only_the_owner(self):
        for method, path, body in (("GET", "defaults", None), ("POST", "prompt", {}),
                                   ("POST", "generate", {"dry": True}),
                                   ("POST", "simulate", {"script": "on poke: hop; end"}),
                                   ("GET", "experiments", None)):
            s, j = self.api(method, path, body, cookie=self.sam)
            self.assertEqual(s, 403, path)
            self.assertTrue(j.get("owner_only"), path)
            self.assertEqual(self.api(method, path, body, cookie=False)[0], 403, path)
            # a device's bearer is not the dashboard's cookie, even the owner's
            st, _, _ = self.raw(method, "/dash/jarlab/api/" + path,
                                json.dumps(body).encode() if body is not None else None,
                                {"Authorization": "Bearer alex-token"})
            self.assertEqual(st, 403, path)
        self.assertEqual(self.ask.calls, [])

    def test_defaults_and_prompt(self):
        s, d = self.api("GET", "defaults")
        self.assertEqual(s, 200)
        self.assertTrue(d["prompt"].startswith(jar.GAME))
        s, p = self.api("POST", "prompt", {"kind": "hanging", "tier": "treasure",
                                           "brief": "a bell that rings for rain", "v2": True})
        self.assertEqual(s, 200)
        self.assertIn("a bell that rings for rain", p["prompt"])
        self.assertIn("hanging decor", p["prompt"])
        self.assertEqual(p["schema"]["properties"]["items"]["items"]["properties"]["kind"],
                         {"enum": ["hanging"]})
        self.assertEqual(self.api("POST", "prompt", {"kind": "dragon"})[0], 400)
        self.assertEqual(self.ask.calls, [])                    # a preview asks nobody

    def test_dry_generate(self):
        s, r = self.api("POST", "generate", {"kind": "floor", "tier": "common", "dry": True})
        self.assertEqual(s, 200)
        it = r["runs"][0]["items"][0]
        self.assertTrue(it["ok"], it["errors"])
        self.assertEqual(len(it["item"]["frames"]), 2)
        self.assertEqual(len(it["item"]["palette"]), 8)
        jar.decode(base64.b64decode(it["record"]["b64"]))
        self.assertEqual(self.ask.calls, [])                    # dry: no Claude
        self.assertEqual(kv.store().list(jar.NS), [])

    def test_a_real_generate_goes_through_ask_shape_with_the_edited_prompt(self):
        s, r = self.api("POST", "generate", {"kind": "critter", "tier": "rare",
                                             "prompt": "EDITED PROMPT", "wait": True,
                                             "effort": "low", "model": "m1"})
        self.assertEqual(s, 200)
        self.assertEqual(len(self.ask.calls), 1)
        prompt, schema, kw = self.ask.calls[0]
        self.assertEqual(prompt, "EDITED PROMPT")
        self.assertEqual((kw["effort"], kw["model"], kw["limit"]), ("low", "m1", None))
        self.assertTrue(r["edited"])
        self.assertTrue(r["runs"][0]["items"][0]["ok"])
        self.assertEqual(kv.store().list(jar.NS), [])          # no pool, no ledger, no next_id

    def test_a_generate_job(self):
        s, r = self.api("POST", "generate", {"kind": "critter", "n": 2})
        self.assertEqual(s, 200)
        jid = r["job"]
        for _ in range(200):
            s, j = self.api("GET", "job?id=" + jid)
            if j["state"] == "done":
                break
            threading.Event().wait(0.05)
        self.assertEqual(j["state"], "done")
        self.assertEqual(len(j["result"]["runs"]), 2)
        self.assertEqual(len(self.ask.calls), 2)
        self.assertEqual(self.api("GET", "job?id=nope")[0], 404)

    def test_simulate_route(self):
        s, r = self.api("POST", "simulate", {"script": "on poke:\n  say 1; hop\nend",
                                             "nbub": 1, "nframes": 1, "v2": False})
        self.assertEqual(s, 200)
        self.assertTrue(r["ok"])
        self.assertTrue(r["trace"])
        s, r = self.api("POST", "simulate", {"script": "on poke\n  hop\nend", "v2": False})
        self.assertEqual(s, 200)
        self.assertFalse(r["ok"])
        self.assertEqual(r["line"], 1)
        self.assertIn("line 1", r["error"])
        item = jarlab.stub_answer(jar.batch_schema(1, "critter"))["items"][0]
        s, r = self.api("POST", "simulate", {"item": item, "v2": False})
        self.assertEqual((s, r["nbub"], r["nframes"]), (200, 2, 2))
        self.assertTrue(r["ok"])

    def test_experiments_route(self):
        s, r = self.api("POST", "experiment", {"name": "v-a", "prompt": "P", "spec": {"kind": "floor"}})
        self.assertEqual(s, 200)
        s, r = self.api("GET", "experiments")
        self.assertEqual([e["name"] for e in r["experiments"]], ["v-a"])
        s, r = self.api("GET", "experiment?name=v-a")
        self.assertEqual(r["spec"], {"kind": "floor"})
        self.assertEqual(self.api("GET", "experiment?name=nope")[0], 404)
        self.assertEqual(self.api("POST", "experiment", {"name": "../x"})[0], 400)
        self.api("POST", "experiment/delete", {"name": "v-a"})
        self.assertEqual(self.api("GET", "experiments")[1]["experiments"], [])


if __name__ == "__main__":
    unittest.main()
