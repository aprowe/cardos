"""Pictures for the device: JPG, PNG (anything Pillow opens) to what Photo
shows.

The device has no JPEG or PNG decoder, and should not grow one -- flash is
what it is short of -- so it sends the file here and gets back its own
format: raw RGB565, high byte first as the panel takes it, behind an 8-byte
header, 'CIMG' u16 width u16 height (little-endian), named .img. A bare
.565 is the same pixels with no header, which Photo takes as 240x135.

    POST /image/convert?w=240&h=135&fit=contain|cover&fmt=img|565   body: the picture
         -> the converted bytes                (the dashboard)
    POST /image/convert?...&keep=1
         -> id <tab> width <tab> height <tab> bytes
    GET  /image/result?id=ID -> the converted bytes, once

The second form is the device's: its upload can only bring back a short
reply, so the picture is kept for a moment and fetched with a download that
streams to the card. Kept results go after ten minutes or once fetched.

Fit: contain shows the whole picture with black bars; cover fills the
screen and crops. Photos from a phone are turned upright first (EXIF).
"""
import io
import secrets
import struct
import threading
import time


MAX_IN = 16 << 20
MAX_W, MAX_H = 320, 240
_kept = {}
_lock = threading.Lock()


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
    rgb = im.tobytes()
    px = bytearray(out_w * out_h * 2)
    for i in range(out_w * out_h):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        px[2 * i] = v >> 8                     # high byte first: the panel's order
        px[2 * i + 1] = v & 0xFF
    if fmt == "565":
        return bytes(px), out_w, out_h
    return b"CIMG" + struct.pack("<HH", out_w, out_h) + bytes(px), out_w, out_h


def _args(args):
    def num(k, d):
        try:
            return int((args.get(k) or [d])[0])
        except ValueError:
            raise ValueError("%s= is a number" % k)
    fit = (args.get("fit") or ["contain"])[0]
    fmt = (args.get("fmt") or ["img"])[0]
    if fit not in ("contain", "cover"):
        raise ValueError("fit= is contain or cover")
    if fmt not in ("img", "565"):
        raise ValueError("fmt= is img or 565")
    return num("w", 240), num("h", 135), fit, fmt


def _octets(h, data):
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(len(data)))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    h.wfile.write(data)


def post_convert(h, path, args):
    """a JPG or PNG, as a picture the device shows"""
    try:
        w, ht, fit, fmt = _args(args)
        out, ow, oh = convert(h.body(MAX_IN), w, ht, fit, fmt)
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    if (args.get("keep") or ["0"])[0] != "1":
        _octets(h, out)
        return
    now = time.time()
    nid = secrets.token_hex(6)
    with _lock:
        for k in [k for k, v in _kept.items() if now - v[0] > 600]:
            del _kept[k]
        _kept[nid] = (now, out)
    h.text("%s\t%d\t%d\t%d\n" % (nid, ow, oh, len(out)))


def get_result(h, path, args):
    """a kept conversion, fetched once"""
    nid = (args.get("id") or [""])[0]
    with _lock:
        got = _kept.pop(nid, None)
    if not got:
        h.text("error no such picture (they are kept ten minutes)\n", 404)
        return
    _octets(h, got[1])


ROUTES = [
    ("POST", "/image/convert", post_convert, "device_or_dash"),
    ("GET", "/image/result", get_result, "device_or_dash"),
    # Under /dash as well: on the public name nginx passes only /dash*.
    ("POST", "/dash/image/convert", post_convert, "device_or_dash"),
]
