"""Pixels the way the device's panel takes them, in one place.

RGB565, high byte first, which is what the ST7789 is sent: as a u16 in
the chip's little-endian memory that is the value byte-swapped. And the
RLE row both /render (server/render) and /screen send, which apps/web.c
and the screen viewer decode a row at a time -- two modes:

    0..127    a run: the next pixel (u16, little-endian) repeated N+1 times
    128..255  a literal: the next N-127 pixels as they are

A literal stops where a run of three or more begins: below that, breaking
out costs more than it saves. Each mode covers at most 128 pixels.

server/tests/test_wire.py pins the bytes; the device has no slack for a
byte out of place.
"""
import struct


def rgb565_swapped(r, g, b):
    """One pixel as the u16 whose little-endian bytes are the panel's."""
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8)


def rgb565_rows(bgra):
    """A captured BGRA frame (numpy, h x w x 4) to one swapped u16 a pixel.
    Vectorised: per pixel in Python at twelve frames a second is thirty-nine
    thousand pixels a frame, and Python is not that fast."""
    import numpy as np
    b = bgra[:, :, 0].astype(np.uint16)
    g = bgra[:, :, 1].astype(np.uint16)
    r = bgra[:, :, 2].astype(np.uint16)
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) | ((v & 0xFF) << 8)).astype(np.uint16)


def rgb565_bytes(rgb):
    """Packed RGB bytes (Pillow's tobytes()) to the panel's bytes, two a
    pixel, high byte first."""
    try:
        import numpy as np
    except ImportError:
        return _rgb565_bytes_py(rgb)
    a = np.frombuffer(rgb, dtype=np.uint8).reshape(-1, 3).astype(np.uint16)
    v = ((a[:, 0] & 0xF8) << 8) | ((a[:, 1] & 0xFC) << 3) | (a[:, 2] >> 3)
    return v.astype(">u2").tobytes()


def _rgb565_bytes_py(rgb):
    """The same, a pixel at a time, for a server without numpy."""
    out = bytearray(len(rgb) // 3 * 2)
    for i in range(len(rgb) // 3):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[2 * i] = v >> 8
        out[2 * i + 1] = v & 0xFF
    return bytes(out)


def encode_row(pixels):
    """One row of u16 pixels -- a list, or a numpy row -- to RLE bytes."""
    out = bytearray()
    i = 0
    n = len(pixels)
    while i < n:
        run = 1
        while i + run < n and pixels[i + run] == pixels[i] and run < 128:
            run += 1
        if run >= 2:
            out.append(run - 1)
            out += struct.pack("<H", int(pixels[i]))
            i += run
            continue
        start = i
        while i < n:
            if i + 2 < n and pixels[i] == pixels[i + 1] == pixels[i + 2]:
                break
            i += 1
            if i - start == 128:
                break
        count = i - start
        out.append(128 + count - 1)
        lit = pixels[start:i]
        if hasattr(lit, "astype"):
            out += lit.astype("<u2").tobytes()
        else:
            out += struct.pack("<%dH" % count, *lit)
    return bytes(out)
