"""Render web pages into something a Cardputer can scroll.

The device has no HTML parser and no layout engine, and no small version of
either exists. So the layout happens here, on a machine with a real browser,
and the device is sent pixels.

Chrome is told the viewport is a chosen number of CSS pixels wide (--width, or
?w= per request) and the shot is scaled down to the panel's 240. That width is
the whole feel of the thing: at 240 the site serves its narrowest mobile layout
and renders text at full size -- crisp, but about four words to a screen. Wider
renders more page per screen and scales it down, which is what makes it look
like a miniature of the page rather than a zoomed-in corner of it.

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

Served at /render by the CardOS server (python -m server); the route is
at the bottom of this file. On the device:  web http://<server>/render?url=...
"""

import io
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageFilter

from .pixelrender import shoot_pixel

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]

WIDTH = 240            # the panel, and the output width, always
MAX_HEIGHT = 4000

# The CSS viewport Chrome is given when the request does not say. 420 renders
# a page about 1.75x too wide and scales it down: text lands at roughly nine
# pixels, small but readable, and a screenful is a paragraph rather than four
# words. 240 is crisper and more zoomed in; 720 looks like a whole page and
# reads like one across a room.
DEFAULT_WIDTH = 420


def find_chrome():
    for p in CHROME_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("no Chrome found; pass --chrome")


def shoot_pixelfont(chrome, url, height, wait_ms):
    """The page at 240 in the device's own 6x8 font: (image, links).

    No downscale anywhere in this path, which is the point. The links come
    back because this is the only moment anything has a DOM to ask -- see
    pixelrender.py beside this file."""
    png, links = shoot_pixel(chrome, url, WIDTH, height, wait_ms)
    if not png:
        return None, []
    return Image.open(io.BytesIO(png)).convert("RGB"), links


def shoot_raw(chrome, url, height, wait_ms, css_width):
    """Full-page screenshot at its own size, before any resampling."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "shot.png")
        cmd = [
            chrome,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--window-size=%d,%d" % (css_width, height),
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


def _gamma(im, g):
    lut = [min(255, int(((v / 255.0) ** g) * 255 + 0.5)) for v in range(256)]
    return im.point(lut * 3)


def downscale(im, width=WIDTH):
    """Resample to the panel width, in linear light, then put the bite back.

    Two things matter here and both were measured rather than assumed. First,
    averaging pixels in sRGB is wrong: the midpoint of black and white in sRGB
    is 188, not 128, so shrinking dark text on a white page bleaches it. Going
    to linear light first keeps the stroke weight. Second, any downscale costs
    acutance whatever the filter, so a small unsharp mask afterwards is not
    cheating -- it restores the edge contrast the average took out. Radius is
    deliberately under a pixel; anything larger rings around the glyphs.
    """
    h = max(1, round(im.size[1] * width / im.size[0]))
    small = _gamma(_gamma(im, 2.2).resize((width, h), Image.LANCZOS), 1 / 2.2)
    return small.filter(ImageFilter.UnsharpMask(0.5, 130, 2))


def shoot(chrome, url, height, wait_ms, css_width=WIDTH):
    """A page rendered wide and brought down to the panel: (image, links).

    Text goes soft -- there is no filter that makes 4-pixel letterforms sharp,
    only ones that make them less mushy -- but the page keeps its own fonts and
    proportions, which is the point of this mode. For text, use the 6x8
    renderer instead.

    This goes through the DevTools path rather than --screenshot for one
    reason: links. Chrome's screenshot flag renders a picture and tells you
    nothing about what was in it, and a browser whose links do not work is a
    slideshow."""
    png, links = shoot_pixel(chrome, url, css_width, height, wait_ms,
                             restyle=False)
    if not png:
        return None, []
    im = Image.open(io.BytesIO(png)).convert("RGB")
    if css_width == WIDTH:
        return im, links

    scaled = downscale(im)
    # The links shrink with the page. Rounding outward by a pixel keeps a
    # one-line link tappable after a 3x reduction, where honest rounding can
    # leave a target two pixels tall.
    k = WIDTH / float(css_width)
    out = []
    for l in links:
        out.append({"x": int(l["x"] * k), "y": int(l["y"] * k),
                    "w": max(6, int(l["w"] * k) + 1),
                    "h": max(6, int(l["h"] * k) + 1),
                    "href": l["href"]})
    return scaled, out


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


# ---- route -----------------------------------------------------------------

def get_render(h, path, args):
    """?url=...: a web page, as pixels

    Open, so the bare path shows the banner, but a render is behind the
    token: Chrome here will fetch any URL it is given, file:// and the LAN
    included, and hand the pixels back."""
    url = (args.get("url") or [""])[0]
    if not url:
        from ..app import get_banner
        get_banner(h, path, args)
        return
    if not h.authorised():
        return
    if "://" not in url:
        url = "https://" + url

    height = min(h.int_arg(args, "h", 2000), MAX_HEIGHT)
    wait = h.int_arg(args, "wait", 6000)

    # px=0 asks for the other renderer: real fonts, rendered wide and scaled
    # down. Softer text, but a page of photographs looks like itself. The
    # device offers both under f.
    pixel = (args.get("px") or ["1"])[0] not in ("0", "no", "off")
    css_w = max(WIDTH, min(h.int_arg(args, "w", DEFAULT_WIDTH), 1280))
    links = []
    if pixel:
        sys.stderr.write("render %s (6x8 font at %dpx)\n" % (url, WIDTH))
        im, links = shoot_pixelfont(h.chrome, url, height, wait)
    else:
        sys.stderr.write("render %s (viewport %dpx)\n" % (url, css_w))
        im, links = shoot(h.chrome, url, height, wait, css_w)
    if im is None:
        h.send_error(502, "render failed")
        return
    im = trim(im)
    # A link past the trimmed tail points at nothing the device can scroll
    # to, so it is dropped rather than shipped. And one parked off-screen --
    # the "skip to content" link at left:-9999px is on every other site --
    # has a coordinate the packer cannot hold, and used to kill the render.
    links = [(l["x"], l["y"], l["w"], l["h"], l["href"])
             for l in links
             if l["y"] < im.size[1] and
             all(0 <= l[k] <= 65535 for k in ("x", "y", "w", "h"))]
    data = to_cpx(im, links)
    sys.stderr.write("  %dx%d -> %d bytes, %d links (raw would be %d)\n"
                     % (im.size[0], im.size[1], len(data), len(links),
                        im.size[0] * im.size[1] * 2))

    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(len(data)))
    h.end_headers()
    h.wfile.write(data)


ROUTES = [("GET", "/render", get_render, "open")]
