"""server/people.py: who is on this server, as a friend sees them.

    python -m server.tests.test_people
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
from server import accounts, app, kv, people
from server import chat as chatmod


class People(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        accounts.migrate("alex", "alex-password", "alex-token")
        accounts.add_user("sam", "sam-password")
        _, self.sam = accounts.add_device("sam", "hers")
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
        h = {"Authorization": "Bearer " + tok} if tok else {}
        r = urllib.request.Request(self.base + path, method=method, data=body, headers=h)
        try:
            resp = urllib.request.urlopen(r, timeout=20)
        except urllib.error.HTTPError as e:
            resp = e
        return (resp.status if hasattr(resp, "status") else resp.code), resp.read()

    def rows(self, tok):
        s, body = self.req("GET", "/people", tok)
        self.assertEqual(s, 200)
        return {line.split("\t")[0]: line.split("\t")[1:] for line in body.decode().splitlines()}

    def test_everyone_with_last_seen(self):
        rows = self.rows("alex-token")
        self.assertEqual(sorted(rows), ["alex", "sam"])
        self.assertEqual(rows["alex"][0], "alex")        # display: the name until chosen
        self.assertGreater(int(rows["alex"][1]), 0)       # this request saw alex
        self.assertEqual(rows["sam"], ["sam", "0"])       # sam never came
        self.req("GET", "/kv/list?ns=me", self.sam)
        self.assertGreater(int(self.rows("alex-token")["sam"][1]), 0)
        self.assertEqual(self.req("GET", "/people", None)[0], 403)

    def test_display_name(self):
        self.assertEqual(self.req("POST", "/people/name", self.sam, "Sam\t the\nGreat ".encode()),
                         (200, b"ok\n"))
        self.assertEqual(self.rows("alex-token")["sam"][0], "Sam the Great")
        self.req("POST", "/people/name", self.sam, b"x" * 40)
        self.assertEqual(self.rows("alex-token")["sam"][0], "x" * people.DISPLAY_MAX)
        self.req("POST", "/people/name", self.sam, b"")
        self.assertEqual(self.rows("alex-token")["sam"][0], "sam")

    def test_seen_is_written_once_a_minute(self):
        people._last.clear()
        people.saw("sam", now=5000)
        people.saw("sam", now=5030)                       # too soon: not written
        self.assertEqual(kv.store().people()["sam"][1], 5000)
        people.saw("sam", now=5061)
        self.assertEqual(kv.store().people()["sam"][1], 5061)


class NoAccounts(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_nobody_to_list(self):
        people.saw(None)
        people.saw("-")
        self.assertEqual(people.listing(), [])


if __name__ == "__main__":
    unittest.main()
