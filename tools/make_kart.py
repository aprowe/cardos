"""Kart's tables: the track, the sprites, the sines.

    python tools/make_kart.py            writes apps/kart_data.h
    python tools/make_kart.py --show x.png   and a picture of the track

The track is a loop of control points through a 1024x1024 world; a
Catmull-Rom spline through them is cut into NWP waypoints an even distance
apart. The device paints the road from those (apps/kart.h, k_build), and
the karts follow them: they are the racing line, the lap counter and the
order of the race. The checks here refuse a track whose road would run into
itself.

Sprites are 16x16, four bits a pixel, index 0 clear, drawn as text below so
they can be changed by eye. A kart's body is index 1, which each racer
swaps for its own colour.
"""
import math
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "apps", "kart_data.h")

NWP = 256            # waypoints round the loop
ROAD_R = 30          # road half-width in texels (the device paints this)

# A circuit in the spirit of Mario Circuit 1: a long straight, a hairpin, an
# S, a sweeper. Clockwise from the start line on the left straight, going up.
CONTROL = [
    (150, 760), (150, 520), (160, 300), (210, 170), (330, 120), (450, 160),
    (500, 270), (470, 390), (520, 480), (640, 470), (720, 380), (760, 250),
    (840, 160), (920, 210), (925, 360), (880, 470), (870, 590), (930, 700),
    (900, 840), (760, 900), (620, 860), (540, 760), (440, 720), (340, 820),
    (240, 900), (160, 880),
]


def catmull(p0, p1, p2, p3, t):
    t2, t3 = t * t, t * t * t
    return tuple(0.5 * ((2 * p1[i]) + (-p0[i] + p2[i]) * t +
                        (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t2 +
                        (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * t3) for i in range(2))


def spline(points, per=200):
    n = len(points)
    out = []
    for i in range(n):
        p0, p1, p2, p3 = points[i - 1], points[i], points[(i + 1) % n], points[(i + 2) % n]
        for k in range(per):
            out.append(catmull(p0, p1, p2, p3, k / per))
    return out


def resample(dense, n):
    lens = [0.0]
    for i in range(1, len(dense) + 1):
        a, b = dense[i - 1], dense[i % len(dense)]
        lens.append(lens[-1] + math.dist(a, b))
    total = lens[-1]
    out, j = [], 0
    for k in range(n):
        want = total * k / n
        while lens[j + 1] < want:
            j += 1
        a, b = dense[j], dense[(j + 1) % len(dense)]
        f = (want - lens[j]) / (lens[j + 1] - lens[j] or 1)
        out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
    return out, total


def check(wps):
    """No two stretches of road that are not neighbours along the loop may
    come within a road's width plus a margin of each other."""
    n = len(wps)
    worst = (1e9, 0, 0)
    for i in range(n):
        for j in range(i + 1, n):
            gap = min(abs(i - j), n - abs(i - j))
            if gap < 40:
                continue
            d = math.dist(wps[i], wps[j])
            if d < worst[0]:
                worst = (d, i, j)
    return worst


# ---- sprites ------------------------------------------------------------------

# Index: 0 clear, 1 body (the racer's colour), 2 body shade, 3 black, 4 grey,
# 5 skin, 6 white, 7 red, 8 yellow, 9 brown, A green, B dark green,
# C light blue, D orange, E dark grey, F light grey.
PALETTE = [
    (0, 0, 0), (220, 40, 40), (140, 20, 20), (16, 16, 20), (110, 110, 120),
    (250, 200, 160), (250, 250, 250), (230, 30, 30), (250, 210, 40),
    (130, 80, 30), (60, 170, 60), (30, 110, 40), (120, 190, 250), (250, 140, 30),
    (60, 60, 70), (190, 190, 200),
]

SPRITES = {
    # A kart from behind: the driver's helmet, the seat, two fat wheels.
    "kart_back": """
................
......6666......
.....611116.....
.....611116.....
......5555......
....11111111....
...1122222211...
..133333333331..
.33311111111333.
.33311111111333.
.33312222221333.
.33344444444333.
..333.4444.333..
................
................
................""",
    # Three-quarters from behind, facing right.
    "kart_q": """
................
.......6666.....
......611116....
......611116....
.......5555.....
.....11111111...
...111222221111.
..1333333333311.
.333111111113333
.333111111113333
.333122222213333
..3344444444433.
..333.44444.333.
................
................
................""",
    # From the side, facing right.
    "kart_side": """
................
........666.....
.......61116....
.......61116....
........555.....
...11111111111..
..1122222222211.
.11111111111111.
.1111111111111D.
..333......333..
.33333....33333.
.33E33....33E33.
..333......333..
................
................
................""",
    # From the front.
    "kart_front": """
................
......6666......
.....611116.....
.....655556.....
......5555......
....11111111....
...1166666611...
..133333333331..
.33311111111333.
.33311888811333.
.33311111111333.
.33344444444333.
..333.4444.333..
................
................
................""",
    "box": """
................
.888888888888...
.8DDDDDDDDDD8...
.8D66666666D8...
.8D6666666.D8...
.8D66.66.6.D8...
.8D6666.66.D8...
.8D666.66.6D8...
.8D66.6666.D8...
.8D666666..D8...
.8D666.66..D8...
.8D66666666D8...
.8DDDDDDDDDD8...
.888888888888...
................
................""",
    "banana": """
................
.........99.....
.........88.....
........888.....
.......8888.....
......8888......
.....88888......
....88888.......
...88888........
..88888.........
..8888..........
..888...........
...88...........
................
................
................""",
    "coin": """
................
.....8888.......
....888888......
...88866888.....
...88868888.....
...88868888.....
...88868888.....
...88868888.....
...88866888.....
....888888......
.....8888.......
................
................
................
................
................""",
    "tree": """
......AAAA......
....AAAAAAAA....
...AAAABAAAAA...
..AAABAAAAABAA..
..AAAAAAAABAAA..
.AABAAAAAAAAAAA.
.AAAAAAAABAAAAA.
.AAAAAABAAAAABA.
..AABAAAAAAAAA..
..AAAAAAAABAA...
...AAAAAAAAA....
.....BB9BB......
.......99.......
.......99.......
.......99.......
......9999......""",
    "mushroom": """
................
.....777777.....
...7766667777...
..777666677777..
..766777777667..
.77666777776677.
.77777777777777.
..6655555556...
....5535355.....
....5555555.....
....5555555.....
.....55555......
................
................
................
................""",
}
ORDER = ["kart_back", "kart_q", "kart_side", "kart_front", "box", "banana", "coin", "tree", "mushroom"]


def sprite_bytes(art):
    rows = [r for r in art.strip("\n").split("\n")]
    assert len(rows) == 16, len(rows)
    out = []
    for r in rows:
        r = (r + "." * 16)[:16]
        vals = [0 if c == "." else int(c, 16) for c in r]
        for k in range(0, 16, 2):
            out.append(vals[k] << 4 | vals[k + 1])
    return out



def main():
    dense = spline(CONTROL)
    wps, total = resample(dense, NWP)
    d, i, j = check(wps)
    print("track length %.0f texels, %d waypoints %.1f apart; closest pass %.0f (wp %d/%d)"
          % (total, NWP, total / NWP, d, i, j))
    if d < 2 * ROAD_R + 24:
        sys.exit("the road runs into itself near waypoints %d and %d" % (i, j))
    xs = [p[0] for p in wps]
    ys = [p[1] for p in wps]
    if min(xs) < ROAD_R + 40 or min(ys) < ROAD_R + 40 or max(xs) > 1024 - ROAD_R - 40 or max(ys) > 1024 - ROAD_R - 40:
        sys.exit("the road leaves the world")

    L = []
    L.append("/* Kart's tables, from tools/make_kart.py -- do not edit. */")
    L.append("#ifndef KART_DATA_H\n#define KART_DATA_H\n")
    L.append("#define K_NWP %d\n#define K_ROAD_R %d\n#define K_TRACK_LEN %d\n" % (NWP, ROAD_R, int(total)))
    L.append("/* The racing line: x, y in texels, %d apart. Waypoint 0 is the start. */" % int(total / NWP))
    L.append("static const int16_t K_WP[K_NWP][2] = {")
    for k in range(0, NWP, 6):
        L.append("  " + " ".join("{%d,%d}," % (round(p[0]), round(p[1])) for p in wps[k:k + 6]))
    L.append("};\n")
    # sine, Q14, a quarter wave in 256 steps (angles are 1024 to the turn)
    L.append("/* sin, Q14, a quarter wave: angles are 0..1023 for a full turn. */")
    L.append("static const int16_t K_SIN[257] = {")
    vals = [round(16384 * math.sin(math.pi / 2 * k / 256)) for k in range(257)]
    for k in range(0, 257, 12):
        L.append("  " + ", ".join(str(v) for v in vals[k:k + 12]) + ",")
    L.append("};\n")
    L.append("/* The sprites' sixteen colours, as r, g, b: CAPP_RGB makes them at start. */")
    L.append("static const uint8_t K_SPR_RGB[16][3] = {")
    L.append("  " + " ".join("{%d,%d,%d}," % c for c in PALETTE))
    L.append("};\n")
    L.append("enum { " + ", ".join("SPR_" + n.upper() for n in ORDER) + ", SPR_COUNT };")
    L.append("static const uint8_t K_SPR[SPR_COUNT][128] = {")
    for n in ORDER:
        b = sprite_bytes(SPRITES[n])
        L.append("  { /* %s */" % n)
        for k in range(0, 128, 16):
            L.append("    " + ", ".join("0x%02X" % v for v in b[k:k + 16]) + ",")
        L.append("  },")
    L.append("};\n")
    L.append("#endif")
    open(OUT, "w", newline="\n").write("\n".join(L) + "\n")
    print("wrote", OUT)

    if "--show" in sys.argv:
        from PIL import Image, ImageDraw
        img = Image.new("RGB", (512, 512), (60, 150, 60))
        dr = ImageDraw.Draw(img)
        for k in range(NWP):
            x, y = wps[k][0] / 2, wps[k][1] / 2
            r = ROAD_R / 2
            dr.ellipse((x - r - 1, y - r - 1, x + r + 1, y + r + 1), fill=(220, 220, 220))
        for k in range(NWP):
            x, y = wps[k][0] / 2, wps[k][1] / 2
            r = ROAD_R / 2
            dr.ellipse((x - r, y - r, x + r, y + r), fill=(110, 110, 120))
        dr.line([(p[0] / 2, p[1] / 2) for p in wps] + [(wps[0][0] / 2, wps[0][1] / 2)], fill=(255, 255, 0))
        dr.ellipse((wps[0][0] / 2 - 4, wps[0][1] / 2 - 4, wps[0][0] / 2 + 4, wps[0][1] / 2 + 4), fill=(255, 0, 0))
        img.save(sys.argv[sys.argv.index("--show") + 1])


if __name__ == "__main__":
    main()
