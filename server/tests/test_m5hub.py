"""LauncherHub for the device: server/m5hub.py, against a fake catalog."""
import io
import json
import unittest
import urllib.parse

from server import m5hub


class Resp(io.BytesIO):
    def __init__(self, data, status=200):
        super().__init__(data)
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


ITEMS = [{"fid": "%032x" % i, "name": "\U0001f680 Game\t%d" % i, "author": "me", "star": i % 2}
         for i in range(130)]


def catalog(req, timeout):
    q = urllib.parse.parse_qs(urllib.parse.urlparse(req.full_url).query)
    if "fid" in q:
        return Resp(json.dumps({"name": "Game", "versions": [
            {"version": "1.0", "published_at": "2026-01-01", "file": "old.bin", "ao": 65536, "as": 1000, "s": 0},
            {"version": "2.0", "published_at": "2026-05-01", "file": "new.bin", "ao": 65536, "as": 2000, "s": 1},
            {"version": "bad", "file": "../etc/passwd", "ao": 0, "as": 5}]}).encode())
    page = int((q.get("page") or ["1"])[0])
    return Resp(json.dumps({"page": page, "page_size": 100, "total": len(ITEMS),
                            "items": ITEMS[(page - 1) * 100:page * 100]}).encode())


class Hub(unittest.TestCase):
    def setUp(self):
        m5hub._cache.clear()

    def test_pages_of_forty_across_the_catalogs_hundreds(self):
        lines = m5hub.list_lines(1, opener=catalog).splitlines()
        self.assertEqual(lines[0], "ok 1 4 130")
        self.assertEqual(len(lines), 41)
        self.assertEqual(lines[1], "%032x\tGame 0\tme\t0" % 0)      # ASCII, one line
        lines = m5hub.list_lines(3, opener=catalog).splitlines()    # 80..119: two pages upstream
        self.assertEqual(lines[1].split("\t")[0], "%032x" % 80)
        self.assertEqual(lines[-1].split("\t")[0], "%032x" % 119)
        self.assertEqual(len(m5hub.list_lines(4, opener=catalog).splitlines()), 11)

    def test_versions_newest_first_and_only_bootable_ones(self):
        lines = m5hub.info_lines("%032x" % 1, opener=catalog).splitlines()
        self.assertEqual(lines[0], "ok Game")
        self.assertEqual(lines[1], "2.0\t2026-05-01\tnew.bin\t65536\t2000\t1")
        self.assertEqual(lines[2], "1.0\t2026-01-01\told.bin\t65536\t1000\t0")
        self.assertEqual(len(lines), 3)                          # the bad file name is gone

    def test_the_app_is_cut_out_of_the_image(self):
        image = b"\x00" * 100 + b"\xe9" + b"A" * 49 + b"tail"

        def ranged(req, timeout):
            lo, hi = req.headers["Range"][6:].split("-")
            return Resp(image[int(lo):int(hi) + 1], 206)

        def whole(req, timeout):                                 # a CDN that ignores Range
            return Resp(image, 200)
        self.assertEqual(m5hub.fetch_app("x.bin", 100, 50, opener=ranged), b"\xe9" + b"A" * 49)
        self.assertEqual(m5hub.fetch_app("x.bin", 100, 50, opener=whole), b"\xe9" + b"A" * 49)
        with self.assertRaises(ValueError):
            m5hub.fetch_app("x.bin", 0, 50, opener=ranged)        # not an app image there


if __name__ == "__main__":
    unittest.main()
