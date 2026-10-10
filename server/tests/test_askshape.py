"""server/ask.py: asking Claude for JSON of a shape, against a stub.

    python -m server.tests.test_askshape
"""
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest import mock
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import accounts, app, ask, kv
from server import chat as chatmod

ITEM = {
    "type": "object",
    "required": ["name", "kind", "frames"],
    "additionalProperties": False,
    "properties": {
        "name": {"type": "string", "minLength": 1, "maxLength": 12},
        "kind": {"enum": ["floor", "hanging", "critter"]},
        "frames": {"type": "integer", "minimum": 1, "maximum": 4},
        "bubbles": {"type": "array", "maxItems": 4, "items": {"type": "string", "maxLength": 12}},
        "id": {"type": "string", "pattern": "^[0-9a-f]{4}$"},
    },
}
GOOD = {"name": "Snail", "kind": "critter", "frames": 2, "bubbles": ["hi"]}


class Answers:
    """chat.ask_once, standing in: hands out the answers given, in order."""

    def __init__(self, *answers):
        self.answers = list(answers)
        self.prompts = []

    def __call__(self, chat, prompt, timeout, **kw):
        self.prompts.append(prompt)
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return (a if isinstance(a, str) else json.dumps(a)), "s-1"


class Validate(unittest.TestCase):

    def test_good(self):
        self.assertEqual(ask.validate(GOOD, ITEM), [])
        self.assertEqual(ask.validate([1, 2.5], {"type": "array", "items": {"type": "number"}}), [])
        self.assertEqual(ask.validate(None, {"type": ["string", "null"]}), [])

    def test_bad(self):
        def errs(v, schema=ITEM):
            return ask.validate(v, schema)
        self.assertEqual(errs([]), ["$: should be object"])
        self.assertIn("$: missing kind", errs({"name": "x", "frames": 1}))
        self.assertTrue(errs(dict(GOOD, kind="hat"))[0].startswith("$.kind: should be one of"))
        self.assertTrue(errs(dict(GOOD, name="a much too long name"))[0].startswith("$.name: at most 12"))
        self.assertEqual(errs(dict(GOOD, frames=5)), ["$.frames: at most 4"])
        self.assertEqual(errs(dict(GOOD, frames=True)), ["$.frames: should be integer"])
        self.assertEqual(errs(dict(GOOD, frames=1.5)), ["$.frames: should be integer"])
        self.assertEqual(errs(dict(GOOD, colour="red")), ["$.colour: not allowed"])
        self.assertEqual(errs(dict(GOOD, bubbles=["x", 3])), ["$.bubbles[1]: should be string"])
        self.assertEqual(errs(dict(GOOD, id="00g1")), ["$.id: should match ^[0-9a-f]{4}$"])
        self.assertEqual(errs(5, {"const": 4}), ["$: should be 4"])

    def test_schema_is_checked_whole(self):
        ask.check_schema(ITEM)
        for bad in ({"type": "object", "oneOf": []}, {"type": "date"}, [],
                    {"properties": {"x": {"format": "email"}}}, {"pattern": "("}):
            with self.assertRaises(ask.BadSchema, msg=repr(bad)):
                ask.check_schema(bad)

    def test_extract(self):
        self.assertEqual(ask.extract('{"a": 1}'), {"a": 1})
        self.assertEqual(ask.extract('Here:\n```json\n{"a": 1}\n```\nEnjoy'), {"a": 1})
        self.assertEqual(ask.extract('Sure! {"a": [1, 2]} hope that helps'), {"a": [1, 2]})
        with self.assertRaises(ValueError):
            ask.extract("no json here")


class AskShape(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.clock = [1760000000.0]
        self.store = kv.Store(os.path.join(self.dir, "kv.sqlite3"), clock=lambda: self.clock[0])
        self.chat = chatmod.ChatService(claude="stub")

    def tearDown(self):
        self.store.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_with(self, *answers):
        stub = Answers(*answers)
        p = mock.patch("server.chat.ask_once", stub)
        p.start()
        self.addCleanup(p.stop)
        return stub

    def shape(self, **kw):
        kw.setdefault("user", "alex")
        kw.setdefault("store", self.store)
        return ask.ask_shape(self.chat, "a critter for a rainy day", ITEM, **kw)

    def test_valid(self):
        stub = self.run_with(GOOD)
        self.assertEqual(self.shape(), GOOD)
        self.assertEqual(len(stub.prompts), 1)
        self.assertIn("a critter for a rainy day", stub.prompts[0])
        self.assertIn('"maxLength":12', stub.prompts[0])          # the schema went with it

    def test_invalid_then_valid(self):
        stub = self.run_with(dict(GOOD, kind="hat"), "```json\n%s\n```" % json.dumps(GOOD))
        self.assertEqual(self.shape(), GOOD)
        self.assertEqual(len(stub.prompts), 2)
        self.assertIn("$.kind: should be one of", stub.prompts[1])
        self.assertIn('"hat"', stub.prompts[1])                    # what it said, shown back

    def test_invalid_twice(self):
        stub = self.run_with("not json", dict(GOOD, frames=9))
        with self.assertRaises(ask.Invalid) as cm:
            self.shape()
        self.assertIn("$.frames: at most 4", str(cm.exception))
        self.assertEqual(len(stub.prompts), 2)

    def test_too_long_is_invalid(self):
        self.run_with("x" * (ask.ANSWER_MAX + 1), GOOD)
        self.assertEqual(self.shape(), GOOD)

    def test_claude_failing_is_its_error(self):
        self.run_with(chatmod.ClaudeError("no answer in 180 s"))
        with self.assertRaises(chatmod.ClaudeError):
            self.shape()

    def test_rate_limit_per_person_per_day(self):
        self.run_with(*([GOOD] * 6))
        self.shape(limit=2)
        self.shape(limit=2)
        with self.assertRaises(ask.RateLimited):
            self.shape(limit=2)
        self.shape(user="sam", limit=2)                     # sam's day is sam's
        self.shape(limit=None)                              # the server's own: uncounted
        self.clock[0] += 86400                              # tomorrow
        self.shape(limit=2)

    def test_a_bad_schema_asks_nothing(self):
        stub = self.run_with(GOOD)
        with self.assertRaises(ask.BadSchema):
            ask.ask_shape(self.chat, "x", {"type": "object", "if": {}}, user="alex", store=self.store)
        self.assertEqual(stub.prompts, [])
        self.assertEqual(self.store.list(ask.NS), [])               # and counts nothing


class Route(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        accounts.migrate("alex", "alex-password", "alex-token")
        accounts.add_user("sam", "sam-password")
        _, self.sam = accounts.add_device("sam", "hers")
        app.Handler.chat = chatmod.ChatService(claude="stub", token="alex-token")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def req(self, method, path, tok, body=None):
        if body is None and method == "POST":
            body = b""
        h = {"Authorization": "Bearer " + tok} if tok else {}
        r = urllib.request.Request(self.base + path, method=method, data=body, headers=h)
        try:
            resp = urllib.request.urlopen(r, timeout=20)
        except urllib.error.HTTPError as e:
            resp = e
        return (resp.status if hasattr(resp, "status") else resp.code), resp.read()

    def wait(self, jid, tok):
        for _ in range(100):
            s, body = self.req("GET", "/ask?id=" + jid, tok)
            if body != b"pending\n":
                return s, body
            time.sleep(0.05)
        self.fail("still pending")

    def test_post_then_poll(self):
        stub = Answers(GOOD)
        with mock.patch("server.chat.ask_once", stub):
            body = (json.dumps(ITEM) + "\na critter for a rainy day").encode()
            s, jid = self.req("POST", "/ask", "alex-token", body)
            self.assertEqual(s, 200)
            jid = jid.decode().strip()
            s, body = self.wait(jid, "alex-token")
        self.assertEqual(s, 200)
        head, line, rest = body.decode().split("\n")
        self.assertEqual((head, json.loads(line), rest), ("ok", GOOD, ""))
        self.assertIn("a critter for a rainy day", stub.prompts[0])
        # another person's request is not there for sam
        self.assertEqual(self.req("GET", "/ask?id=" + jid, self.sam)[0], 404)

    def test_refusals(self):
        def post(body, tok="alex-token"):
            return self.req("POST", "/ask", tok, body.encode())[0]
        self.assertEqual(post(""), 400)
        self.assertEqual(post(json.dumps(ITEM)), 400)                         # no prompt
        self.assertEqual(post("{not json\nhi"), 400)
        self.assertEqual(post(json.dumps({"oneOf": []}) + "\nhi"), 400)
        self.assertEqual(post(json.dumps(ITEM) + "\n" + "x" * (ask.PROMPT_MAX + 1)), 400)
        self.assertEqual(self.req("POST", "/ask", None, b"x")[0], 403)

    def test_daily_cap(self):
        old = ask.ROUTE_DAILY
        ask.ROUTE_DAILY = 1
        try:
            with mock.patch("server.chat.ask_once", Answers(GOOD, GOOD)):
                body = (json.dumps(ITEM) + "\nhi").encode()
                s, jid = self.req("POST", "/ask", self.sam, body)
                self.assertEqual(s, 200)
                self.wait(jid.decode().strip(), self.sam)
                s, why = self.req("POST", "/ask", self.sam, body)
                self.assertEqual(s, 429)
                self.assertTrue(why.startswith(b"error "))
                self.assertEqual(self.req("POST", "/ask", "alex-token", body)[0], 200)
        finally:
            ask.ROUTE_DAILY = old

    def test_a_failure_is_one_line(self):
        with mock.patch("server.chat.ask_once", Answers("nope", "still\nnope")):
            s, jid = self.req("POST", "/ask", "alex-token", (json.dumps(ITEM) + "\nhi").encode())
            s, body = self.wait(jid.decode().strip(), "alex-token")
        self.assertTrue(body.startswith(b"error "))
        self.assertEqual(body.count(b"\n"), 1)


if __name__ == "__main__":
    unittest.main()
