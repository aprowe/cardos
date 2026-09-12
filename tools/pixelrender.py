"""Screenshot a page that has been re-typeset in CardOS's own 6x8 font.

Rendering a page at a wide viewport and scaling the shot down to 240 does make
a whole page fit, but it destroys the text: antialiased 12-pixel type reduced to
four pixels is grey mush, and there is no filter that fixes that.

A bitmap font has no such failure mode. So instead of shrinking the page, the
page is laid out *in the device's font at its native size*: Chrome does the
layout it always does, at the panel's own 240-pixel viewport, but every run of
text is set in the 6x8 font at 8 (or 16, or 24) pixels, on the pixel grid, with
nothing to antialias. A screenful is a paragraph rather than four words, and
every letter is as crisp as it is on the console.

Chrome's --screenshot flag cannot inject anything, so this drives it over the
DevTools protocol instead: navigate, restyle, wait for the font, capture.
"""

import base64
import json
import os
import re
import socket
import subprocess
import tempfile
import time
import urllib.request

import websocket

from pixelfont import ensure as ensure_font

# Body text becomes 8px, subheads 16, the page title 24. Snapping to multiples
# of 8 is what keeps every baseline on the pixel grid; a 13px line would be
# resampled and the crispness is the whole point.
SNAP_JS = r"""
(async () => {
  const s = document.createElement('style');
  s.textContent = `@font-face{font-family:CardOSPixel;`
    + `src:url(data:font/ttf;base64,__FONT__) format('truetype');`
    + `font-display:block;}`
    // Pages written before phones existed lay out to a fixed width and simply
    // run off the side of a 240px viewport -- Hacker News is a 796px table.
    // Nothing can be done about that by scrolling, because the shot is one
    // column of pixels wide, so the page is squeezed into the panel instead:
    // tables get a fixed layout, long words break, and nothing may be wider
    // than its parent.
    + `html,body{max-width:__W__px!important;overflow-x:hidden!important}`
    + `*{max-width:100%!important;box-sizing:border-box!important;`
    // min-width:0 is the one that does the work. A table is never laid out
    // narrower than its min-content width, and neither overflow-wrap nor
    // word-break reduces that -- Hacker News stayed 796px wide through both.
    // This releases the floor, and the table then honours width:100%.
    + `overflow-wrap:break-word!important;min-width:0!important}`
    // Auto layout, not fixed: fixed splits the width evenly between columns,
    // which gives HN's rank and vote arrow as much room as the headline.
    + `table{table-layout:auto!important;width:100%!important}`
    // Images are capped well below the panel width, not at it. At a 240px
    // viewport a photo that obeys max-width:100% is the whole screen, and
    // next to 8-pixel text it reads as a page of pictures with captions.
    // Two thirds leaves the article looking like an article.
    + `img,video,iframe,svg,canvas,picture{max-width:160px!important;`
    + `height:auto!important;object-fit:contain!important}`
    + `pre,code{white-space:pre-wrap!important}`;
  document.documentElement.appendChild(s);
  // A site with a strict Content-Security-Policy blocks a data: font, and an
  // unhandled rejection here would abandon the restyle and leave the page in
  // its own fonts at its own width -- which, clipped to 240 pixels, is a blank
  // screen. Page.setBypassCSP is what actually lets the font through; this is
  // the belt to that pair of braces.
  try {
    await document.fonts.load('8px CardOSPixel');
    await document.fonts.ready;
  } catch (e) { /* carry on: the layout still wants doing */ }

  // Icon fonts carry meaning in their codepoints, not their letters -- swapping
  // the family turns every icon into a random glyph, so leave them alone.
  const ICON = /icon|awesome|material|glyph|symbol|dashicons/i;
  for (const el of document.querySelectorAll('*')) {
    const cs = getComputedStyle(el);
    if (ICON.test(cs.fontFamily)) continue;
    const px = parseFloat(cs.fontSize) || 16;
    const snap = px <= 18 ? 8 : px <= 28 ? 16 : 24;
    const st = el.style;
    st.setProperty('font-family', 'CardOSPixel, monospace', 'important');
    st.setProperty('font-size', snap + 'px', 'important');
    st.setProperty('line-height', (snap + snap / 2) + 'px', 'important');
    st.setProperty('letter-spacing', '0', 'important');
    st.setProperty('font-weight', '400', 'important');
    st.setProperty('font-style', 'normal', 'important');
    st.setProperty('text-rendering', 'geometricPrecision', 'important');
    st.setProperty('-webkit-font-smoothing', 'none', 'important');
  }
  // One more frame so the reflow the restyle caused is finished before the
  // layout metrics are asked for.
  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));

  // Where every link ended up. The device has no DOM and never will, so this
  // is the only chance to record what is clickable -- after the restyle,
  // because the restyle moves everything.
  const links = [];
  for (const a of document.querySelectorAll('a[href]')) {
    const r = a.getBoundingClientRect();
    const href = a.href;
    if (r.width < 6 || r.height < 5) continue;
    if (!/^https?:/.test(href)) continue;
    links.push({ x: Math.round(r.x + scrollX), y: Math.round(r.y + scrollY),
                 w: Math.round(r.width), h: Math.round(r.height), href: href });
    if (links.length >= 400) break;
  }
  return { title: document.title || '', links: links };
})()
"""


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Chrome:
    """A headless Chrome with its DevTools socket open, as a context manager."""

    def __init__(self, exe, width, height):
        self.dir = tempfile.mkdtemp(prefix="cardos-chrome-")
        self.port = _free_port()
        self.proc = subprocess.Popen([
            exe,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--window-size=%d,%d" % (width, height),
            "--remote-debugging-port=%d" % self.port,
            # Chrome refuses a DevTools socket that carries an Origin header,
            # and websocket-client always sends one. The port is bound to
            # loopback and lives as long as one screenshot.
            "--remote-allow-origins=*",
            "--user-data-dir=" + self.dir,
            "--no-first-run",
            "--disable-extensions",
            "about:blank",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.ws = None
        self.next_id = 0

    def __enter__(self):
        url = self._target()
        self.ws = websocket.create_connection(url, timeout=90)
        return self

    def __exit__(self, *a):
        try:
            if self.ws:
                self.ws.close()
        finally:
            self.proc.kill()

    def _target(self):
        """The page target's websocket URL, once Chrome is listening."""
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                raw = urllib.request.urlopen(
                    "http://127.0.0.1:%d/json/list" % self.port, timeout=2).read()
                for t in json.loads(raw):
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        return t["webSocketDebuggerUrl"]
            except Exception:
                pass
            time.sleep(0.2)
        raise RuntimeError("Chrome never opened its DevTools port")

    def send(self, method, **params):
        self.next_id += 1
        mid = self.next_id
        self.ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError("%s: %s" % (method, msg["error"]))
                return msg.get("result", {})

    def wait_event(self, name, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.ws.settimeout(max(0.5, deadline - time.time()))
            try:
                msg = json.loads(self.ws.recv())
            except Exception:
                return False
            if msg.get("method") == name:
                return True
        return False


def shoot_pixel(chrome_exe, url, width, height, wait_ms, restyle=True):
    """(PNG bytes, links) with the page typeset in the CardOS font.

    Each link is {x, y, w, h, href} in page pixels, which at this viewport are
    also panel pixels -- the device does no scaling, so a rectangle here is a
    rectangle there."""
    font_b64 = base64.b64encode(open(ensure_font(), "rb").read()).decode()
    js = SNAP_JS.replace("__FONT__", font_b64).replace("__W__", str(width))
    # Photo mode wants the page in its own fonts, but it wants the links just
    # as much -- a page you cannot click is a picture of a page. So the same
    # script runs either way, with the restyle half switched off.
    if not restyle:
        js = js.replace("for (const el of document.querySelectorAll('*')) {",
                        "for (const el of []) {")

    with Chrome(chrome_exe, width, height) as c:
        c.send("Page.enable")
        # Without this a site's own CSP refuses the injected font, which is
        # the whole rendering.
        c.send("Page.setBypassCSP", enabled=True)
        # mobile=False on purpose: with it set, Chrome honours the page's own
        # meta viewport, and a desktop page asking for width=1000 then lays out
        # at 1000 however small the window is. False makes the CSS viewport
        # exactly the panel, and a responsive site lays out for 240.
        #
        # The user agent is left alone for the same reason: sites that sniff
        # for a phone serve a layout built around tapping -- sections collapsed
        # behind headers, infoboxes that overflow a 240px viewport -- and this
        # has a scroll wheel, not a thumb. The desktop HTML at a narrow
        # viewport reads better.
        c.send("Emulation.setDeviceMetricsOverride",
               width=width, height=height, deviceScaleFactor=1, mobile=False)
        c.send("Page.navigate", url=url)
        c.wait_event("Page.loadEventFired", wait_ms / 1000.0)
        # Even after load, a page with scripts is still settling; this is the
        # same grace the --virtual-time-budget flag bought before.
        time.sleep(min(3.0, wait_ms / 3000.0))

        got = c.send("Runtime.evaluate", expression=js, awaitPromise=True,
                     returnByValue=True)
        links = (got.get("result", {}).get("value") or {}).get("links") or []

        m = c.send("Page.getLayoutMetrics")
        size = m.get("cssContentSize") or m.get("contentSize")
        h = int(min(size["height"], height))
        w = int(min(size["width"], width))

        shot = c.send("Page.captureScreenshot", format="png",
                      captureBeyondViewport=True,
                      clip={"x": 0, "y": 0, "width": w, "height": h, "scale": 1})
        return base64.b64decode(shot["data"]), links
