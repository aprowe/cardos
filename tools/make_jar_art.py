"""Jar Factory's pictures and its hand-made items.

    python tools/make_jar_art.py               writes apps/jar_art.h
    python tools/make_jar_art.py --show x.png  and a contact sheet, 4x

The art is text in tools/jar_art.txt, so it can be changed by eye: each
picture names up to seven colours by letter ('.' is clear), then its frames
as rows of letters. Two kinds of block:

    sprite NAME W H          the scene: mosslings, the snail, machines, ...
    item ID KIND "Name"      a built-in item (apps/jaritem.h), 16x16,
                             with its line, recipe, habits, bubbles, tags
                             and price (0: one of the starting four)

Both pack to 3 bits a pixel. A sprite's palette is written as panel colours
(CAPP_RGB); an item's as plain RGB565, which is what the item record holds.
The device builds each built-in's record from its entry with jitem_encode,
so a built-in goes through the same decoder as one from the server will.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "jar_art.txt")
OUT = os.path.join(ROOT, "apps", "jar_art.h")

KINDS = {"floor": 0, "hanging": 1, "critter": 2}
MOVES = {"sits": 0, "hops": 1, "wanders": 2, "sways": 3, "floats": 4}
SPEEDS = {"slow": 0, "medium": 1, "fast": 2}
ZONES = {"garden": 0, "works": 1, "dock": 2, "water": 3, "high": 4, "anywhere": 5}
EVENTS = {"tick": 0, "poke": 1, "near": 2, "shipped": 3, "jam": 4, "gift": 5,
          "time": 6, "weather": 7}
ACTIONS = {"none": 0, "hop": 1, "walk": 2, "float": 3, "face": 4, "stop": 5,
           "frame": 6, "flip": 7, "glow": 8, "particle": 9, "say": 10, "wait": 11}
NEAR = {"any": 0, "moss": 1, "snail": 2, "critter": 3, "decor": 4}
TIMES = {"any": 0, "dawn": 1, "day": 2, "dusk": 3, "night": 4}
JAMS = {"any": 0, "jammed": 1, "fixed": 2}
PARTS = {"sparkle": 0, "heart": 1, "note": 2, "zzz": 3, "puff": 4}

# 2026-10-09 00:00 UTC: when the built-ins were made.
MADE = 1791504000


def parse_colour(s):
    m = re.fullmatch(r"#([0-9a-fA-F]{6})", s)
    if not m:
        raise SystemExit("bad colour %r" % s)
    v = int(m.group(1), 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def pack(frames, w, h, pal_letters, where):
    """Frames of rows of letters -> bytes, 3 bits a pixel, LSB first."""
    out = []
    for f in frames:
        if len(f) != h:
            raise SystemExit("%s: a frame has %d rows, wants %d" % (where, len(f), h))
        bits = 0
        nbits = 0
        data = bytearray()
        for y, row in enumerate(f):
            if len(row) != w:
                raise SystemExit("%s: row %d is %d wide, wants %d: %r" % (where, y, len(row), w, row))
            for ch in row:
                if ch == ".":
                    v = 0
                elif ch in pal_letters:
                    v = pal_letters.index(ch) + 1
                else:
                    raise SystemExit("%s: row %d: no colour %r" % (where, y, ch))
                bits |= v << nbits
                nbits += 3
                while nbits >= 8:
                    data.append(bits & 255)
                    bits >>= 8
                    nbits -= 8
        if nbits:
            data.append(bits & 255)
        out.append(bytes(data))
    return out


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def read_blocks():
    blocks = []
    cur = None
    frame = None
    with open(SRC, encoding="utf-8") as f:
        for ln, raw in enumerate(f, 1):
            line = raw.rstrip("\n").rstrip()
            if not line or line.lstrip().startswith("#"):
                continue
            words = line.split()
            head = words[0]
            if head in ("sprite", "item"):
                cur = {"type": head, "line_no": ln, "frames": [], "pal": [], "habits": [],
                       "bubbles": []}
                frame = None
                if head == "sprite":
                    cur["name"], cur["w"], cur["h"] = words[1], int(words[2]), int(words[3])
                else:
                    m = re.fullmatch(r'item\s+(\d+)\s+(\w+)\s+"([^"]*)"', line)
                    if not m:
                        raise SystemExit("line %d: item ID KIND \"Name\"" % ln)
                    cur["id"], cur["kind"], cur["title"] = int(m.group(1)), m.group(2), m.group(3)
                    cur["w"] = cur["h"] = 16
                blocks.append(cur)
            elif head == "end":
                cur = None
                frame = None
            elif cur is None:
                raise SystemExit("line %d: outside a block" % ln)
            elif head == "pal":
                for wd in words[1:]:
                    k, v = wd.split("=")
                    cur["pal"].append((k, parse_colour(v)))
            elif head == "frame":
                frame = []
                cur["frames"].append(frame)
            elif frame is not None and re.fullmatch(r"[.A-Za-z]+", line.strip()):
                frame.append(line.strip())
            elif head == "line":
                cur["text"] = re.fullmatch(r'line\s+"([^"]*)"', line).group(1)
            elif head == "recipe":                  # recipe MOVEMENT SPEED ZONE
                cur["move"], cur["speed"], cur["zone"] = words[1], words[2], words[3]
            elif head == "habit":
                cur["habits"].append((words[1], words[2]))
            elif head == "bubble":
                cur["bubbles"].append(re.fullmatch(r'bubble\s+"([^"]*)"', line).group(1))
            elif head == "tags":
                cur["tags"] = words[1]
            elif head == "price":
                cur["price"] = int(words[1])
            else:
                raise SystemExit("line %d: what is %r?" % (ln, head))
    return blocks


def habit_bytes(ev, ac, where):
    en, _, ea = ev.partition(":")
    an, _, aa = ac.partition(":")
    if en not in EVENTS or an not in ACTIONS:
        raise SystemExit("%s: habit %s %s" % (where, ev, ac))
    if en == "near":
        earg = NEAR[ea or "any"]
    elif en == "time":
        earg = TIMES[ea or "any"]
    elif en == "jam":
        earg = JAMS[ea or "any"]
    else:
        earg = int(ea or 0)
    if an == "particle":
        aarg = PARTS[aa]
    elif an == "walk":
        aarg = ZONES[aa]
    else:
        aarg = int(aa or 0)
    return (EVENTS[en], earg, ACTIONS[an], aarg)


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    show = None
    if "--show" in sys.argv:
        show = sys.argv[sys.argv.index("--show") + 1]
    blocks = read_blocks()
    sprites = [b for b in blocks if b["type"] == "sprite"]
    items = [b for b in blocks if b["type"] == "item"]

    out = []
    out.append("/* Generated by tools/make_jar_art.py from tools/jar_art.txt -- do not edit.")
    out.append(" *")
    out.append(" * The scene's sprites (3 bits a pixel, panel colours, index 0 clear) and")
    out.append(" * Jar Factory's built-in items (apps/jaritem.h's fields, plain RGB565).")
    out.append(" */")
    out.append("#ifndef CARDOS_JAR_ART_H")
    out.append("#define CARDOS_JAR_ART_H")
    out.append("")
    out.append('#include "kernel/app/capp.h"')
    out.append("")
    out.append("typedef struct {")
    out.append("  uint8_t w, h, nframes;")
    out.append("  uint16_t stride;            /* bytes a frame */")
    out.append("  const uint16_t *pal;        /* 8 panel colours, [0] unused */")
    out.append("  const uint8_t *px;")
    out.append("} JSprite;")
    out.append("")
    out.append("enum {")
    for s in sprites:
        out.append("  SPR_%s," % s["name"].upper())
    out.append("  SPR_COUNT")
    out.append("};")
    out.append("")
    total = 0
    for s in sprites:
        where = "sprite %s (line %d)" % (s["name"], s["line_no"])
        letters = [k for k, _ in s["pal"]]
        if len(letters) > 7:
            raise SystemExit("%s: more than 7 colours" % where)
        packed = pack(s["frames"], s["w"], s["h"], letters, where)
        stride = len(packed[0])
        data = b"".join(packed) + b"\0"
        total += len(data) + 16
        pal = ["0"] + ["CAPP_RGB(%d, %d, %d)" % c for _, c in s["pal"]]
        pal += ["0"] * (8 - len(pal))
        out.append("static const uint16_t JPAL_%s[8] = { %s };" % (s["name"], ", ".join(pal)))
        out.append("static const uint8_t JPX_%s[%d] = {" % (s["name"], len(data)))
        for i in range(0, len(data), 16):
            out.append("  " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        out.append("};")
        s["stride"] = stride
    out.append("")
    out.append("static const JSprite SPRITES[SPR_COUNT] = {")
    for s in sprites:
        out.append("  { %d, %d, %d, %d, JPAL_%s, JPX_%s }," % (s["w"], s["h"], len(s["frames"]),
                                                          s["stride"], s["name"], s["name"]))
    out.append("};")
    out.append("")

    # ---- the built-in items ----
    out.append("/* A built-in item: what jitem_encode needs. `text` is the name, the line,")
    out.append(" * the tags, the maker and then each bubble, NUL after each. */")
    out.append("typedef struct {")
    out.append("  uint16_t id, price;        /* price 0: one of the starting items */")
    out.append("  uint8_t kind, nframes, move, speed, zone, nhab, nbub;")
    out.append("  uint8_t hab[3][4];")
    out.append("  uint16_t pal[8];")
    out.append("  const char *text;")
    out.append("  const uint8_t *frames;")
    out.append("} JBuiltin;")
    out.append("")
    out.append("#define JB_MADE %du" % MADE)
    out.append("")
    seen = set()
    for it in items:
        where = "item %d (line %d)" % (it["id"], it["line_no"])
        if it["id"] in seen:
            raise SystemExit("%s: id used twice" % where)
        seen.add(it["id"])
        letters = [k for k, _ in it["pal"]]
        if len(letters) > 7:
            raise SystemExit("%s: more than 7 colours" % where)
        if not 1 <= len(it["frames"]) <= 4:
            raise SystemExit("%s: 1 to 4 frames" % where)
        if len(it["title"]) > 12 or len(it.get("text", "")) > 32:
            raise SystemExit("%s: name over 12 or line over 32" % where)
        if len(it["bubbles"]) > 4 or any(len(b) > 12 for b in it["bubbles"]):
            raise SystemExit("%s: at most 4 bubbles of 12" % where)
        if len(it["habits"]) > 3:
            raise SystemExit("%s: at most 3 habits" % where)
        if len(it.get("tags", "")) > 24:
            raise SystemExit("%s: tags over 24" % where)
        packed = pack(it["frames"], 16, 16, letters, where)
        assert all(len(p) == 96 for p in packed)
        data = b"".join(packed)
        total += len(data) + 60
        it["packed"] = packed
        out.append("static const uint8_t JIF_%d[%d] = {" % (it["id"], len(data)))
        for i in range(0, len(data), 16):
            out.append("  " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
        out.append("};")
    out.append("")
    out.append("#define JB_COUNT %d" % len(items))
    out.append("static const JBuiltin BUILTINS[JB_COUNT] = {")
    for it in items:
        where = "item %d" % it["id"]
        habs = [habit_bytes(e, a, where) for e, a in it["habits"]]
        habs_c = ", ".join("{ %d, %d, %d, %d }" % h for h in habs + [(0, 0, 0, 0)] * (3 - len(habs)))
        pal = [0] + [rgb565(c) for _, c in it["pal"]]
        pal += [0] * (8 - len(pal))
        text = "\\0".join([it["title"], it.get("text", ""), it.get("tags", ""), "Alex"] + it["bubbles"])
        text = text.replace('"', '\\"')
        out.append("  { %d, %d, %d, %d, %d, %d, %d, %d, %d, { %s }," % (
            it["id"], it.get("price", 0), KINDS[it["kind"]], len(it["frames"]),
            MOVES[it["move"]], SPEEDS[it["speed"]], ZONES[it["zone"]], len(habs),
            len(it["bubbles"]), habs_c))
        out.append("    { %s }," % ", ".join("0x%04X" % p for p in pal))
        out.append('    "%s", JIF_%d },' % (text, it["id"]))
    out.append("};")
    out.append("")
    out.append("#endif /* CARDOS_JAR_ART_H */")
    out.append("")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote %s: %d sprites, %d items, about %d bytes" % (
        os.path.relpath(OUT, ROOT), len(sprites), len(items), total))

    if show:
        from PIL import Image
        S = 4
        pics = []
        for b in sprites + items:
            for fr in b["frames"]:
                pics.append((b, fr))
        cols = 10
        cw, ch = 22 * S, 26 * S
        rows = (len(pics) + cols - 1) // cols
        img = Image.new("RGB", (cols * cw, rows * ch), (0x1f, 0x33, 0x40))
        for n, (b, fr) in enumerate(pics):
            ox, oy = (n % cols) * cw + S, (n // cols) * ch + S
            pal = dict(b["pal"])
            for y, row in enumerate(fr):
                for x, c in enumerate(row):
                    if c == ".":
                        continue
                    for dy in range(S):
                        for dx in range(S):
                            img.putpixel((ox + x * S + dx, oy + y * S + dy), pal[c])
        img.save(show)
        print("wrote", show)


if __name__ == "__main__":
    main()
