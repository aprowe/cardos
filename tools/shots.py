"""Screenshots of the Cardputer, driven from the PC.

The panel is write-only and there is no framebuffer, so a screenshot is made
the only way it can be: the device repaints the whole screen while
display_blit mirrors every row it sends into /shots/NAME.565 on the card --
64,800 bytes of raw RGB565, the card standing in for the framebuffer -- and
then posts that file to webproxy.py, which calls decode_rgb565 below and
writes docs/shots/NAME.png and NAME@3x.png.

This file is both the codec (imported by webproxy.py) and the remote control:

    python tools/shots.py tour             # every app, into docs/shots
    python tools/shots.py keys "run mines\r" --wait 2 --shot mines
    python tools/shots.py shot console     # just what is on screen now

Keys go down the serial line as the raw key alphabet from kernel/drv/keyboard.h
-- 0x80 is Up, 0xC0+n is Opt+letter, 0x01 is ctrl-a -- and the main loop in
src/main.c feeds them to whichever shell has the screen, exactly as the matrix
would. One byte is not a key: 0xFF asks for a screenshot, and the loop takes
it before any app can see it, so a shot can be taken inside anything.
"""

import struct
import sys
import time

from PIL import Image

WIDTH = 240
HEIGHT = 135
SHOT_BYTE = 0xFF


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
    import os
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name + ".png")
    img.save(path)
    scale(img, 3).save(os.path.join(out_dir, name + "@3x.png"))
    return path


# ---- the remote control ---------------------------------------------------

ESC = b"\x1b"
ENTER = b"\r"
UP, DOWN, LEFT, RIGHT = b"\x80", b"\x81", b"\x82", b"\x83"


def opt(c):
    """Opt+letter, as the keyboard driver spells it."""
    return bytes([0xC0 + ord(c) - ord("a")])


def ctrl(c):
    return bytes([ord(c) - ord("a") + 1])


class Device:
    """The Cardputer on a serial port, driven like a keyboard."""

    def __init__(self, port="COM3", out_dir="docs/shots"):
        import os
        import serial
        self.out_dir = out_dir
        # DTR/RTS are what reset an ESP32-S3 over USB, and pyserial raises
        # them on open by default. Low before the port opens, so attaching
        # the remote control does not reboot the machine.
        self.s = serial.Serial(None, 115200, timeout=0.2)
        self.s.port = port
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        os.makedirs(out_dir, exist_ok=True)

    def keys(self, data, gap=0.03):
        """Bytes down the line, one at a time: the loop polls the port for
        one character a pass, and a burst just queues, but a pause between
        them is what lets a key be acted on before the next arrives."""
        if isinstance(data, str):
            data = data.encode("latin-1")
        for b in data:
            self.s.write(bytes([b]))
            time.sleep(gap)

    def line(self, text, wait=0.5):
        """A console command, entered."""
        self.keys(text)
        self.keys(ENTER)
        time.sleep(wait)

    def shot(self, name, timeout=30):
        """Ask for one and wait for the proxy to have written it."""
        import glob
        import os
        before = set(glob.glob(os.path.join(self.out_dir, "serial*.png")))
        self.s.write(bytes([SHOT_BYTE]))
        t = time.time()
        while time.time() - t < timeout:
            new = set(glob.glob(os.path.join(self.out_dir, "serial*.png"))) - before
            new = {p for p in new if "@" not in os.path.basename(p)}
            if new:
                time.sleep(0.3)               # the @3x is written second
                src = new.pop()
                stem = os.path.splitext(src)[0]
                for suffix in ("", "@3x"):
                    dst = os.path.join(self.out_dir, name + suffix + ".png")
                    if os.path.exists(dst):
                        os.remove(dst)
                    os.replace(stem + suffix + ".png", dst)
                print("  %s.png" % name)
                return True
            time.sleep(0.2)
        print("  %s: no shot arrived -- is webproxy.py running?" % name)
        return False

    def drain(self):
        """Whatever the device printed, for a look."""
        return self.s.read(65536).decode("utf-8", "replace")


# Every app that has a face. `run` opens an app from the console into the
# launcher; escape leaves it, and opt-3 is the console from anywhere.
# (shot name, console line, keys to press once it is up, seconds to wait).
# An app is opened on something worth looking at where it takes an argument,
# and prodded where its first screen is an invitation rather than the thing.
TOUR = [
    ("mines",    "run mines",                        b"",       0),
    ("pinball",  "run pinball",                      b"",       0),
    ("calendar", "run calendar",                     b"",       0),
    ("todo",     "run todo",                         b"",       0),
    ("edit",     "run edit /notes.txt",              b"",       0),
    ("ide",      "run ide /asm/sum.s",               b"",       0),
    ("files",    "run files",                        b"",       0),
    ("explorer", "run explorer",                     b"",       0),
    ("photos",   "run photos /shots/",              b"",       0),   # its own screenshots; the slash means folder
    ("web",      "run web https://news.ycombinator.com", b"",   12),
    ("claude",   "run claude",                       b"",       0),
    ("stocks",   "run stocks",                       b"r",      8),
    ("screen",   "run screen",                       ENTER,     4),
    ("memory",   "run memory",                       b"",       0),
    ("settings", "run settings",                     b"",       0),
]

OPT = lambda d: bytes([0xA0 + d])     # opt-digit: 1 launcher, 2 desktop, 3 console


def home(dev):
    """Back to a clean console prompt from wherever the device is: escape
    closes whatever is in front (app, folder, window), then opt-3 changes
    shell, which no escape does on purpose."""
    dev.keys(ESC + ESC + ESC, gap=0.3)
    dev.keys(OPT(3)); time.sleep(0.6)
    dev.line("clear", 0.3)


def tour(dev, only=None):
    home(dev)
    if not only:
        dev.line("help", 1.0)
        dev.shot("console")

        dev.keys(OPT(1)); time.sleep(1.5)            # launcher, on Memory
        dev.shot("launcher")
        dev.keys(RIGHT + RIGHT, gap=0.4); time.sleep(0.5)   # Games
        dev.keys(ENTER); time.sleep(1.0)
        dev.shot("launcher-folder")
        home(dev)

        dev.keys(OPT(2)); time.sleep(2.0)            # desktop
        dev.shot("desktop")
        dev.keys(ENTER); time.sleep(1.0)             # Memory, in a window
        dev.shot("desktop-window")
        home(dev)

    for name, cmd, keys, wait in TOUR:
        if only and name not in only:
            continue
        dev.line(cmd, 4.0)               # network apps join WiFi first
        if keys:
            dev.keys(keys)
        time.sleep(wait)
        dev.shot(name)
        home(dev)
    dev.drain()


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--out", default="docs/shots")
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tour", help="every app, into --out")
    t.add_argument("only", nargs="*", help="just these apps")
    s = sub.add_parser("shot", help="what is on the screen now")
    s.add_argument("name")
    k = sub.add_parser("keys", help="send keys, then optionally shoot")
    k.add_argument("text", help=r"bytes; \r for enter, \x1b for escape")
    k.add_argument("--wait", type=float, default=1.0)
    k.add_argument("--shot", default=None)
    a = ap.parse_args(argv)

    dev = Device(a.port, a.out)
    if a.cmd == "tour":
        tour(dev, set(a.only) or None)
    elif a.cmd == "shot":
        dev.shot(a.name)
    elif a.cmd == "keys":
        dev.keys(a.text.encode("latin-1").decode("unicode_escape").encode("latin-1"))
        time.sleep(a.wait)
        if a.shot:
            dev.shot(a.shot)


if __name__ == "__main__":
    main(sys.argv[1:])
