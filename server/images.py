"""Pictures for the device: JPG, PNG (anything Pillow opens) to what Photos
shows. server/photos.py keeps each picture's screen copy made here.

The device has no JPEG or PNG decoder, and should not grow one -- flash is
what it is short of -- so the server makes its own format: raw RGB565, high
byte first as the panel takes it, behind an 8-byte header, 'CIMG' u16 width
u16 height (little-endian), named .img. A bare .565 is the same pixels with
no header, which Photo takes as 240x135.

Fit: contain shows the whole picture with black bars; cover fills the
screen and crops. Photos from a phone are turned upright first (EXIF).

(/image/convert and /image/result, which converted a picture sent by a
device or the page, had no caller left and were removed on 2026-10-09.)
"""
import io
import struct

from .pixels import rgb565_bytes

MAX_W, MAX_H = 320, 240


def convert(data, w=240, h=135, fit="contain", fmt="img"):
    """The picture as the device's bytes, and its size."""
    from PIL import Image, ImageOps
    try:
        im = Image.open(io.BytesIO(data))
        im = ImageOps.exif_transpose(im)
    except Exception as e:                     # Pillow says many things
        raise ValueError("not a picture this server can read (%s)" % e)
    if not (1 <= w <= MAX_W and 1 <= h <= MAX_H):
        raise ValueError("size from 1x1 to %dx%d" % (MAX_W, MAX_H))
    if fmt == "565" and (w, h) != (240, 135):
        raise ValueError("a bare .565 is the whole screen, 240x135; use fmt=img")
    im = im.convert("RGB")
    if fit == "cover":
        im = ImageOps.fit(im, (w, h), Image.LANCZOS)
        out_w, out_h = w, h
    else:
        im.thumbnail((w, h), Image.LANCZOS)
        if fmt == "565":                       # the whole screen, barred
            pad = Image.new("RGB", (w, h))
            pad.paste(im, ((w - im.width) // 2, (h - im.height) // 2))
            im = pad
        out_w, out_h = im.size
    px = rgb565_bytes(im.tobytes())          # high byte first: the panel's order
    if fmt == "565":
        return bytes(px), out_w, out_h
    return b"CIMG" + struct.pack("<HH", out_w, out_h) + bytes(px), out_w, out_h
