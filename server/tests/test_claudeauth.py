"""Claude's login from the dashboard: the link and the token are read out of
what `claude setup-token` draws, a token is kept and used, and the routes
answer. On Linux the whole sign-in runs against a stand-in for the CLI.

    python -m server.tests.test_claudeauth
"""
import json
import os
import stat
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, claudeauth, dash
from server import chat as chatmod

TOKEN = "tok"
GOOD = "sk-ant-oat01-" + "Ab3_-" * 20

# Captured from `claude setup-token` on the droplet (v2.1.277), shortened:
# the link as a terminal hyperlink, then drawn again, wrapped.
RAW = (b"\x1b[2K\x1b[1G Browser didn't open? Use the url below to sign in (c to copy)\r\n"
       b"\x1b]8;id=14dx15o;https://claude.com/cai/oauth/authorize?code=true&client_id=9d1c"
       b"&response_type=code&state=_LCGI\x07https://claude.com/cai/oauth/authori\r\n"
       b"ze?code=true&client_id=9d1c\x1b]8;;\x07\r\n\x1b[36m Paste code here if prompted >\x1b[39m ")


class Parsing(unittest.TestCase):
    def test_the_link_comes_whole_from_the_hyperlink(self):
        self.assertEqual(claudeauth.find_url(RAW),
                         "https://claude.com/cai/oauth/authorize?code=true&client_id=9d1c"
                         "&response_type=code&state=_LCGI")
        self.assertIsNone(claudeauth.find_url(b"Welcome to Claude Code"))

    def test_the_token_without_the_words_around_it(self):
        out = b"\x1b[32m\xe2\x9c\x93 Long-lived token created\x1b[39m\r\n" + GOOD.encode() + \
              b"\r\nStore this token securely."
        self.assertEqual(claudeauth.find_token(out), GOOD)
        self.assertIsNone(claudeauth.find_token(b"sk-ant-oat01-short"))


class Routes(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.addCleanup(setattr, dash, "logged_in", dash.logged_in)
        dash.logged_in = lambda h: True
        self.addCleanup(setattr, dash, "password", dash.password)
        self.addCleanup(setattr, dash, "_server_token", dash._server_token)
        dash.password = lambda: "pw"
        dash._server_token = lambda h: TOKEN
        self.checks = []

        def fake_status(chat, fresh=False):
            self.checks.append(fresh)
            src = "saved" if claudeauth.saved_token() else "none"
            return {"source": src, "ok": src == "saved", "why": "", "since": None, "checked": 1}
        real = claudeauth.status
        claudeauth.status = fake_status
        self.addCleanup(setattr, claudeauth, "status", real)
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None):
        r = urllib.request.Request(self.base + path, method=method,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_a_pasted_token_is_kept_used_and_forgotten(self):
        self.assertEqual(self.req("GET", "/dash/api/claude")[1]["source"], "none")
        s, j = self.req("POST", "/dash/api/claude/token", {"token": "not a token"})
        self.assertEqual(s, 400)
        s, j = self.req("POST", "/dash/api/claude/token", {"token": GOOD})
        self.assertEqual((s, j["status"]["ok"]), (200, True))
        env = app.Handler.chat._child_env()
        self.assertEqual(env["CLAUDE_CODE_OAUTH_TOKEN"], GOOD)
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(os.stat(claudeauth.path()).st_mode), 0o600)
        self.req("POST", "/dash/api/claude/forget", {})
        self.assertEqual(claudeauth.saved_token(), "")

    def test_a_code_with_no_sign_in_waiting_is_refused(self):
        s, j = self.req("POST", "/dash/api/claude/code", {"code": "abc"})
        self.assertEqual(s, 400)


# A stand-in for `claude setup-token`: draws the link, reads a code, prints a
# token made from it -- or refuses a wrong one.
STUB = r'''#!/usr/bin/env python3
import sys
sys.stdout.write("Welcome\r\n\x1b]8;id=1;https://claude.com/cai/oauth/authorize?code=true&state=s1\x07link\x1b]8;;\x07\r\n")
sys.stdout.write("Paste code here if prompted > ")
sys.stdout.flush()
code = sys.stdin.readline().strip()
if code != "good-code":
    print("\r\nOAuth error: invalid code")
else:
    print("\r\n\x1b[32mLong-lived token created\x1b[39m\r\n%s\r\nStore this token securely." % ("sk-ant-oat01-" + "Zz9_-" * 20))
sys.stdout.flush()
import time; time.sleep(5)
'''


@unittest.skipUnless(os.name == "posix", "a pseudo-terminal needs Linux")
class SignInOnLinux(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.cli = os.path.join(os.environ["CARDOS_STATE"], "claude")
        with open(self.cli, "w") as f:
            f.write(STUB)
        os.chmod(self.cli, 0o755)

    def test_link_then_code_then_a_saved_token(self):
        s = claudeauth.SignIn.begin(self.cli, dict(os.environ))
        self.assertEqual(s.url, "https://claude.com/cai/oauth/authorize?code=true&state=s1")
        token = s.finish("good-code", wait=10)
        self.assertEqual(token, "sk-ant-oat01-" + "Zz9_-" * 20)
        self.assertEqual(claudeauth.saved_token(), token)
        self.assertIsNone(claudeauth.SignIn.current())

    def test_a_wrong_code_says_what_the_cli_said(self):
        s = claudeauth.SignIn.begin(self.cli, dict(os.environ))
        with self.assertRaises(RuntimeError) as e:
            s.finish("bad-code", wait=3)
        self.assertIn("invalid code", str(e.exception))
        self.assertEqual(claudeauth.saved_token(), "")
        s.close()


if __name__ == "__main__":
    unittest.main()
