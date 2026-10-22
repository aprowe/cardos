"""Hangman's picture: an old photograph of a gallows, and a man drawn for it.

The photograph is tools/hangman/gallows.jpg -- the gallows on the Western
film set at Fort Bravo, Tabernas, photographed in 2024 by Melenn14 and
given to the public domain (CC0):
https://commons.wikimedia.org/wiki/File:Fort_Bravo_gallows_(Tabernas_-_Texas_Hollywood)_(2024-04-27).jpg
No one is on it. This script makes it look 1880: cropped to Hangman's left
panel, toned sepia, vignetted, scratched and grained, framed like a card
photograph, and dithered to fifteen tones.

The man is not a photograph of anyone. He is drawn here out of shaded shapes
-- hat, head, coat, arms, trousers, boots -- at eight times the size and
brought down, then put through the same tones and grain so he sits in the
picture. He comes in pieces, so the game can add one a miss: legs, coat,
arms (hanging and raised), head (and the head fallen to one side).

    python tools/make_hangman.py                  writes apps/hangman/hangman_art.h
    python tools/make_hangman.py --preview F.png  and draws every pose, 4x

Index 0 of the palette is "nothing here" in a sprite; the photograph uses
1-15. A sprite's position is from the man's neck, which the game puts at the
noose (HA_HANG_*) or, cut down, on the platform (HA_STAND_*).
"""
import argparse
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageOps

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "hangman", "gallows.jpg")
OUT = os.path.join(ROOT, "apps", "hangman", "hangman_art.h")

W, H = 92, 124                       # the panel left of the word
CROP = (75, 30, 470, 562)            # in gallows.jpg: beam to below the platform
NOOSE = (44, 77)                     # the man's neck, hanging (panel pixels)
ROPE_TOP = (43, 25)                  # where the rope leaves the beam
STAND = (68, 51)                     # his neck, standing on the platform
SS = 8                               # the man is drawn this much bigger

# Sepia, dark to light: index 1-15.
RAMP_DARK = (30, 20, 13)
RAMP_MID = (128, 94, 62)
RAMP_LIGHT = (238, 222, 188)


def ramp():
    out = [(0, 0, 0)]
    for i in range(15):
        t = i / 14
        if t < 0.5:
            a, b, u = RAMP_DARK, RAMP_MID, t / 0.5
        else:
            a, b, u = RAMP_MID, RAMP_LIGHT, (t - 0.5) / 0.5
        out.append(tuple(round(a[k] + (b[k] - a[k]) * u) for k in range(3)))
    return out


def dither(grey, mask=None):
    """A greyscale image (0-255) to indices 1-15, Floyd-Steinberg: the grain
    is in the dither as much as in the noise. Where `mask` is 0, index 0."""
    w, h = grey.size
    px = [[float(grey.getpixel((x, y))) for x in range(w)] for y in range(h)]
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if mask is not None and mask.getpixel((x, y)) < 128:
                continue
            v = min(255.0, max(0.0, px[y][x]))
            q = round(v / 255 * 14)
            out[y][x] = q + 1
            e = v - q * 255 / 14
            for dx, dy, f in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and yy < h and (mask is None or mask.getpixel((xx, yy)) >= 128):
                    px[yy][xx] += e * f / 16
    return out


def grain(img, rng, sigma):
    w, h = img.size
    for y in range(h):
        for x in range(w):
            img.putpixel((x, y), max(0, min(255, round(img.getpixel((x, y)) + rng.gauss(0, sigma)))))
    return img


def photograph(rng):
    im = Image.open(SRC).convert("L").crop(CROP)
    im = im.resize((W * 4, H * 4), Image.LANCZOS)
    im = ImageOps.autocontrast(im, cutoff=1)
    # Old emulsion: the sky blown out, the blacks lifted, a little soft.
    im = im.point(lambda v: round(26 + (v / 255) ** 0.85 * 214))
    im = im.filter(ImageFilter.GaussianBlur(1.0))
    d = ImageDraw.Draw(im)
    # The rope, which is a thread at this size: drawn over the photograph's
    # own, beam to noose, and the loop below the knot.
    d.line([(ROPE_TOP[0] * 4, ROPE_TOP[1] * 4), (NOOSE[0] * 4, (NOOSE[1] - 6) * 4)], fill=48, width=4)
    d.ellipse([(NOOSE[0] - 2.6) * 4, (NOOSE[1] - 7) * 4, (NOOSE[0] + 2.6) * 4, (NOOSE[1] + 1) * 4], outline=48, width=4)
    # Scratches and specks in the plate.
    for _ in range(5):
        x = rng.uniform(0, W * 4)
        y0 = rng.uniform(0, H * 4 * 0.6)
        y1 = y0 + rng.uniform(40, H * 4 * 0.6)
        d.line([(x, y0), (x + rng.uniform(-6, 6), y1)], fill=rng.choice((235, 245, 60)), width=1)
    for _ in range(26):
        x, y, r = rng.uniform(0, W * 4), rng.uniform(0, H * 4), rng.uniform(0.6, 2.2)
        d.ellipse([x - r, y - r, x + r, y + r], fill=rng.choice((20, 40, 240)))
    im = im.resize((W, H), Image.LANCZOS)
    # The vignette: the corners go the way old lenses and old silver do.
    for y in range(H):
        for x in range(W):
            dx, dy = (x - W / 2) / (W / 2), (y - H / 2) / (H / 2)
            r = math.hypot(dx * 0.95, dy * 0.85)
            k = 1 - max(0, r - 0.55) * 0.95
            im.putpixel((x, y), round(im.getpixel((x, y)) * max(0.15, k)))
    grain(im, rng, 6)
    idx = dither(im)
    # The card it is mounted on: a cream edge, darker at the corners.
    for y in range(H):
        for x in range(W):
            edge = min(x, y, W - 1 - x, H - 1 - y)
            corner = min(math.hypot(x - cx, y - cy) for cx in (0, W - 1) for cy in (0, H - 1))
            if corner < 4:
                idx[y][x] = 2
            elif edge == 0:
                idx[y][x] = 9
            elif edge == 1:
                idx[y][x] = 13
    return idx


# ---- the man ----------------------------------------------------------------
#
# Drawn in panel pixels from his neck (0, 0), scaled by SS. Greys: the coat is
# dark, the shirt light, the skin between. Each piece is a light side and a
# shadow side, so he reads as round at 50 pixels tall.

COAT, COAT_HI, SHIRT, SKIN, SKIN_HI, TROUSER, TROUSER_HI, BOOT, HAT, HAT_HI, ROPE = (
    70, 108, 204, 156, 188, 92, 120, 34, 50, 86, 150)


class Canvas:
    def __init__(self, box):
        self.box = box                                   # x0, y0, x1, y1, panel px
        w, h = (box[2] - box[0]) * SS, (box[3] - box[1]) * SS
        self.grey = Image.new("L", (w, h), 0)
        self.alpha = Image.new("L", (w, h), 0)
        self.g = ImageDraw.Draw(self.grey)
        self.a = ImageDraw.Draw(self.alpha)

    def p(self, pts):
        return [((x - self.box[0]) * SS, (y - self.box[1]) * SS) for x, y in pts]

    def poly(self, pts, v):
        q = self.p(pts)
        self.g.polygon(q, fill=v)
        self.a.polygon(q, fill=255)

    def ellipse(self, cx, cy, rx, ry, v):
        (x0, y0), (x1, y1) = self.p([(cx - rx, cy - ry), (cx + rx, cy + ry)])
        self.g.ellipse([x0, y0, x1, y1], fill=v)
        self.a.ellipse([x0, y0, x1, y1], fill=255)

    def limb(self, a, b, w0, w1, v, hi):
        """A tapered limb from a to b, its left half lit."""
        ang = math.atan2(b[1] - a[1], b[0] - a[0])
        nx, ny = -math.sin(ang), math.cos(ang)
        l0 = (a[0] + nx * w0 / 2, a[1] + ny * w0 / 2)
        r0 = (a[0] - nx * w0 / 2, a[1] - ny * w0 / 2)
        l1 = (b[0] + nx * w1 / 2, b[1] + ny * w1 / 2)
        r1 = (b[0] - nx * w1 / 2, b[1] - ny * w1 / 2)
        self.poly([l0, l1, r1, r0], v)
        lit = (l0, l1) if l0[0] < r0[0] else (r0, r1)
        m0 = (a[0] * 0.55 + lit[0][0] * 0.45, a[1] * 0.55 + lit[0][1] * 0.45)
        m1 = (b[0] * 0.55 + lit[1][0] * 0.45, b[1] * 0.55 + lit[1][1] * 0.45)
        self.poly([lit[0], lit[1], m1, m0], hi)
        self.ellipse(a[0], a[1], w0 / 2, w0 / 2, v)


def rotate(pts, deg, about):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [(about[0] + (x - about[0]) * c - (y - about[1]) * s,
             about[1] + (x - about[0]) * s + (y - about[1]) * c) for x, y in pts]


def draw_head(c, tilt, noose):
    o = (0, 0)
    def R(pts):
        return rotate(pts, tilt, o)
    def E(cx, cy, rx, ry, v):
        (x, y), = R([(cx, cy)])
        c.ellipse(x, y, rx, ry, v)
    c.poly(R([(-1.6, -2), (1.6, -2), (1.8, 1), (-1.8, 1)]), SKIN)       # the neck
    E(0, -5.2, 3.4, 4.2, SKIN)                                             # the head
    E(-0.9, -5.6, 2.0, 3.0, SKIN_HI)
    c.poly(R([(-3.4, -3.6), (3.4, -3.6), (2.4, -1.4), (0, -0.9), (-2.4, -1.4)]), COAT)  # beard
    c.poly(R([(-3.2, -6.4), (-0.6, -6.6), (-0.6, -6.0), (-3.0, -5.8)]), 70)    # brow
    c.poly(R([(0.6, -6.6), (3.2, -6.4), (3.0, -5.8), (0.6, -6.0)]), 70)
    if tilt:
        c.poly(R([(-2.6, -5.3), (-0.9, -5.3), (-0.9, -4.9), (-2.6, -4.9)]), 60)   # eyes shut
        c.poly(R([(0.9, -5.3), (2.6, -5.3), (2.6, -4.9), (0.9, -4.9)]), 60)
    else:
        E(-1.6, -5.0, 0.6, 0.5, 40)
        E(1.6, -5.0, 0.6, 0.5, 40)
    c.poly(R([(-5.6, -8.6), (5.6, -8.6), (5.0, -7.4), (-5.0, -7.4)]), HAT)   # brim
    c.poly(R([(-3.4, -8.4), (3.4, -8.4), (3.0, -12.4), (0, -11.6), (-3.0, -12.4)]), HAT)  # crown
    c.poly(R([(-3.4, -8.4), (-0.8, -8.4), (-0.6, -11.9), (-3.0, -12.4)]), HAT_HI)
    c.poly(R([(-3.4, -9.4), (3.4, -9.4), (3.4, -8.4), (-3.4, -8.4)]), 24)   # band
    if noose:
        c.poly(R([(-2.0, -1.4), (2.0, -1.4), (2.2, 0.2), (-2.2, 0.2)]), ROPE)   # the rope
        c.poly(R([(1.2, -3.0), (3.4, -2.6), (3.2, 0.4), (1.4, 0.2)]), ROPE - 30)  # the knot


def draw_torso(c):
    c.poly([(-6.6, 1.2), (6.6, 1.2), (5.8, 14), (6.2, 23), (-6.2, 23), (-5.8, 14)], COAT)
    c.poly([(-6.6, 1.2), (-1, 1.2), (-1, 23), (-6.2, 23), (-5.8, 14)], COAT_HI)
    c.poly([(-2.2, 1.0), (2.2, 1.0), (0, 8)], SHIRT)                    # shirt, a V
    c.poly([(-0.7, 1.4), (0.7, 1.4), (0.4, 6.4), (-0.4, 6.4)], 40)       # string tie
    c.poly([(-2.2, 1.0), (-0.3, 8), (-1.2, 9)], 30)                      # lapels
    c.poly([(2.2, 1.0), (0.3, 8), (1.2, 9)], 30)
    c.poly([(-0.25, 9), (0.25, 9), (0.25, 23), (-0.25, 23)], 30)         # the opening
    for y in (11, 14, 17):
        c.ellipse(-0.9, y, 0.45, 0.45, 150)                              # buttons


def draw_arm(c, side, up):
    s = -1 if side == "l" else 1
    sh = (s * 5.4, 2.8)
    if up:
        el = (s * 9.0, -3.5)
        hand = (s * 10.0, -10.5)
    else:
        el = (s * 6.6, 10.5)
        hand = (s * 6.4, 18.5)
    c.limb(sh, el, 3.6, 3.2, COAT, COAT_HI)
    c.limb(el, hand, 3.2, 2.8, COAT, COAT_HI)
    c.ellipse(hand[0], hand[1] + (-1.2 if up else 1.2), 1.4, 1.7, SKIN)


def draw_leg(c, side, stand):
    s = -1 if side == "l" else 1
    hip = (s * 2.6, 22)
    knee = (s * 2.9, 32)
    ankle = (s * (3.2 if stand else 2.8), 41)
    c.limb(hip, knee, 4.4, 3.8, TROUSER, TROUSER_HI)
    c.limb(knee, ankle, 3.8, 3.2, TROUSER, TROUSER_HI)
    if stand:
        c.poly([(ankle[0] - 1.8, ankle[1] - 1), (ankle[0] + 1.8, ankle[1] - 1),
                (ankle[0] + 1.8 + s * 2.2, ankle[1] + 2.2), (ankle[0] - 1.8 + s * 0.2, ankle[1] + 2.2)], BOOT)
    else:                                           # hanging: the toes droop
        c.poly([(ankle[0] - 1.7, ankle[1] - 1), (ankle[0] + 1.7, ankle[1] - 1),
                (ankle[0] + 1.2 + s * 0.8, ankle[1] + 4.0), (ankle[0] - 1.2 + s * 0.8, ankle[1] + 4.0)], BOOT)


# name, bounding box from the neck, how to draw it
PIECES = [
    ("LEG_L", (-7, 18, 2, 46), lambda c: draw_leg(c, "l", False)),
    ("LEG_R", (-2, 18, 7, 46), lambda c: draw_leg(c, "r", False)),
    ("STAND_L", (-8, 18, 2, 45), lambda c: draw_leg(c, "l", True)),
    ("STAND_R", (-2, 18, 8, 45), lambda c: draw_leg(c, "r", True)),
    ("TORSO", (-7, 0, 7, 24), draw_torso),
    ("ARM_L", (-9, 0, -3, 21), lambda c: draw_arm(c, "l", False)),
    ("ARM_R", (3, 0, 9, 21), lambda c: draw_arm(c, "r", False)),
    ("UP_L", (-12, -13, -3, 5), lambda c: draw_arm(c, "l", True)),
    ("UP_R", (3, -13, 12, 5), lambda c: draw_arm(c, "r", True)),
    ("HEAD", (-6, -13, 6, 2), lambda c: draw_head(c, 0, True)),
    ("HEAD_DEAD", (-6, -14, 10, 3), lambda c: draw_head(c, 28, True)),
    ("HEAD_FREE", (-6, -13, 6, 2), lambda c: draw_head(c, 0, False)),
]


def piece(rng, box, fn):
    c = Canvas(box)
    fn(c)
    w, h = box[2] - box[0], box[3] - box[1]
    grey = c.grey.resize((w, h), Image.BOX)
    alpha = c.alpha.resize((w, h), Image.BOX)
    # The coverage is a mask: what is mostly him is him, at the tone of his
    # shapes rather than the background blended in.
    for y in range(h):
        for x in range(w):
            a = alpha.getpixel((x, y))
            if a:
                grey.putpixel((x, y), round(grey.getpixel((x, y)) * 255 / a))
    grain(grey, rng, 6)
    mask = alpha.point(lambda a: 255 if a >= 110 else 0)
    return dither(grey, mask)


def pack(rows):
    out = []
    for row in rows:
        for i in range(0, len(row), 2):
            hi = row[i]
            lo = row[i + 1] if i + 1 < len(row) else 0
            out.append(hi << 4 | lo)
    return out


def hexlines(data, per=24):
    return "\n".join("  " + "".join("0x%02x," % b for b in data[i:i + per]) for i in range(0, len(data), per))


def build():
    rng = random.Random(1887)
    pal = ramp()
    bg = photograph(rng)
    pieces = [(n, box, piece(rng, box, fn)) for n, box, fn in PIECES]
    return pal, bg, pieces


def write_header(pal, bg, pieces):
    lines = [
        "/* Generated by tools/make_hangman.py -- do not edit. The photograph is the",
        " * gallows on the Fort Bravo film set (CC0, Melenn14, Wikimedia Commons);",
        " * the man is drawn by the script. */",
        "#ifndef HANGMAN_ART_H",
        "#define HANGMAN_ART_H",
        "",
        "#define HA_W %d" % W,
        "#define HA_H %d" % H,
        "#define HA_HANG_X %d" % NOOSE[0],
        "#define HA_HANG_Y %d" % NOOSE[1],
        "#define HA_STAND_X %d" % STAND[0],
        "#define HA_STAND_Y %d" % STAND[1],
        "",
        "static const uint16_t ha_pal[16] = {",
        "  " + ", ".join("CAPP_RGB(%d, %d, %d)" % c for c in pal[:4]) + ",",
    ]
    for i in range(4, 16, 4):
        lines.append("  " + ", ".join("CAPP_RGB(%d, %d, %d)" % c for c in pal[i:i + 4]) + ",")
    lines += ["};", "", "/* The photograph, two pixels a byte, the left one high. */",
              "static const uint8_t ha_photo[HA_W / 2 * HA_H] = {", hexlines(pack(bg)), "};", ""]
    lines.append("enum {")
    lines.append("  " + ", ".join("HA_" + n for n, _, _ in pieces) + ", HA_PIECES")
    lines.append("};")
    lines.append("")
    lines.append("/* A piece of him: where it is from his neck, its size, where its pixels")
    lines.append(" * start in ha_px (rows of (w + 1) / 2 bytes; 0 is not him). */")
    lines.append("typedef struct { int8_t dx, dy; uint8_t w, h; uint16_t at; } HaPiece;")
    lines.append("")
    data, table = [], []
    for n, box, rows in pieces:
        table.append("  { %d, %d, %d, %d, %d },  /* %s */" % (box[0], box[1], box[2] - box[0], box[3] - box[1], len(data), n))
        data += pack(rows)
    lines += ["static const HaPiece ha_piece[HA_PIECES] = {"] + table + ["};", ""]
    lines += ["static const uint8_t ha_px[%d] = {" % len(data), hexlines(data), "};", "", "#endif", ""]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s: photo %d bytes, man %d bytes" % (os.path.relpath(OUT, ROOT), len(pack(bg)), len(data)))


POSES = {
    "hang": (NOOSE, ["LEG_L", "LEG_R", "TORSO", "ARM_L", "ARM_R", "HEAD"]),
    "dead": (NOOSE, ["LEG_L", "LEG_R", "TORSO", "ARM_L", "ARM_R", "HEAD_DEAD"]),
    "free": (STAND, ["STAND_L", "STAND_R", "TORSO", "UP_L", "UP_R", "HEAD_FREE"]),
    "empty": (NOOSE, []),
}


def preview(path, pal, bg, pieces):
    by = {n: (box, rows) for n, box, rows in pieces}
    shots = []
    for name, (at, names) in POSES.items():
        im = Image.new("RGB", (W, H))
        for y in range(H):
            for x in range(W):
                im.putpixel((x, y), pal[bg[y][x]])
        for n in names:
            box, rows = by[n]
            for y, row in enumerate(rows):
                for x, v in enumerate(row):
                    px, py = at[0] + box[0] + x, at[1] + box[1] + y
                    if v and 0 <= px < W and 0 <= py < H:
                        im.putpixel((px, py), pal[v])
        shots.append(im)
    sheet = Image.new("RGB", (len(shots) * (W + 4), H), (0, 0, 0))
    for i, s in enumerate(shots):
        sheet.paste(s, (i * (W + 4), 0))
    sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(path)
    print("wrote", path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    args = ap.parse_args()
    pal, bg, pieces = build()
    write_header(pal, bg, pieces)
    if args.preview:
        preview(args.preview, pal, bg, pieces)


if __name__ == "__main__":
    main()
