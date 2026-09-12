#!/usr/bin/env python3
"""Generate the colour icons that ship on the card.

Each is 16x16 RGB565, already byte-swapped for the panel, in a .cic file:

    'C' 'I' 'C' '1'   u16 width   u16 height   then width*height pixels

520 bytes for a 16x16, drawn by the launcher at 4x as 64x64 of pixel art. The
byte swap is done here rather than on the device for the same reason the
palette is stored pre-swapped: the panel is MSB-first and the chip is little-
endian, and doing it once at build time costs nothing at runtime.

Written as pictures with a per-icon palette, like the font and the 1bpp icons,
because a wrong pixel is visible in the art and invisible in the hex.
Emits both the .cic files and a header embedding them, so first boot can write
them to /desktop/icons without a card reader in the loop.

    python tools/make_color_icons.py
    python tools/make_color_icons.py --show
"""

import os
import sys

OUT = os.path.join("build", "icons")
HEADER = os.path.join("kernel", "ui", "icons_color.h")

# A shared vocabulary, so the set looks like a set rather than six unrelated
# drawings. Keyed by the character used in the art.
INK = {
    ".": None,              # transparent: the desktop shows through
    "K": (24, 26, 32),      # near-black outline
    "W": (255, 255, 255),
    "G": (150, 158, 170),   # cool grey
    "D": (92, 100, 112),    # darker grey
    "y": (255, 204, 82),    # amber
    "o": (232, 136, 48),    # deeper amber, for shading
    "b": (86, 158, 232),    # blue
    "n": (44, 92, 158),     # deeper blue
    "g": (110, 196, 128),   # green
    "e": (52, 138, 82),     # deeper green
    "r": (228, 86, 76),     # red
    "m": (168, 52, 52),     # deeper red
    "p": (186, 148, 232),   # violet
    "t": (96, 204, 196),    # teal
}

ICONS = {
    # A manila folder with a tab.
    "Files": [
        "................",
        "................",
        "..KKKK..........",
        ".KyyyyK.........",
        ".KyyyyyKKKKKKK..",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KyoooooooooooK.",
        ".KyooooooooooKK.",
        ".KyooooooooooK..",
        ".KyoooooooooKK..",
        ".KyooooooooK....",
        "..KKKKKKKKK.....",
        "................",
        "................",
        "................",
    ],
    # A DIP chip: body, legs, and the notch that says which end is pin one.
    "Memory": [
        "................",
        "...G........G...",
        "..GGG......GGG..",
        "..KKKKKKKKKKKK..",
        ".GKDDDDDDDDDDKG.",
        ".GKDttttttttDKG.",
        "..KDtKKKKKKtDK..",
        ".GKDtKDDDDKtDKG.",
        ".GKDtKDDDDKtDKG.",
        "..KDtKKKKKKtDK..",
        ".GKDttttttttDKG.",
        ".GKDDDDDDDDDDKG.",
        "..KKKKKKKKKKKK..",
        "..GGG......GGG..",
        "...G........G...",
        "................",
    ],
    # A gear, four teeth and a hollow centre.
    "Settings": [
        "................",
        "......KKKK......",
        ".....KGGGGK.....",
        "...KKKGGGGKKK...",
        "..KGGGGGGGGGGK..",
        "..KGGGKKKKGGGK..",
        "KKKGGKDDDDKGGKKK",
        "KGGGGKD..DKGGGGK",
        "KGGGGKD..DKGGGGK",
        "KKKGGKDDDDKGGKKK",
        "..KGGGKKKKGGGK..",
        "..KGGGGGGGGGGK..",
        "...KKKGGGGKKK...",
        ".....KGGGGK.....",
        "......KKKK......",
        "................",
    ],
    # An i in a ring.
    "About": [
        "................",
        "....KKKKKKKK....",
        "..KKbbbbbbbbKK..",
        ".Kbbbbbbbbbbbb K",
        "Kbbbbb WW bbbbbK",
        "Kbbbbb WW bbbbbK",
        "Kbbbbbbbbbbbbb K",
        "Kbbbb WWWW bbbbK",
        "Kbbbbbb WW bbbbK",
        "Kbbbbbb WW bbbbK",
        "Kbbbbb WWWWWbbbK",
        ".Kbbbbbbbbbbbb K",
        "..KKbbbbbbbbKK..",
        "....KKKKKKKK....",
        "................",
        "................",
    ],
    # A mine with a fuse.
    "Mines": [
        "................",
        "..........KK....",
        ".........KyyK...",
        "........KyyK....",
        "....KKKKKK......",
        "...KDDDDDDK.....",
        "..KDDDDDDDDK....",
        ".KDDWDDDDDDDK...",
        ".KDDDDDDDDDDK...",
        ".KDDDDDDDDDDK...",
        "..KDDDDDDDDK....",
        "...KDDDDDDK.....",
        "....KKKKKK......",
        "................",
        "................",
        "................",
    ],
    # A framed landscape: sun over a hill.
    "Photos": [
        "................",
        ".KKKKKKKKKKKKKK.",
        ".KbbbbbbbbbbbbK.",
        ".KbbyybbbbbbbbK.",
        ".KbyyyybbbbbbbK.",
        ".KbbyybbbbbbbbK.",
        ".KbbbbbbbbbbbbK.",
        ".KbbbbbbbbeebbK.",
        ".KbbbbbbbeggebK.",
        ".KbbbbeeegggeeK.",
        ".KbbeeggggggggK.",
        ".KeegggggggggeK.",
        ".KgggggggggggeK.",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
    ],
    # A rising line over an axis.
    "Stocks": [
        "................",
        ".K..............",
        ".K...........gg.",
        ".K..........gg..",
        ".K.........gg...",
        ".K........gg....",
        ".K...rr..gg.....",
        ".K..rrrrgg......",
        ".K.rr..gg.......",
        ".Krr..gg........",
        ".K...gg.........",
        ".K..gg..........",
        ".K.gg...........",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
    ],
    # A page with a pencil across it.
    "Edit": [
        "................",
        "..KKKKKKKKK.....",
        "..KWWWWWWWKK....",
        "..KWWWWWWWWK....",
        "..KWKKKKWWWK..K.",
        "..KWWWWWWWW..Ky.",
        "..KWKKKKWW..KyyK",
        "..KWWWWWW..KyyK.",
        "..KWKKKKW.KyyK..",
        "..KWWWWWW KyK...",
        "..KWKKKKKKKK....",
        "..KWWWWWWWWK....",
        "..KWWWWWWWWK....",
        "..KKKKKKKKKK....",
        "................",
        "................",
    ],
    # A clipboard with a ticked line.
    # A globe: equator, meridian, and a pair of arcs. Latitude stripes alone
    # read as a beach ball at this size; the converging verticals are what say
    # sphere.
    # A table seen from above: the ball up in the bumpers, two flippers at the
    # bottom. The flippers are what make it read as pinball rather than as a
    # dial -- they are worth the four pixels each.
    # A speech bubble with a tail. Not a face and not a logo: the app is a
    # conversation, and a bubble is the one shape that says so at 16 pixels.
    "Claude": [
        "................",
        "...KKKKKKKKKK...",
        "..KooooooooooK..",
        ".KooooooooooooK.",
        ".KoWWWWWWWWWWoK.",
        ".KooooooooooooK.",
        ".KoWWWWWWWWWWoK.",
        ".KooooooooooooK.",
        ".KoWWWWWWWoooooK",
        ".KooooooooooooK.",
        "..KooooooooooK..",
        "...KooooooooK...",
        "...KooK.KKKK....",
        "..KooK..........",
        "..KK............",
        "................",
    ],

    "Pinball": [
        "................",
        "..KKKKKKKKKKKK..",
        "..KnnnnnnnnnnK..",
        "..KnnbnWWnbnnK..",
        "..KnnnWWWWnnnK..",
        "..KnbnWWWWnbnK..",
        "..KnnnnWWnnnnK..",
        "..KnnnnnnnnnnK..",
        "..KnnnnnnnnnnK..",
        "..KGnnnnnnnnGK..",
        "..KnGGnnnnGGnK..",
        "..KnnGGnnGGnnK..",
        "..KnnnGGGGnnnK..",
        "..KnnnnnnnnnnK..",
        "..KKKKKKKKKKKK..",
        "................",
    ],

    "Web": [
        "................",
        ".....KKKKKK.....",
        "...KKWbWWbWKK...",
        "..KKbWbWWbWbKK..",
        "..KbWbbWWbbWbK..",
        ".KbbWbbWWbbWbbK.",
        ".KbbWbbWWbbWbbK.",
        ".KWWWWWWWWWWWWK.",
        ".KbbWbbWWbbWbbK.",
        ".KbbWbbWWbbWbbK.",
        ".KbbWbbWWbbWbbK.",
        "..KbWbbWWbbWbK..",
        "..KKbWbWWbWbKK..",
        "...KKWbWWbWKK...",
        ".....KKKKKK.....",
        "................",
    ],

    "Todo": [
        "................",
        "......KKKK......",
        "....KKyyyyKK....",
        "..KKKKKKKKKKKK..",
        "..KWWWWWWWWWWK..",
        "..KWKKWWWWWWWK..",
        "..KWKgKWWWWWWK..",
        "..KWgKgWWWWWWK..",
        "..KWggKWWWWWWK..",
        "..KWWWWWWWWWWK..",
        "..KWKKKKWWWWWK..",
        "..KWWWWWWWWWWK..",
        "..KWKKKKKKWWWK..",
        "..KWWWWWWWWWWK..",
        "..KKKKKKKKKKKK..",
        "................",
    ],
    # A board with a chip and a header: a firmware image.
    "firmware": [
        "................",
        ".KKKKKKKKKKKKKK.",
        ".KeeeeeeeeeeeeK.",
        ".KeyKyKyKyKyeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeeKKKKKKKKeeK.",
        ".KeeKDDDDDDKeeK.",
        ".KeeKDttttDKeeK.",
        ".KeeKDttttDKeeK.",
        ".KeeKDDDDDDKeeK.",
        ".KeeKKKKKKKKeeK.",
        ".KeeeeeeeeeeeeK.",
        ".KeKKeKKeKKeeeK.",
        ".KKKKKKKKKKKKKK.",
        "................",
        "................",
    ],
    # The fallback: a blank page with a folded corner.
    "generic": [
        "................",
        "...KKKKKKKK.....",
        "...KWWWWWWKK....",
        "...KWWWWWWWKK...",
        "...KWWWWWWWWKK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KWWWWWWWWWK..",
        "...KKKKKKKKKKK..",
        "................",
        "................",
        "................",
    ],
}

# Transparent is drawn as the launcher's own background, so an icon never has
# a box of its own colour around it.
TRANSPARENT = 0x0000


def rgb565_swapped(rgb):
    r, g, b = rgb
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((v >> 8) & 0xFF) | ((v & 0xFF) << 8)


def pack(art, name):
    out = bytearray()
    out += b"CIC1"
    out += (16).to_bytes(2, "little") + (16).to_bytes(2, "little")
    for y, row in enumerate(art):
        if len(row) != 16:
            raise SystemExit("%s row %d is %d wide" % (name, y, len(row)))
        for ch in row:
            if ch == " ":
                ch = "."
            if ch not in INK:
                raise SystemExit("%s: unknown ink %r" % (name, ch))
            v = TRANSPARENT if INK[ch] is None else rgb565_swapped(INK[ch])
            out += v.to_bytes(2, "little")
    return bytes(out)


def main():
    if "--show" in sys.argv:
        for name, art in ICONS.items():
            print(name + ":")
            for row in art:
                print("  " + row)
        return

    os.makedirs(OUT, exist_ok=True)
    built = []
    for name, art in ICONS.items():
        data = pack(art, name)
        path = os.path.join(OUT, name + ".cic")
        with open(path, "wb") as f:
            f.write(data)
        built.append((name, data))
        print("  %-10s %d bytes" % (name + ".cic", len(data)))

    lines = [
        "/* Colour icons, generated by tools/make_color_icons.py -- do not edit.",
        " *",
        " * Embedded so first boot can write them to /desktop/icons. 16x16 RGB565,",
        " * pre-swapped for the panel, drawn at 4x. Edit the pictures in the",
        " * generator, never the hex.",
        " */",
        "#ifndef CARDOS_ICONS_COLOR_H",
        "#define CARDOS_ICONS_COLOR_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
    ]
    for name, data in built:
        sym = name.replace("-", "_")
        lines.append("static const uint8_t cic_%s[] = {" % sym)
        for i in range(0, len(data), 16):
            lines.append("  " + " ".join("0x%02X," % b for b in data[i:i + 16]))
        lines.append("};")
        lines.append("")

    lines += [
        "typedef struct {",
        "  const char    *name;      /* file name under /desktop/icons */",
        "  const uint8_t *data;",
        "  size_t         size;",
        "} CicBlob;",
        "",
        "static const CicBlob CIC_BLOBS[] = {",
    ]
    for name, _ in built:
        sym = name.replace("-", "_")
        lines.append('  { "%s.cic", cic_%s, sizeof cic_%s },' % (name, sym, sym))
    lines += [
        "};",
        "",
        "#define CIC_BLOB_COUNT (sizeof CIC_BLOBS / sizeof CIC_BLOBS[0])",
        "",
        "#endif /* CARDOS_ICONS_COLOR_H */",
    ]

    with open(HEADER, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    total = sum(len(d) for _, d in built)
    print("  %s (%d bytes of flash)" % (HEADER, total))


if __name__ == "__main__":
    main()
