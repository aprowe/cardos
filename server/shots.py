"""Screenshots of the Cardputer: the codec the server uses.

The panel is write-only and there is no framebuffer, so a screenshot is made
the only way it can be: the device repaints the whole screen while
display_blit mirrors every row it sends into /home/shots/NAME.565 on the card --
64,800 bytes of raw RGB565, the card standing in for the framebuffer -- and
then posts that file to the server's /shot, which calls decode_rgb565 below and
writes docs/shots/NAME.png and NAME@3x.png.

The remote control that asks for shots over serial is tools/shots.py.
"""

import os
import struct
import sys

from PIL import Image

WIDTH = 240
HEIGHT = 135


def decode_rgb565(raw, w=WIDTH, h=HEIGHT):
    """RGB565 rows, as the ST7789 was sent them, to an RGB image.

    Big-endian: display.h's RGB565 macro swaps every colour at compile time so
    the DMA can send the high byte first, and the tap copies what was sent.
    The 5/6/5 bits are replicated into the low bits rather than zero-padded,
    so white comes back as (255,255,255) and not (248,252,248)."""
    need = w * h * 2
    if len(raw) != need:
        raise ValueError("expected %d bytes for %dx%d, got %d" % (need, w, h, len(raw)))
    img = Image.new("RGB", (w, h))
    px = img.load()
    vals = struct.unpack(">%dH" % (w * h), raw)
    for i, v in enumerate(vals):
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px[i % w, i // w] = ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    return img


def scale(img, n):
    """Blown up for a blog, every pixel still a crisp square."""
    return img.resize((img.width * n, img.height * n), Image.NEAREST)


def save(img, name, out_dir):
    """NAME.png at one pixel per pixel, and NAME@3x.png for a page. Returns
    the path of the first."""
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name + ".png")
    img.save(path)
    scale(img, 3).save(os.path.join(out_dir, name + "@3x.png"))
    return path


# ---- route -----------------------------------------------------------------

SHOTS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "docs", "shots")


def post_shot(h, path, args):
    """a raw screen in; docs/shots/NAME.png out

    Kept small on purpose. The device did the hard part -- watching its own
    blits -- and all this adds is a codec and a folder."""
    name = (args.get("name") or ["shot"])[0]
    name = "".join(c for c in name if c.isalnum() or c in "-_") or "shot"
    raw = h.body(2 * WIDTH * HEIGHT + 64)          # one screen of RGB565
    try:
        img = decode_rgb565(raw)
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    out = save(img, name, SHOTS_DIR)
    sys.stderr.write("shot: %s\n" % out)
    h.text("ok %s\n" % out)


ROUTES = [("POST", "/shot", post_shot)]
