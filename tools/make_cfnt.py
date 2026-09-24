#!/usr/bin/env python3
"""Turn a TrueType font into a .cfnt: a bitmap font the device can draw.

The device has no TrueType rasteriser and should not grow one -- FreeType is
larger than the kernel and slower than the screen. So fonts are rendered here,
once, at the size an app asked for, and the device only copies pixels. The
same reason every small-screen library (LVGL, Adafruit GFX) converts fonts
ahead of time.

    python tools/make_cfnt.py fonts/src/X.ttf 44 fonts/clock44.cfnt --chars 0123456789: --tabular
    python tools/make_cfnt.py fonts/src/X.ttf 24 fonts/body24.cfnt --bpp 1
    python tools/make_cfnt.py fonts/clock44.cfnt --show out.png --text 12:34
    python tools/make_cfnt.py --all      every font in fonts/fonts.txt

--chars picks what goes in (default printable ASCII); a clock needs eleven
glyphs, not ninety-five. --tabular gives every digit the widest digit's
advance, so a running time does not shuffle sideways as it counts. --bpp 4 is
sixteen levels of coverage, blended on the device against the background the
text is drawn on; --bpp 1 is for the thermal printer, which has two colours.
--tight makes the line only as tall as the ink, for a clock's digits.
--show decodes a .cfnt back into a picture, which is the check that the file
says what was meant.

The format, little-endian, read by kernel/ui/cfont.c:

    0   "CFNT"
    4   u8  version (1)
    5   u8  bits per pixel (1 or 4)
    6   u8  line height
    7   u8  ascent: the baseline, in rows from the top of the line
    8   u16 first character
    10  u16 count
    12  u32 reserved
    16  count x 10-byte glyphs, first..first+count-1 in order:
          u32 offset into the bitmaps   u8 w  u8 h
          i8  x offset from the pen     i8 y offset from the line's top
          u8  advance                    u8 present (0: not in the font)
    ..  bitmaps: each glyph's rows back to back, starting on a byte, MSB
        (or high nibble) first; 4bpp is coverage 0..15.
"""

import argparse
import os
import shlex
import struct
import sys

from PIL import Image, ImageFont

MAGIC = b"CFNT"
HEADER = struct.Struct("<4sBBBBHHI")
GLYPH = struct.Struct("<IBBbbBB")


def render(ttf, size, chars, bpp, tabular, leading, tight=False):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    height = ascent + descent + leading
    codes = sorted(set(ord(c) for c in chars))
    first, last = codes[0], codes[-1]
    if first < 32 or last > 255:
        raise SystemExit("characters must be in 32..255")

    glyphs = {}
    for code in codes:
        ch = chr(code)
        mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
        w, h = mask.size
        adv = int(round(font.getlength(ch)))
        px = [mask.getpixel((x, y)) for y in range(h) for x in range(w)]
        glyphs[code] = dict(w=w, h=h, x=ox, y=ascent + oy, adv=adv, px=px)

    if tabular:
        digits = [glyphs[c] for c in range(ord("0"), ord("9") + 1) if c in glyphs]
        if digits:
            widest = max(g["adv"] for g in digits)
            for g in digits:
                g["x"] += (widest - g["adv"]) // 2
                g["adv"] = widest

    if tight:
        # The line is the ink of these glyphs and nothing else: a clock face
        # made of digits has no descenders and no accents to leave room for,
        # and the font's own line height is mostly that room.
        inked = [g for g in glyphs.values() if g["h"]]
        top = min(g["y"] for g in inked)
        bottom = max(g["y"] + g["h"] for g in inked)
        for g in glyphs.values():
            g["y"] -= top
        ascent -= top
        height = bottom - top + leading

    return dict(height=height, ascent=ascent, first=first, count=last - first + 1,
                bpp=bpp, glyphs=glyphs)


def pack(px, w, h, bpp):
    out = bytearray()
    if bpp == 4:
        vals = [min(15, (p * 15 + 127) // 255) for p in px]
        for i in range(0, len(vals), 2):
            hi = vals[i]
            lo = vals[i + 1] if i + 1 < len(vals) else 0
            out.append((hi << 4) | lo)
    else:
        bits = [1 if p >= 128 else 0 for p in px]
        for i in range(0, len(bits), 8):
            b = 0
            for j, bit in enumerate(bits[i:i + 8]):
                b |= bit << (7 - j)
            out.append(b)
    return bytes(out)


def encode(f):
    table, bitmaps = bytearray(), bytearray()
    for i in range(f["count"]):
        g = f["glyphs"].get(f["first"] + i)
        if g is None:
            table += GLYPH.pack(0, 0, 0, 0, 0, 0, 0)
            continue
        for k, lim in (("w", 255), ("h", 255), ("adv", 255)):
            if not 0 <= g[k] <= lim:
                raise SystemExit("glyph %r: %s %d out of range" % (chr(f["first"] + i), k, g[k]))
        if not (-128 <= g["x"] <= 127 and -128 <= g["y"] <= 127):
            raise SystemExit("glyph %r: offset out of range" % chr(f["first"] + i))
        table += GLYPH.pack(len(bitmaps), g["w"], g["h"], g["x"], g["y"], g["adv"], 1)
        bitmaps += pack(g["px"], g["w"], g["h"], f["bpp"])
    if f["height"] > 255 or f["ascent"] > 255:
        raise SystemExit("too tall for the format")
    head = HEADER.pack(MAGIC, 1, f["bpp"], f["height"], f["ascent"],
                       f["first"], f["count"], 0)
    return head + bytes(table) + bytes(bitmaps)


def decode(data):
    """The file back into glyphs -- the same reading kernel/ui/cfont.c does."""
    magic, ver, bpp, height, ascent, first, count, _ = HEADER.unpack_from(data, 0)
    if magic != MAGIC or ver != 1 or bpp not in (1, 4):
        raise SystemExit("not a version-1 .cfnt")
    base = HEADER.size + count * GLYPH.size
    glyphs = {}
    for i in range(count):
        off, w, h, x, y, adv, present = GLYPH.unpack_from(data, HEADER.size + i * GLYPH.size)
        if not present:
            continue
        px = []
        for n in range(w * h):
            if bpp == 4:
                b = data[base + off + n // 2]
                v = (b >> 4) if n % 2 == 0 else (b & 15)
                px.append(v * 17)
            else:
                b = data[base + off + n // 8]
                px.append(255 if (b >> (7 - n % 8)) & 1 else 0)
        glyphs[first + i] = dict(w=w, h=h, x=x, y=y, adv=adv, px=px)
    return dict(height=height, ascent=ascent, bpp=bpp, glyphs=glyphs)


def show(data, text, out):
    f = decode(data)
    width = sum(f["glyphs"].get(ord(c), {"adv": 0})["adv"] for c in text) + 8
    img = Image.new("L", (max(width, 8), f["height"] + 8), 0)
    pen = 4
    for c in text:
        g = f["glyphs"].get(ord(c))
        if not g:
            continue
        for yy in range(g["h"]):
            for xx in range(g["w"]):
                v = g["px"][yy * g["w"] + xx]
                if v:
                    img.putpixel((pen + g["x"] + xx, 4 + g["y"] + yy), v)
        pen += g["adv"]
    img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
    img.save(out)
    print("  %s: %r, line %d px, baseline %d, %d-bit" % (out, text, f["height"],
                                                          f["ascent"], f["bpp"]))


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "fonts")
BLOBS = os.path.join(ROOT, "kernel", "ui", "font_blobs.h")


def build_all():
    """Every line of fonts/fonts.txt into fonts/NAME.cfnt, then all of them
    into kernel/ui/font_blobs.h for the firmware to seed the card with."""
    built = []
    with open(os.path.join(FONTS, "fonts.txt"), encoding="utf-8") as fh:
        for line in fh:
            words = shlex.split(line.split("#", 1)[0])
            if not words:
                continue
            if len(words) < 3:
                raise SystemExit("fonts.txt: %r needs NAME SOURCE SIZE" % line.strip())
            name, src, size, opts = words[0], words[1], int(words[2]), words[3:]
            a = parser().parse_args([os.path.join(FONTS, "src", src), str(size),
                                     os.path.join(FONTS, name + ".cfnt")] + opts)
            f = render(a.src, a.size, a.chars, a.bpp, a.tabular, a.leading, a.tight)
            data = encode(f)
            with open(a.out, "wb") as out:
                out.write(data)
            print("  %-10s %2d px line, %d-bit, %5d bytes" % (name, f["height"], a.bpp, len(data)))
            built.append((name, data))

    lines = ["/* Generated by tools/make_cfnt.py --all from fonts/fonts.txt -- do not edit.",
             " *",
             " * The fonts, embedded so the firmware can write them to /fonts on the",
             " * card (icons.c), the way it seeds the apps. */",
             "#ifndef CARDOS_FONT_BLOBS_H", "#define CARDOS_FONT_BLOBS_H", "",
             "#include <stddef.h>", "#include <stdint.h>", ""]
    for name, data in built:
        lines.append("static const uint8_t font_blob_%s[] = {" % name)
        for i in range(0, len(data), 16):
            lines.append("  " + " ".join("0x%02X," % b for b in data[i:i + 16]))
        lines.append("};")
    lines += ["", "typedef struct {", "  const char    *name;      /* NAME.cfnt in /fonts */",
              "  const uint8_t *data;", "  size_t         size;", "} FontBlob;", "",
              "static const FontBlob FONT_BLOBS[] = {"]
    for name, data in built:
        lines.append('  { "%s.cfnt", font_blob_%s, sizeof font_blob_%s },' % (name, name, name))
    lines += ["};", "#define FONT_BLOB_COUNT (sizeof FONT_BLOBS / sizeof FONT_BLOBS[0])", "",
              "#endif", ""]
    with open(BLOBS, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines))
    print("  %s (%d bytes of flash)" % (os.path.relpath(BLOBS, ROOT),
                                        sum(len(d) for _, d in built)))


def parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", nargs="?")
    ap.add_argument("size", nargs="?", type=int)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--chars", default="".join(chr(c) for c in range(32, 127)))
    ap.add_argument("--bpp", type=int, choices=(1, 4), default=4)
    ap.add_argument("--tabular", action="store_true",
                    help="every digit as wide as the widest")
    ap.add_argument("--leading", type=int, default=0, help="extra rows under each line")
    ap.add_argument("--tight", action="store_true",
                    help="line height is the ink of the included glyphs")
    ap.add_argument("--show", metavar="PNG", help="decode SRC (a .cfnt) into a picture")
    ap.add_argument("--text", default="0123456789", help="what --show draws")
    ap.add_argument("--all", action="store_true", help="build every font in fonts/fonts.txt")
    return ap


def main():
    ap = parser()
    a = ap.parse_args()
    if a.all:
        build_all()
        return

    if a.show:
        with open(a.src, "rb") as fh:
            show(fh.read(), a.text, a.show)
        return
    if not a.src or not a.size or not a.out:
        ap.error("SRC SIZE OUT, SRC.cfnt --show PNG [--text T], or --all")

    f = render(a.src, a.size, a.chars, a.bpp, a.tabular, a.leading, a.tight)
    data = encode(f)
    with open(a.out, "wb") as fh:
        fh.write(data)
    print("  %s: %d glyphs, line %d px, %d-bit, %d bytes" % (
        a.out, len(f["glyphs"]), f["height"], a.bpp, len(data)))


if __name__ == "__main__":
    main()
