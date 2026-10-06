"""Music: tracks dragged onto the dashboard, kept here, synced to the device.

The device plays PCM WAV and nothing else -- no MP3 decoder, and flash is
what it is short of -- so what arrives here is converted: ffmpeg turns MP3,
AAC, Ogg, FLAC or anything else it reads into 22050 Hz mono 16-bit, loudness
evened out, which is what a one-watt speaker can use and about 2.6 MB a
minute on the card. A WAV the device can already play is kept as it is.
Without ffmpeg only such a WAV is taken, and the dashboard falls back to
converting in the browser. The device's Music app downloads what it does
not have.

    GET    /music                  -> id <tab> title <tab> ms <tab> bytes, newest first
    GET    /music/track?id=        -> the WAV
    DELETE /music/track?id=        -> ok
    POST   /music/upload?title=    body: any audio -> id <tab> title <tab> ms  (bearer)
    GET    /dash/music             -> JSON, for the dashboard
    POST   /dash/music/upload?title=TITLE   body: any audio -> JSON
    GET    /dash/music/track?id=   -> the WAV, to listen to on the page
    DELETE /dash/music/track?id=   -> ok
"""
import json
import os
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from . import dash, notes

MAX_IN = 60 << 20                  # half an hour at 22050 mono, or a long MP3
RATE = 22050
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


def ffmpeg():
    return shutil.which("ffmpeg")


def convert(data):
    """Any audio ffmpeg reads, as the WAV the device wants: 22050 Hz mono
    16-bit, loudness-normalised so tracks from different places play at
    one volume. ValueError if it cannot be read."""
    exe = ffmpeg()
    if not exe:
        raise ValueError("this server has no ffmpeg; upload a 16-bit WAV")
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "in"), os.path.join(d, "out.wav")
        with open(src, "wb") as f:
            f.write(data)
        r = subprocess.run([exe, "-v", "error", "-y", "-i", src, "-vn", "-ac", "1",
                            "-af", "loudnorm=I=-14:TP=-1.5:LRA=11", "-ar", str(RATE),
                            "-c:a", "pcm_s16le", "-map_metadata", "-1", "-fflags", "+bitexact", out],
                           capture_output=True, timeout=600)
        if r.returncode != 0 or not os.path.exists(out):
            why = r.stderr.decode("utf-8", "replace").strip().splitlines()
            raise ValueError("could not read it as audio" + (": " + why[-1][:80] if why else ""))
        with open(out, "rb") as f:
            return f.read()


def playable(data):
    """The WAV to keep: as it came if the device plays it, else converted."""
    try:
        wav_info(data)
        return data
    except ValueError:
        return convert(data)


def add(data, title):
    data = playable(data)
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


def post_music_upload(h, path, args):
    """a track in any format, converted and kept (the device's token)"""
    if not notes._allowed(h):
        return
    try:
        meta = add(h.body(MAX_IN), (args.get("title") or ["track"])[0])
    except ValueError as e:
        h.text(_route_err % e, 400)
        return
    h.text("%s\t%s\t%d\n" % (meta["id"], meta["title"], meta["ms"]))


def get_dash_list(h, path, args):
    """every track, as JSON"""
    if not dash.logged_in(h):
        h._send(403, "application/json", '{"error": "signed out"}', ())
        return
    h._send(200, "application/json", json.dumps(all_tracks()), (("Cache-Control", "no-store"),))


def post_upload(h, path, args):
    """a track, converted here if it needs it, kept"""
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
    ("POST", "/music/upload", post_music_upload, "open"),
    ("GET", "/dash/music", get_dash_list, "open"),
    ("POST", "/dash/music/upload", post_upload, "open"),
    ("GET", "/dash/music/track", get_track, "open"),
    ("DELETE", "/dash/music/track", delete_track, "open"),
]
