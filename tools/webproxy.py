#!/usr/bin/env python3
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

Run it, then on the device:  web http://<this machine>:8080/render?url=...

    python tools/webproxy.py --port 8080
"""

import argparse
import hmac
import io
import os
import struct
import subprocess
import sys
import tempfile
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pixelrender import shoot_pixel
from chat import ChatService, ROOT as ROOT_DIR

SHOTS_DIR = os.path.join(ROOT_DIR, "docs", "shots")
from voice import Voice
from screen import Screen
import shots
import updates

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
    tools/pixelrender.py."""
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


def _int_arg(args, name, default):
    """A query parameter as an int, or the default; a value that is not a
    number is a client error, not a traceback."""
    raw = (args.get(name) or [None])[0]
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError:
        raise ValueError("%s=%r is not a number" % (name, raw))


class Handler(BaseHTTPRequestHandler):
    chrome = None
    chat = None
    voice = None

    # ---- small helpers ----------------------------------------------------

    def _text(self, body, code=200):
        data = body.encode("utf-8", "replace")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _file(self, path):
        """A whole file, with its length up front so the device's download
        knows when it is done rather than waiting for the socket to close."""
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _authorised(self):
        """A shared secret, if one was asked for.

        Off by default because the common case is a device and a laptop on one
        home network, and a token the device has no way to be told is a token
        nobody uses. On when it matters -- see the warning at startup."""
        if not self.chat or not self.chat.token:
            return True
        # Two spellings, because the device has only one. CardApi's http()
        # takes a bearer token and sends "Authorization: Bearer x" -- there is
        # no way to set an arbitrary header from an app -- while curl and a
        # browser console reach for X-Token. Both are the same string.
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            auth = auth[7:]
        for given in (self.headers.get("X-Token", ""), auth):
            if hmac.compare_digest(self.chat.token, given):
                return True
        self._text("unauthorised" + "\n", 403)
        return False

    # ---- talking to Claude ------------------------------------------------

    def _do_shot(self, args):
        """A raw screen in, PNGs out: docs/shots/NAME.png and NAME@3x.png.

        Kept small on purpose. The device did the hard part -- watching its
        own blits -- and all the PC adds is a codec and a folder; see
        tools/shots.py for both halves."""
        name = (args.get("name") or ["shot"])[0]
        name = "".join(c for c in name if c.isalnum() or c in "-_") or "shot"
        raw = self._body(2 * 240 * 135 + 64)     # one screen of RGB565
        try:
            img = shots.decode_rgb565(raw)
        except ValueError as e:
            self._text("error %s\n" % e, 400)
            return
        out = shots.save(img, name, SHOTS_DIR)
        sys.stderr.write("shot: %s\n" % out)
        self._text("ok %s\n" % out)

    def _do_voice(self):
        """A WAV in; the words out, or one command line.

        Both halves answer on this one request rather than through the job
        queue the chat uses. Recognition of a ten-second clip takes about two
        seconds and a command translation about one, which is inside what the
        device will wait for -- and unlike a chat turn, there is nothing useful
        to show while it happens."""
        wav = self._body(4 << 20)       # 16 kHz mono: two minutes is 3.8 MB
        if len(wav) <= 44:              # a header and no audio
            self._text("error nothing recorded\n", 400)
            return

        text, err = self.voice.transcribe(wav)
        if err:
            sys.stderr.write("voice: %s\n" % err)
            self._text("error %s\n" % err)
            return
        sys.stderr.write("voice: heard %r\n" % text[:80])

        # The wake word is checked here as well as on the device: the device
        # decides what to do, but the translation only happens if it is asked
        # for, and asking costs a model call.
        low = text.lstrip().lower()
        wake = None
        for name in ("carlos", "karlos", "carlus", "carlo"):
            if low.startswith(name):
                rest = text.lstrip()[len(name):].lstrip(" ,.:!?")
                wake = rest
                break

        if wake is None:
            self._text("text %s\n" % text)
            return
        if not wake:
            self._text("error I heard my name and nothing after it\n")
            return

        line = self.voice.command(wake, self.chat)
        sys.stderr.write("voice: %r -> %s\n" % (wake[:60], line))
        self._text("cmd %s\n" % line)

    # An exception inside a handler used to close the socket with no status
    # line at all: the device then waited out its whole timeout rather than
    # reading one line saying what went wrong. Everything the handlers do is
    # wrapped, and the traceback goes to the terminal as before.
    def do_GET(self):
        self._guarded(self._get)

    def do_POST(self):
        self._guarded(self._post)

    def _guarded(self, fn):
        try:
            fn()
        except (BrokenPipeError, ConnectionResetError):
            pass                     # the device gave up first; nothing to tell
        except Exception as e:
            traceback.print_exc()
            try:
                self._text("error %s: %s\n" % (type(e).__name__, e), 500)
            except Exception:
                pass

    def _body(self, limit):
        """The request body, refused rather than read if it is longer than
        the route could possibly want. Content-Length was trusted whole: one
        request claiming four gigabytes had the server trying to hold it."""
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ValueError("bad Content-Length")
        if n < 0 or n > limit:
            raise ValueError("body of %d bytes is more than %d" % (n, limit))
        return self.rfile.read(n) if n else b""

    def _post(self):
        q = urllib.parse.urlparse(self.path)
        if q.path == "/voice":
            if not self._authorised():
                return
            self._do_voice()
            return
        if q.path == "/shot":
            if not self._authorised():
                return
            self._do_shot(urllib.parse.parse_qs(q.query))
            return
        if q.path != "/chat":
            self.send_error(404)
            return
        if not self._authorised():
            return
        text = self._body(64 << 10).decode("utf-8", "replace").strip()
        if not text:
            self._text("empty" + "\n", 400)
            return
        jid = self.chat.start(text)
        sys.stderr.write("chat #%d: %s" % (jid, text[:70]) + "\n")
        self._text("id %d" % jid + "\n")

    def _do_screen(self, args):
        """The desktop, streamed until the device stops reading.

        No Content-Length: this response has no end, and the socket closing is
        how it finishes. Written straight to wfile so nothing buffers a frame
        here either -- the encoder yields, this sends, and back pressure from
        a device that cannot keep up arrives as a slow write, which paces the
        capture for free."""
        mode = (args.get("mode") or ["follow"])[0]
        fps = _int_arg(args, "fps", 12)

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        sys.stderr.write("screen: streaming, mode=%s fps=%d" % (mode, fps) + "\n")

        sent = 0
        try:
            for chunk in Screen(mode=mode, fps=fps).frames():
                self.wfile.write(chunk)
                sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass          # the viewer quit, which is the normal ending
        sys.stderr.write("screen: stopped after %d bytes" % sent + "\n")

    def _do_chat_get(self, args):
        jid = _int_arg(args, "id", 0)
        state, reply = self.chat.poll(jid)
        if state == "pending":
            self._text("pending" + "\n")
            return
        # The state on its own line, so the device can tell an answer from a
        # failure without parsing anything.
        self._text(state + "\n" + reply)
        sys.stderr.write("chat #%d: %s, %d chars" % (jid, state, len(reply)) + "\n")

    def _get(self):
        q = urllib.parse.urlparse(self.path)
        args = urllib.parse.parse_qs(q.query)

        if q.path == "/screen":
            if not self._authorised():
                return
            self._do_screen(args)
            return

        if q.path == "/chat":
            if not self._authorised():
                return
            self._do_chat_get(args)
            return

        if q.path == "/status":
            # Which build of this file is actually running, and what it will
            # hand the agent. There is no way to tell a stale server from a
            # fresh one by looking at it, and a stale one answers every
            # question with the last bug you fixed.
            c = self.chat
            env = c._child_env() if c else {}
            # Behind the token when there is one: it names the repo and the
            # open session, which is more than a stranger needs.
            if c and c.token and not self._authorised():
                return
            self._text(
                "claude:        %s\n" % (c.claude if c else "-") +
                "api key given: %s\n" % ("yes" if "ANTHROPIC_API_KEY" in env else "no") +
                "session:       %s\n" % (c.session_id if c and c.session_id else "none yet") +
                "token needed:  %s\n" % ("yes" if c and c.token else "no") +
                "repo:          %s\n" % ROOT_DIR)
            return

        if q.path == "/chat/new":
            if not self._authorised():
                return
            self.chat.reset()
            sys.stderr.write("chat: new conversation" + "\n")
            self._text("ok" + "\n")
            return

        # ---- updates: the firmware and the apps, as built here ------------
        if q.path == "/update":
            if not self._authorised():
                return
            self._text(updates.manifest())
            return
        if q.path == "/update/firmware":
            if not self._authorised():
                return
            if not os.path.isfile(updates.FIRMWARE):
                self._text("no firmware built\n", 404)
                return
            self._file(updates.FIRMWARE)
            sys.stderr.write("update: sent firmware\n")
            return
        if q.path.startswith("/update/app/"):
            if not self._authorised():
                return
            path = updates.app_path(q.path[len("/update/app/"):])
            if not path:
                self._text("no such app\n", 404)
                return
            self._file(path)
            sys.stderr.write("update: sent %s\n" % os.path.basename(path))
            return

        if q.path not in ("/render", "/"):
            self.send_error(404)
            return

        url = (args.get("url") or [""])[0]
        # The banner is public; a render is not. Chrome here will fetch any
        # URL it is given, file:// and the LAN included, and hand the pixels
        # back -- with a token set, that is not something a stranger on the
        # network gets to do.
        if url and not self._authorised():
            return
        if not url:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(
                b"cardos server\n"
                b"  GET  /render?url=https://...  a web page, as pixels\n"
                b"  POST /chat                    ask Claude; returns an id\n"
                b"  GET  /chat?id=N               the answer, once it is ready\n"
                b"  GET  /chat/new                forget the conversation\n")
            return
        if "://" not in url:
            url = "https://" + url

        height = min(_int_arg(args, "h", 2000), MAX_HEIGHT)
        wait = _int_arg(args, "wait", 6000)

        # px=0 asks for the other renderer: real fonts, rendered wide and
        # scaled down. Softer text, but a page of photographs looks like
        # itself. The device offers both under f.
        pixel = (args.get("px") or ["1"])[0] not in ("0", "no", "off")
        css_w = max(WIDTH, min(_int_arg(args, "w", DEFAULT_WIDTH), 1280))
        links = []
        if pixel:
            sys.stderr.write("render %s (6x8 font at %dpx)\n" % (url, WIDTH))
            im, links = shoot_pixelfont(self.chrome, url, height, wait)
        else:
            sys.stderr.write("render %s (viewport %dpx)\n" % (url, css_w))
            im, links = shoot(self.chrome, url, height, wait, css_w)
        if im is None:
            self.send_error(502, "render failed")
            return
        im = trim(im)
        # A link past the trimmed tail points at nothing the device can
        # scroll to, so it is dropped rather than shipped.
        # And one parked off-screen -- the "skip to content" link at
        # left:-9999px is on every other site -- has a coordinate the packer
        # cannot hold, and used to kill the whole render.
        links = [(l["x"], l["y"], l["w"], l["h"], l["href"])
                 for l in links
                 if l["y"] < im.size[1] and
                 all(0 <= l[k] <= 65535 for k in ("x", "y", "w", "h"))]
        data = to_cpx(im, links)
        sys.stderr.write("  %dx%d -> %d bytes, %d links (raw would be %d)\n"
                         % (im.size[0], im.size[1], len(data), len(links),
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
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH,
                    help="CSS viewport width, --no-pixel only; wider is scaled to 240")
    ap.add_argument("--no-pixel", action="store_true",
                    help="photographic mode: real fonts, rendered wide and scaled down")
    ap.add_argument("--claude-cli", default=None,
                    help="path to the claude executable, if it is not on PATH")
    ap.add_argument("--model", default=None,
                    help="model for the chat session, e.g. claude-opus-5")
    ap.add_argument("--whisper", default=None,
                    help="directory holding whisper-cli and a ggml model "
                         "(default: the sibling cardlet project's)")
    ap.add_argument("--token", default=None,
                    help="require this shared secret on /chat (bearer or X-Token)")
    ap.add_argument("--api-key", action="store_true",
                    help="let the agent use ANTHROPIC_API_KEY from the environment "
                         "instead of the login this machine already has")
    args = ap.parse_args()

    Handler.chrome = args.chrome or find_chrome()
    Handler.chat = ChatService(claude=args.claude_cli, token=args.token,
                               model=args.model, use_api_key=args.api_key)
    Handler.voice = Voice(whisper_dir=args.whisper)

    if args.test:
        if args.no_pixel:
            im, links = shoot(Handler.chrome, args.test, 2000, 6000, args.width)
        else:
            im, links = shoot_pixelfont(Handler.chrome, args.test, 2000, 6000)
        if im is None:
            raise SystemExit("render failed")
        im = trim(im)
        data = to_cpx(im)
        with open("page.cpx", "wb") as f:
            f.write(data)
        print("page.cpx: %dx%d, %d bytes (raw %d)"
              % (im.size[0], im.size[1], len(data), im.size[0] * im.size[1] * 2))
        return

    # Threading, because a chat turn takes a minute, a render takes ten
    # seconds, and the device polls for its answer throughout. On the
    # single-threaded server every poll queued behind the work it was polling.
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("cardos server on port %d" % args.port)
    print("  chrome: %s" % Handler.chrome)
    print("  claude: %s" % (Handler.chat.claude or "NOT FOUND -- /chat will fail"))
    print("  voice:  %s" % ("whisper ready" if Handler.voice.ready()
                            else "NOT FOUND -- /voice will fail"))
    print("on the device:  web http://<this machine>:%d/render?url=..." % args.port)
    print("                claude, once its proxy is set to this machine")
    if not args.token:
        # Said plainly, once, where it can still be acted on.
        print("")
        print("  This server runs Claude Code in %s with permission" % ROOT_DIR)
        print("  to edit it, and asks nothing of whoever connects. Anything that")
        print("  can reach this port can change that folder. --token SECRET")
        print("  requires a shared string, which the device reads from")
        print("  /claude.token on its card.")
    srv.serve_forever()


if __name__ == "__main__":
    main()
