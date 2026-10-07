"""Notes on the server: the device's bearer and the dashboard's cookie both
open them; the hash is the one the device computes; a memo becomes a note.

    python -m server.tests.test_notes
"""
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, notes
from server import chat as chatmod

TOKEN = "tok"


class FakeVoice:
    def ready(self):
        return True

    def transcribe(self, wav):
        return "Buy oat milk and call the dentist", None


class NotesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        self.cookie = False
        self.addCleanup(setattr, dash, "logged_in", dash.logged_in)
        dash.logged_in = lambda h: self.cookie
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.voice = FakeVoice()
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None, bearer=TOKEN):
        r = urllib.request.Request(self.base + path, method=method,
                                   data=body.encode() if isinstance(body, str) else body)
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def test_make_read_change_list_delete(self):
        s, line = self.req("POST", "/notes/note", "# Groceries\n- milk\n")
        self.assertEqual(s, 200)
        nid, h = line.strip().split("\t")
        self.assertEqual(h, notes.fnv("# Groceries\n- milk\n"))
        self.assertEqual(self.req("GET", "/notes/note?id=" + nid), (200, "# Groceries\n- milk\n"))
        s, line = self.req("POST", "/notes/note?id=" + nid, "# Groceries\n- milk\n- eggs\n")
        self.assertEqual(line.split("\t")[0], nid)
        s, text = self.req("GET", "/notes")
        f = text.strip().split("\t")
        self.assertEqual((f[0], f[1], f[3]), (nid, notes.fnv("# Groceries\n- milk\n- eggs\n"),
                                              "Groceries"))
        self.assertEqual(self.req("DELETE", "/notes/note?id=" + nid), (200, "ok\n"))
        self.assertEqual(self.req("GET", "/notes"), (200, ""))
        self.assertEqual(self.req("GET", "/notes/note?id=" + nid)[0], 404)

    def test_the_hash_is_the_devices(self):
        # FNV-1a 32 over the bytes, as apps/notes.c computes it.
        self.assertEqual(notes.fnv(""), "811c9dc5")
        self.assertEqual(notes.fnv("a"), "e40c292c")

    def test_a_memo_becomes_a_note(self):
        s, line = self.req("POST", "/notes/audio", b"RIFF" + b"\0" * 200)
        self.assertEqual(s, 200)
        nid, h, title = line.strip().split("\t")
        self.assertEqual(title, "Voice memo")
        self.assertEqual(notes.load(nid)["text"],
                         "# Voice memo\n\nBuy oat milk and call the dentist\n")
        s, line = self.req("POST", "/notes/audio?name=1001-164847.wav", b"RIFF" + b"\0" * 200)
        self.assertEqual(line.strip().split("\t")[2], "Voice memo, 1 Oct 16:48")

    def test_a_memo_title_from_its_file_name(self):
        self.assertEqual(notes.memo_title("/home/memos/1225-0700.wav"), "Voice memo, 25 Dec 07:00")
        self.assertEqual(notes.memo_title("memo3.wav"), "Voice memo, memo3")
        self.assertEqual(notes.memo_title(""), "Voice memo")

    def test_cookie_or_bearer_and_nothing_else(self):
        self.assertEqual(self.req("GET", "/notes", bearer=None)[0], 403)
        self.assertEqual(self.req("GET", "/notes", bearer="wrong")[0], 403)
        self.cookie = True
        self.assertEqual(self.req("GET", "/notes", bearer=None)[0], 200)
        s, body = self.req("GET", "/dash/notes", bearer=None)
        self.assertEqual((s, json.loads(body)), (200, []))

    def test_the_dashboard_reaches_a_note_under_dash(self):
        # nginx passes only /dash* on the public name.
        self.cookie = True
        s, line = self.req("POST", "/dash/notes/note", "# Plans\nsoon\n", bearer=None)
        self.assertEqual(s, 200)
        nid = line.split("\t")[0]
        self.assertEqual(self.req("GET", "/dash/notes/note?id=" + nid, bearer=None),
                         (200, "# Plans\nsoon\n"))
        self.assertEqual(self.req("DELETE", "/dash/notes/note?id=" + nid, bearer=None)[0], 200)

    def test_bad_ids_are_refused(self):
        self.assertEqual(self.req("GET", "/notes/note?id=../google")[0], 400)
        self.assertEqual(self.req("POST", "/notes/note?id=nope", "x")[0], 404)


if __name__ == "__main__":
    unittest.main()
