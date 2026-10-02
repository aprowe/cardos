"""Photos: an upload is kept with its screen and print forms; the device lists,
downloads and deletes; the print form decodes back to the picture.

    python -m server.tests.test_photos
"""
import io
import json
import os
import re
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from PIL import Image
from server import app, dash, photos
from server import chat as chatmod

TOKEN = "tok"
B64 = photos.B64


def png(im):
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def decode(doc):
    """printdoc.c's reading of `%%` lines, for checking: rows of 384 bits."""
    rows = []
    for line in doc.splitlines():
        m = re.fullmatch(r"%%(?:(\d+)\*)?([\d,]*)(?:=([A-Za-z0-9+/]*))?", line)
        if not m:
            continue
        rep = int(m.group(1) or 1)
        bits, x, black = [False] * 384, 0, False
        for run in filter(None, (m.group(2) or "").split(",")):
            n = int(run)
            if black:
                for i in range(x, min(384, x + n)):
                    bits[i] = True
            x += n
            black = not black
        for ch in m.group(3) or "":
            v = B64.index(ch)
            for b in range(5, -1, -1):
                if x < 384:
                    bits[x] = bool(v >> b & 1)
                x += 1
        rows += [bits] * rep
    return rows


class PrintForm(unittest.TestCase):
    def test_half_black_comes_back_half_black_at_the_papers_width(self):
        im = Image.new("RGB", (200, 100), (255, 255, 255))
        im.paste((0, 0, 0), (0, 0, 100, 100))
        doc = photos.print_doc(im, "halves")
        rows = decode(doc)
        self.assertEqual(len(rows), 192)                 # 384 wide keeps 2:1
        self.assertTrue(all(r[:180].count(True) > 170 for r in rows))
        self.assertTrue(all(r[204:].count(True) < 10 for r in rows))
        self.assertTrue(doc.rstrip().endswith("halves"))
        self.assertLess(len(doc), 2000)                  # flat rows merge

    def test_a_busy_picture_uses_raw_rows_and_stays_printable(self):
        im = Image.effect_noise((384, 288), 80).convert("RGB")
        doc = photos.print_doc(im, "noise")
        self.assertEqual(len(decode(doc)), 288)
        self.assertLess(len(doc), 48 * 1024)             # what `print FILE` reads

    def test_a_tall_picture_is_shrunk_not_run_off_the_roll(self):
        rows = decode(photos.print_doc(Image.new("RGB", (100, 1000), (0, 0, 0)), "tall"))
        self.assertEqual(len(rows), photos.PRINT_H_MAX)


class Routes(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.cookie = False
        dash.logged_in = lambda h: self.cookie
        app.Handler.chat = chatmod.ChatService(claude="stub", token=TOKEN)
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def req(self, method, path, body=None, bearer=TOKEN):
        r = urllib.request.Request(self.base + path, method=method, data=body)
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        try:
            with urllib.request.urlopen(r, timeout=30) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def test_upload_list_fetch_delete(self):
        self.cookie = True
        s, body = self.req("POST", "/dash/photos/upload?name=Beach%20day.jpg",
                           png(Image.new("RGB", (480, 270), (255, 0, 0))), bearer=None)
        self.assertEqual(s, 200)
        meta = json.loads(body)
        self.assertEqual(meta["name"], "Beach day")
        self.cookie = False
        s, body = self.req("GET", "/photos")
        pid, name, created = body.decode().strip().split("\t")
        self.assertEqual((pid, name), (meta["id"], "Beach day"))
        s, img = self.req("GET", "/photos/img?id=" + pid)
        self.assertEqual((img[:4], img[8:10]), (b"CIMG", b"\xf8\x00"))
        s, doc = self.req("GET", "/photos/print?id=" + pid)
        self.assertEqual(s, 200)
        self.assertEqual(len(decode(doc.decode())), 216)
        self.assertEqual(self.req("DELETE", "/photos/photo?id=" + pid), (200, b"ok\n"))
        self.assertEqual(self.req("GET", "/photos"), (200, b""))

    def test_not_a_picture_and_not_signed_in(self):
        self.cookie = True
        self.assertEqual(self.req("POST", "/dash/photos/upload?name=x", b"junk", bearer=None)[0], 400)
        self.cookie = False
        self.assertEqual(self.req("POST", "/dash/photos/upload?name=x", b"junk", bearer=None)[0], 403)
        self.assertEqual(self.req("GET", "/photos", bearer=None)[0], 403)
        self.assertEqual(self.req("GET", "/photos/img?id=../x")[0], 400)


if __name__ == "__main__":
    unittest.main()
