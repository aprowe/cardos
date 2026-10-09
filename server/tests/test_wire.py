"""Bytes and lines the device reads, pinned.

The RLE rows of /render and /screen, the RGB565 of a converted picture,
and the one-line text the device's apps parse (Chat, Hub, Toggl, Google).
The device decodes these with no slack: a byte out of place is a garbled
row or a field in the wrong column. The expected values were taken from
the code as it was before server/pixels.py and server/wire.py replaced
the copies in each module, so any change shows here first.

    python -m server.tests.test_wire
"""
import hashlib
import os
import random
import sys
import unittest
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from server import google, images, m5hub, msg, toggl


def rows():
    """Rows to encode: flat, noisy, runs of every length either side of the
    128 limits, pairs inside literals, and a desktop-ish mix."""
    rnd = random.Random(565)
    out = [[], [7], [7, 7], [1, 2], [5] * 240, [5] * 128, [5] * 129, [5] * 257,
           list(range(240)), list(range(128)), list(range(129)), list(range(300)),
           [1, 2, 2, 3, 3, 3, 4, 4, 4, 4, 5], [0xFFFF, 0, 0xFFFF, 0, 0, 0xF800]]
    for _ in range(60):
        r = []
        while len(r) < 240:
            if rnd.random() < 0.4:
                r += [rnd.randrange(65536)] * rnd.choice((1, 2, 3, 4, 50, 127, 128, 129, 200))
            else:
                r += [rnd.randrange(65536) for _ in range(rnd.randrange(1, 140))]
        out.append(r[:240])
    return out


def digest(chunks):
    h = hashlib.sha256()
    for c in chunks:
        h.update(len(c).to_bytes(4, "little"))
        h.update(c)
    return h.hexdigest()


ROWS_SHA = "f59c79e8aa0a6caba03a2a7cc5a31441846275b202ec3845f0dfc33c77329306"
RGB_SHA = "4b74576b730f4a816e4c1b4d69b6491072b64b39dff5d8cb0f60669f4607cd89"
CIMG_SHA = "338f5ff5d83e8ec82f53566b09245edbe3911be58be407183f405c6cbdc9e184"


class Pixels(unittest.TestCase):

    def test_render_rows(self):
        from server.render import render
        self.assertEqual(digest(render.encode_row(r) for r in rows()), ROWS_SHA)

    def test_screen_rows_are_the_same_bytes(self):
        try:
            import numpy as np
            from server import screen
        except ImportError:
            self.skipTest("numpy or mss missing")
        self.assertEqual(digest(screen.encode_row(np.array(r, dtype=np.uint16)) for r in rows()),
                         ROWS_SHA)

    def colours(self):
        rnd = random.Random(16)
        return [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255),
                (7, 3, 7), (8, 4, 8)] + [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
                                         for _ in range(500)]

    def test_rgb565(self):
        from server.render import render
        got = b"".join(render.rgb565_swapped(*c).to_bytes(2, "little") for c in self.colours())
        self.assertEqual(hashlib.sha256(got).hexdigest(), RGB_SHA)

    def test_screen_rgb565_is_the_same(self):
        try:
            import numpy as np
            from server import screen
        except ImportError:
            self.skipTest("numpy or mss missing")
        c = self.colours()
        bgra = np.array([[(b, g, r, 255) for r, g, b in c]], dtype=np.uint8)
        got = screen.rgb565_rows(bgra)[0].astype("<u2").tobytes()
        self.assertEqual(hashlib.sha256(got).hexdigest(), RGB_SHA)

    def test_rgb565_bytes_with_and_without_numpy(self):
        from server import pixels
        rgb = bytes(v for c in self.colours() for v in c)
        want = b"".join(pixels.rgb565_swapped(*c).to_bytes(2, "little") for c in self.colours())
        self.assertEqual(pixels._rgb565_bytes_py(rgb), want)
        try:
            import numpy  # noqa: F401
        except ImportError:
            return
        self.assertEqual(pixels.rgb565_bytes(rgb), want)

    def test_a_converted_picture(self):
        from PIL import Image
        import io
        im = Image.new("RGB", (64, 40))
        im.putdata([((x * 4) % 256, (y * 6) % 256, (x * y) % 256) for y in range(40) for x in range(64)])
        buf = io.BytesIO()
        im.save(buf, "PNG")
        outs = [images.convert(buf.getvalue(), 32, 20, fit, fmt)[0]
                for fit, fmt in (("contain", "img"), ("cover", "img"))]
        outs.append(images.convert(buf.getvalue(), 240, 135, "contain", "565")[0])
        self.assertEqual(outs[0][:8], b"CIMG" + (32).to_bytes(2, "little") + (20).to_bytes(2, "little"))
        self.assertEqual(digest(outs), CIMG_SHA)


TEXT = ["", None, "plain", "  two\t\ttabs\there  ", "new\nlines\r\nand\x0bvt",
        "café über", "emoji \U0001F600 here", "　ideographic　space",
        "x" * 100, "a b"]

PIN_MSG = ['', 'plain', 'two tabs here', 'new lines and vt', 'caf\xe9 \xfcber', 'emoji \U0001f600 here', 'ideographic spac', 'xxxxxxxxxxxxxxxx', 'a b']
PIN_HUB = ['', '', 'plain', 'two tabs here', 'new lines and vt', 'caf ber', 'emoji here', 'ideographicspace', 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'ab', '', '', 'plain', 'two t', 'new l', 'caf b', 'emoji', 'ideog', 'xxxxx', 'ab']
PIN_TOGGL = ['', '', 'plain', 'two tabs here', 'new lines and vt', 'caf\xe9 \xfcber', 'emoji \U0001f600 here', 'ideographic space', 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'a b']
PIN_GOOGLE = ['', '', 'plain', 'two tabs here', 'new lines and vt', 'caf\xe9 \xfcber', 'emoji \U0001f600 here', 'ideographic space', 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx', 'a b']


class Lines(unittest.TestCase):

    def test_msg(self):
        self.assertEqual([msg._flat(s, 16) for s in TEXT if s is not None], PIN_MSG)

    def test_m5hub(self):
        self.assertEqual([m5hub._flat(s) for s in TEXT] + [m5hub._flat(s, 5) for s in TEXT], PIN_HUB)

    def test_toggl(self):
        self.assertEqual([toggl.clean(s) for s in TEXT], PIN_TOGGL)

    def test_google(self):
        self.assertEqual([google.clean(s) for s in TEXT], PIN_GOOGLE)


if __name__ == "__main__":
    unittest.main()
