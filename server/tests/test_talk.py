"""Talking about a document: the device's four calls, a revision taken out
of the answer and kept for /talk/doc, the conversation resumed, and a bad
song revision sent back once.

    python -m server.tests.test_talk
"""
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, msg, talk
from server import chat as chatmod

TOKEN = "tok"


class Split(unittest.TestCase):
    def test_the_document_comes_out_of_the_words(self):
        words, rev = talk.split("Tightened it.\n```doc\n# Hi\nthere\n```\n")
        self.assertEqual(words, "Tightened it.")
        self.assertEqual(rev, "# Hi\nthere\n")

    def test_words_alone_are_no_revision(self):
        self.assertEqual(talk.split("It reads well."), ("It reads well.", None))


class Talk(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.addCleanup(setattr, dash, "logged_in", dash.logged_in)
        dash.logged_in = lambda h: False
        self.calls = []
        real = talk._claude

        def fake(chat, prompt, sid):
            self.calls.append((prompt, sid))
            if "bad song" in prompt:
                return "Changed.\n```doc\ntempo 90\nn 0 H9 1\n```", "S1"
            if "mistake on line" in prompt:
                return "```doc\ntempo 90\nn 0 C4 1\n```", sid
            if "shorter" in prompt:
                return "Cut the middle.\n```doc\nshort\n```", sid or "S1"
            return "It is about cats.", sid or "S1"
        talk._claude = fake
        self.addCleanup(setattr, talk, "_claude", real)
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None):
        r = urllib.request.Request(self.base + path, method=method, data=body)
        r.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def say(self, sid, text):
        self.assertEqual(self.req("POST", "/talk/say?s=" + sid, text.encode()), (200, "ok\n"))
        for _ in range(100):
            s, body = self.req("GET", "/talk/poll?s=" + sid)
            if body != "pending\n":
                return body
            time.sleep(0.05)

    def test_a_question_then_a_change(self):
        s, sid = self.req("POST", "/talk/start?name=cats.md", b"Cats are good.\n")
        sid = sid.strip()
        self.assertEqual(self.say(sid, "what is this about?"), "reply\nIt is about cats.\n")
        self.assertIn("Cats are good.", self.calls[0][0])
        self.assertIsNone(self.calls[0][1])
        self.assertEqual(self.req("GET", "/talk/doc?s=" + sid)[0], 404)
        self.assertEqual(self.say(sid, "make it shorter"), "reply rev\nCut the middle.\n")
        self.assertEqual(self.calls[1], ("make it shorter", "S1"))      # resumed, document not resent
        self.assertEqual(self.req("GET", "/talk/doc?s=" + sid), (200, "short\n"))

    def test_a_song_revision_is_checked(self):
        s, sid = self.req("POST", "/talk/start?name=a.song&kind=song", b"tempo 90\nn 0 C4 1\n")
        self.assertEqual(self.say(sid.strip(), "a bad song please"), "reply rev\nChanged.\n")
        self.assertIn("line 2", self.calls[1][0])
        self.assertEqual(self.req("GET", "/talk/doc?s=" + sid.strip())[1], "tempo 90\nn 0 C4 1\n")

    def notes(self):
        return msg.notes_since(None, 0)[0]

    def test_a_reply_notifies_the_claude_app(self):
        before = len(self.notes())
        s, sid = self.req("POST", "/talk/start?name=cats.md", b"Cats.\n")
        self.say(sid.strip(), "what is this about?")
        n = self.notes()[before:]
        self.assertEqual([(x["app"], x["title"], x["text"]) for x in n],
                         [("Claude", "replied", "It is about cats.")])
        self.say(sid.strip(), "make it shorter")
        n = self.notes()[before + 1:]
        self.assertEqual([(x["app"], x["title"]) for x in n], [("Claude", "revised cats.md")])

    def test_an_error_notifies(self):
        def boom(chat, prompt, sid):
            raise RuntimeError("claude is away")
        talk._claude = boom
        before = len(self.notes())
        s, sid = self.req("POST", "/talk/start?name=a.md", b"x\n")
        self.say(sid.strip(), "hello")
        n = self.notes()[before:]
        self.assertEqual([(x["app"], x["title"], x["text"]) for x in n],
                         [("Claude", "error", "claude is away")])

    def test_it_goes_to_the_user_who_started_it(self):
        s = talk.Session("a.md", "text", "x", user="ann")
        s.answer = "Done."
        talk._notify(s, "reply")
        self.assertEqual([x["text"] for x in msg.notes_since("ann", 0)[0]], ["Done."])
        self.assertEqual(msg.notes_since("bob", 0)[0], [])

    def test_an_unknown_conversation_is_over(self):
        self.assertEqual(self.req("GET", "/talk/poll?s=nope")[0], 404)


if __name__ == "__main__":
    unittest.main()
