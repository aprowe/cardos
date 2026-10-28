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
Emits every .cic into build/icons, which the server offers with `update apps`
(server/updates.py, the manifest's `icon` lines), and a header embedding only
SEED -- what a blank card needs before its first `update apps`: generic, the
folders, firmware, the built-ins and the apps the firmware itself carries.
tools/build_apps.py writes build/icons too (write_cic_files), so a build that
never ran this one -- the droplet's -- still has them to publish.

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
    "P": (232, 120, 210),   # pink, for Toggl
    "R": (102, 45, 145),    # Roku's purple
}

ICONS = {









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
    # The folders the apps are grouped into (Plan, Make, Net, Games, System
    # since 2026-10-01). One folder body -- amber,
    # tab on the left, a darker lip along the bottom -- with a different mark
    # on the face of each, so they read as a set at 16 pixels and still tell
    # each other apart at a glance. Keyed by the folder's name on the card,
    # which is how load_cic finds them.

    # Games: a d-pad.
    "folder-Games": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyKKKyyyyyK.",
        ".KyyyyKgKyyyyyK.",
        ".KyyKKKgKKKyyyK.",
        ".KyyKgggggKyyyK.",
        ".KyyKKKgKKKyyyK.",
        ".KyyyyKgKyyyyyK.",
        ".KyyyyKKKyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
        ".KKKKKKKKKKKKKK.",
        "................",
    ],

    # Net: a globe, with a meridian and an equator.
    "folder-Net": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyKKKKyyyyK.",
        ".KyyyKbnbnbKyyK.",
        ".KyyKbbnbbnbKyK.",
        ".KyynnnnnnnnKyK.",
        ".KyyKbbnbbnbKyK.",
        ".KyyyKbnbnbKyyK.",
        ".KyyyyKKKKyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
        ".KKKKKKKKKKKKKK.",
        "................",
    ],

    # Make: a spanner, laid across the face -- Tools' mark, kept.
    "folder-Make": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyyyyKKyyK.",
        ".KyyyyyyyKGGKyK.",
        ".KyyyyyyKGGGKyK.",
        ".KyyyyyKGGKKKyK.",
        ".KyyyyKGGKyyyyK.",
        ".KyyyKGGKyyyyyK.",
        ".KyyKGGKyyyyyyK.",
        ".KyyKKKyyyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
        ".KKKKKKKKKKKKKK.",
        "................",
    ],

    # Plan: a box with a tick in it.
    "folder-Plan": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyKKKKKKKyyK.",
        ".KyyyKWWWWWgKyK.",
        ".KyyyKWWWWggKyK.",
        ".KyyyKgWWggWKyK.",
        ".KyyyKggggWWKyK.",
        ".KyyyKWggWWWKyK.",
        ".KyyyKKKKKKKyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
        ".KKKKKKKKKKKKKK.",
        "................",
    ],

    # System: a gear.
    "folder-System": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyyyKKyyyyyK.",
        ".KyyyKKDDKKyyyK.",
        ".KyyyKDDDDKyyyK.",
        ".KyyKDDKKDDKyyK.",
        ".KyyKDDKKDDKyyK.",
        ".KyyyKDDDDKyyyK.",
        ".KyyyKKDDKKyyyK.",
        ".KyyyyyKKyyyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
        ".KKKKKKKKKKKKKK.",
        "................",
    ],

    # Firmware: a chip on the folder face. These replace CardOS rather than
    # run on it, which is why they are behind a folder at all.
    "folder-Firmware": [
        "................",
        "..KKKKK.........",
        ".KyyyyyKKKKKKKK.",
        ".KyyyyyyyyyyyyK.",
        ".KyyyKKKKKKyyyK.",
        ".KyyyKGGGGKyyyK.",
        ".KyKKKGDDGKKKyK.",
        ".KyyyKGDDGKyyyK.",
        ".KyKKKGDDGKKKyK.",
        ".KyyyKGGGGKyyyK.",
        ".KyyyKKKKKKyyyK.",
        ".KyyyyyyyyyyyyK.",
        ".KooooooooooooK.",
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

# Every app's icon is its own file, beside its source: NAME.icon.txt, the
# app's name (as in its capp_info) on the first line and sixteen rows of art
# under it, in the INK letters above (tools/appmeta.py). Only the system's
# are drawn here: the fallback, the firmware chip, the folders and the
# built-ins. A file per app so an app carries its own picture -- and a Build
# turn fenced to its own app's files (server/fence.py) can draw one.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import appmeta  # noqa: E402


def load_icon_file(path, icons=None):
    """Add one NAME.icon.txt to icons (ICONS by default): its name."""
    icons = ICONS if icons is None else icons
    rel = os.path.relpath(path, appmeta.ROOT)
    with open(path, encoding="utf-8") as f:
        rows = [r.rstrip() for r in f if r.strip()]
    if len(rows) != 17:
        raise SystemExit("%s: the name, then 16 rows of art (%d lines)" % (rel, len(rows)))
    name, art = rows[0].strip(), [r.strip() for r in rows[1:]]
    if name in icons:
        raise SystemExit("%s: '%s' already has an icon" % (rel, name))
    icons[name] = art
    return name


def load_icon_files(*dirs):
    for path in appmeta.icon_files(*dirs):
        load_icon_file(path)


load_icon_files()

# The icons the firmware embeds (2026-10-16). Everything else arrives with
# `update apps`, so only what a blank card shows before that: the fallback,
# the chip a firmware image wears, the folders, the built-ins (Settings,
# Memory, About -- nothing can deliver those but the firmware), and the apps
# in EMBED in tools/build_apps.py (dashlink, files, edit, update), by the
# names their capp_info gives them.
SEED = ("generic", "firmware", "Settings", "Memory", "About",
        "Dashboard Link", "Files", "Edit", "Update")


def seeded(name):
    return name in SEED or name.startswith("folder-")


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


def write_cic_files(out=OUT, quiet=False):
    """Every icon into `out` as NAME.cic, and any .cic there that is no
    longer drawn removed: the server offers whatever is in it. Returns
    [(name, data)]."""
    os.makedirs(out, exist_ok=True)
    built = []
    for name, art in ICONS.items():
        data = pack(art, name)
        path = os.path.join(out, name + ".cic")
        try:
            with open(path, "rb") as f:
                same = f.read() == data
        except OSError:
            same = False
        if not same:
            with open(path, "wb") as f:
                f.write(data)
        built.append((name, data))
        if not quiet:
            print("  %-10s %d bytes" % (name + ".cic", len(data)))
    for fn in os.listdir(out):
        if fn.endswith(".cic") and fn[:-len(".cic")] not in ICONS:
            os.remove(os.path.join(out, fn))
            if not quiet:
                print("  removed %s: nothing draws it any more" % fn)
    return built


def main():
    if "--show" in sys.argv:
        for name, art in ICONS.items():
            print(name + ":")
            for row in art:
                print("  " + row)
        return

    built = write_cic_files()
    missing = [n for n in SEED if n not in ICONS]
    if missing:
        raise SystemExit("SEED names icons that are not drawn: %s" % ", ".join(missing))
    built = [(n, d) for n, d in built if seeded(n)]

    lines = [
        "/* Colour icons, generated by tools/make_color_icons.py -- do not edit.",
        " *",
        " * Embedded so first boot can write them to /apps/icons. 16x16 RGB565,",
        " * pre-swapped for the panel, drawn at 4x. Edit the pictures in the",
        " * generator, never the hex. Only SEED is here: every other app's icon",
        " * arrives with `update apps`.",
        " */",
        "#ifndef CARDOS_ICONS_COLOR_H",
        "#define CARDOS_ICONS_COLOR_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
    ]
    for name, data in built:
        sym = "".join(c if c.isalnum() else "_" for c in name)   # "Remote Files"
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
        sym = "".join(c if c.isalnum() else "_" for c in name)   # "Remote Files"
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
    print("  %s (%d icons, %d bytes of flash; %d in %s)" % (
        HEADER, len(built), total, len(ICONS), OUT))


if __name__ == "__main__":
    main()
