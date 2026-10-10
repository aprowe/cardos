"""server/kv.py: the general-purpose store -- values, counters, queues --
and its routes, as two people with their own devices.

    python -m server.tests.test_kv
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
from server import accounts, app, kv
from server import chat as chatmod


class Clock:
    def __init__(self, t=1000000.0):
        self.t = t

    def __call__(self):
        return self.t


class StoreTest(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "kv.sqlite3")
        self.clock = Clock()
        self.s = kv.Store(self.path, clock=self.clock, quota=4096)

    def tearDown(self):
        self.s.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_values(self):
        self.assertIsNone(self.s.get("u/alex", "a"))
        self.s.put("u/alex", "a", b"\x00\x01bytes\n", who="alex")
        self.assertEqual(self.s.get("u/alex", "a"), b"\x00\x01bytes\n")
        self.s.put("u/alex", "a", b"again", who="alex")
        self.assertEqual(self.s.get("u/alex", "a"), b"again")
        self.assertTrue(self.s.delete("u/alex", "a"))
        self.assertFalse(self.s.delete("u/alex", "a"))
        self.assertIsNone(self.s.get("u/alex", "a"))

    def test_namespaces_are_apart(self):
        self.s.put("u/alex", "k", b"alex's", who="alex")
        self.s.put("u/sam", "k", b"sam's", who="sam")
        self.s.put("u/alex/jar", "k", b"alex's jar", who="alex")
        self.assertEqual(self.s.get("u/alex", "k"), b"alex's")
        self.assertEqual(self.s.get("u/sam", "k"), b"sam's")
        self.assertEqual(self.s.get("u/alex/jar", "k"), b"alex's jar")
        self.assertEqual([r[0] for r in self.s.list("u/alex")], ["k"])

    def test_resolve(self):
        self.assertEqual(kv.resolve("me", "alex"), "u/alex")
        self.assertEqual(kv.resolve("me/jar", "alex"), "u/alex/jar")
        self.assertEqual(kv.resolve("app/jar", "alex"), "app/jar")
        self.assertEqual(kv.resolve("srv/jar", "alex"), "srv/jar")
        with self.assertRaises(kv.NotAllowed):
            kv.resolve("srv/jar", "alex", writing=True)
        self.assertEqual(kv.resolve("srv/jar", "alex", writing=True, server=True), "srv/jar")
        for bad in ("", "u/sam", "me/", "me/../x", "app/Jar", "other/x", "me/a/b"):
            with self.assertRaises(kv.KVError, msg=bad):
                kv.resolve(bad, "alex")

    def test_list_by_prefix(self):
        for k in ("stock/1", "stock/2", "stocky", "other"):
            self.s.put("app/jar", k, b"xyz", who="alex")
        self.s.put("app/jar", "stock/3", b"x", ttl=10)
        rows = self.s.list("app/jar", "stock/")
        self.assertEqual([(r[0], r[1]) for r in rows], [("stock/1", 3), ("stock/2", 3), ("stock/3", 1)])
        self.assertIsNone(rows[0][2])
        self.assertEqual(rows[2][2], self.clock.t + 10)
        self.assertEqual(len(self.s.list("app/jar", "", 2)), 2)
        # LIKE's wildcards and case are not a prefix's
        self.s.put("app/jar", "STOCK/9", b"x")
        self.s.put("app/jar", "a_b", b"x")
        self.s.put("app/jar", "axb", b"x")
        self.assertEqual([r[0] for r in self.s.list("app/jar", "a_")], ["a_b"])
        self.assertNotIn("STOCK/9", [r[0] for r in self.s.list("app/jar", "stock")])

    def test_ttl(self):
        self.s.put("u/alex", "t", b"soon gone", ttl=60, who="alex")
        self.clock.t += 59
        self.assertEqual(self.s.get("u/alex", "t"), b"soon gone")
        self.clock.t += 2
        self.assertIsNone(self.s.get("u/alex", "t"))
        self.assertEqual(self.s.list("u/alex"), [])
        self.assertEqual(self.s.usage("alex"), 0)      # the expired counts for nothing
        self.assertFalse(self.s.delete("u/alex", "t"))
        with self.assertRaises(kv.KVError):
            self.s.put("u/alex", "t", b"x", ttl=0)

    def test_size_and_quota(self):
        with self.assertRaises(kv.TooBig):
            self.s.put("u/alex", "big", b"x" * (kv.VALUE_MAX + 1), who="alex")
        self.s.put("u/alex", "a", b"x" * 3000, who="alex")
        self.assertEqual(self.s.usage("alex"), 3001)
        with self.assertRaises(kv.Full):
            self.s.put("u/alex", "b", b"x" * 1200, who="alex")
        self.s.put("u/alex", "a", b"x" * 4000, who="alex")       # replacing frees the old
        self.s.put("u/sam", "b", b"x" * 4000, who="sam")         # another's quota is theirs
        self.s.put("srv/jar", "big", b"x" * 9000)                # the server's is no one's
        with self.assertRaises(kv.Full):
            self.s.push("alex", "mail", b"x" * 200, sender="alex")
        self.s.delete("u/alex", "a")
        self.s.push("alex", "mail", b"x" * 200, sender="alex")
        self.assertEqual(self.s.usage("alex"), 200)

    def test_counters(self):
        self.assertEqual(self.s.incr("app/jar", "next_id"), 1)
        self.assertEqual(self.s.incr("app/jar", "next_id"), 2)
        self.assertEqual(self.s.incr("app/jar", "next_id", by=10), 12)
        self.assertEqual(self.s.incr("app/jar", "next_id", by=-2), 10)
        self.assertEqual(self.s.get("app/jar", "next_id"), b"10")
        self.s.put("app/jar", "word", b"jam")
        with self.assertRaises(kv.NotANumber):
            self.s.incr("app/jar", "word")
        # a ttl counts a day; the expiry is kept on later adds
        self.s.incr("u/alex", "asks", ttl=100, who="alex")
        self.clock.t += 50
        self.assertEqual(self.s.incr("u/alex", "asks", who="alex"), 2)
        self.clock.t += 51
        self.assertEqual(self.s.incr("u/alex", "asks", who="alex"), 1)

    def test_counters_are_atomic(self):
        def bump():
            for _ in range(50):
                self.s.incr("app/x", "n")
        ts = [threading.Thread(target=bump) for _ in range(8)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual(self.s.get("app/x", "n"), b"400")

    def test_queue_needs_the_owners_leave(self):
        with self.assertRaises(kv.NotAllowed):
            self.s.push("alex", "gifts", b"a jar", sender="sam")
        self.assertEqual(self.s.qlen("alex", "gifts"), 0)
        self.s.allow("alex", "gifts", "sam")
        self.assertEqual(self.s.allowed("alex", "gifts"), ["sam"])
        i1 = self.s.push("alex", "gifts", b"a jar", sender="sam")
        i2 = self.s.push("alex", "gifts", b"a snail", sender="sam")
        i3 = self.s.push("alex", "gifts", b"from me", sender="alex")      # one's own: always
        i4 = self.s.push("alex", "gifts", b"from the server")             # server code: always
        self.assertTrue(i1 < i2 < i3 < i4)
        # allowed on gifts is not allowed on another queue
        with self.assertRaises(kv.NotAllowed):
            self.s.push("alex", "notes", b"x", sender="sam")
        got = self.s.peek("alex", "gifts")
        self.assertEqual([(m[0], m[1], m[3]) for m in got],
                         [(i1, "sam", b"a jar"), (i2, "sam", b"a snail"),
                          (i3, "alex", b"from me"), (i4, "", b"from the server")])
        self.assertEqual(self.s.peek("alex", "gifts", after=i2, limit=1)[0][0], i3)
        # peeking takes nothing
        self.assertEqual(self.s.qlen("alex", "gifts"), 4)
        self.assertEqual(self.s.ack("alex", "gifts", i2), 2)
        self.assertEqual([m[0] for m in self.s.peek("alex", "gifts")], [i3, i4])
        self.assertEqual(self.s.pop("alex", "gifts")[3], b"from me")
        self.assertEqual(self.s.qlen("alex", "gifts"), 1)
        # sam's own queue of the same name is another queue
        self.assertEqual(self.s.qlen("sam", "gifts"), 0)
        self.s.allow("alex", "gifts", "sam", on=False)
        with self.assertRaises(kv.NotAllowed):
            self.s.push("alex", "gifts", b"x", sender="sam")
        self.assertEqual(self.s.allowed("alex", "gifts"), [])
        self.s.allow("alex", "gifts", "*")
        self.s.push("alex", "gifts", b"anyone", sender="kim")

    def test_peek_bytes_and_ttl(self):
        for n in range(3):
            self.s.push("alex", "q", b"x" * 1000, ttl=30 if n == 1 else None)
        self.assertEqual(len(self.s.peek("alex", "q", max_bytes=1500)), 1)
        self.assertEqual(len(self.s.peek("alex", "q", max_bytes=10)), 1)     # always one
        self.clock.t += 31
        self.assertEqual(self.s.qlen("alex", "q"), 2)

    def test_queue_full(self):
        old = kv.QUEUE_MAX
        kv.QUEUE_MAX = 3
        try:
            for _ in range(3):
                self.s.push("alex", "q", b"x")
            with self.assertRaises(kv.Full):
                self.s.push("alex", "q", b"x")
        finally:
            kv.QUEUE_MAX = old

    def test_persists_across_reopen(self):
        self.s.put("u/alex", "k", b"kept", who="alex")
        self.s.incr("app/jar", "n", by=5)
        self.s.allow("alex", "gifts", "sam")
        i = self.s.push("alex", "gifts", b"parcel", sender="sam")
        self.s.saw("alex", 123)
        self.s.close()
        self.s = kv.Store(self.path, clock=self.clock, quota=4096)
        self.assertEqual(self.s.get("u/alex", "k"), b"kept")
        self.assertEqual(self.s.incr("app/jar", "n"), 6)
        self.assertEqual(self.s.peek("alex", "gifts")[0][0], i)
        self.assertEqual(self.s.allowed("alex", "gifts"), ["sam"])
        self.assertEqual(self.s.people()["alex"], (None, 123))
        self.assertGreater(self.s.push("alex", "gifts", b"next"), i)     # ids only grow

    def test_file_is_owner_only(self):
        if os.name == "posix":
            self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)


# ---- the routes, as two people ------------------------------------------------------

ALEX_PW = "alex-password"


class Routes(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        accounts.migrate("alex", ALEX_PW, "alex-token")
        accounts.add_user("sam", "sam-password")
        _, self.sam = accounts.add_device("sam", "hers")
        self.alex = "alex-token"
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

    def test_values_are_each_persons(self):
        self.assertEqual(self.req("POST", "/kv/put?ns=me&key=coins", self.alex, b"120"),
                         (200, b"ok\n"))
        self.assertEqual(self.req("GET", "/kv/get?ns=me&key=coins", self.alex), (200, b"120"))
        self.assertEqual(self.req("GET", "/kv/get?ns=me&key=coins", self.sam)[0], 404)
        self.assertEqual(self.req("GET", "/kv/get?ns=me&key=coins", None)[0], 403)
        self.req("POST", "/kv/put?ns=me/jar&key=save", self.sam, b"\x00\xffraw")
        self.assertEqual(self.req("GET", "/kv/get?ns=me/jar&key=save", self.sam), (200, b"\x00\xffraw"))
        self.assertEqual(self.req("GET", "/kv/get?ns=me/jar&key=save", self.alex)[0], 404)
        # app/ is shared
        self.req("POST", "/kv/put?ns=app/jar&key=world", self.sam, b"sunny")
        self.assertEqual(self.req("GET", "/kv/get?ns=app/jar&key=world", self.alex), (200, b"sunny"))

    def test_list_del_incr(self):
        self.req("POST", "/kv/put?ns=me&key=a/1&ttl=3600", self.alex, b"abc")
        self.req("POST", "/kv/put?ns=me&key=a/2", self.alex, b"de")
        self.req("POST", "/kv/put?ns=me&key=b", self.alex, b"f")
        s, body = self.req("GET", "/kv/list?ns=me&prefix=a/", self.alex)
        lines = body.decode().splitlines()
        self.assertEqual(s, 200)
        self.assertEqual(len(lines), 2)
        k, size, exp = lines[0].split("\t")
        self.assertEqual((k, size), ("a/1", "3"))
        self.assertGreater(int(exp), 0)
        self.assertEqual(lines[1], "a/2\t2\t0")
        self.assertEqual(self.req("POST", "/kv/del?ns=me&key=b", self.alex), (200, b"ok 1\n"))
        self.assertEqual(self.req("POST", "/kv/del?ns=me&key=b", self.alex), (200, b"ok 0\n"))
        self.assertEqual(self.req("POST", "/kv/incr?ns=app/jar&key=n", self.alex), (200, b"1\n"))
        self.assertEqual(self.req("POST", "/kv/incr?ns=app/jar&key=n&by=4", self.sam), (200, b"5\n"))
        self.assertEqual(self.req("GET", "/kv/get?ns=app/jar&key=n", self.sam), (200, b"5"))
        self.req("POST", "/kv/put?ns=me&key=w", self.alex, b"word")
        self.assertEqual(self.req("POST", "/kv/incr?ns=me&key=w", self.alex)[0], 409)

    def test_refusals(self):
        self.assertEqual(self.req("POST", "/kv/put?ns=srv/jar&key=x", self.alex, b"x")[0], 403)
        self.assertEqual(self.req("POST", "/kv/put?ns=nope&key=x", self.alex, b"x")[0], 400)
        self.assertEqual(self.req("POST", "/kv/put?ns=me&key=a%20b", self.alex, b"x")[0], 400)
        self.assertEqual(self.req("POST", "/kv/put?ns=me&key=x&ttl=soon", self.alex, b"x")[0], 400)
        s, body = self.req("POST", "/kv/put?ns=me&key=x", self.alex, b"x" * (kv.VALUE_MAX + 1))
        self.assertEqual(s, 413)
        self.assertTrue(body.startswith(b"error "))
        # srv/ is readable
        kv.store().put("srv/jar", "stock", b"signed")
        self.assertEqual(self.req("GET", "/kv/get?ns=srv/jar&key=stock", self.sam), (200, b"signed"))

    def test_queues(self):
        # sam may not send to alex until alex allows it
        s, body = self.req("POST", "/q/push?q=gifts&to=alex", self.sam, b"a jar")
        self.assertEqual(s, 403)
        self.assertEqual(self.req("POST", "/q/push?q=gifts&to=kim", self.sam, b"x")[0], 404)
        self.assertEqual(self.req("POST", "/q/allow?q=gifts&who=kim", self.alex)[0], 404)
        self.assertEqual(self.req("POST", "/q/allow?q=gifts&who=sam", self.alex), (200, b"ok\n"))
        self.assertEqual(self.req("GET", "/q/allow?q=gifts", self.alex), (200, b"sam\n"))
        s, body = self.req("POST", "/q/push?q=gifts&to=alex", self.sam, b"a jar")
        self.assertEqual(s, 200)
        i1 = int(body.split()[1])
        s, body = self.req("POST", "/q/push?q=gifts&to=alex", self.sam, b"two\nlines")
        i2 = int(body.split()[1])
        self.assertEqual(self.req("GET", "/q/len?q=gifts", self.alex), (200, b"2\n"))
        self.assertEqual(self.req("GET", "/q/len?q=gifts", self.sam), (200, b"0\n"))
        s, body = self.req("GET", "/q/peek?q=gifts", self.alex)
        self.assertEqual(s, 200)
        # the framing: header line, SIZE bytes, newline
        msgs = []
        while body:
            head, body = body.split(b"\n", 1)
            mid, frm, at, size = head.decode().split("\t")
            size = int(size)
            msgs.append((int(mid), frm, body[:size]))
            self.assertEqual(body[size:size + 1], b"\n")
            body = body[size + 1:]
        self.assertEqual(msgs, [(i1, "sam", b"a jar"), (i2, "sam", b"two\nlines")])
        s, body = self.req("GET", "/q/peek?q=gifts&after=%d" % i1, self.alex)
        self.assertTrue(body.startswith(b"%d\tsam\t" % i2))
        self.assertEqual(self.req("POST", "/q/ack?q=gifts&upto=%d" % i1, self.alex), (200, b"ok 1\n"))
        # sam cannot ack alex's queue: an ack is always one's own
        self.assertEqual(self.req("POST", "/q/ack?q=gifts&upto=%d" % i2, self.sam), (200, b"ok 0\n"))
        self.assertEqual(self.req("GET", "/q/len?q=gifts", self.alex), (200, b"1\n"))
        self.assertEqual(self.req("POST", "/q/ack?q=gifts", self.alex)[0], 400)
        self.req("POST", "/q/ack?q=gifts&upto=%d" % i2, self.alex)
        self.assertEqual(self.req("GET", "/q/peek?q=gifts", self.alex), (200, b""))
        # off
        self.req("POST", "/q/allow?q=gifts&who=sam&off=1", self.alex)
        self.assertEqual(self.req("GET", "/q/allow?q=gifts", self.alex), (200, b""))
        self.assertEqual(self.req("POST", "/q/push?q=gifts&to=alex", self.sam, b"x")[0], 403)
        # one's own queue, no to=
        self.assertEqual(self.req("POST", "/q/push?q=todo", self.sam, b"x")[0], 200)
        self.assertEqual(self.req("POST", "/q/push?q=Bad%20Name", self.sam, b"x")[0], 400)


class WithoutAccounts(unittest.TestCase):
    """One person, as a server with no accounts.json is."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_me_is_the_one_person(self):
        self.assertEqual(kv.me(), kv.NOBODY)
        self.assertEqual(kv._person(""), kv.NOBODY)
        with self.assertRaises(kv.NotFound):
            kv._person("sam")
        st = kv.store()
        self.assertIs(st, kv.store())
        self.assertEqual(st.path, os.path.abspath(os.path.join(self.dir, "kv.sqlite3")))


if __name__ == "__main__":
    unittest.main()
