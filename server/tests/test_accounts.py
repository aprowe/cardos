"""More than one person on one server: the first start makes the owner from
what there was; each person's devices see their own music and nobody
else's; the Chat room is everyone's; the agent that edits the repository
is the owner's alone; the dashboard signs people in by name, makes device
tokens, and a new password ends that person's sessions.

    python -m server.tests.test_accounts
"""
import json
import os
import shutil
import struct
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
from server import accounts, app, dash
from server import chat as chatmod

TOKEN = "server-token"
PASSWORD = "owner-password"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def wav(seconds=0.5):
    n = int(22050 * seconds) * 2
    fmt = struct.pack("<HHIIHH", 1, 1, 22050, 44100, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", n) + b"\0" * n
    return b"RIFF" + struct.pack("<I", len(body)) + body


class Accounts(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        os.environ["DASH_PASSWORD"] = PASSWORD
        self.addCleanup(os.environ.pop, "DASH_PASSWORD", None)
        # the owner's single-person files, before accounts
        os.makedirs(os.path.join(self.dir, "notes"))
        with open(os.path.join(self.dir, "toggl.json"), "w") as f:
            f.write('{"token": "t"}')
        self.assertTrue(accounts.migrate("alex", PASSWORD, TOKEN))
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def req(self, method, path, body=None, bearer=None, cookie=None, form=None, js=None):
        data = body
        if form is not None:
            data = urllib.parse.urlencode(form).encode()
        if js is not None:
            data = json.dumps(js).encode()
        r = urllib.request.Request(self.base + path, method=method, data=data)
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        if cookie:
            r.add_header("Cookie", "%s=%s" % (dash.COOKIE, cookie))
        try:
            resp = OPENER.open(r, timeout=10)
        except urllib.error.HTTPError as e:
            resp = e
        code = resp.status if hasattr(resp, "status") else resp.code
        return code, dict(resp.headers), resp.read().decode()

    def login(self, name, pw):
        code, hdrs, _ = self.req("POST", "/dash/login", form={"name": name, "password": pw})
        if code != 303:
            return None
        return hdrs["Set-Cookie"].split(";")[0].split("=", 1)[1]

    def her(self):
        """A second person with a device, as the owner would set them up."""
        accounts.add_user("sam", "sams-password")
        _, tok = accounts.add_device("sam", "her Cardputer")
        return tok

    def test_the_first_start_makes_the_owner(self):
        self.assertTrue(accounts.is_admin("alex"))
        self.assertEqual(accounts.user_for_token(TOKEN), "alex")
        self.assertTrue(os.path.exists(os.path.join(self.dir, "users", "alex", "toggl.json")))
        self.assertTrue(os.path.isdir(os.path.join(self.dir, "users", "alex", "notes")))
        self.assertFalse(os.path.exists(os.path.join(self.dir, "toggl.json")))
        self.assertFalse(accounts.migrate("alex", PASSWORD, TOKEN))     # once only
        # the owner's existing device token still works
        self.assertEqual(self.req("GET", "/msg", bearer=TOKEN)[0], 200)
        self.assertEqual(self.req("GET", "/msg", bearer="nonsense")[0], 403)

    def test_each_persons_music_is_their_own(self):
        tok = self.her()
        s, _, body = self.req("POST", "/music/upload?title=Mine", body=wav(), bearer=TOKEN)
        self.assertEqual(s, 200)
        self.assertIn("Mine", self.req("GET", "/music", bearer=TOKEN)[2])
        self.assertEqual(self.req("GET", "/music", bearer=tok)[2], "")      # not hers
        self.req("POST", "/music/upload?title=Hers", body=wav(), bearer=tok)
        self.assertNotIn("Hers", self.req("GET", "/music", bearer=TOKEN)[2])

    def test_the_chat_room_is_shared(self):
        tok = self.her()
        self.req("POST", "/msg?name=Alex", body=b"hi", bearer=TOKEN)
        self.assertIn("Alex\thi", self.req("GET", "/msg?since=0", bearer=tok)[2])

    def test_the_repo_agent_is_the_owners(self):
        tok = self.her()
        self.assertEqual(self.req("GET", "/chat/new", bearer=tok)[0], 403)
        self.assertEqual(self.req("GET", "/chat/new", bearer=TOKEN)[0], 200)

    def test_the_dashboard_signs_people_in_by_name(self):
        self.her()
        self.assertIsNone(self.login("alex", "wrong"))
        mine = self.login("alex", PASSWORD)
        hers = self.login("sam", "sams-password")
        me = json.loads(self.req("GET", "/dash/api/me", cookie=mine)[2])
        self.assertEqual((me["user"], me["admin"]), ("alex", True))
        self.assertEqual([u["name"] for u in me["users"]], ["alex", "sam"])
        her_me = json.loads(self.req("GET", "/dash/api/me", cookie=hers)[2])
        self.assertEqual((her_me["user"], her_me["admin"]), ("sam", False))
        self.assertNotIn("users", her_me)
        # people are the owner's to make, and so is Claude's login
        self.assertEqual(self.req("POST", "/dash/api/user", cookie=hers, js={"name": "x", "password": "123456"})[0], 403)
        self.assertEqual(self.req("GET", "/dash/api/claude", cookie=hers)[0], 403)
        self.assertEqual(self.req("POST", "/dash/api/user", cookie=mine, js={"name": "kid", "password": "123456"})[0], 200)
        # a cookie made up for someone else does not work
        forged = "alex:" + hers.split(":", 1)[1]
        self.assertEqual(self.req("GET", "/dash/api/me", cookie=forged)[0], 403)

    def test_a_device_made_on_the_dashboard_is_hers(self):
        self.her()
        hers = self.login("sam", "sams-password")
        s, _, body = self.req("POST", "/dash/api/device", cookie=hers, js={"label": "spare"})
        tok = json.loads(body)["token"]
        self.assertEqual(accounts.user_for_token(tok), "sam")
        devs = json.loads(self.req("GET", "/dash/api/me", cookie=hers)[2])["devices"]
        self.assertEqual(sorted(d["label"] for d in devs), ["her Cardputer", "spare"])
        spare = [d["id"] for d in devs if d["label"] == "spare"][0]
        self.assertEqual(self.req("POST", "/dash/api/device/remove", cookie=hers, js={"id": spare})[0], 200)
        self.assertIsNone(accounts.user_for_token(tok))

    def test_a_new_password_ends_that_persons_sessions(self):
        self.her()
        mine = self.login("alex", PASSWORD)
        hers = self.login("sam", "sams-password")
        s, hdrs, _ = self.req("POST", "/dash/api/password", cookie=hers,
                              js={"old": "sams-password", "new": "a-new-one"})
        self.assertEqual(s, 200)
        fresh = hdrs["Set-Cookie"].split(";")[0].split("=", 1)[1]
        self.assertEqual(self.req("GET", "/dash/api/me", cookie=hers)[0], 403)      # the old one
        self.assertEqual(self.req("GET", "/dash/api/me", cookie=fresh)[0], 200)
        self.assertEqual(self.req("GET", "/dash/api/me", cookie=mine)[0], 200)      # mine untouched
        self.assertEqual(self.req("POST", "/dash/api/password", cookie=fresh,
                                  js={"old": "wrong", "new": "whatever"})[0], 403)


class Passwords(unittest.TestCase):
    def test_a_hash_checks_only_its_password(self):
        h = accounts.hash_password("secret")
        self.assertTrue(accounts.check_password(h, "secret"))
        self.assertFalse(accounts.check_password(h, "Secret"))
        self.assertFalse(accounts.check_password("garbage", "secret"))
        self.assertNotEqual(h, accounts.hash_password("secret"))         # salted


if __name__ == "__main__":
    unittest.main()
