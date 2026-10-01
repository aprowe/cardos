"""The Carlos prompt: what the translator is told it may answer with.

No model call and no whisper -- only the text the model is handed, which is
where "add milk to my todo list" becomes a `do` and "what's on my calendar
today" becomes an `ask`.

    python -m server.tests.test_voice
"""
import json, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import voice

CATALOG = {
    "todo": [
        {"id": "add", "about": "add a task to the current list", "net": False,
         "params": [{"name": "text", "type": "text", "about": "what the task says"}]},
        {"id": "sync", "about": "sync every list with Google", "net": True,
         "params": []},
    ],
    "calendar": [
        {"id": "today", "about": "today's events", "net": False, "params": []},
    ],
    "mines": [],
}


class Prompt(unittest.TestCase):

    def with_catalog(self, cat):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "commands.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(cat, f)
        return voice.system_prompt(p)

    def test_the_commands_are_listed_as_the_device_writes_them(self):
        s = self.with_catalog(CATALOG)
        self.assertIn("todo add text:text # add a task to the current list", s)
        self.assertIn("todo sync net # sync every list with Google", s)
        self.assertIn("calendar today # today's events", s)

    def test_do_and_ask_are_verbs(self):
        s = self.with_catalog(CATALOG)
        self.assertIn("do APP COMMAND ARGS", s)
        self.assertIn("ask QUESTION", s)

    def test_the_rule_separates_actions_from_questions(self):
        s = self.with_catalog(CATALOG)
        # Both of the user's own examples are in the prompt as examples.
        self.assertIn('"add milk to my todo list" -> do todo add milk', s)
        self.assertIn("\"what's on my calendar today\" -> ask", s)

    def test_no_catalog_still_makes_a_prompt(self):
        s = voice.system_prompt(os.path.join(tempfile.mkdtemp(), "missing.json"))
        self.assertIn("ask QUESTION", s)
        self.assertIn("open NAME", s)
        self.assertIn("(none on this server)", s)
        self.assertNotIn("todo add text:text", s)

    def test_open_names_come_from_the_catalog_too(self):
        s = self.with_catalog(CATALOG)
        # An app with no commands is still an app that can be opened.
        self.assertIn("mines", s.split("open NAME")[1].split("\n\n")[0])




class Route(unittest.TestCase):
    """/voice: the words, or a command, as the gesture asked."""

    def setUp(self):
        import threading
        from http.server import ThreadingHTTPServer
        from server import app
        from server import chat as chatmod

        class FakeVoice:
            heard = "open todo"

            def ready(self):
                return True

            def transcribe(self, wav):
                return self.heard, None

            def command(self, text, chat):
                return "open " + text.split()[-1]

        self.voice = FakeVoice()
        app.Handler.voice = self.voice
        app.Handler.chat = chatmod.ChatService(claude="stub")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def post(self, q=""):
        import urllib.request
        r = urllib.request.Request(self.base + "/voice" + q, data=b"RIFF" + b"\0" * 100,
                                   method="POST")
        return urllib.request.urlopen(r, timeout=10).read().decode()

    def test_text_mode_is_only_the_words(self):
        self.voice.heard = "Carlos open todo"
        self.assertEqual(self.post("?mode=text"), "text Carlos open todo\n")

    def test_cmd_mode_needs_no_name(self):
        self.voice.heard = "open todo"
        self.assertEqual(self.post("?mode=cmd"), "cmd open todo\n")
        self.voice.heard = "Carlos, open calendar"
        self.assertEqual(self.post("?mode=cmd"), "cmd open calendar\n")

    def test_no_mode_is_as_before(self):
        self.voice.heard = "buy milk"
        self.assertEqual(self.post(), "text buy milk\n")
        self.voice.heard = "Carlos open todo"
        self.assertEqual(self.post(), "cmd open todo\n")


if __name__ == "__main__":
    unittest.main(verbosity=1)
