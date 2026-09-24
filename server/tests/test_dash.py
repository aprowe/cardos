"""The dashboard's two doors, and Google sign-in against a stubbed Google.

    python -m server.tests.test_dash
"""
import base64, json, os, shutil, sys, tempfile, threading, time, unittest
import urllib.error, urllib.parse, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import app, dash
from server import chat as chatmod
from http.server import ThreadingHTTPServer

TOKEN = "s3cret-token"
PASSWORD = "pw-for-tests"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def fake_id_token(email):
    body = base64.urlsafe_b64encode(json.dumps({"email": email}).encode()).decode().rstrip("=")
    return "h." + body + ".sig"


class Cookie(unittest.TestCase):

    def test_round_trip(self):
        self.assertTrue(dash.cookie_ok(TOKEN, dash.make_cookie(TOKEN)))

    def test_another_token_refuses_it(self):
        self.assertFalse(dash.cookie_ok("other", dash.make_cookie(TOKEN)))

    def test_expired(self):
        old = dash.make_cookie(TOKEN, now=time.time() - 40 * 86400)
        self.assertFalse(dash.cookie_ok(TOKEN, old))

    def test_forged_expiry(self):
        exp, sig = dash.make_cookie(TOKEN).split(".")
        self.assertFalse(dash.cookie_ok(TOKEN, str(int(exp) + 1) + "." + sig))

    def test_junk(self):
        for v in ("", "x", ".", "abc.def", None):
            self.assertFalse(dash.cookie_ok(TOKEN, v), v)

    def test_no_server_token(self):
        self.assertFalse(dash.cookie_ok(None, dash.make_cookie(TOKEN)))


class Server(unittest.TestCase):
    token = TOKEN

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ.update(CARDOS_STATE=self.dir, GOOGLE_CLIENT_ID="cid.apps",
                          GOOGLE_CLIENT_SECRET="csec", DASH_URL="https://dash.example",
                          DASH_PASSWORD=PASSWORD)
        self.exchanged, self.revoked = [], []

        def fake_exchange(code, cid, csec, redirect):
            self.exchanged.append((code, cid, csec, redirect))
            if code == "norefresh":
                return {"access_token": "a"}
            return {"access_token": "a", "refresh_token": "r-" + code,
                    "id_token": fake_id_token("me@example.com"), "scope": "x"}
        dash.exchange_code = fake_exchange
        dash.revoke = self.revoked.append
        app.Handler.chat = chatmod.ChatService(claude="stub", token=self.token)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.dir)

    def req(self, path, data=None, cookie=None, bearer=None):
        r = urllib.request.Request(self.base + path,
                                   data=urllib.parse.urlencode(data).encode()
                                   if data is not None else None)
        if cookie:
            r.add_header("Cookie", "%s=%s" % (dash.COOKIE, cookie))
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        try:
            resp = OPENER.open(r, timeout=10)
        except urllib.error.HTTPError as e:
            resp = e
        return resp.status if hasattr(resp, "status") else resp.code, \
            dict(resp.headers), resp.read().decode()

    def login(self):
        code, hdrs, _ = self.req("/dash/login", {"password": PASSWORD})
        self.assertEqual(code, 303)
        return hdrs["Set-Cookie"].split(";")[0].split("=", 1)[1]

    def signed_in(self, cookie, code="abc"):
        _, hdrs, _ = self.req("/dash/google/start", cookie=cookie)
        q = urllib.parse.parse_qs(urllib.parse.urlparse(hdrs["Location"]).query)
        return self.req("/dash/google/callback?" + urllib.parse.urlencode(
            {"state": q["state"][0], "code": code}), cookie=cookie)


class Doors(Server):

    def test_no_cookie_is_the_login_form(self):
        code, _, body = self.req("/dash")
        self.assertEqual(code, 200)
        self.assertIn("name=password", body)
        self.assertNotIn("Log out", body)

    def test_wrong_password(self):
        code, hdrs, _ = self.req("/dash/login", {"password": "nope"})
        self.assertEqual(code, 403)
        self.assertNotIn("Set-Cookie", hdrs)

    def test_right_password_sets_a_safe_cookie(self):
        _, hdrs, _ = self.req("/dash/login", {"password": PASSWORD})
        for flag in ("HttpOnly", "Secure", "SameSite=Lax", "Path=/dash"):
            self.assertIn(flag, hdrs["Set-Cookie"])
        code, _, body = self.req("/dash", cookie=self.login())
        self.assertIn("Log out", body)
        self.assertIn("Server", body)

    def test_the_token_is_not_the_password(self):
        code, _, _ = self.req("/dash/login", {"password": TOKEN})
        self.assertEqual(code, 403)

    def test_changing_the_password_ends_sessions(self):
        cookie = self.login()
        os.environ["DASH_PASSWORD"] = "new"
        _, _, body = self.req("/dash", cookie=cookie)
        self.assertIn("name=password", body)

    def test_bearer_does_not_open_the_dashboard(self):
        _, _, body = self.req("/dash", bearer=TOKEN)
        self.assertIn("name=password", body)

    def test_cookie_does_not_open_creds(self):
        code, _, _ = self.req("/google/creds", cookie=self.login())
        self.assertEqual(code, 403)

    def test_google_start_needs_login(self):
        code, hdrs, _ = self.req("/dash/google/start")
        self.assertEqual((code, hdrs["Location"]), (303, "/dash"))


class Google(Server):

    def test_start_goes_to_google_with_offline_consent(self):
        _, hdrs, _ = self.req("/dash/google/start", cookie=self.login())
        url = urllib.parse.urlparse(hdrs["Location"])
        q = urllib.parse.parse_qs(url.query)
        self.assertEqual(url.netloc, "accounts.google.com")
        self.assertEqual(q["redirect_uri"], ["https://dash.example/dash/google/callback"])
        self.assertEqual(q["access_type"], ["offline"])
        self.assertEqual(q["prompt"], ["consent"])
        self.assertIn("https://www.googleapis.com/auth/tasks", q["scope"][0])

    def test_sign_in_then_the_device_pulls(self):
        cookie = self.login()
        code, hdrs, _ = self.signed_in(cookie)
        self.assertEqual(code, 303)
        self.assertIn("Signed%20in", hdrs["Location"])
        self.assertEqual(self.exchanged,
                         [("abc", "cid.apps", "csec", "https://dash.example/dash/google/callback")])
        self.assertEqual(os.stat(dash.creds_path()).st_mode & 0o077 if os.name != "nt" else 0, 0)

        _, _, page = self.req("/dash", cookie=cookie)
        self.assertIn("me@example.com", page)

        code, _, body = self.req("/google/creds", bearer=TOKEN)
        self.assertEqual((code, body), (200, "cid.apps\ncsec\nr-abc\n"))
        self.assertIsNotNone(dash.load_creds()["pulled_at"])

    def test_creds_without_bearer(self):
        self.signed_in(self.login())
        code, _, _ = self.req("/google/creds")
        self.assertEqual(code, 403)

    def test_creds_before_sign_in(self):
        code, _, body = self.req("/google/creds", bearer=TOKEN)
        self.assertEqual(code, 404)
        self.assertTrue(body.startswith("error "))

    def test_bad_state_is_refused(self):
        code, hdrs, _ = self.req("/dash/google/callback?state=made-up&code=abc",
                                 cookie=self.login())
        self.assertIn("expired", urllib.parse.unquote(hdrs["Location"]))
        self.assertEqual(self.exchanged, [])

    def test_a_state_works_once(self):
        cookie = self.login()
        _, hdrs, _ = self.req("/dash/google/start", cookie=cookie)
        state = urllib.parse.parse_qs(urllib.parse.urlparse(hdrs["Location"]).query)["state"][0]
        self.req("/dash/google/callback?state=%s&code=a" % state, cookie=cookie)
        self.req("/dash/google/callback?state=%s&code=b" % state, cookie=cookie)
        self.assertEqual(len(self.exchanged), 1)

    def test_no_refresh_token_saves_nothing(self):
        _, hdrs, _ = self.signed_in(self.login(), code="norefresh")
        self.assertIn("no%20refresh%20token", hdrs["Location"])
        self.assertIsNone(dash.load_creds())

    def test_signing_in_again_revokes_the_old_token(self):
        cookie = self.login()
        self.signed_in(cookie, "one")
        self.signed_in(cookie, "two")
        self.assertEqual(self.revoked, ["r-one"])
        self.assertEqual(dash.load_creds()["refresh_token"], "r-two")

    def test_forget_revokes_and_deletes(self):
        cookie = self.login()
        self.signed_in(cookie)
        self.req("/dash/google/forget", {}, cookie=cookie)
        self.assertEqual(self.revoked, ["r-abc"])
        self.assertIsNone(dash.load_creds())


class NoPassword(Server):

    def setUp(self):
        super().setUp()
        os.environ["DASH_PASSWORD"] = ""

    def test_dashboard_refuses_to_run(self):
        self.assertEqual(self.req("/dash")[0], 503)
        self.assertEqual(self.req("/dash/login", {"password": ""})[0], 503)


class NoToken(Server):
    token = None

    def test_dashboard_refuses_to_run(self):
        code, _, _ = self.req("/dash")
        self.assertEqual(code, 503)

    def test_creds_are_never_open(self):
        dash.save_creds({"client_id": "a", "client_secret": "b", "refresh_token": "c"})
        code, _, body = self.req("/google/creds")
        self.assertEqual(code, 503)
        self.assertNotIn("c\n", body.split(":")[0])


if __name__ == "__main__":
    unittest.main(verbosity=1)
