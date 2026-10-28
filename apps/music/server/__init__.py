"""Music: tracks dragged onto the dashboard, kept here, synced to the device.

The device decodes MP3 itself (2026-10-12, kernel/sys/mp3stream.c), so what
arrives is made into one: ffmpeg turns MP3, AAC, Ogg, FLAC, WAV or anything
else it reads into a 44.1 kHz mono MP3 at 96 kbps, loudness evened out --
the speaker is one channel, and that is 0.7 MB a minute on the card where
the 22050 Hz WAV it used to be was 2.6. The title and artist come from the
file's tags when it has them. Without ffmpeg only a WAV the device can play
is taken, as before, and the dashboard falls back to converting in the
browser. Tracks kept as WAV before this stay WAV until `python -m
apps.music.server mp3` makes them MP3 (the device then fetches the new file and
deletes the old). The device's Music app downloads what it does not have.

    GET    /music                  -> id <tab> title <tab> ms <tab> bytes <tab> mp3|wav
                                      <tab> artist, newest first (older apps read four)
    GET    /music/track?id=        -> the MP3 (or WAV)
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

from server import accounts
from server.routes import arg, text_route

MAX_IN = 60 << 20                  # half an hour at 22050 mono, or a long MP3
RATE = 22050                       # a WAV made in the browser
MP3_RATE, MP3_KBPS = 44100, 96
_route_err = "error %s\n"


def music_dir():
    return os.path.join(accounts.user_dir(), "music")


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


def ffprobe():
    return shutil.which("ffprobe")


def _clean(s, n=60):
    return "".join(c for c in (s or "") if c.isprintable() and c not in "\t\r\n").strip()[:n]


def probe(path):
    """(ms, tags) of an audio file, by ffprobe: tags lower-cased. (0, {})
    when there is no ffprobe or it cannot say."""
    exe = ffprobe()
    if not exe:
        return 0, {}
    r = subprocess.run([exe, "-v", "error", "-of", "json", "-show_entries",
                        "format=duration:format_tags", path], capture_output=True, timeout=60)
    try:
        fmt = json.loads(r.stdout.decode("utf-8", "replace")).get("format", {})
    except ValueError:
        return 0, {}
    tags = {k.lower(): v for k, v in (fmt.get("tags") or {}).items()}
    try:
        ms = int(float(fmt.get("duration", 0)) * 1000)
    except ValueError:
        ms = 0
    return ms, tags


def convert(data):
    """Any audio ffmpeg reads, as the MP3 the device plays: 44.1 kHz mono,
    96 kbps, loudness-normalised so tracks from different places play at
    one volume, no tags (the device reads none; they are in meta.json).
    (mp3 bytes, ms, tags of the original). ValueError if it cannot be read."""
    exe = ffmpeg()
    if not exe:
        raise ValueError("this server has no ffmpeg; upload a 16-bit WAV")
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "in"), os.path.join(d, "out.mp3")
        with open(src, "wb") as f:
            f.write(data)
        _, tags = probe(src)
        # Loudness evened out -- except that silence through loudnorm is
        # NaNs, which LAME asserts on, so then once more without it.
        for af in (["-af", "loudnorm=I=-14:TP=-1.5:LRA=11"], []):
            r = subprocess.run([exe, "-v", "error", "-y", "-i", src, "-vn", "-ac", "1"] + af +
                               ["-ar", str(MP3_RATE), "-c:a", "libmp3lame", "-b:a", "%dk" % MP3_KBPS,
                                "-map_metadata", "-1", "-id3v2_version", "0", "-write_xing", "1",
                                "-fflags", "+bitexact", out],
                               capture_output=True, timeout=600)
            if r.returncode == 0 and os.path.exists(out):
                break
        if r.returncode != 0 or not os.path.exists(out):
            why = r.stderr.decode("utf-8", "replace").strip().splitlines()
            raise ValueError("could not read it as audio" + (": " + why[-1][:80] if why else ""))
        ms, _ = probe(out)
        with open(out, "rb") as f:
            mp3 = f.read()
    if not ms:
        ms = len(mp3) * 8 // MP3_KBPS
    return mp3, ms, tags


def playable(data):
    """What to keep: (bytes, "mp3"|"wav", ms, tags) -- an MP3 made by
    convert, or with no ffmpeg a WAV the device plays as it came."""
    if not ffmpeg():
        _, _, ms = wav_info(data)
        return data, "wav", ms, {}
    mp3, ms, tags = convert(data)
    return mp3, "mp3", ms, tags


def track_path(tid, meta=None):
    meta = meta if meta is not None else _meta(tid)
    return os.path.join(_dir(tid), "track." + (meta.get("fmt") or "wav"))


def _meta(tid):
    try:
        with open(os.path.join(_dir(tid), "meta.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def add(data, title):
    data, fmt, ms, tags = playable(data)
    tid = secrets.token_hex(5)
    d = _dir(tid)
    os.makedirs(d, mode=0o700, exist_ok=True)
    with open(os.path.join(d, "track." + fmt), "wb") as f:
        f.write(data)
    meta = {"id": tid, "title": _clean(tags.get("title")) or clean_title(title),
            "artist": _clean(tags.get("artist") or tags.get("album_artist")),
            "album": _clean(tags.get("album")), "created": int(time.time()),
            "ms": ms, "bytes": len(data), "fmt": fmt}
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

def _id(args):
    return arg(args, "id")


@text_route
def get_list(h, args):
    """every track: id, title, length, size"""
    h.text("".join("%s\t%s\t%d\t%d\t%s\t%s\n" % (m["id"], m["title"], m["ms"], m["bytes"],
                                               m.get("fmt") or "wav", m.get("artist") or "")
                   for m in all_tracks()))


@text_route
def get_track(h, args):
    """a track's MP3 (or WAV)"""
    path = track_path(_id(args))
    if not os.path.isfile(path):
        h.text("error no such track\n", 404)
        return
    h.send_response(200)
    h.send_header("Content-Type", "audio/mpeg" if path.endswith(".mp3") else "audio/wav")
    h.send_header("Content-Length", str(os.path.getsize(path)))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    with open(path, "rb") as f:          # streamed: a track is megabytes
        while True:
            chunk = f.read(64 * 1024)
            if not chunk:
                break
            h.wfile.write(chunk)


@text_route
def delete_track(h, args):
    """delete a track, here and so on the device at its next sync"""
    if not remove(_id(args)):
        h.text("error no such track\n", 404)
        return
    h.text("ok\n")


def post_music_upload(h, path, args):
    """a track in any format, converted and kept (the device's token)"""
    try:
        meta = add(h.body(MAX_IN), (args.get("title") or ["track"])[0])
    except ValueError as e:
        h.text(_route_err % e, 400)
        return
    h.text("%s\t%s\t%d\n" % (meta["id"], meta["title"], meta["ms"]))


def get_dash_list(h, path, args):
    """every track, as JSON"""
    h._send(200, "application/json", json.dumps(all_tracks()), (("Cache-Control", "no-store"),))


def post_upload(h, path, args):
    """a track, converted here if it needs it, kept"""
    try:
        meta = add(h.body(MAX_IN), (args.get("title") or ["track"])[0])
    except ValueError as e:
        h._send(400, "application/json", json.dumps({"error": str(e)}), ())
        return
    h._send(200, "application/json", json.dumps(meta), ())


# The dashboard's Music page (server/plugins.py): dash/view.html and view.js.
DASH_VIEWS = [{"id": "music", "title": "Music", "order": 50,
               "icon": '<path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/>'
                       '<circle cx="17" cy="16" r="3"/>',
               "html": "dash/view.html", "js": "dash/view.js"}]

ROUTES = [
    ("GET", "/music", get_list, "device_or_dash"),
    ("GET", "/music/track", get_track, "device_or_dash"),
    ("DELETE", "/music/track", delete_track, "device_or_dash"),
    ("POST", "/music/upload", post_music_upload, "device_or_dash"),
    ("GET", "/dash/music", get_dash_list, "dash"),
    ("POST", "/dash/music/upload", post_upload, "dash"),
    ("GET", "/dash/music/track", get_track, "device_or_dash"),
    ("DELETE", "/dash/music/track", delete_track, "device_or_dash"),
]


# ---- the WAVs kept before the device played MP3 ----------------------------------------

def to_mp3(root):
    """Every track under `root` (CARDOS_STATE) still kept as WAV, made an
    MP3 under the same id; the device sees the format change at its next
    sync, fetches the MP3 and deletes the WAV. Returns how many."""
    n = 0
    for dirpath, _, files in os.walk(root):
        if "meta.json" not in files or "track.wav" not in files:
            continue
        if os.path.basename(os.path.dirname(dirpath)) != "music":
            continue
        meta_path = os.path.join(dirpath, "meta.json")
        with open(meta_path, encoding="utf-8") as f:
            meta = json.load(f)
        with open(os.path.join(dirpath, "track.wav"), "rb") as f:
            mp3, ms, _ = convert(f.read())
        with open(os.path.join(dirpath, "track.mp3"), "wb") as f:
            f.write(mp3)
        meta.update(fmt="mp3", ms=ms, bytes=len(mp3))
        meta.pop("rate", None)
        meta.pop("channels", None)
        with open(meta_path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(meta, f)
        os.replace(meta_path + ".tmp", meta_path)
        os.remove(os.path.join(dirpath, "track.wav"))
        sys.stderr.write("music: %s is MP3 now (%d KB)\n" % (meta.get("id"), len(mp3) // 1024))
        n += 1
    return n


def main(argv=None):
    """python -m apps.music.server mp3 (__main__.py)"""
    argv = sys.argv[1:] if argv is None else argv
    if argv == ["mp3"]:
        from server import dash
        print("%d tracks made MP3" % to_mp3(dash.state_dir()))
        return 0
    print("usage: python -m apps.music.server mp3")
    return 2
