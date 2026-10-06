"""Music: tracks dragged onto the dashboard, kept here, synced to the device.

The device plays PCM WAV and nothing else -- no MP3 decoder, and flash is
what it is short of -- so the dashboard converts in the browser before it
uploads: the browser already decodes MP3, AAC, Ogg and FLAC, and resamples
to 22050 Hz mono 16-bit, which is what a one-watt speaker can use and about
2.6 MB a minute on the card. This file checks what arrives is that kind of
WAV and keeps it; the device's Music app downloads what it does not have.

    GET    /music                  -> id <tab> title <tab> ms <tab> bytes, newest first
    GET    /music/track?id=        -> the WAV
    DELETE /music/track?id=        -> ok
    GET    /dash/music             -> JSON, for the dashboard
    POST   /dash/music/upload?title=TITLE   body: a 16-bit PCM WAV -> JSON
    GET    /dash/music/track?id=   -> the WAV, to listen to on the page
    DELETE /dash/music/track?id=   -> ok
"""
import json
import os
import secrets
import shutil
import struct
import sys
import time

from . import dash, notes

MAX_IN = 60 << 20                  # half an hour at 22050 mono
_route_err = "error %s\n"


def music_dir():
    return os.path.join(dash.state_dir(), "music")


def _dir(tid):
    if not tid or not tid.isalnum() or len(tid) > 16:
        raise ValueError("not a track id")
    return os.path.join(music_dir(), tid)


def wav_info(data):
    """(rate, channels, ms) of a 16-bit PCM WAV, or ValueError saying why
    the device could not play it."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    at, fmt, size = 12, None, None
    while at + 8 <= len(data):
        cid, clen = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, at + 8)
        elif cid == b"data":
            size = min(clen, len(data) - at - 8)
            break
        at += 8 + clen + (clen & 1)
    if not fmt or size is None:
        raise ValueError("a WAV with no format or no audio")
    tag, ch, rate, _, _, bits = fmt
    if tag != 1 or bits != 16 or ch not in (1, 2) or not 8000 <= rate <= 48000:
        raise ValueError("the device plays 16-bit PCM, mono or stereo, 8-48 kHz")
    return rate, ch, size * 1000 // (rate * ch * 2)


def clean_title(name):
    base = os.path.basename(name or "").rsplit(".", 1)[0]
    base = "".join(c for c in base if c.isprintable() and c not in "\t\r\n")
    return (base.strip() or "track")[:60]


def all_tracks():
    out = []
    try:
        names = os.listdir(music_dir())
    except OSError:
        return out
    for tid in names:
        try:
            with open(os.path.join(music_dir(), tid, "meta.json"), encoding="utf-8") as f:
                out.append(json.load(f))
        except (OSError, ValueError):
            continue
    out.sort(key=lambda m: m.get("created", 0), reverse=True)
    return out


def add(data, title):
    rate, ch, ms = wav_info(data)
    tid = secrets.token_hex(5)
    d = _dir(tid)
    os.makedirs(d, mode=0o700, exist_ok=True)
    with open(os.path.join(d, "track.wav"), "wb") as f:
        f.write(data)
    meta = {"id": tid, "title": clean_title(title), "created": int(time.time()),
            "ms": ms, "bytes": len(data), "rate": rate, "channels": ch}
    with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    sys.stderr.write("music: %s (%s, %d s)\n" % (tid, meta["title"], ms // 1000))
    return meta


def remove(tid):
    d = _dir(tid)
    if not os.path.isdir(d):
        return False
    shutil.rmtree(d, ignore_errors=True)
    return True


# ---- routes ---------------------------------------------------------------------------

def _route(fn):
    def wrapped(h, path, args):
        if not notes._allowed(h):
            return
        try:
            fn(h, args)
        except ValueError as e:
            h.text(_route_err % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _id(args):
    return (args.get("id") or [""])[0]


@_route
def get_list(h, args):
    """every track: id, title, length, size"""
    h.text("".join("%s\t%s\t%d\t%d\n" % (m["id"], m["title"], m["ms"], m["bytes"])
                   for m in all_tracks()))


@_route
def get_track(h, args):
    """a track's WAV"""
    path = os.path.join(_dir(_id(args)), "track.wav")
    if not os.path.isfile(path):
        h.text("error no such track\n", 404)
        return
    h.send_response(200)
    h.send_header("Content-Type", "audio/wav")
    h.send_header("Content-Length", str(os.path.getsize(path)))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    with open(path, "rb") as f:          # streamed: a track is megabytes
        while True:
            chunk = f.read(64 * 1024)
            if not chunk:
                break
            h.wfile.write(chunk)


@_route
def delete_track(h, args):
    """delete a track, here and so on the device at its next sync"""
    if not remove(_id(args)):
        h.text("error no such track\n", 404)
        return
    h.text("ok\n")


def get_dash_list(h, path, args):
    """every track, as JSON"""
    if not dash.logged_in(h):
        h._send(403, "application/json", '{"error": "signed out"}', ())
        return
    h._send(200, "application/json", json.dumps(all_tracks()), (("Cache-Control", "no-store"),))


def post_upload(h, path, args):
    """a track the page converted, kept"""
    if not dash.logged_in(h):
        h._send(403, "application/json", '{"error": "signed out"}', ())
        return
    try:
        meta = add(h.body(MAX_IN), (args.get("title") or ["track"])[0])
    except ValueError as e:
        h._send(400, "application/json", json.dumps({"error": str(e)}), ())
        return
    h._send(200, "application/json", json.dumps(meta), ())


ROUTES = [
    ("GET", "/music", get_list, "open"),
    ("GET", "/music/track", get_track, "open"),
    ("DELETE", "/music/track", delete_track, "open"),
    ("GET", "/dash/music", get_dash_list, "open"),
    ("POST", "/dash/music/upload", post_upload, "open"),
    ("GET", "/dash/music/track", get_track, "open"),
    ("DELETE", "/dash/music/track", delete_track, "open"),
]
