#!/usr/bin/env python3
"""The Traitors' pictures: seven portraits and the little icons, as gothic
pixel art on one 16-colour palette.

    python tools/make_traitors.py                 writes apps/traitors/traitors_art.h
    python tools/make_traitors.py --preview F.png  draws everything big, to look at

Portraits are 20x20, drawn here from a few choices each (hair, cloak,
trimmings) over one face, outlined where they meet the dark; icons are 9x9,
drawn by hand below. Everything is a palette index, 4 bits a pixel for the
portraits. The palette is RGB565 in the panel's byte order, as CAPP_RGB makes
it, so the app hands it to api->pixels unchanged.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "apps", "traitors", "traitors_art.h")

PAL = [
    (14, 11, 20),     # 0  the dark behind a portrait
    (5, 4, 8),        # 1  outline
    (226, 206, 192),  # 2  skin
    (168, 136, 132),  # 3  skin in shadow
    (36, 22, 28),     # 4  eyes
    (128, 22, 34),    # 5  crimson
    (78, 12, 22),     # 6  crimson, dark
    (28, 66, 46),     # 7  forest
    (34, 40, 92),     # 8  midnight
    (82, 46, 104),    # 9  plum
    (176, 180, 190),  # 10 silver
    (226, 178, 72),   # 11 gold
    (30, 26, 34),     # 12 black hair
    (140, 136, 146),  # 13 grey hair
    (140, 64, 36),    # 14 auburn
    (210, 180, 110),  # 15 flaxen
]

W = H = 20

# name, hair (style, colour), cloak colour, trimming
PEOPLE = [
    ("Morwen", ("long", 12), 9, "necklace"),
    ("Silas", ("short", 13), 8, "pin"),
    ("Ingrid", ("braids", 15), 7, None),
    ("Corvin", ("hood", 5), 5, None),
    ("Agatha", ("bun", 14), 9, "necklace"),
    ("Lucan", ("slick", 12), 8, "monocle"),
    ("Esme", ("hood", 13), 6, "pin"),
]


def portrait(hair, cloak, trim):
    g = [[0] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            g[y][x] = c

    style, hc = hair
    hooded = style == "hood"
    # shoulders: a cloak widening to the bottom edge
    for y in range(14, 20):
        half = 4 + (y - 14) * 2
        for x in range(10 - half, 10 + half):
            put(x, y, cloak)
    # a darker fold down the middle
    for y in range(15, 20):
        put(10, y, 1 if cloak != 6 else 4)
    # neck
    for y in range(12, 15):
        for x in range(8, 12):
            put(x, y, 3)
    # the face: an oval, its right side in shadow (shadowed whole under a hood)
    for y in range(3, 14):
        for x in range(4, 16):
            dx, dy = (x - 9.5) / 4.6, (y - 8.0) / 5.4
            if dx * dx + dy * dy <= 1.0:
                put(x, y, 3 if hooded or x >= 12 else 2)
    put(7, 8, 4); put(12, 8, 4)                        # eyes
    if hooded:
        put(7, 8, 5); put(12, 8, 5)                   # a gleam under the hood
    for x in range(9, 11):
        put(x, 11, 3 if not hooded else 4)            # mouth
    put(9, 9, 3)                                      # nose
    # hair
    if style == "long":
        for y in range(2, 16):
            for x in range(3, 17):
                if g[y][x] == 0 and (y < 6 or x < 5 or x > 14):
                    put(x, y, hc)
        for x in range(5, 15):
            put(x, 3, hc); put(x, 4, hc if x < 8 or x > 12 else g[4][x])
    elif style == "short":
        for y in range(2, 6):
            for x in range(4, 16):
                if y < 4 or x < 6 or x > 13:
                    put(x, y, hc)
    elif style == "slick":
        for y in range(2, 5):
            for x in range(4, 16):
                put(x, y, hc)
        put(4, 5, hc); put(15, 5, hc)
        for x in range(6, 14):
            put(x, 3, 13 if x % 3 == 0 else hc)      # a shine
    elif style == "bun":
        for y in range(0, 3):
            for x in range(8, 12):
                put(x, y, hc)
        for y in range(2, 6):
            for x in range(4, 16):
                if y < 4 or x < 6 or x > 13:
                    put(x, y, hc)
    elif style == "braids":
        for y in range(2, 5):
            for x in range(4, 16):
                put(x, y, hc)
        for y in range(5, 17):
            for x in (3, 4, 15, 16):
                put(x, y, hc if y % 3 else 14)
    elif hooded:
        # the hood: around the head and down to the shoulders, a deep shadow inside
        for y in range(1, 16):
            for x in range(2, 18):
                dx, dy = (x - 9.5) / 7.2, (y - 8.5) / 7.6
                if dx * dx + dy * dy <= 1.0 and g[y][x] in (0, 3) and \
                        not ((x - 9.5) / 4.6) ** 2 + ((y - 8.0) / 5.4) ** 2 <= 0.8:
                    put(x, y, hc)
        for x in range(5, 15):
            put(x, 3, 1)                              # the hood's shadow on the brow
    # trimmings
    if trim == "pin":
        put(7, 16, 11); put(6, 16, 11); put(7, 17, 11)
    elif trim == "necklace":
        for x in range(7, 13):
            put(x, 14, 10)
        put(10, 15, 5)
    elif trim == "monocle":
        for x, y in ((11, 7), (12, 7), (13, 7), (11, 9), (12, 9), (13, 9), (11, 8), (13, 8)):
            put(x, y, 11)
        put(13, 10, 11); put(13, 11, 11)
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
    lines = ["/* Generated by tools/make_traitors.py -- do not edit; edit the script. */",
             "#ifndef CARDOS_TRAITORS_ART_H", "#define CARDOS_TRAITORS_ART_H", "",
             "#include <stdint.h>", "",
             "#define TA_FACE 20                /* a portrait's side */",
             "#define TA_ICON 9                 /* an icon's side */",
             "#define TA_PEOPLE %d" % len(PEOPLE), "",
             "/* The palette, in the panel's byte order (CAPP_RGB's). */",
             "static const uint16_t TA_PAL[16] = {"]
    lines.append("  " + ", ".join("0x%04X" % rgb565_panel(*c) for c in PAL))
    lines.append("};")
    lines.append("")
    lines.append("static const char *const TA_NAME[TA_PEOPLE] = { %s };" %
                 ", ".join('"%s"' % p[0] for p in PEOPLE))
    lines.append("")
    lines.append("/* Portraits, 4 bits a pixel, the left pixel in the high half. */")
    lines.append("static const uint8_t TA_FACES[TA_PEOPLE][TA_FACE * TA_FACE / 2] = {")
    for name, hair, cloak, trim in PEOPLE:
        g = portrait(hair, cloak, trim)
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
    im = Image.new("RGB", (w, h), PAL[0])
    for i, (name, hair, cloak, trim) in enumerate(PEOPLE):
        g = portrait(hair, cloak, trim)
        for y in range(H):
            for x in range(W):
                for dy in range(scale):
                    for dx in range(scale):
                        im.putpixel(((2 + i * 22 + x) * scale + dx, (1 + y) * scale + dy), PAL[g[y][x]])
    for i, n in enumerate(ICONS):
        for y, row in enumerate(ICONS[n]):
            for x, ch in enumerate(row):
                if KEY[ch] < 0:
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        im.putpixel(((2 + i * 11 + x) * scale + dx, (23 + y) * scale + dy), PAL[KEY[ch]])
    im.save(path)
    print("wrote", path)


if __name__ == "__main__":
    if "--preview" in sys.argv:
        preview(sys.argv[sys.argv.index("--preview") + 1])
    else:
        write_header()
