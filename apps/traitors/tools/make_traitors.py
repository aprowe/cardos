#!/usr/bin/env python3
"""Traitors: The Game's pictures: seven contestants and the little icons, as
pixel art.

    python apps/traitors/tools/make_traitors.py                 writes apps/traitors/traitors_art.h
    python apps/traitors/tools/make_traitors.py --preview F.png  draws everything big, to look at

The contestants are people you would see on the show: a first name, an age
and a job (TA_BIO), and a portrait drawn here from a few choices -- skin,
hair, what they wear, glasses or a beard or a scarf -- over one face,
outlined where it meets the dark. Portraits are 20x20, 4 bits a pixel on
their own palette (FACE_PAL); the 9x9 icons are drawn by hand below on
another (ICON_PAL). Both palettes are RGB565 in the panel's byte order, as
CAPP_RGB makes it, so the app hands them to api->pixels unchanged.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUT = os.path.join(ROOT, "apps", "traitors", "traitors_art.h")

FACE_PAL = [
    (14, 11, 20),     # 0  the dark behind a portrait
    (5, 4, 8),        # 1  outline
    (234, 204, 182),  # 2  light skin
    (190, 152, 132),  # 3  light skin, shadow
    (34, 22, 24),     # 4  eyes, brows
    (196, 142, 102),  # 5  medium skin
    (150, 102, 72),   # 6  medium skin, shadow
    (122, 80, 54),    # 7  deep skin
    (86, 54, 36),     # 8  deep skin, shadow
    (28, 24, 28),     # 9  black hair, a black coat
    (102, 64, 40),    # 10 brown hair
    (214, 182, 112),  # 11 blonde
    (150, 148, 152),  # 12 grey hair, a grey knit
    (36, 48, 84),     # 13 navy
    (112, 30, 42),    # 14 burgundy
    (78, 88, 52),     # 15 olive
]

# The icons keep the colours they were drawn in.
ICON_PAL = [
    (14, 11, 20), (5, 4, 8), (226, 206, 192), (168, 136, 132), (36, 22, 28), (128, 22, 34),
    (78, 12, 22), (28, 66, 46), (34, 40, 92), (82, 46, 104), (176, 180, 190), (226, 178, 72),
    (30, 26, 34), (140, 136, 146), (140, 64, 36), (210, 180, 110),
]

W = H = 20
SKIN = {"light": (2, 3), "medium": (5, 6), "deep": (7, 8)}

# name, bio, skin, hair (style, colour), what they wear, extra
PEOPLE = [
    ("Jess", "29, paramedic", "light", ("long", 10), 14, None),
    ("Marcus", "34, firefighter", "deep", ("buzz", 9), 13, "beard"),
    ("Priya", "27, lawyer", "medium", ("long", 9), 15, None),
    ("Dan", "41, builder", "light", ("short", 10), 12, "beard"),
    ("Chloe", "25, influencer", "light", ("bob", 11), 9, "scarf"),
    ("Ray", "58, retired cop", "medium", ("short", 12), 14, "glasses"),
    ("Tasha", "31, teacher", "deep", ("curly", 9), 12, "glasses"),
]


def portrait(skin, hair, clothes, extra):
    g = [[0] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            g[y][x] = c

    sk, sh = SKIN[skin]
    style, hc = hair
    # shoulders: a jumper or a coat, widening to the bottom edge, a neckline
    for y in range(14, 20):
        half = 4 + (y - 14) * 2
        for x in range(10 - half, 10 + half):
            put(x, y, clothes)
    for x in range(8, 12):
        put(x, 14, sh)                                # the neck in the neckline
    put(9, 15, sh); put(10, 15, sh)
    # neck
    for y in range(11, 14):
        for x in range(8, 12):
            put(x, y, sh)
    # the face: an oval, its right side in shadow
    for y in range(3, 13):
        for x in range(5, 15):
            dx, dy = (x - 9.5) / 4.3, (y - 7.6) / 5.0
            if dx * dx + dy * dy <= 1.0:
                put(x, y, sh if x >= 12 else sk)
    put(7, 8, 4); put(12, 8, 4)                       # eyes
    put(6, 7, 4); put(7, 7, 4); put(12, 7, 4); put(13, 7, 4)   # brows
    put(9, 9, sh)                                     # nose
    put(9, 11, sh); put(10, 11, sh)                   # mouth
    # hair
    if style == "long":
        for y in range(2, 17):
            for x in range(4, 16):
                if g[y][x] in (0,) and (y < 6 or x < 6 or x > 13):
                    put(x, y, hc)
        for x in range(5, 15):
            put(x, 3, hc)
        for x in range(6, 14):
            put(x, 4, hc if x < 9 or x > 11 else g[4][x])   # a parting
    elif style == "bob":
        for y in range(2, 12):
            for x in range(4, 16):
                if y < 5 or x < 6 or x > 13:
                    put(x, y, hc)
        for x in range(6, 14):
            put(x, 5, hc)                             # a fringe
    elif style == "short":
        for y in range(2, 5):
            for x in range(5, 15):
                put(x, y, hc)
        put(5, 5, hc); put(14, 5, hc); put(5, 6, hc); put(14, 6, hc)
    elif style == "buzz":
        for x in range(6, 14):
            put(x, 2, hc)
        for x in range(5, 15):
            put(x, 3, hc)
    elif style == "curly":
        for y in range(0, 13):
            for x in range(2, 18):
                dx, dy = (x - 9.5) / 7.5, (y - 5.5) / 6.0
                inside_face = ((x - 9.5) / 4.3) ** 2 + ((y - 7.6) / 5.0) ** 2 <= 1.0 and y >= 5
                if dx * dx + dy * dy <= 1.0 and not inside_face:
                    put(x, y, hc if (x + y) % 3 else 10)
    # extras
    if extra == "beard":
        for y in range(9, 14):
            for x in range(6, 14):
                if g[y][x] in (sk, sh) and not (y == 11 and x in (9, 10)):
                    put(x, y, hc if hc != 12 else 12)
    elif extra == "glasses":
        for x, y in ((6, 7), (7, 7), (8, 7), (11, 7), (12, 7), (13, 7), (6, 9), (7, 9), (8, 9),
                     (11, 9), (12, 9), (13, 9), (6, 8), (8, 8), (11, 8), (13, 8), (9, 8), (10, 8)):
            put(x, y, 1)
    elif extra == "scarf":
        for y in range(12, 16):
            for x in range(6, 14):
                put(x, y, 14)
        put(12, 16, 14); put(12, 17, 14)
    # outline: the dark next to the figure
    out = [row[:] for row in g]
    for y in range(H):
        for x in range(W):
            if g[y][x] == 0:
                for ax, ay in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + ax, y + ay
                    if 0 <= nx < W and 0 <= ny < H and g[ny][nx] not in (0, 1):
                        out[y][x] = 1
                        break
    return out


# 9x9 icons: '.' is the background (left as it is), letters the palette.
KEY = {".": -1, "K": 1, "W": 10, "G": 13, "y": 11, "r": 5, "m": 6, "s": 2, "e": 4, "D": 12}
ICONS = {
    "SHIELD": [
        ".KKKKKKK.",
        "KWWWrWWWK",
        "KWWWrWWWK",
        "KrrrrrrrK",
        "KWWWrWWWK",
        ".KWWrWWK.",
        ".KWWrWWK.",
        "..KWrWK..",
        "...KKK...",
    ],
    "GOLD": [
        "...KKK...",
        ".KKyyyKK.",
        ".KyyyyyK.",
        "KyyWyyyyK",
        "KyWyyyyyK",
        "KyyyyyyyK",
        ".KyyyyyK.",
        ".KKyyyKK.",
        "...KKK...",
    ],
    "LEAD": [
        "...KKK...",
        ".KKGGGKK.",
        ".KGGGGGK.",
        "KGGWGGGGK",
        "KGWGGGDGK",
        "KGGGGGDGK",
        ".KGGGDDK.",
        ".KKGGGKK.",
        "...KKK...",
    ],
    "DAGGER": [
        "........K",
        ".......KW",
        "......KWK",
        ".....KWK.",
        "....KWK..",
        ".yyKWK...",
        "..yyK....",
        ".KKyy....",
        "KK..y....",
    ],
    "SKULL": [
        "..KKKKK..",
        ".KWWWWWK.",
        "KWWWWWWWK",
        "KWKKWKKWK",
        "KWKKWKKWK",
        "KWWWKWWWK",
        ".KWWWWWK.",
        "..KWKWK..",
        "...KKK...",
    ],
    "CROWN": [
        ".........",
        "y...y...y",
        "yy.yyy.yy",
        "yyyyyyyyy",
        "yyryyyryy",
        "yyyyyyyyy",
        "KKKKKKKKK",
        ".........",
        ".........",
    ],
    "CANDLE": [
        "....y....",
        "...yry...",
        "....y....",
        "...WWW...",
        "...WWW...",
        "...WsW...",
        "...WWW...",
        "..KKKKK..",
        ".KKKKKKK.",
    ],
    "CLOAK": [
        "...KKK...",
        "..KrrrK..",
        ".KrKKKrK.",
        ".KrKeKrK.",
        "KrrrKrrrK",
        "KrrrrrrrK",
        "KmrrrrrmK",
        "KmmrrrmmK",
        "KKKKKKKKK",
    ],
}


def rgb565_panel(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) | (v << 8)) & 0xFFFF


def write_header():
    lines = ["/* Generated by apps/traitors/tools/make_traitors.py -- do not edit; edit the script. */",
             "#ifndef CARDOS_TRAITORS_ART_H", "#define CARDOS_TRAITORS_ART_H", "",
             "#include <stdint.h>", "",
             "#define TA_FACE 20                /* a portrait's side */",
             "#define TA_ICON 9                 /* an icon's side */",
             "#define TA_PEOPLE %d" % len(PEOPLE), "",
             "/* The palettes, in the panel's byte order (CAPP_RGB's): the faces', the icons'. */",
             "static const uint16_t TA_PAL[16] = {"]
    lines.append("  " + ", ".join("0x%04X" % rgb565_panel(*c) for c in FACE_PAL))
    lines.append("};")
    lines.append("static const uint16_t TA_IPAL[16] = {")
    lines.append("  " + ", ".join("0x%04X" % rgb565_panel(*c) for c in ICON_PAL))
    lines.append("};")
    lines.append("")
    lines.append("static const char *const TA_NAME[TA_PEOPLE] = { %s };" %
                 ", ".join('"%s"' % p[0] for p in PEOPLE))
    lines.append("static const char *const TA_BIO[TA_PEOPLE] = { %s };" %
                 ", ".join('"%s"' % p[1] for p in PEOPLE))
    lines.append("")
    lines.append("/* Portraits, 4 bits a pixel, the left pixel in the high half. */")
    lines.append("static const uint8_t TA_FACES[TA_PEOPLE][TA_FACE * TA_FACE / 2] = {")
    for name, bio, skin, hair, clothes, extra in PEOPLE:
        g = portrait(skin, hair, clothes, extra)
        b = []
        for y in range(H):
            for x in range(0, W, 2):
                b.append((g[y][x] << 4) | g[y][x + 1])
        lines.append("  { /* %s */" % name)
        for i in range(0, len(b), 20):
            lines.append("    " + ", ".join("0x%02X" % v for v in b[i:i + 20]) + ",")
        lines.append("  },")
    lines.append("};")
    lines.append("")
    lines.append("/* Icons: a palette index a pixel, 0xFF where the background shows. */")
    names = list(ICONS)
    for i, n in enumerate(names):
        lines.append("#define TA_%s %d" % (n, i))
    lines.append("#define TA_ICONS %d" % len(names))
    lines.append("static const uint8_t TA_ICON_PX[TA_ICONS][TA_ICON * TA_ICON] = {")
    for n in names:
        px = [KEY[ch] & 0xFF for row in ICONS[n] for ch in row]
        assert len(px) == 81, n
        lines.append("  { /* %s */ %s }," % (n, ", ".join(str(v) for v in px)))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", os.path.relpath(OUT, ROOT))


def preview(path):
    from PIL import Image
    scale = 8
    w = (len(PEOPLE) * 22 + 2) * scale
    h = (22 + 12) * scale
    im = Image.new("RGB", (w, h), FACE_PAL[0])
    for i, (name, bio, skin, hair, clothes, extra) in enumerate(PEOPLE):
        g = portrait(skin, hair, clothes, extra)
        for y in range(H):
            for x in range(W):
                for dy in range(scale):
                    for dx in range(scale):
                        im.putpixel(((2 + i * 22 + x) * scale + dx, (1 + y) * scale + dy), FACE_PAL[g[y][x]])
    for i, n in enumerate(ICONS):
        for y, row in enumerate(ICONS[n]):
            for x, ch in enumerate(row):
                if KEY[ch] < 0:
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        im.putpixel(((2 + i * 11 + x) * scale + dx, (23 + y) * scale + dy), ICON_PAL[KEY[ch]])
    im.save(path)
    print("wrote", path)


if __name__ == "__main__":
    if "--preview" in sys.argv:
        preview(sys.argv[sys.argv.index("--preview") + 1])
    else:
        write_header()
