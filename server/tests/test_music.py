"""Music: a 16-bit PCM WAV is kept and listed; anything else is refused with
a reason; the device lists, downloads and deletes.

    python -m server.tests.test_music
"""
import json
import os
import struct
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, dash, music
from server import chat as chatmod

TOKEN = "tok"


def wav(seconds=1.0, rate=22050, ch=1, bits=16, tag=1):
    n = int(rate * seconds) * ch * (bits // 8)
    fmt = struct.pack("<HHIIHH", tag, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", n) + b"\0" * n
    return b"RIFF" + struct.pack("<I", len(body)) + body


class Info(unittest.TestCase):
    def test_a_device_wav_is_measured(self):
        self.assertEqual(music.wav_info(wav(2.0)), (22050, 1, 2000))
        self.assertEqual(music.wav_info(wav(1.0, 44100, 2)), (44100, 2, 1000))

    def test_what_the_device_cannot_play_says_why(self):
        for bad in (b"ID3\x03 an mp3", wav(bits=8), wav(tag=3, bits=32), wav(rate=96000)):
            with self.assertRaises(ValueError):
                music.wav_info(bad)

    def test_a_title_is_the_file_name_cleaned(self):
        self.assertEqual(music.clean_title("C:/x/01 - Song\t.mp3"), "01 - Song")
        self.assertEqual(music.clean_title(""), "track")


class Routes(unittest.TestCase):
    def setUp(self):
        os.environ["CARDOS_STATE"] = tempfile.mkdtemp()
        self.cookie = False
        self.addCleanup(setattr, dash, "logged_in", dash.logged_in)
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
            with urllib.request.urlopen(r, timeout=20) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def test_upload_list_download_delete(self):
        self.cookie = True
        s, body = self.req("POST", "/dash/music/upload?title=Night%20Drive.mp3", wav(3.0), bearer=None)
        self.assertEqual(s, 200)
        meta = json.loads(body)
        self.assertEqual((meta["title"], meta["ms"]), ("Night Drive", 3000))
        self.cookie = False
        s, body = self.req("GET", "/music")
        tid, title, ms, size = body.decode().strip().split("\t")
        self.assertEqual((tid, title, ms), (meta["id"], "Night Drive", "3000"))
        s, data = self.req("GET", "/music/track?id=" + tid)
        self.assertEqual((s, len(data), data[:4]), (200, int(size), b"RIFF"))
        self.assertEqual(self.req("DELETE", "/music/track?id=" + tid), (200, b"ok\n"))
        self.assertEqual(self.req("GET", "/music"), (200, b""))

    def test_refusals(self):
        self.cookie = True
        s, body = self.req("POST", "/dash/music/upload?title=x", b"ID3 not a wav", bearer=None)
        self.assertEqual(s, 400)
        self.assertRegex(json.loads(body)["error"], "WAV|audio")
        self.cookie = False
        self.assertEqual(self.req("POST", "/dash/music/upload?title=x", wav(), bearer=None)[0], 403)
        self.assertEqual(self.req("GET", "/music", bearer=None)[0], 403)
        self.assertEqual(self.req("GET", "/music/track?id=../x")[0], 400)


@unittest.skipUnless(music.ffmpeg(), "no ffmpeg here")
class Convert(unittest.TestCase):
    """What the device cannot play is made into what it can."""

    def test_an_8_bit_wav_comes_out_22050_mono_16_bit(self):
        out = music.playable(wav(1.0, 44100, 2, bits=8))
        rate, ch, ms = music.wav_info(out)
        self.assertEqual((rate, ch), (22050, 1))
        self.assertAlmostEqual(ms, 1000, delta=30)

    def test_an_mp3(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            mp3 = os.path.join(d, "t.mp3")
            subprocess.run([music.ffmpeg(), "-v", "error", "-f", "lavfi", "-i", "sine=440:duration=2",
                            "-c:a", "libmp3lame", mp3], check=True)
            with open(mp3, "rb") as f:
                out = music.playable(f.read())
        rate, ch, ms = music.wav_info(out)
        self.assertEqual((rate, ch), (22050, 1))
        self.assertAlmostEqual(ms, 2000, delta=100)

    def test_a_playable_wav_is_kept_as_it_came(self):
        w = wav(1.0)
        self.assertIs(music.playable(w), w)


if __name__ == "__main__":
    unittest.main()
