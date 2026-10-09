"""Photos: dragged onto the dashboard, kept here, synced to the device.

Each picture is kept with the two forms the device needs, made once at
upload so the device never decodes a JPEG or PNG -- it has no decoder and
flash is what it is short of:

    screen.img   RGB565 fitted to 240x135 with bars, for Photos (images.py)
    print.txt    the picture dithered to the printer's 384 dots, as printdoc
                 `%%` lines, and its name under it -- printed by `print FILE`
    thumb.jpg    for the dashboard's grid

In CARDOS_STATE/photos/<id>/, with meta.json. The device's Photos app lists,
downloads what it does not have, forgets what is gone here, and deletes here
when it deletes.

    GET    /photos                 -> id <tab> name <tab> created, newest first
    GET    /photos/img?id=         -> screen.img
    GET    /photos/print?id=       -> print.txt
    DELETE /photos/photo?id=       -> ok
    GET    /dash/photos            -> JSON, for the dashboard
    POST   /dash/photos/upload?name=NAME   body: the picture -> JSON
    GET    /dash/photos/thumb?id=  -> JPEG
    DELETE /dash/photos/photo?id=  -> ok
"""
import io
import json
import os
import secrets
import shutil
import sys
import threading
import time

from . import accounts, images
from .routes import arg, text_route

MAX_IN = 24 << 20
PRINT_W = 384                      # the printer's dots across
PRINT_H_MAX = 480                  # a tall photo is shrunk to this, not run off the roll
REPEAT_MAX = 64                    # printdoc.h: PRINTDOC_LINE_H_MAX
B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_lock = threading.Lock()


def photos_dir():
    return os.path.join(accounts.user_dir(), "photos")


def _dir(pid):
    if not pid or not pid.isalnum() or len(pid) > 16:
        raise ValueError("not a photo id")
    return os.path.join(photos_dir(), pid)


def all_photos():
    out = []
    try:
        names = os.listdir(photos_dir())
    except OSError:
        return out
    for pid in names:
        try:
            with open(os.path.join(photos_dir(), pid, "meta.json"), encoding="utf-8") as f:
                out.append(json.load(f))
        except (OSError, ValueError):
            continue
    out.sort(key=lambda m: m.get("created", 0), reverse=True)
    return out


def clean_name(name):
    """A caption: the file's name without its extension, printable, short."""
    base = os.path.basename(name or "").rsplit(".", 1)[0]
    base = "".join(c for c in base if c.isprintable() and c not in "\t\r\n")
    return (base.strip() or "photo")[:40]


# ---- the print form -------------------------------------------------------------------

def _row_line(bits):
    """One printed row as a `%%` line: runs or raw pixels, whichever is
    shorter (printdoc.h)."""
    runs, x, black = [], 0, False
    while x < PRINT_W:
        n = 0
        while x < PRINT_W and bits[x] == black:
            n += 1
            x += 1
        runs.append(n)
        black = not black
    if len(runs) % 2 == 1:             # ends in white: that needs no saying
        runs.pop()
    as_runs = ",".join(str(r) for r in runs)
    raw = []
    for i in range(0, PRINT_W, 6):
        v = 0
        for b in range(6):
            v = v << 1 | (1 if i + b < PRINT_W and bits[i + b] else 0)
        raw.append(B64[v])
    as_raw = "=" + "".join(raw)
    return as_runs if len(as_runs) <= len(as_raw) else as_raw


def print_doc(im, caption):
    """The picture, dithered to the paper's width, as printdoc lines."""
    from PIL import Image, ImageOps
    g = ImageOps.autocontrast(im.convert("L"), cutoff=1)
    w = PRINT_W
    h = max(1, round(g.height * w / g.width))
    if h > PRINT_H_MAX:
        w = max(1, round(g.width * PRINT_H_MAX / g.height))
        h = PRINT_H_MAX
    g = g.resize((w, h), Image.LANCZOS)
    if w < PRINT_W:                    # centred on the paper
        pad = Image.new("L", (PRINT_W, h), 255)
        pad.paste(g, ((PRINT_W - w) // 2, 0))
        g = pad
    d = g.convert("1")                 # Floyd-Steinberg
    px = d.load()
    lines, prev, count = [], None, 0
    for y in range(h):
        line = _row_line([px[x, y] == 0 for x in range(PRINT_W)])
        if line == prev and count < REPEAT_MAX:
            count += 1
            continue
        if prev is not None:
            lines.append("%%%%%d*%s" % (count, prev) if count > 1 else "%%" + prev)
        prev, count = line, 1
    lines.append("%%%%%d*%s" % (count, prev) if count > 1 else "%%" + prev)
    return "\n".join(lines) + "\n\n" + caption + "\n"


# ---- adding and removing --------------------------------------------------------------

def add(data, name):
    from PIL import Image, ImageOps
    try:
        im = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGB")
    except Exception as e:
        raise ValueError("not a picture this server can read (%s)" % e)
    caption = clean_name(name)
    pid = secrets.token_hex(5)
    d = _dir(pid)
    os.makedirs(d, mode=0o700, exist_ok=True)
    screen, w, h = images.convert(data)
    with open(os.path.join(d, "screen.img"), "wb") as f:
        f.write(screen)
    with open(os.path.join(d, "print.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(print_doc(im, caption))
    thumb = im.copy()
    thumb.thumbnail((320, 320))
    thumb.save(os.path.join(d, "thumb.jpg"), "JPEG", quality=82)
    meta = {"id": pid, "name": caption, "created": int(time.time()),
            "w": im.width, "h": im.height, "bytes": len(screen)}
    with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    sys.stderr.write("photos: %s (%s)\n" % (pid, caption))
    return meta


def remove(pid):
    d = _dir(pid)
    if not os.path.isdir(d):
        return False
    shutil.rmtree(d, ignore_errors=True)
    return True


# ---- routes ---------------------------------------------------------------------------

def _id(args):
    return arg(args, "id")


def _send_file(h, path, ctype):
    if not os.path.isfile(path):
        h.text("error no such photo\n", 404)
        return
    with open(path, "rb") as f:
        data = f.read()
    h.send_response(200)
    h.send_header("Content-Type", ctype)
    h.send_header("Content-Length", str(len(data)))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    h.wfile.write(data)


@text_route
def get_list(h, args):
    """every photo: id, name, when"""
    h.text("".join("%s\t%s\t%d\n" % (m["id"], m["name"], m["created"]) for m in all_photos()))


@text_route
def get_img(h, args):
    """a photo as the device shows it"""
    _send_file(h, os.path.join(_dir(_id(args)), "screen.img"), "application/octet-stream")


@text_route
def get_print(h, args):
    """a photo as the printer prints it"""
    _send_file(h, os.path.join(_dir(_id(args)), "print.txt"), "text/plain; charset=utf-8")


@text_route
def get_thumb(h, args):
    """a small JPEG, for the dashboard"""
    _send_file(h, os.path.join(_dir(_id(args)), "thumb.jpg"), "image/jpeg")


@text_route
def delete_photo(h, args):
    """delete a photo, here and so on the device at its next sync"""
    if not remove(_id(args)):
        h.text("error no such photo\n", 404)
        return
    h.text("ok\n")


def get_dash_list(h, path, args):
    """every photo, as JSON"""
    h._send(200, "application/json", json.dumps(all_photos()), (("Cache-Control", "no-store"),))


def post_upload(h, path, args):
    """a picture, kept and converted"""
    try:
        meta = add(h.body(MAX_IN), (args.get("name") or ["photo"])[0])
    except ValueError as e:
        h._send(400, "application/json", json.dumps({"error": str(e)}), ())
        return
    h._send(200, "application/json", json.dumps(meta), ())


ROUTES = [
    ("GET", "/photos", get_list, "device_or_dash"),
    ("GET", "/photos/img", get_img, "device_or_dash"),
    ("GET", "/photos/print", get_print, "device_or_dash"),
    ("DELETE", "/photos/photo", delete_photo, "device_or_dash"),
    ("GET", "/dash/photos", get_dash_list, "dash"),
    ("POST", "/dash/photos/upload", post_upload, "dash"),
    ("GET", "/dash/photos/thumb", get_thumb, "device_or_dash"),
    ("DELETE", "/dash/photos/photo", delete_photo, "device_or_dash"),
]
