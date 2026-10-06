"""Songs from Claude for the MIDI app: the check catches what the device's
parser would refuse, a bad song gets one chance to be mended, and the
device's three calls answer.

    python -m server.tests.test_midi
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
from server import app, dash, midi
from server import chat as chatmod

TOKEN = "tok"
GOOD = "# Arp\ntempo 100\nn 0 C4 0.5\nn 0.5 E4,G4 1/3 80 ; ok\nloop\n"


class Check(unittest.TestCase):
    def test_a_good_song_passes(self):
        self.assertIsNone(midi.check(GOOD))

    def test_mistakes_name_their_line(self):
        self.assertEqual(midi.check("n 0 H4 1\n")[0], 1)
        self.assertEqual(midi.check("tempo 90\nn 0 C4\n"), (2, "n needs BEAT NOTE LEN"))
        self.assertEqual(midi.check("tempo 90\nrepeat 4\nn 0 C4 1\n")[0], 2)
        self.assertEqual(midi.check("tempo 90\n")[1], "no notes at all")
        self.assertEqual(midi.check("tempo 42.5\nn 0 C4 1\n")[0], 1)
        self.assertEqual(midi.check("ch 17\nn 0 C4 1\n")[0], 1)

    def test_the_song_comes_out_of_its_fence(self):
        self.assertEqual(midi.extract("Here:\n```song\nn 0 C4 1\n```\nenjoy"), "n 0 C4 1\n")


class Compose(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        dash.logged_in = lambda h: False
        self.asks = []
        real = midi._claude

        def fake(chat, prompt):
            self.asks.append(prompt)
            if "Fur Elise" in prompt and len(self.asks) == 1:
                return "```song\n# Fur Elise\nn 0 E5 0.25\nn 0.25 X9 0.25\n```"
            return "```song\n" + GOOD + "```"
        midi._claude = fake
        self.addCleanup(setattr, midi, "_claude", real)
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

    def wait(self, jid):
        for _ in range(100):
            s, body = self.req("GET", "/midi/compose?id=" + jid)
            if body != "pending\n":
                return s, body
            time.sleep(0.05)
        return None

    def test_a_mistake_goes_back_once_with_its_line(self):
        s, jid = self.req("POST", "/midi/compose", b"the first section of Fur Elise")
        self.assertEqual(s, 200)
        s, body = self.wait(jid.strip())
        self.assertEqual(body, "ok\n" + GOOD)
        self.assertEqual(len(self.asks), 2)
        self.assertIn("line 3", self.asks[1])

    def test_a_change_sends_the_song_and_the_ask(self):
        body = ("make it louder\n" + midi.SONG_MARK + "\n" + GOOD).encode()
        s, jid = self.req("POST", "/midi/compose", body)
        s, out = self.wait(jid.strip())
        self.assertEqual(out, "ok\n" + GOOD)
        self.assertIn("Change it: make it louder", self.asks[0])
        self.assertIn("n 0.5 E4,G4", self.asks[0])

    def test_nothing_asked_is_refused(self):
        self.assertEqual(self.req("POST", "/midi/compose", b"   ")[0], 400)
        self.assertEqual(self.req("GET", "/midi/compose?id=nope")[0], 404)


if __name__ == "__main__":
    unittest.main()
