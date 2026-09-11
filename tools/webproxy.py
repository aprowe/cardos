#!/usr/bin/env python3
"""Render web pages into something a Cardputer can scroll.

The device has no HTML parser and no layout engine, and no small version of
either exists. So the layout happens here, on a machine with a real browser,
and the device is sent pixels.

The trick that makes it look like a web page rather than a squashed one: Chrome
is told the viewport is 240 CSS pixels wide. Sites then serve their narrowest
mobile layout and render it at that size, so text is laid out small and *crisp*
rather than being shrunk into mush afterwards. The screenshot is already the
right width; nothing is downscaled.

The output is a .cpx file:

    "CPX1"  u16 width  u16 height  u16 nlinks  u16 flags
    u32 rows_offset   u32 links_offset
    u32 row_offset[height]           -- absolute, for seeking to any row
    rows: each   u16 byte_length, then RLE
    links: each  u16 x, y, w, h, url_len, then url bytes

RLE, per row, over RGB565 pixels already byte-swapped for the panel:

    n < 128   the next pixel, repeated n + 1 times
    n >= 128  the next (n - 127) pixels, literally

Two modes rather than one because a run-only encoder expands noisy rows by 3x,
and a page with a photograph in it is mostly noisy rows. This bounds the worst
case to about 1/128 over raw.

Run it, then on the device:  web http://<this machine>:8080/render?url=...

    python tools/webproxy.py --port 8080
"""

import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

from PIL import Image

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]

WIDTH = 240
MAX_HEIGHT = 4000


def find_chrome():
    for p in CHROME_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("no Chrome found; pass --chrome")


def shoot(chrome, url, height, wait_ms):
    """Full-page screenshot at a 240px viewport."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "shot.png")
        cmd = [
            chrome,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--window-size=%d,%d" % (WIDTH, height),
            # Fast-forwards timers and animations so the page settles instead
            # of being captured mid-fade.
            "--virtual-time-budget=%d" % wait_ms,
            "--screenshot=" + out,
            url,
        ]
        subprocess.run(cmd, capture_output=True, timeout=90)
        if not os.path.exists(out):
            return None
        return Image.open(out).convert("RGB")


def trim(im):
    """Drop the uniform tail. A page shorter than the window leaves a block of
    background that is not worth sending or scrolling through."""
    w, h = im.size
    px = im.load()
    last = 0
    for y in range(h):
        if len(set(px[x, y] for x in range(0, w, 6))) > 1:
            last = y
    return im.crop((0, 0, w, min(h, last + 12)))


def rgb565_swapped(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8)


def encode_row(pixels):
    """One row of u16 pixels to RLE bytes."""
    out = bytearray()
    i = 0
    n = len(pixels)
    while i < n:
        # How far does the current value run?
        run = 1
        while i + run < n and pixels[i + run] == pixels[i] and run < 128:
            run += 1
        if run >= 2:
            out.append(run - 1)
            out += struct.pack("<H", pixels[i])
            i += run
            continue
        # A literal stretch, ending where a run of 3 or more begins -- below
        # that, breaking out of the literal costs more than it saves.
        start = i
        while i < n and len(out) < 1 << 20:
            if i + 2 < n and pixels[i] == pixels[i + 1] == pixels[i + 2]:
                break
            i += 1
            if i - start == 128:
                break
        count = i - start
        out.append(128 + count - 1)
        for p in pixels[start:start + count]:
            out += struct.pack("<H", p)
    return bytes(out)


def to_cpx(im, links=()):
    w, h = im.size
    px = im.load()

    rows = []
    for y in range(h):
        line = [rgb565_swapped(*px[x, y]) for x in range(w)]
        rows.append(encode_row(line))

    header_size = 20
    index_size = 4 * h
    rows_offset = header_size + index_size

    body = bytearray()
    offsets = []
    for r in rows:
        offsets.append(rows_offset + len(body))
        body += struct.pack("<H", len(r))
        body += r

    links_offset = rows_offset + len(body)
    linkbuf = bytearray()
    for (x, y, lw, lh, url) in links:
        u = url.encode("utf-8")[:255]
        linkbuf += struct.pack("<HHHHH", x, y, lw, lh, len(u))
        linkbuf += u

    out = bytearray()
    out += b"CPX1"
    out += struct.pack("<HHHH", w, h, len(links), 0)
    out += struct.pack("<II", rows_offset, links_offset)
    for o in offsets:
        out += struct.pack("<I", o)
    out += body
    out += linkbuf
    return bytes(out)


class Handler(BaseHTTPRequestHandler):
    chrome = None

    def do_GET(self):
        q = urllib.parse.urlparse(self.path)
        args = urllib.parse.parse_qs(q.query)

        if q.path not in ("/render", "/"):
            self.send_error(404)
            return

        url = (args.get("url") or [""])[0]
        if not url:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"cardos web proxy: /render?url=https://...\n")
            return
        if "://" not in url:
            url = "https://" + url

        height = min(int((args.get("h") or [2000])[0]), MAX_HEIGHT)
        wait = int((args.get("wait") or [6000])[0])

        sys.stderr.write("render %s\n" % url)
        im = shoot(self.chrome, url, height, wait)
        if im is None:
            self.send_error(502, "render failed")
            return
        im = trim(im)
        data = to_cpx(im)
        sys.stderr.write("  %dx%d -> %d bytes (raw would be %d)\n"
                         % (im.size[0], im.size[1], len(data),
                            im.size[0] * im.size[1] * 2))

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--chrome", default=None)
    ap.add_argument("--test", help="render this URL to a file and exit")
    args = ap.parse_args()

    Handler.chrome = args.chrome or find_chrome()

    if args.test:
        im = shoot(Handler.chrome, args.test, 2000, 6000)
        if im is None:
            raise SystemExit("render failed")
        im = trim(im)
        data = to_cpx(im)
        with open("page.cpx", "wb") as f:
            f.write(data)
        print("page.cpx: %dx%d, %d bytes (raw %d)"
              % (im.size[0], im.size[1], len(data), im.size[0] * im.size[1] * 2))
        return

    srv = HTTPServer(("0.0.0.0", args.port), Handler)
    print("cardos web proxy on port %d, using %s" % (args.port, Handler.chrome))
    print("on the device:  web http://<this machine>:%d/render?url=..." % args.port)
    srv.serve_forever()


if __name__ == "__main__":
    main()
