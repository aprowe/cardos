"""server/friends.py, pub/ in server/kv.py, server/rooms.py and /share:
what two or three friends on one server share.

    python -m server.tests.test_friends
"""
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import accounts, app, kv, msg, people
from server import chat as chatmod


class Friends(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        accounts.migrate("alex", "alex-password", "alex-token")
        accounts.add_user("sam", "sam-password")
        accounts.add_user("kim", "kim-password")
        _, self.sam = accounts.add_device("sam", "hers")
        _, self.kim = accounts.add_device("kim", "theirs")
        self.alex = "alex-token"
        people._last.clear()
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
        if isinstance(body, str):
            body = body.encode()
        h = {"Authorization": "Bearer " + tok} if tok else {}
        r = urllib.request.Request(self.base + path, method=method, data=body, headers=h)
        try:
            resp = urllib.request.urlopen(r, timeout=20)
        except urllib.error.HTTPError as e:
            resp = e
        return (resp.status if hasattr(resp, "status") else resp.code), resp.read()

    def ok(self, method, path, tok, body=None):
        s, b = self.req(method, path, tok, body)
        self.assertEqual(s, 200, b)
        return b.decode()

    def befriend(self, a, b, name_b):
        self.ok("POST", "/friends/ask?who=" + name_b, a)
        self.ok("POST", "/friends/ask?who=" + {self.alex: "alex", self.sam: "sam",
                                                self.kim: "kim"}[a], b)

    def notes(self, user):
        return [x["text"] for x in msg.notes_since(user, 0)[0]]

    # ---- friends ----------------------------------------------------------------

    def test_ask_then_ask_back(self):
        self.assertEqual(self.ok("POST", "/friends/ask?who=sam", self.alex), "ok asked\n")
        self.assertIn("wants to be friends", self.notes("sam"))
        rows = self.ok("GET", "/friends", self.sam).splitlines()
        self.assertEqual(rows[0].split("\t")[:3], ["alex", "alex", "asking"])
        self.assertEqual(self.ok("POST", "/friends/ask?who=alex", self.sam), "ok friend\n")
        self.assertEqual(self.ok("GET", "/friends", self.alex).split("\t")[2], "friend")
        self.assertIn("is your friend now", self.notes("alex"))
        self.ok("POST", "/friends/drop?who=alex", self.sam)
        self.assertEqual(self.ok("GET", "/friends", self.alex), "")

    def test_ask_refusals(self):
        self.assertEqual(self.req("POST", "/friends/ask?who=alex", self.alex)[0], 400)
        self.assertEqual(self.req("POST", "/friends/ask?who=nobody", self.alex)[0], 404)

    # ---- pub/ -------------------------------------------------------------------

    def test_pub_is_read_by_friends_only(self):
        self.ok("POST", "/kv/put?ns=pub/wordle&key=stats", self.alex, "day 300 3")
        self.ok("POST", "/kv/put?ns=pub/wordle&key=stats", self.sam, "day 300 5")
        self.ok("POST", "/kv/put?ns=pub/wordle&key=stats", self.kim, "day 300 X")
        self.assertEqual(self.req("GET", "/kv/get?ns=pub/wordle&key=stats&of=sam", self.alex)[0], 403)
        self.befriend(self.alex, self.sam, "sam")
        self.assertEqual(self.ok("GET", "/kv/get?ns=pub/wordle&key=stats&of=sam", self.alex),
                         "day 300 5")
        self.assertEqual(self.ok("GET", "/kv/get?ns=pub/wordle&key=stats", self.alex), "day 300 3")
        each = self.ok("GET", "/kv/each?ns=pub/wordle&key=stats", self.alex)
        self.assertEqual(each, "alex\talex\t9\nday 300 3\nsam\tsam\t9\nday 300 5\n")
        # kim is nobody's friend: kim's value is not in alex's each
        self.assertNotIn("kim", each)
        # a pub/ write is one's own: sam cannot write alex's
        self.ok("POST", "/kv/put?ns=pub/wordle&key=stats", self.sam, "day 301 2")
        self.assertEqual(self.ok("GET", "/kv/get?ns=pub/wordle&key=stats", self.alex), "day 300 3")
        self.assertEqual(self.req("GET", "/kv/each?ns=me/wordle&key=stats", self.alex)[0], 400)

    # ---- rooms ------------------------------------------------------------------

    def test_room_with_a_friend(self):
        self.assertEqual(self.req("POST", "/room/new?app=crossword&with=sam", self.alex, "xw")[0], 403)
        self.befriend(self.alex, self.sam, "sam")
        rid = int(self.ok("POST", "/room/new?app=crossword&with=sam&title=Thursday",
                          self.alex, "xw 1 2026-10-22").split()[1])
        self.assertIn("Thursday", self.notes("sam"))
        rows = self.ok("GET", "/room/list?app=crossword", self.sam).splitlines()
        self.assertEqual(rows[0].split("\t")[:4], [str(rid), "Thursday", "alex,sam", "0"])
        self.assertEqual(self.ok("GET", "/room/list?app=crossword", self.kim), "")
        self.assertEqual(self.req("GET", "/room/get?id=%d" % rid, self.kim)[0], 403)

        self.assertEqual(self.ok("POST", "/room/move?id=%d" % rid, self.alex, "1 1 A"), "ok 1\n")
        self.assertEqual(self.ok("POST", "/room/move?id=%d" % rid, self.sam, "1 2 B\n1 3 C"), "ok 2\n")
        got = self.ok("GET", "/room/moves?id=%d&after=0" % rid, self.alex)
        self.assertEqual(got.split("\n")[0].split("\t")[:2], ["1", "alex"])
        self.assertIn("2\tsam\t", got)
        self.assertTrue(got.endswith("1 2 B\n1 3 C\n"))
        self.assertTrue(self.ok("GET", "/room/moves?id=%d&after=1" % rid, self.alex).startswith("2\tsam"))
        self.assertEqual(self.ok("GET", "/room/moves?id=%d&after=2" % rid, self.alex), "")
        # moves without say= tell nobody; with it the others are told
        before = len(self.notes("alex"))
        self.ok("POST", "/room/move?id=%d&say=your+move" % rid, self.sam, "x")
        self.assertEqual(self.notes("alex")[before:], ["your move"])

    def test_room_state_versions(self):
        self.befriend(self.alex, self.sam, "sam")
        rid = int(self.ok("POST", "/room/new?app=words&with=sam", self.alex, "board 0").split()[1])
        head = self.ok("GET", "/room/get?id=%d" % rid, self.sam)
        self.assertEqual(head, "%d\t1\t0\talex,sam\nboard 0" % rid)
        self.assertEqual(self.ok("POST", "/room/state?id=%d&if=1" % rid, self.sam, "board 1"), "ok 2\n")
        s, b = self.req("POST", "/room/state?id=%d&if=1" % rid, self.alex, "board 1b")
        self.assertEqual((s, b), (409, b"error changed 2\n"))
        self.ok("POST", "/room/leave?id=%d" % rid, self.alex)
        self.assertEqual(self.req("GET", "/room/get?id=%d" % rid, self.alex)[0], 403)
        self.ok("POST", "/room/leave?id=%d" % rid, self.sam)
        self.assertEqual(self.req("GET", "/room/get?id=%d" % rid, self.sam)[0], 404)

    # ---- share ------------------------------------------------------------------

    def test_share_lands_in_chat(self):
        self.ok("POST", "/people/name", self.sam, "Sam")
        self.ok("POST", "/share?app=Wordle", self.sam, "Wordle #300 3/6")
        last = self.ok("GET", "/msg?since=0", self.alex).splitlines()[-1].split("\t")
        self.assertEqual(last[2:], ["Sam", "[Wordle] Wordle #300 3/6"])


if __name__ == "__main__":
    unittest.main()
