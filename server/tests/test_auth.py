"""Who gets a 403, route by route.

Three doors besides "open": the device's token ("token"), the dashboard's
cookie ("dash"), and either ("device_or_dash") -- notes, photos, music,
Chat, the MIDI and document helpers, which both the device and the page
use. This pins, for every route that is not open, which credentials it
takes: a route given the wrong kind lets the wrong people in, or shuts
out the device.

    python -m server.tests.test_auth
"""
import os
import shutil
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
from server import accounts, app, dash, files, kv
from server import chat as chatmod

TOKEN = "auth-test-token"
PASSWORD = "auth-test-password"

DEVICE_OR_DASH = [
    ("GET", "/notes"), ("GET", "/notes/note"), ("POST", "/notes/note"),
    ("DELETE", "/notes/note"), ("POST", "/notes/audio"),
    ("GET", "/dash/notes/note"), ("POST", "/dash/notes/note"), ("DELETE", "/dash/notes/note"),
    ("GET", "/photos"), ("GET", "/photos/img"), ("GET", "/photos/print"),
    ("DELETE", "/photos/photo"), ("GET", "/dash/photos/thumb"), ("DELETE", "/dash/photos/photo"),
    ("GET", "/music"), ("GET", "/music/track"), ("DELETE", "/music/track"),
    ("POST", "/music/upload"), ("GET", "/dash/music/track"), ("DELETE", "/dash/music/track"),
    ("POST", "/midi/compose"), ("GET", "/midi/compose"),
    ("POST", "/talk/start"), ("POST", "/talk/say"), ("GET", "/talk/poll"), ("GET", "/talk/doc"),
    ("GET", "/notify/poll"), ("GET", "/msg"), ("POST", "/msg"),
    ("GET", "/kv/get"), ("POST", "/kv/put"), ("POST", "/kv/del"), ("GET", "/kv/list"),
    ("POST", "/kv/incr"), ("POST", "/q/push?q=t"), ("GET", "/q/peek?q=t"), ("GET", "/q/len?q=t"),
    ("POST", "/q/ack?q=t"), ("GET", "/q/allow?q=t"), ("POST", "/q/allow?q=t"),
    ("GET", "/people"), ("POST", "/people/name"), ("POST", "/ask"), ("GET", "/ask"),
]

DASH = [
    ("GET", "/dash/api/me"), ("POST", "/dash/api/device"), ("POST", "/dash/api/device/remove"),
    ("POST", "/dash/api/device/token"), ("POST", "/dash/api/user"),
    ("POST", "/dash/api/user/remove"), ("GET", "/dash/api/claude"),
    ("POST", "/dash/api/claude/code"), ("POST", "/dash/api/claude/token"),
    ("POST", "/dash/api/claude/forget"), ("GET", "/dash/api/state"),
    ("POST", "/dash/api/google/forget"), ("POST", "/dash/api/toggl/token"),
    ("POST", "/dash/api/toggl/forget"), ("GET", "/dash/api/toggl/targets"),
    ("POST", "/dash/api/toggl/target"),
    ("GET", "/dash/notes"), ("GET", "/dash/photos"), ("POST", "/dash/photos/upload"),
    ("GET", "/dash/music"), ("POST", "/dash/music/upload"),
    ("GET", "/dash/files/status"), ("GET", "/dash/files/ls"), ("GET", "/dash/files/get"),
    ("POST", "/dash/files/put"), ("POST", "/dash/files/mkdir"), ("POST", "/dash/files/rm"),
    ("POST", "/dash/files/mv"), ("POST", "/dash/term"),
]

TOKEN_ONLY = [
    ("GET", "/daily?kind=nonsense"), ("GET", "/toggl/status"), ("GET", "/todo/lists"),
    ("POST", "/files/poll"), ("GET", "/m5hub/info"), ("GET", "/update"),
]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


class Doors(unittest.TestCase):
    token = TOKEN
    with_accounts = False

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        os.environ["DASH_PASSWORD"] = PASSWORD
        self.addCleanup(os.environ.pop, "DASH_PASSWORD", None)
        if self.with_accounts:
            accounts.migrate("alex", PASSWORD, TOKEN)
        app.Handler.chat = chatmod.ChatService(claude="stub", token=self.token)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.cookie = None
        if self.token:
            form = {"name": "alex", "password": PASSWORD} if self.with_accounts else {"password": PASSWORD}
            s, hdrs = self.req("POST", "/dash/login", body=urllib.parse.urlencode(form).encode())
            self.assertEqual(s, 303)
            self.cookie = hdrs["Set-Cookie"].split(";")[0].split("=", 1)[1]

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        kv.close_all()
        shutil.rmtree(self.dir, ignore_errors=True)
        files.broker = files.Broker()          # /files/poll said a device was there
        files._brokers.clear()

    def req(self, method, path, body=None, headers=None):
        if body is None and method != "GET":
            body = b""
        r = urllib.request.Request(self.base + path, method=method, data=body,
                                   headers=headers or {})
        try:
            resp = OPENER.open(r, timeout=20)
        except urllib.error.HTTPError as e:
            resp = e
        code = resp.status if hasattr(resp, "status") else resp.code
        hdrs = dict(resp.headers)
        resp.read()
        return code, hdrs

    def status(self, method, path, cred):
        headers = {"none": {}, "wrong": {"Authorization": "Bearer nope"},
                   "bearer": {"Authorization": "Bearer " + TOKEN},
                   "xtoken": {"X-Token": TOKEN},
                   "cookie": {"Cookie": "%s=%s" % (dash.COOKIE, self.cookie)}}[cred]
        return self.req(method, path, headers=headers)[0]

    def check(self, routes, cred, refused):
        for method, path in routes:
            s = self.status(method, path, cred)
            if refused:
                self.assertEqual(s, 403, "%s %s with %s: %d" % (method, path, cred, s))
            else:
                self.assertNotEqual(s, 403, "%s %s with %s" % (method, path, cred))

    def test_device_or_dash(self):
        self.check(DEVICE_OR_DASH, "none", True)
        self.check(DEVICE_OR_DASH, "wrong", True)
        self.check(DEVICE_OR_DASH, "bearer", False)
        self.check(DEVICE_OR_DASH, "cookie", False)
        # X-Token is the bearer's other spelling on token routes; these took
        # only the bearer unless accounts are on. Kept as it was.
        self.check(DEVICE_OR_DASH, "xtoken", not self.with_accounts)

    def test_dash(self):
        for cred in ("none", "wrong", "bearer", "xtoken"):
            self.check(DASH, cred, True)
        self.check(DASH, "cookie", False)

    def test_token(self):
        for cred in ("none", "wrong", "cookie"):
            self.check(TOKEN_ONLY, cred, True)
        self.check(TOKEN_ONLY, "bearer", False)
        self.check(TOKEN_ONLY, "xtoken", False)

    def test_a_refusal_is_json_where_the_page_reads_it(self):
        s, hdrs = self.req("GET", "/dash/notes")
        self.assertEqual((s, hdrs["Content-Type"]), (403, "application/json"))
        s, hdrs = self.req("GET", "/notes")
        self.assertEqual(s, 403)
        self.assertTrue(hdrs["Content-Type"].startswith("text/plain"))


class DoorsWithAccounts(Doors):
    with_accounts = True

    def test_a_cookie_without_dash_password(self):
        # A cookie made by name needs no DASH_PASSWORD. The page's API still
        # says the dashboard is not set up; the lists the page reads do not.
        os.environ.pop("DASH_PASSWORD")
        cookie = {"Cookie": "%s=%s" % (dash.COOKIE, self.cookie)}
        self.assertEqual(self.req("GET", "/dash/api/me", headers=cookie)[0], 503)
        self.assertEqual(self.req("GET", "/dash/notes", headers=cookie)[0], 200)
        self.assertEqual(self.req("GET", "/dash/api/me")[0], 403)

    def test_the_owners_routes(self):
        accounts.add_user("sam", "sams-password")
        _, tok = accounts.add_device("sam", "hers")
        s, hdrs = self.req("POST", "/dash/login", body=b"name=sam&password=sams-password")
        hers = hdrs["Set-Cookie"].split(";")[0].split("=", 1)[1]
        for method, path in (("GET", "/dash/api/claude"), ("POST", "/dash/api/user"),
                             ("POST", "/dash/api/user/remove")):
            s, _ = self.req(method, path, headers={"Cookie": "%s=%s" % (dash.COOKIE, hers)})
            self.assertEqual(s, 403, path)
        self.assertEqual(self.req("GET", "/chat/new",
                                  headers={"Authorization": "Bearer " + tok})[0], 403)
        # her device opens her notes
        self.assertEqual(self.req("GET", "/notes",
                                  headers={"Authorization": "Bearer " + tok})[0], 200)


class NoToken(unittest.TestCase):
    """No --token: the device's doors are open, the dashboard's are shut."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        app.Handler.chat = chatmod.ChatService(claude="stub", token=None)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    tearDown = Doors.tearDown

    def test_open_and_shut(self):
        for method, path in DEVICE_OR_DASH + TOKEN_ONLY:
            self.assertNotEqual(Doors.req(self, method, path)[0], 403, path)
        for method, path in DASH:
            s = Doors.req(self, method, path)[0]
            self.assertIn(s, (403, 503), path)


if __name__ == "__main__":
    unittest.main()
