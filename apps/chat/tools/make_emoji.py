"""Chat's dog emojis: Arty and Marcy, as pixel art.

Each dog is one 18x18 face, placed by hand a pixel at a time (`ARTY`,
`MARCY`, a letter a pixel, `PAL` says which colour), and each mood is
stamps over it: eyes, a mouth, a mark in the gap above the head. Then
every empty pixel touching the dog becomes the outline, because a black
dog on Chat's near-black background is otherwise not there. On the
device a sprite pixel is 2x2, so an emoji is 36x36: four chat rows.

    python apps/chat/tools/make_emoji.py --preview out.html   every emoji, big and at device size
    python apps/chat/tools/make_emoji.py                      apps/chat/chat_emoji.h, what Chat draws

Arty is the small terrier: black head, tall pointed ears tan inside, tan
spots over the eyes, tan cheeks, a grizzled bridge, white chest, blue
harness. Marcy is the black one: folded ears, the white blaze, a white
muzzle with grey in it, grey brows, white chest.
"""
import argparse
import base64
import io
import os

from PIL import Image

N = 18                    # the dog: sprite pixels a side
W = N + 4                 # and a margin on the right for the mood's mark
SCALE = 2                 # device pixels a sprite pixel
CHAT_BG = (18, 20, 26)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUTLINE = (112, 118, 138)

MOODS = ["happy", "love", "sad", "sleepy", "hungry", "grumpy", "zoomies", "huh"]

# One palette; L (the line drawn over black fur) is each dog's own.
PAL = {
    "K": (34, 32, 38),     # fur
    "k": (70, 68, 80),     # fur, the fold of an ear
    "T": (196, 130, 66),   # tan
    "t": (226, 174, 112),  # light tan
    "g": (120, 116, 124),  # grizzle, grey
    "W": (244, 242, 236),  # white
    "w": (196, 196, 202),  # off-white
    "B": (30, 156, 214),   # harness
    "N": (14, 12, 14),     # nose, mouth line
    "n": (110, 110, 122),  # the shine on a nose
    "e": (122, 72, 40),    # iris
    "E": (14, 10, 10),     # pupil
    "H": (255, 255, 255),  # shine, marks
    "P": (240, 110, 136),  # tongue
    "M": (110, 30, 46),    # inside a mouth
    "R": (232, 54, 80),    # hearts, anger
    "D": (110, 192, 255),  # tears, drool
}

ARTY = dict(name="arty", line=(255, 230, 186), grid=[
    "..................",
    "..K............K..",
    "..KK..........KK..",
    "..KTK........KTK..",
    "..KTTK......KTTK..",
    "..KTTKKKKKKKKTTK..",
    "..KKKTTKKKKTTKKK..",
    "...KKKKKKKKKKKK...",
    "...KKeeKKKKeeKK...",
    "...KKeeKkkKeeKK...",
    "...TTKKKggKKKTT...",
    "...TTTttNnttTTT...",
    "....TTttttttTT....",
    ".....TttttttT.....",
    "....WWWttttWWW....",
    "...WBBWWWWWWBBW...",
    "..WWWBBBBBBBBWWW..",
    "..................",
])

MARCY = dict(name="marcy", line=(214, 216, 226), grid=[
    "..................",
    "..................",
    "..................",
    ".....KKKKKKKK.....",
    "...kkKKKwwKKKkk...",
    "..kkkkKKwwKKkkkk..",
    ".kkkkKKKWWKKKkkkk.",
    ".kkKKggKWWKggKKkk.",
    ".kkKKeeKWWKeeKKkk.",
    ".kk.KeeKWWKeeK.kk.",
    ".kk.KKKWWWWKKK.kk.",
    "....KKWWNnWWKK....",
    "....KgWWWWWWgK....",
    ".....KwWWWWwK.....",
    "......wWWWWw......",
    "....WWWWWWWWWW....",
    "...WWWWWWWWWWWW...",
    "..................",
])

EYE_ROW, EYE_COL = 8, 5          # the left eye's top-left; the right is mirrored
MOUTH_ROW, MOUTH_COL = 12, 6     # under the nose, six wide


def blank(d):
    return [list(r + "." * (W - N)) for r in d["grid"]]


def stamp(g, r, c, rows, mirror=False):
    """Put `rows` at (r, c); '.' leaves what is there. With mirror, also put
    it flipped at the mirrored place for the right eye."""
    for i, row in enumerate(rows):
        for j, ch in enumerate(row):
            if ch != ".":
                g[r + i][c + j] = ch
    if mirror:
        w = len(rows[0])
        stamp(g, r, N - 1 - (c + w - 1), [row[::-1] for row in rows])


def dots(g, pts, ch):
    for r, c in pts:
        g[r][c] = ch


def mirror_pts(pts):
    return pts + [(r, N - 1 - c) for r, c in pts]


def eyes(g, rows):
    stamp(g, EYE_ROW, EYE_COL, rows)
    stamp(g, EYE_ROW, N - 1 - EYE_COL - 1, rows)       # the same, not flipped: one light


MOUTHS = {
    "smile": ["tNttNt", "ttNNtt"],
    "grin":  [".MMMM.", ".MPPM.", "..PP.."],
    "frown": ["..NN..", ".N..N."],
    "flat":  [".NNNN."],
    "small": ["..NN.."],
    "lick":  ["tNttNt", "ttNNPP", "....PD"],
    "flap":  [".MMMM.", ".MMPPP", "....PPP"],
}


def mouth(g, kind):
    rows = MOUTHS[kind]
    # 't' in a mouth means "the muzzle here": Marcy's is white.
    out = []
    for i, row in enumerate(rows):
        base = g[MOUTH_ROW + i][MOUTH_COL:MOUTH_COL + len(row)]
        out.append("".join(base[j] if ch == "t" else ch for j, ch in enumerate(row)))
    stamp(g, MOUTH_ROW, MOUTH_COL, out)


MARKS = {   # beside the head, top right, at (row, col); no outline
    "love":    (0, 17, [".R.R.", "RRRRR", "RRRRR", ".RRR.", "..R.."]),
    "sleepy":  (0, 18, ["HHHH", "..H.", ".H..", "HHHH", "....", "HHH.", ".H..", "HHH."]),
    "grumpy":  (0, 17, [".R.R.", "RR.RR", ".....", "RR.RR", ".R.R."]),
    "huh":     (0, 18, [".HH.", "H..H", "..H.", ".H..", "....", ".H.."]),
}


def draw(d, mood):
    """The emoji as rows of letters, outline included ('O')."""
    g = blank(d)
    if mood == "happy":
        stamp(g, EYE_ROW, 4, [".LL.", "LKKL"], mirror=True)
        mouth(g, "grin")
    elif mood == "love":
        stamp(g, 7, 4, ["RKR", "RRR", "KRK"], mirror=True)
        mouth(g, "smile")
    elif mood == "sad":
        eyes(g, ["He", "eH"])
        dots(g, mirror_pts([(7, 4), (6, 5), (6, 6)]), "L")
        dots(g, [(10, 5), (11, 5)], "D")
        mouth(g, "frown")
    elif mood == "sleepy":
        stamp(g, EYE_ROW, 4, ["LKKL", ".LL."], mirror=True)
        mouth(g, "small")
    elif mood == "hungry":
        eyes(g, ["eH", "He"])
        mouth(g, "lick")
    elif mood == "grumpy":
        eyes(g, ["KK", "eE"])
        dots(g, mirror_pts([(6, 4), (7, 5), (7, 6)]), "L")
        mouth(g, "flat")
    elif mood == "zoomies":
        stamp(g, 7, 4, ["HHH", "HEH", "HHH"], mirror=True)
        mouth(g, "flap")
        dots(g, [(8, 19), (8, 20), (8, 21), (10, 18), (10, 19), (10, 20), (10, 21), (12, 20), (12, 21)], "D")
    elif mood == "huh":
        eyes(g, ["eH", "eE"])
        dots(g, [(7, 5), (7, 6), (6, 10), (5, 11), (5, 12), (6, 13)], "L")
        mouth(g, "small")
    else:
        raise ValueError(mood)
    # The outline: every empty pixel beside the dog. Marks come after and
    # need none: they are white or red, on the dark.
    filled = [[c < N and ch != "." for c, ch in enumerate(row)] for row in g]
    for r in range(N):
        for c in range(W):
            if not filled[r][c] and g[r][c] == "." and any(
                    0 <= r + dr < N and 0 <= c + dc < W and filled[r + dr][c + dc]
                    for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                g[r][c] = "O"
    if mood in MARKS:
        r, c, rows = MARKS[mood]
        stamp(g, r, c, rows)
    return ["".join(row) for row in g]


DOGS = (ARTY, MARCY)


def rgb(d, ch):
    if ch == "L":
        return d["line"]
    if ch == "O":
        return OUTLINE
    return PAL[ch]


def image(d, mood, scale=SCALE, bg=CHAT_BG):
    rows = draw(d, mood)
    im = Image.new("RGB", (W, N), bg)
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch != ".":
                im.putpixel((c, r), rgb(d, ch))
    return im.resize((W * scale, N * scale), Image.NEAREST)


# ---- the header Chat includes -----------------------------------------------------------

HEADER = os.path.join(ROOT, "apps", "chat", "chat_emoji.h")


def rgb565(c):
    """As CAPP_RGB makes it: RGB565 with its bytes swapped, the panel's
    order, which api->pixels sends as it is."""
    v = (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3)
    return (v >> 8) | (v & 0xFF) << 8


def header():
    """Every sprite as palette indices (0 is nothing), one palette for both
    dogs: 16 sprites of 396 bytes, constants, so in flash."""
    pal, index, sprites = [None], {}, []
    for d in DOGS:
        for m in MOODS:
            px = []
            for row in draw(d, m):
                for ch in row:
                    if ch == ".":
                        px.append(0)
                        continue
                    c = rgb(d, ch)
                    if c not in index:
                        index[c] = len(pal)
                        pal.append(c)
                    px.append(index[c])
            sprites.append(px)
    assert len(pal) <= 256
    out = ["/* Generated by apps/chat/tools/make_emoji.py from the pixel art there -- do not edit;",
           " * change the drawing and run it again. */",
           "#ifndef CARDOS_CHAT_EMOJI_DATA_H", "#define CARDOS_CHAT_EMOJI_DATA_H", "",
           "#include <stdint.h>", "",
           "#define EMO_W %d" % W, "#define EMO_H %d" % N,
           "#define EMO_DOGS %d" % len(DOGS), "#define EMO_MOODS %d" % len(MOODS), "",
           "static const char EMO_DOG[EMO_DOGS][8] = { %s };" % ", ".join('"%s"' % d["name"] for d in DOGS),
           "static const char EMO_MOOD[EMO_MOODS][8] = { %s };" % ", ".join('"%s"' % m for m in MOODS), "",
           "/* RGB565 in the panel's byte order, as CAPP_RGB makes it; [0] is never drawn. */",
           "static const uint16_t EMO_PAL[%d] = {" % len(pal)]
    vals = ["0x0000"] + ["0x%04X" % rgb565(c) for c in pal[1:]]
    for i in range(0, len(vals), 8):
        out.append("  " + ", ".join(vals[i:i + 8]) + ",")
    out += ["};", "", "/* [dog * EMO_MOODS + mood][row * EMO_W + col] */",
            "static const uint8_t EMO_PIX[EMO_DOGS * EMO_MOODS][EMO_W * EMO_H] = {"]
    k = 0
    for d in DOGS:
        for m in MOODS:
            out.append("  { /* %s %s */" % (d["name"], m))
            px = sprites[k]
            k += 1
            for r in range(N):
                out.append("    " + ",".join("%d" % v for v in px[r * W:(r + 1) * W]) + ",")
            out.append("  },")
    out += ["};", "", "#endif", ""]
    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))


# ---- the preview ---------------------------------------------------------------------

def png64(im):
    b = io.BytesIO()
    im.save(b, "PNG")
    return "data:image/png;base64," + base64.b64encode(b.getvalue()).decode()


def preview(path):
    rows = []
    for d in (ARTY, MARCY):
        cells = []
        for m in MOODS:
            cells.append(
                '<figure><img class="big" src="%s"><div class="dev"><img src="%s" width="44" height="36"></div>'
                '<figcaption>:%s-%s:</figcaption></figure>'
                % (png64(image(d, m, 10)), png64(image(d, m)), d["name"], m))
        rows.append('<h2>%s</h2><div class="grid">%s</div>' % (d["name"].title(), "".join(cells)))
    chat = []
    for who, d, m, line in (("alex", None, None, "who wants dinner?"), ("home", ARTY, "hungry", None),
                            ("alex", MARCY, "sleepy", None), ("home", None, None, "she is always asleep"),
                            ("alex", MARCY, "grumpy", None)):
        if d is None:
            chat.append('<div class="ln"><b>%s:</b> %s</div>' % (who, line))
        else:
            chat.append('<div class="ln pic"><b>%s:</b> <img src="%s" width="44" height="36"></div>'
                        % (who, png64(image(d, m))))
    html = """<!doctype html><meta charset=utf-8><title>Dog emojis</title>
<style>
body{background:#12141a;color:#e2e6f0;font:15px system-ui,sans-serif;margin:24px}
h1{font-size:22px;margin:0 0 4px}p{color:#848c9e;margin:0 0 20px;max-width:760px}
h2{font-size:18px;margin:28px 0 10px}
img{image-rendering:pixelated}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,220px));gap:18px}
figure{margin:0;background:#12141a;border:1px solid #2a2e3a;border-radius:12px;padding:12px;text-align:center}
.big{width:198px;height:162px}
.dev{margin-top:8px}
figcaption{font:13px ui-monospace,monospace;color:#9aa4ba;margin-top:6px}
.screen{width:240px;height:135px;transform:scale(2.5);transform-origin:0 0;background:#12141a;
 outline:1px solid #333;font:8px/9px ui-monospace,monospace;overflow:hidden;margin-bottom:220px}
.bar{background:#28466e;height:10px;padding-left:2px;line-height:10px}
.ln{padding-left:2px;color:#e2e6f0}.ln b{color:#8cd296;font-weight:normal}
.pic{height:36px;display:flex;align-items:center;gap:4px}
@media(max-width:900px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
</style>
<h1>Arty &amp; Marcy</h1>
<p>Pixel art, 22&times;18 (the dog 18&times;18, the mood's mark beside it), each sprite pixel 2&times;2 on the Cardputer. Big, then at real device size,
on Chat's own background. Underneath, a mock of the chat screen at 2.5&times;.</p>
%s
<h2>In the chat</h2>
<div class="screen"><div class="bar">Chat  alex</div>%s</div>
""" % ("".join(rows), "".join(chat))
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", metavar="HTML")
    a = ap.parse_args()
    if a.preview:
        preview(a.preview)
        print("wrote", a.preview)
    else:
        header()
        print("wrote", os.path.relpath(HEADER, ROOT))


if __name__ == "__main__":
    main()
