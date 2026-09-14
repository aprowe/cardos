"""Your desktop, on the Cardputer.

A 1920x1080 screen scaled to 240x135 is unreadable mush -- the same lesson the
web renderer taught, for the same reason. So the default is not a scaled
screen: it is a **240x135 window of the real one, at one pixel per pixel,
following your cursor**. Text stays sharp because nothing is resampled, and
the thing you are pointing at is always the thing on the device.

`fit` scales the whole screen down for orientation. It is deliberately
unreadable and useful anyway: it tells you where you are before you go back to
following.

The wire format, SCR1, sends rows rather than frames:

    "SCR1" u16 w u16 h        once, at the start
    then, forever:
      u16 y                   the row, or 0xFFFF meaning end of frame
      u16 n                   bytes of RLE
      n bytes                 RLE, the same codec .cpx uses

Only rows that changed are sent. A still desktop costs nothing, a blinking
cursor costs one row, and the device never holds a frame: it reads a row,
decodes it, blits it and forgets it -- 480 bytes of working memory for a live
video stream. There is no reference frame on the device because a row it is
not sent is a row it does not touch, which is the same trick the damage
channel uses on space, applied to time.
"""

import struct
import time

import mss
import numpy as np

WIDTH = 240
HEIGHT = 135

# Twelve is enough to read a cursor moving and cheap enough that the encoder is
# never the bottleneck: capture measured at 17 ms, so the budget is elsewhere.
DEFAULT_FPS = 12

# A row has to differ by more than this many pixels to be worth sending. Zero
# would resend rows that changed by one antialiased pixel of a blinking caret.
CHANGE_THRESHOLD = 0


def rgb565_rows(bgra):
    """A captured BGRA frame to one u16 per pixel, byte-swapped for the panel.

    Vectorised because doing it per pixel in Python at twelve frames a second
    is thirty-nine thousand pixels a frame and Python is not that fast."""
    b = bgra[:, :, 0].astype(np.uint16)
    g = bgra[:, :, 1].astype(np.uint16)
    r = bgra[:, :, 2].astype(np.uint16)
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # The panel is MSB-first and the chip is little-endian.
    return ((v >> 8) | ((v & 0xFF) << 8)).astype(np.uint16)


def encode_row(row):
    """One row of u16 to RLE. Same two modes as the .cpx codec: a run, or a
    literal stretch, because a run-only encoder expands noisy rows by 3x and a
    desktop is mostly noisy rows with long flat ones between them."""
    out = bytearray()
    i = 0
    n = len(row)
    while i < n:
        run = 1
        while i + run < n and row[i + run] == row[i] and run < 128:
            run += 1
        if run >= 2:
            out.append(run - 1)
            out += struct.pack("<H", int(row[i]))
            i += run
            continue
        start = i
        while i < n:
            if i + 2 < n and row[i] == row[i + 1] == row[i + 2]:
                break
            i += 1
            if i - start == 128:
                break
        count = i - start
        out.append(128 + count - 1)
        out += row[start:start + count].astype("<u2").tobytes()
    return bytes(out)


class Screen:
    """Capture, diff and encode. One instance per viewer."""

    def __init__(self, mode="follow", fps=DEFAULT_FPS):
        self.mode = mode
        self.fps = max(1, min(fps, 30))
        self.prev = None
        self.sct = None

    def _region(self, sct):
        """What to grab: a window around the cursor, or the whole screen."""
        mon = sct.monitors[1]
        if self.mode != "follow":
            return mon

        # Where the pointer is. ctypes rather than a dependency: this runs on
        # the machine the screen belongs to, which on Windows means user32.
        try:
            import ctypes
            pt = ctypes.wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            cx, cy = pt.x, pt.y
        except Exception:
            cx = mon["left"] + mon["width"] // 2
            cy = mon["top"] + mon["height"] // 2

        left = cx - WIDTH // 2
        top = cy - HEIGHT // 2
        # Clamped so the window never runs off the desktop, which would give
        # a short capture and a torn frame.
        left = max(mon["left"], min(left, mon["left"] + mon["width"] - WIDTH))
        top = max(mon["top"], min(top, mon["top"] + mon["height"] - HEIGHT))
        return {"left": left, "top": top, "width": WIDTH, "height": HEIGHT}

    def frames(self):
        """Yield encoded frames forever. A generator so the HTTP handler can
        stop simply by stopping asking."""
        with mss.mss() as sct:
            yield b"SCR1" + struct.pack("<HH", WIDTH, HEIGHT)
            self.prev = None
            period = 1.0 / self.fps

            while True:
                started = time.time()
                shot = sct.grab(self._region(sct))
                a = np.asarray(shot)[:, :, :3][:, :, ::-1]      # BGRA -> RGB

                if a.shape[0] != HEIGHT or a.shape[1] != WIDTH:
                    # `fit`, or a monitor that is not the size we asked for.
                    from PIL import Image
                    im = Image.frombytes("RGB", (a.shape[1], a.shape[0]), a.tobytes())
                    im = im.resize((WIDTH, HEIGHT), Image.BILINEAR)
                    a = np.asarray(im)

                bgra = np.dstack([a[:, :, 2], a[:, :, 1], a[:, :, 0],
                                  np.zeros(a.shape[:2], np.uint8)])
                cur = rgb565_rows(bgra)

                out = bytearray()
                for y in range(HEIGHT):
                    if self.prev is not None:
                        diff = np.count_nonzero(cur[y] != self.prev[y])
                        if diff <= CHANGE_THRESHOLD:
                            continue
                    payload = encode_row(cur[y])
                    out += struct.pack("<HH", y, len(payload))
                    out += payload
                out += struct.pack("<HH", 0xFFFF, 0)            # end of frame
                self.prev = cur

                yield bytes(out)

                # Pace it. Sending as fast as the encoder can go would flood a
                # device that is blitting each row as it lands.
                slack = period - (time.time() - started)
                if slack > 0:
                    time.sleep(slack)
