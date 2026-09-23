"""Tests for the screenshot conversion. Run: python -m server.tests.test_shots"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root
from server import shots


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def test_decodes_big_endian_rgb565_rows_as_the_panel_is_sent_them():
    w, h = 3, 2
    px = [rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
          rgb565(0, 0, 0), rgb565(255, 255, 255), rgb565(8, 4, 8)]
    raw = struct.pack(">%dH" % (w * h), *px)
    img = shots.decode_rgb565(raw, w, h)
    assert img.size == (3, 2)
    assert img.getpixel((0, 0)) == (255, 0, 0)
    assert img.getpixel((1, 0)) == (0, 255, 0)
    assert img.getpixel((2, 0)) == (0, 0, 255)
    assert img.getpixel((1, 1)) == (255, 255, 255)
    # 5/6/5 bits are replicated into the low bits, so full-scale stays
    # full-scale and black stays black -- the trick 0xFF above depends on.
    assert img.getpixel((2, 1)) == (8, 4, 8)


def test_rejects_short_files():
    try:
        shots.decode_rgb565(b"\0" * 10, 240, 135)
    except ValueError as e:
        assert "64800" in str(e)
    else:
        assert False, "short file accepted"


def test_scale_is_nearest_neighbour():
    raw = struct.pack(">2H", rgb565(255, 0, 0), rgb565(0, 0, 255))
    img = shots.scale(shots.decode_rgb565(raw, 2, 1), 3)
    assert img.size == (6, 3)
    assert img.getpixel((2, 2)) == (255, 0, 0)
    assert img.getpixel((3, 0)) == (0, 0, 255)


if __name__ == "__main__":
    n = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); n += 1
    print("%d tests ok" % n)
