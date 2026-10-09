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


class Server(unittest.TestCase):
    """A server with accounts on: the owner made from a single-person state."""

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

    def wait(self, jid, tok):
        import time
        for _ in range(50):
            s, _, body = self.req("GET", "/chat?id=" + jid, bearer=tok)
            if not body.startswith("pending"):
                return body
            time.sleep(0.1)
        return body



class Accounts(Server):

    def test_the_first_start_makes_the_owner(self):
        self.assertTrue(accounts.is_admin("alex"))
        self.assertEqual(accounts.user_for_token(TOKEN), "alex")
        self.assertTrue(os.path.exists(os.path.join(self.dir, "users", "alex", "toggl.json")))
        self.assertTrue(os.path.isdir(os.path.join(self.dir, "users", "alex", "notes")))
        self.assertFalse(os.path.exists(os.path.join(self.dir, "toggl.json")))
        self.assertFalse(accounts.migrate("alex", PASSWORD, TOKEN))     # once only
        self.assertEqual(accounts.devices("alex")[0]["token"], TOKEN)    # and it can be looked up
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

    def test_build_given_to_someone_is_fenced_and_their_own(self):
        tok = self.her()
        accounts.set_build("sam", True)
        seen = []

        def fake_stream(cmd, env, cwd, on_status, idle, cap, on_log=None):
            seen.append((cmd, dict(env)))
            return "ok", "sid-" + env.get("FENCE_USER", "owner")
        real = chatmod.run_stream
        chatmod.run_stream = fake_stream
        self.addCleanup(setattr, chatmod, "run_stream", real)
        app.Handler.chat._plan = lambda text: [text]
        s, _, body = self.req("POST", "/chat", body=b"make me a maze game", bearer=tok)
        self.assertEqual(s, 200)
        jid = body.split()[1]
        # the owner cannot read her answer, nor she his
        b = self.wait(jid, tok); self.assertTrue(b.startswith("done"), b)
        cmd, env = seen[-1]
        self.assertEqual(env["FENCE_USER"], "sam")          # the hook knows whose
        self.assertIn("--settings", cmd)                     # and is installed
        self.assertIn("Bash", cmd[cmd.index("--disallowed-tools") + 1])
        self.assertIn("may make new apps", cmd[cmd.index("-p") + 1])
        self.assertEqual(app.Handler.chat.sessions.get("sam"), "sid-sam")
        self.assertIsNone(app.Handler.chat.session_id)      # not the owner's conversation
        # the owner's turn is not fenced, and its answer is not hers to read
        s, _, body = self.req("POST", "/chat", body=b"fix the kernel", bearer=TOKEN)
        jid2 = body.split()[1]
        self.assertIn("no such request", self.req("GET", "/chat?id=" + jid2, bearer=tok)[2])
        self.assertTrue(self.wait(jid2, TOKEN).startswith("done"))
        cmd, env = seen[-1]
        self.assertNotIn("FENCE_USER", env)
        self.assertNotIn("--settings", cmd)
        self.assertEqual(app.Handler.chat.session_id, "sid-owner")

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
        s, _, body = self.req("GET", "/dash/api/claude", cookie=hers)
        self.assertEqual(s, 403)
        self.assertTrue(json.loads(body)["owner_only"])        # not "signed out" to the page
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
        # kept, so it can be looked up again
        self.assertEqual([d["token"] for d in devs if d["label"] == "spare"], [tok])
        # a new token: the old one stops, the new one works
        spare = [d["id"] for d in devs if d["label"] == "spare"][0]
        s, _, body = self.req("POST", "/dash/api/device/token", cookie=hers, js={"id": spare})
        fresh = json.loads(body)["token"]
        self.assertIsNone(accounts.user_for_token(tok))
        self.assertEqual(accounts.user_for_token(fresh), "sam")
        tok = fresh
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


class PerPerson(Server):
    """State kept per person, checked with accounts on -- how the bugs these
    pin got through: every other test of Google and Toggl runs single-person."""

    def as_me(self, name="alex"):
        accounts.set_current(name)
        self.addCleanup(accounts.set_current, None)

    def google(self):
        from server import google
        os.environ.update(GOOGLE_CLIENT_ID="cid", GOOGLE_CLIENT_SECRET="csec",
                          DASH_URL="https://dash.example")
        refreshes = []

        def fake_http(method, url, body=None, headers=None, form=False):
            refreshes.append(body["refresh_token"])
            return 200, {"access_token": "AT%d" % len(refreshes), "expires_in": 3600}
        for name, fake in (("http", fake_http),):
            self.addCleanup(setattr, google, name, getattr(google, name))
            setattr(google, name, fake)
        self.addCleanup(setattr, dash, "exchange_code", dash.exchange_code)
        dash.exchange_code = lambda code, *a: {"refresh_token": "r-" + code, "scope": "tasks"}
        self.addCleanup(setattr, dash, "revoke", dash.revoke)
        dash.revoke = lambda token: None
        google._access_by.clear()
        self.addCleanup(google._access_by.clear)
        return google, refreshes

    def sign_in(self, cookie, code):
        _, hdrs, _ = self.req("GET", "/dash/google/start", cookie=cookie)
        q = urllib.parse.parse_qs(urllib.parse.urlparse(hdrs["Location"]).query)
        s, hdrs, _ = self.req("GET", "/dash/google/callback?" + urllib.parse.urlencode(
            {"state": q["state"][0], "code": code}), cookie=cookie)
        self.assertIn("Signed%20in", hdrs["Location"])

    def test_a_google_sign_in_drops_that_persons_access_token(self):
        google, refreshes = self.google()
        mine = self.login("alex", PASSWORD)
        self.sign_in(mine, "one")
        self.as_me()
        self.assertEqual(google.access_token(), "AT1")
        self.assertEqual(google.access_token(), "AT1")          # cached
        # Signing in again (to add the Tasks scope, say) is a new login: the
        # next call must use it, not the hour-long token from the old one.
        self.sign_in(mine, "two")
        self.assertEqual(google.access_token(), "AT2")
        self.assertEqual(refreshes, ["r-one", "r-two"])

    def test_signing_out_of_google_drops_the_access_token(self):
        google, _ = self.google()
        mine = self.login("alex", PASSWORD)
        for route in ("/dash/api/google/forget", "/dash/google/forget"):
            self.sign_in(mine, "x")
            self.as_me()
            google.access_token()
            self.assertTrue(google._access_by["alex"]["token"])
            self.req("POST", route, js={}, cookie=mine)
            self.assertIsNone(google._access_by["alex"]["token"], route)
            with self.assertRaises(google.GoogleError):
                google.access_token()

    def test_pushed_google_creds_drop_the_access_token(self):
        google, _ = self.google()
        self.as_me()
        google.save_creds("cid", "csec", "r-a")
        self.assertEqual(google.access_token(), "AT1")
        google.save_creds("cid", "csec", "r-b")
        self.assertEqual(google.access_token(), "AT2")

    def test_a_new_toggl_token_keeps_the_targets(self):
        from server import toggl
        self.as_me()
        target = {"project": 7, "kind": "week", "hours": 5}
        toggl.save({"token": "old", "name": "Alex", "workspace": 5, "targets": [target]})
        self.addCleanup(setattr, toggl, "check_token", toggl.check_token)
        toggl.check_token = lambda token: ("Alex", 6)
        s, _, _ = self.req("POST", "/dash/api/toggl/token", js={"token": "new"},
                           cookie=self.login("alex", PASSWORD))
        self.assertEqual(s, 200)
        c = toggl.load()
        self.assertEqual((c["token"], c["workspace"]), ("new", 6))
        self.assertEqual(c["targets"], [target])

    def test_a_toggl_save_drops_only_that_persons_cache(self):
        from server import toggl
        toggl._cache.clear()
        self.addCleanup(toggl._cache.clear)
        toggl._cache[("sam", "projects")] = (1e12, {"sams": 1})
        self.as_me()
        toggl._cache[("alex", "projects")] = (1e12, {"mine": 1})
        toggl.save({"token": "t"})
        self.assertEqual(list(toggl._cache), [("sam", "projects")])

    def chat_two_steps(self, during_first):
        """A two-step turn for whoever posts it; `during_first(env)` runs
        inside its first step. The env of every step run, in order."""
        seen = []

        def fake_stream(cmd, env, cwd, on_status, idle, cap, on_log=None):
            seen.append(dict(env))
            if len(seen) == 1:
                during_first(env)
            return "ok", "sid-" + env.get("FENCE_USER", "owner")
        self.addCleanup(setattr, chatmod, "run_stream", chatmod.run_stream)
        chatmod.run_stream = fake_stream
        app.Handler.chat._plan = lambda text: ["one", "two"]
        return seen

    def test_the_owners_new_conversation_does_not_stop_her_build(self):
        tok = self.her()
        accounts.set_build("sam", True)
        seen = self.chat_two_steps(lambda env: self.req("GET", "/chat/new", bearer=TOKEN))
        jid = self.req("POST", "/chat", body=b"make me a maze game", bearer=tok)[2].split()[1]
        body = self.wait(jid, tok)
        self.assertTrue(body.startswith("done"), body)
        self.assertEqual([e.get("FENCE_USER") for e in seen], ["sam", "sam"])
        self.assertIn("[x] 2. two", body)
        self.assertEqual(app.Handler.chat.sessions.get("sam"), "sid-sam")

    def test_her_new_conversation_does_not_stop_the_owners(self):
        tok = self.her()
        accounts.set_build("sam", True)
        seen = self.chat_two_steps(lambda env: self.req("GET", "/chat/new", bearer=tok))
        jid = self.req("POST", "/chat", body=b"fix the kernel", bearer=TOKEN)[2].split()[1]
        body = self.wait(jid, TOKEN)
        self.assertEqual(len(seen), 2, body)
        self.assertEqual(app.Handler.chat.session_id, "sid-owner")

    def test_her_own_new_conversation_stops_her_build(self):
        tok = self.her()
        accounts.set_build("sam", True)
        chat = app.Handler.chat
        seen = self.chat_two_steps(lambda env: self.req("GET", "/chat/new", bearer=tok))
        done, real = threading.Event(), chat._run
        chat._run = lambda *a: (real(*a), done.set())
        self.req("POST", "/chat", body=b"make me a maze game", bearer=tok)
        self.assertTrue(done.wait(10))
        self.assertEqual(len(seen), 1)                           # step two never ran
        self.assertIsNone(chat.sessions.get("sam"))              # nor came back


class Passwords(unittest.TestCase):
    def test_a_hash_checks_only_its_password(self):
        h = accounts.hash_password("secret")
        self.assertTrue(accounts.check_password(h, "secret"))
        self.assertFalse(accounts.check_password(h, "Secret"))
        self.assertFalse(accounts.check_password("garbage", "secret"))
        self.assertNotEqual(h, accounts.hash_password("secret"))         # salted


if __name__ == "__main__":
    unittest.main()
