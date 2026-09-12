"""Turn CardOS's 6x8 bitmap font into a TTF, so Chrome can lay out a page in it.

The point is not nostalgia. The device panel is 240 pixels wide; a page
screenshot taken at a wider viewport and scaled down turns antialiased 12-pixel
text into four pixels of grey. A bitmap font does not have that failure mode --
if it is laid out at its native size, on the pixel grid, it is exactly as
readable in the shot as it is on the device.

So the same table the console draws from is emitted as outlines: one square per
lit pixel, one pixel = 100 font units, 800 units to the em. At `font-size: 8px`
one font unit-square lands on one CSS pixel exactly, every edge is on an integer
boundary, and Chrome's rasteriser has nothing to antialias.
"""

import os
import re
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_H_PATH = os.path.join(HERE, "..", "kernel", "console", "font6x8.h")
CACHE = os.path.join(HERE, "cardos6x8.ttf")

UPM = 800          # 8 rows to the em
PX = 100           # one font pixel
ASCENT = 700       # rows 0..6 sit above the baseline, row 7 is the descender
DESCENT = 100
ADVANCE = 6 * PX

FIRST, LAST = 32, 126


def read_table(path=FONT_H_PATH):
    """The glyph table, straight out of the header the console compiles in."""
    src = open(path, encoding="utf-8").read()
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){6})\}", src)
    table = [[int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", r)] for r in rows]
    if len(table) != LAST - FIRST + 1:
        raise SystemExit("font table is %d glyphs, expected %d"
                         % (len(table), LAST - FIRST + 1))
    return table


def glyph_outline(cols):
    """One square per lit pixel.

    Merging adjacent pixels into bigger rectangles would make a smaller file and
    no difference to the raster, and a wrong merge is invisible until a glyph is
    subtly wrong -- which is the exact bug this font already survived once."""
    pen = TTGlyphPen(None)
    for x, bits in enumerate(cols):
        for y in range(8):
            if not (bits >> y) & 1:
                continue
            x0, y1 = x * PX, ASCENT - y * PX
            x1, y0 = x0 + PX, y1 - PX
            pen.moveTo((x0, y0))
            pen.lineTo((x0, y1))
            pen.lineTo((x1, y1))
            pen.lineTo((x1, y0))
            pen.closePath()
    return pen.glyph()


def build(out=CACHE):
    table = read_table()

    names = {}
    order = [".notdef"]
    for code in range(FIRST, LAST + 1):
        n = "uni%04X" % code
        names[code] = n
        order.append(n)

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(names)

    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    metrics = {".notdef": (ADVANCE, 0)}
    for code in range(FIRST, LAST + 1):
        g = names[code]
        glyphs[g] = glyph_outline(table[code - FIRST])
        metrics[g] = (ADVANCE, 0)

    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ASCENT, descent=-DESCENT)
    fb.setupNameTable({
        "familyName": "CardOS 6x8",
        "styleName": "Regular",
        "psName": "CardOS6x8-Regular",
        "fullName": "CardOS 6x8",
        "version": "1.0",
    })
    fb.setupOS2(sTypoAscender=ASCENT, sTypoDescender=-DESCENT,
                usWinAscent=ASCENT, usWinDescent=DESCENT,
                achVendID="CDOS")
    fb.setupPost()
    fb.save(out)
    return out


def ensure(out=CACHE):
    """Rebuild whenever the header is newer, so editing the font is enough."""
    if (not os.path.exists(out) or
            os.path.getmtime(out) < os.path.getmtime(FONT_H_PATH)):
        build(out)
    return out


if __name__ == "__main__":
    p = build(sys.argv[1] if len(sys.argv) > 1 else CACHE)
    print("%s  %d bytes" % (p, os.path.getsize(p)))
