"""Chat between devices: what one posts, the other gets after its last id;
names and text are flattened to one line; the room keeps its tail.

    python -m server.tests.test_msg
"""
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, msg
from server import chat as chatmod

TOKEN = "tok"


class Room(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        dash.logged_in = lambda h: False
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None, tok=TOKEN):
        r = urllib.request.Request(self.base + path, method=method, data=body)
        if tok:
            r.add_header("Authorization", "Bearer " + tok)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_two_devices_talk(self):
        self.assertEqual(self.req("POST", "/msg?name=Alex", b"hello\tthere\nyou"), (200, "1\n"))
        self.assertEqual(self.req("POST", "/msg?name=ADV", "hi! é".encode()), (200, "2\n"))
        s, body = self.req("GET", "/msg?since=0")
        rows = [l.split("\t") for l in body.splitlines()]
        self.assertEqual([(r[0], r[2], r[3]) for r in rows],
                         [("1", "Alex", "hello there you"), ("2", "ADV", "hi! é")])
        s, body = self.req("GET", "/msg?since=1")
        self.assertEqual(len(body.splitlines()), 1)
        self.assertEqual(self.req("GET", "/msg?since=2"), (200, ""))
        msg.post("A", "three")
        s, body = self.req("GET", "/msg?since=1&max=1")       # the next one, not the last
        self.assertEqual(body.split("\t")[0], "2")

    def test_refusals(self):
        self.assertEqual(self.req("POST", "/msg?name=", b"x")[0], 400)
        self.assertEqual(self.req("POST", "/msg?name=A", b"   ")[0], 400)
        self.assertEqual(self.req("GET", "/msg", tok=None)[0], 403)

    def test_the_room_keeps_its_tail(self):
        for i in range(msg.KEEP + 5):
            msg.post("A", "m%d" % i)
        self.assertEqual(len(msg.load()), msg.KEEP)
        self.assertEqual(msg.since(0)[-1]["text"], "m%d" % (msg.KEEP + 4))
        self.assertEqual(len(msg.since(0)), msg.SHOW)


if __name__ == "__main__":
    unittest.main()
