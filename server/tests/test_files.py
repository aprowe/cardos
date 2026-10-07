"""Remote Files through the server, against a device that is a dict.

The fake device polls /files/poll over real HTTP, the way apps/rfiles.c does,
and answers jobs from an in-memory card. What matters: a file comes back
byte for byte however many chunks it takes, an upload lands under its name
only when the last chunk is in, the page is shut to anyone not signed in,
and a device that is not there is said plainly rather than waited on.

    python -m server.tests.test_files
"""
import base64
import json
import os
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, files
from server import chat as chatmod

TOKEN = "tok"


class FakeDevice:
    """A card: path -> bytes for files, a set of folders."""

    def __init__(self, base):
        self.base = base
        self.files = {}
        self.dirs = {"/"}
        self.run = True
        self.jobs = 0
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        jid, answer = 0, ""
        while self.run:
            r = urllib.request.Request("%s/files/poll?id=%d" % (self.base, jid),
                                       data=answer.encode(), method="POST",
                                       headers={"Authorization": "Bearer " + TOKEN})
            job = urllib.request.urlopen(r, timeout=10).read().decode()
            if job.startswith("idle"):
                jid, answer = 0, ""
                continue
            head, _, data = job.partition("\n")
            jid, op, path, arg = head.split("\t")
            jid = int(jid)
            self.jobs += 1
            try:
                answer = self.do(op, path, arg, data)
            except Exception as e:
                answer = "error %s" % e

    def do(self, op, path, arg, data):
        if op == "list":
            if path not in self.dirs:
                return "error no such folder"
            pre = path.rstrip("/") + "/"
            rows = ["d\t0\t%s" % d[len(pre):] for d in self.dirs
                    if d.startswith(pre) and "/" not in d[len(pre):] and d != path]
            rows += ["f\t%d\t%s" % (len(b), f[len(pre):]) for f, b in self.files.items()
                     if f.startswith(pre) and "/" not in f[len(pre):]]
            return "ok\n" + "\n".join(rows)
        if op == "stat":
            if path in self.dirs:
                return "ok d 0"
            if path in self.files:
                return "ok f %d" % len(self.files[path])
            return "error no such file"
        if op == "read":
            off, n = map(int, arg.split(","))
            return "ok\n" + base64.b64encode(self.files[path][off:off + n]).decode()
        if op == "write":
            part, off = path + ".part", int(arg)
            if off == 0:
                self.files[part] = b""
            elif len(self.files.get(part, b"")) != off:
                return "error out of order"
            self.files[part] += base64.b64decode(data)
            return "ok"
        if op == "commit":
            self.files[path] = self.files.pop(path + ".part")
            return "ok"
        if op == "mkdir":
            self.dirs.add(path)
            return "ok"
        if op == "rm":
            if self.files.pop(path, None) is None:
                return "error no such file"
            return "ok"
        if op == "sh":
            line = data.strip()
            if line.startswith("launch"):
                return "ok refused\nlaunch takes over the device's screen\n"
            return "ok\n$ ran %s\nheap free 120000\n" % line
        if op == "mv":
            self.files[arg] = self.files.pop(path)
            return "ok"
        return "error unknown job"


class FilesTest(unittest.TestCase):
    def setUp(self):
        files.broker = files.Broker()
        self.signed_in = True
        self.addCleanup(setattr, dash, "logged_in", dash.logged_in)
        dash.logged_in = lambda h: self.signed_in
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.dev = None

    def tearDown(self):
        if self.dev:
            self.dev.run = False
        self.srv.shutdown()
        self.srv.server_close()

    def device(self):
        self.dev = FakeDevice(self.base)
        for _ in range(50):
            if files.broker.connected():
                return self.dev
            time.sleep(0.05)
        self.fail("the device never polled")

    def req(self, method, path, body=None):
        r = urllib.request.Request(self.base + path, data=body, method=method)
        try:
            with urllib.request.urlopen(r, timeout=30) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def j(self, method, path, body=None):
        s, b = self.req(method, path, body)
        return s, json.loads(b)

    def test_a_folder_lists_folders_first(self):
        d = self.device()
        d.dirs.update({"/apps", "/config"})
        d.files.update({"/notes.txt": b"hi", "/apps/x.capp": b"zz"})
        s, j = self.j("GET", "/dash/files/ls?path=/")
        self.assertEqual(s, 200)
        self.assertEqual([e["name"] for e in j["entries"]], ["apps", "config", "notes.txt"])
        self.assertEqual(j["entries"][2], {"name": "notes.txt", "dir": False, "size": 2})

    def test_a_large_file_comes_back_whole(self):
        d = self.device()
        data = os.urandom(files.CHUNK * 3 + 17)
        d.files["/big.bin"] = data
        s, b = self.req("GET", "/dash/files/get?path=/big.bin")
        self.assertEqual((s, b), (200, data))
        self.assertGreaterEqual(d.jobs, 5)            # a stat and four reads

    def test_an_upload_lands_only_when_whole(self):
        d = self.device()
        data = os.urandom(files.CHUNK * 2 + 5)
        s, j = self.j("POST", "/dash/files/put?path=/up.bin", data)
        self.assertEqual((s, j["size"]), (200, len(data)))
        self.assertEqual(d.files["/up.bin"], data)
        self.assertNotIn("/up.bin.part", d.files)

    def test_an_empty_file_uploads(self):
        d = self.device()
        self.assertEqual(self.j("POST", "/dash/files/put?path=/empty", b"")[0], 200)
        self.assertEqual(d.files["/empty"], b"")

    def test_mkdir_rename_delete(self):
        d = self.device()
        d.files["/a.txt"] = b"a"
        self.assertEqual(self.j("POST", "/dash/files/mkdir?path=/new")[0], 200)
        self.assertIn("/new", d.dirs)
        self.assertEqual(self.j("POST", "/dash/files/mv?from=/a.txt&to=/new/b.txt")[0], 200)
        self.assertEqual(d.files["/new/b.txt"], b"a")
        self.assertEqual(self.j("POST", "/dash/files/rm?path=/new/b.txt")[0], 200)
        self.assertNotIn("/new/b.txt", d.files)

    def test_the_devices_error_is_the_pages_error(self):
        self.device()
        s, j = self.j("POST", "/dash/files/rm?path=/nothing")
        self.assertEqual((s, j["error"]), (400, "no such file"))

    def test_no_device_is_said_at_once(self):
        t = time.time()
        s, j = self.j("GET", "/dash/files/ls?path=/")
        self.assertEqual(s, 503)
        self.assertIn("Remote Files", j["error"])
        self.assertLess(time.time() - t, 2)
        self.assertEqual(self.j("GET", "/dash/files/status")[1],
                         {"connected": False, "last_seen": None})

    def test_dotdot_is_refused_before_the_device_hears_of_it(self):
        d = self.device()
        self.assertEqual(self.j("GET", "/dash/files/ls?path=/apps/../config")[0], 400)
        self.assertEqual(d.jobs, 0)

    def test_signed_out_is_shut(self):
        self.signed_in = False
        self.assertEqual(self.req("GET", "/dash/files/ls?path=/")[0], 403)
        self.assertEqual(self.req("POST", "/dash/files/rm?path=/x")[0], 403)
        s, body = self.req("GET", "/dash/files")      # sent to /dash to sign in
        self.assertNotIn(b"/dash/files/ls", body)

    def test_a_console_line_and_what_it_printed(self):
        self.device()
        s, j = self.j("POST", "/dash/term", json.dumps({"line": "  do  calendar sync "}).encode())
        self.assertEqual((s, j["refused"]), (200, False))
        self.assertEqual(j["output"], "$ ran do calendar sync\nheap free 120000\n")
        s, j = self.j("POST", "/dash/term", json.dumps({"line": "launch"}).encode())
        self.assertEqual((s, j["refused"]), (200, True))
        s, j = self.j("POST", "/dash/term", json.dumps({"line": "   "}).encode())
        self.assertEqual(s, 400)

    def test_the_poll_needs_the_token(self):
        r = urllib.request.Request(self.base + "/files/poll", data=b"", method="POST")
        with self.assertRaises(urllib.error.HTTPError) as e:
            urllib.request.urlopen(r, timeout=5)
        self.assertEqual(e.exception.code, 403)


if __name__ == "__main__":
    unittest.main()
