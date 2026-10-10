"""Jar Factory's server: the daily shop stock, friends, and gifts.

docs/superpowers/specs/2026-10-09-jar-factory-design.md is the game, and
its section "Server (steps 5 and 6)" is this file's contract with the
device. Everything is kept in the general store (server/kv.py), in the
server's own namespace srv/jar, which a device can read but never write:

    code/CODE        -> NAME           a friend code, made on first use
    codeof/NAME      -> CODE
    friends/NAME     -> names, one a line: whom NAME has added
    next_id          a counter; an item's id is ID_BASE + it
    own/ID           -> NAME           the ledger: who holds item ID
    day/NAME         -> JSON           today's stock: state, date, tags, n
    stock/NAME/I     -> bytes          item I of NAME's last finished stock

Asking Claude goes through ask.ask_shape, signing through sign.sign, the
inbox is the store's own queues (jar.gifts, jar.thanks) read with /q/peek
and /q/ack, and names and last-seen come from server/people.py.

The item record is apps/jaritem.h's, byte for byte: encode() and decode()
below mirror jitem_encode and jitem_decode, and the enums (kinds,
movements, events, actions, ...) are read out of that header rather than
typed twice. A signed record is the unsigned one with the signature put in
after the header: the signed message is the record with no signature --
bytes [0, header) with the signature length (offset 204) 0, the 8 memory
slots (bytes 136..151) 0, and the total length (offset 2) not counting the
signature, then the script and frames. Memory is out of it because scripts
write it on the device, and a gift must still verify afterwards.

Routes (all "device_or_dash"; a refusal is "error WHY\\n" with its status):

    GET  /jar/me                "code CODE\\n" "name DISPLAY\\n", then
                                "friend NAME\\tDISPLAY\\tLAST_SEEN\\tSTATE\\n"
    POST /jar/friend?code=CODE  "ok NAME mutual|waiting\\n"
    POST /jar/unfriend?name=N   "ok\\n"
    POST /jar/day               body: garden/shelf/owned/tz lines
                                -> "pending\\n" | "ok DATE\\n"
    GET  /jar/day               "pending\\n" | "ok DATE\\ntags A, B\\nitems N\\n"
                                | "error WHY\\n"
    GET  /jar/item?i=N          base64 of item N (0-based) + "\\n"
    GET  /jar/pubkey            the signing key, 130 hex digits + "\\n"
    POST /jar/gift?to=NAME      body: "NOTE\\nBASE64\\n" -> "ok\\n"
    POST /jar/thanks?to=NAME&id=ID  -> "ok\\n"
"""
import base64
import datetime
import json
import os
import random
import re
import secrets
import sys
import threading
import time
import urllib.request

from . import accounts, ask, kv, people, sign, wire, jarvm
from .kv import kv_route, KVError, NotAllowed, NotFound
from .routes import arg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "apps", "jaritem.h")

NS = "srv/jar"
MAKER = "Jar Works"
ID_BASE = 100                 # built-ins are 1..12; the server's start past them
ITEMS = 8                     # a day's stock
# A stock is made one item a call, each stored the moment it passes, so the
# shop shows the first in under a minute instead of all eight after six or
# seven (one call for all 8 took 391 s on the droplet). Each call is told its
# kind, so the mix holds without one call seeing the whole batch.
KIND_PLAN = ("critter", "floor", "hanging", "floor", "critter", "hanging", "floor", "critter")
HINTED = (1, 4, 6)            # the items a word to the shopkeeper leans
MAX_FAILS = 4                 # calls that may come to nothing before the day settles for less
GEN_TIMEOUT = 600             # one Claude call, one item
SCRIPT_SRC_MAX = 1200         # a script's source, as Claude writes it
STOCK_TTL = 3 * 86400
FRIENDS_MAX = 64
NOTE_MAX = 24
CODE_CHARS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"     # no I L O 0 1
CODE_LEN = 6
GIFTS_Q = "jar.gifts"
THANKS_Q = "jar.thanks"

# ---- the record (apps/jaritem.h) -----------------------------------------------------

VERSION = 1
HDR = 208
REC_MAX = 1024
FRAME_BYTES = 96
MAX_FRAMES = 4
NAME_LEN, LINE_LEN, BUB_LEN, MAX_BUB, MAX_HAB, MEM, WHO_LEN, TAGS_LEN = 12, 32, 12, 4, 3, 8, 12, 24
SIG_MAX, SCRIPT_MAX = 64, 256
F_GIFT, F_BUILTIN = 0x01, 0x02

# where each field is: (offset, size)
OFF = {
    "version": (0, 1), "hdr": (1, 1), "total": (2, 2), "id": (4, 4), "kind": (8, 1),
    "frames": (9, 1), "flags": (10, 1), "nbub": (11, 1), "name": (12, 12), "line": (24, 32),
    "palette": (56, 16), "move": (72, 1), "speed": (73, 1), "zone": (74, 1), "nhab": (75, 1),
    "habits": (76, 12), "bubbles": (88, 48), "memory": (136, 16), "maker": (152, 12),
    "made": (164, 4), "tags": (168, 24), "gifted": (192, 12), "sig_len": (204, 1),
    "script_len": (205, 2), "reserved": (207, 1),
}

_ENUM_PREFIX = {"JK": "kinds", "JM": "moves", "JSP": "speeds", "JZ": "zones", "JE": "events",
                "JN": "near", "JA": "actions", "JP": "particles"}
_enums = None


def enums(path=HEADER):
    """{"kinds": {"floor": 0, ...}, "moves", "speeds", "zones", "events",
    "near", "actions", "particles"}, read from apps/jaritem.h's enums so
    the server and the device cannot disagree about a number."""
    global _enums
    if _enums is not None and path == HEADER:
        return _enums
    with open(path, encoding="utf-8") as f:
        src = re.sub(r"/\*.*?\*/", " ", f.read(), flags=re.S)
    out = {}
    for body in re.findall(r"enum\s*\{([^}]*)\}", src):
        n = 0
        for part in body.split(","):
            part = part.strip()
            if not part:
                continue
            name, _, val = part.partition("=")
            name = name.strip()
            if val.strip():
                n = int(val.strip(), 0)
            prefix, _, word = name.partition("_")
            group = _ENUM_PREFIX.get(prefix)
            if group and word != "KINDS":
                out.setdefault(group, {})[word.lower()] = n
            n += 1
    missing = set(_ENUM_PREFIX.values()) - set(out)
    if missing:
        raise ValueError("%s has no enum for %s" % (path, ", ".join(sorted(missing))))
    if path == HEADER:
        _enums = out
    return out


def rgb565(colour):
    """'#rrggbb' -> plain RGB565, as tools/make_jar_art.py does it."""
    v = int(colour.lstrip("#"), 16)
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def pack_frame(rows):
    """16 strings of 16 digits 0-7 -> 96 bytes: pixel i is bits 3i..3i+2 of
    the frame as one little-endian bit string (jaritem.h's ji_px)."""
    bits = 0
    for i, ch in enumerate("".join(rows)):
        bits |= (ord(ch) - 48) << (3 * i)
    return bits.to_bytes(FRAME_BYTES, "little")


def unpack_frame(data):
    """96 bytes -> 256 colour indexes, row-major."""
    bits = int.from_bytes(bytes(data[:FRAME_BYTES]), "little")
    return [(bits >> (3 * i)) & 7 for i in range(256)]


def _put_str(buf, off, s, n):
    raw = (s or "").encode("ascii", "replace")[:n]
    buf[off:off + n] = raw + b"\0" * (n - len(raw))


def _get_str(data, off, n):
    out = []
    for b in data[off:off + n]:
        if not b:
            break
        out.append(chr(b) if 32 <= b < 127 else "?")
    return "".join(out)


def encode(it):
    """An item (a dict: see decode() for the keys) -> its record, exactly as
    jitem_encode writes it. ValueError if it breaks a limit."""
    frames = it.get("frames") or []
    sig = bytes(it.get("sig") or b"")
    script = bytes(it.get("script") or b"")
    habits = it.get("habits") or []
    bubbles = it.get("bubbles") or []
    if not 0 <= it["kind"] < 3 or not 1 <= len(frames) <= MAX_FRAMES:
        raise ValueError("a bad kind or frame count")
    if len(bubbles) > MAX_BUB or len(habits) > MAX_HAB:
        raise ValueError("too many bubbles or habits")
    if len(sig) > SIG_MAX or len(script) > SCRIPT_MAX:
        raise ValueError("signature or script too long")
    n = HDR + len(sig) + len(script) + len(frames) * FRAME_BYTES
    if n > REC_MAX:
        raise ValueError("%d bytes is more than %d" % (n, REC_MAX))
    out = bytearray(HDR)
    out[0] = VERSION
    out[1] = HDR
    out[2:4] = n.to_bytes(2, "little")
    out[4:8] = int(it.get("id", 0)).to_bytes(4, "little")
    out[8] = it["kind"]
    out[9] = len(frames)
    out[10] = it.get("flags", 0) & (F_GIFT | F_BUILTIN)
    out[11] = len(bubbles)
    _put_str(out, 12, it.get("name"), NAME_LEN)
    _put_str(out, 24, it.get("line"), LINE_LEN)
    pal = list(it.get("pal") or [0] * 8)
    for i in range(8):
        out[56 + 2 * i:58 + 2 * i] = (pal[i] if i else 0).to_bytes(2, "little")
    out[72], out[73], out[74] = it.get("move", 0), it.get("speed", 1), it.get("zone", 5)
    out[75] = len(habits)
    for i, h in enumerate(habits):
        out[76 + 4 * i:80 + 4 * i] = bytes(h)
    for i, b in enumerate(bubbles):
        _put_str(out, 88 + BUB_LEN * i, b, BUB_LEN)
    mem = list(it.get("mem") or [0] * MEM)
    for i in range(MEM):
        out[136 + 2 * i:138 + 2 * i] = (mem[i] & 0xFFFF).to_bytes(2, "little")
    _put_str(out, 152, it.get("maker"), WHO_LEN)
    out[164:168] = int(it.get("made", 0)).to_bytes(4, "little")
    _put_str(out, 168, it.get("tags"), TAGS_LEN)
    _put_str(out, 192, it.get("gifted"), WHO_LEN)
    out[204] = len(sig)
    out[205:207] = len(script).to_bytes(2, "little")
    out += sig + script
    for f in frames:
        f = bytes(f)
        if len(f) != FRAME_BYTES:
            raise ValueError("a frame is %d bytes" % FRAME_BYTES)
        out += f
    return bytes(out)


def decode(data):
    """A record -> a dict, as jitem_decode reads it (unknown habits dropped,
    an unknown movement/speed/zone made sits/medium/anywhere). ValueError
    with jitem_decode's reason: -1 short or long, -2 lengths disagree, -3 a
    bad count."""
    data = bytes(data)
    e = enums()
    if len(data) < HDR or data[0] == 0:
        raise ValueError("-1 too short")
    hdr = data[1]
    total = int.from_bytes(data[2:4], "little")
    if hdr < HDR or total > REC_MAX or total > len(data) or total < hdr:
        raise ValueError("-1 bad lengths")
    kind, nframes, nbub, nhab, sig_len = data[8], data[9], data[11], data[75], data[204]
    script_len = int.from_bytes(data[205:207], "little")
    if (kind >= 3 or not 1 <= nframes <= MAX_FRAMES or nbub > MAX_BUB or nhab > MAX_HAB
            or sig_len > SIG_MAX or script_len > SCRIPT_MAX):
        raise ValueError("-3 a bad count")
    if hdr + sig_len + script_len + nframes * FRAME_BYTES != total:
        raise ValueError("-2 the lengths disagree")
    habits = []
    for i in range(nhab):
        h = data[76 + 4 * i:80 + 4 * i]
        if h[0] < len(e["events"]) and h[2] < len(e["actions"]):
            habits.append(tuple(h))
    o = hdr + sig_len
    newer = data[0] > VERSION
    return {
        "version": data[0], "hdr": hdr, "total": total,
        "id": int.from_bytes(data[4:8], "little"), "kind": kind,
        "flags": data[10] & (F_GIFT | F_BUILTIN), "newer": newer,
        "name": _get_str(data, 12, NAME_LEN), "line": _get_str(data, 24, LINE_LEN),
        "pal": [int.from_bytes(data[56 + 2 * i:58 + 2 * i], "little") if i else 0
                for i in range(8)],
        "move": data[72] if data[72] < len(e["moves"]) else e["moves"]["sits"],
        "speed": data[73] if data[73] < len(e["speeds"]) else e["speeds"]["medium"],
        "zone": data[74] if data[74] < len(e["zones"]) else e["zones"]["anywhere"],
        "habits": habits,
        "bubbles": [_get_str(data, 88 + BUB_LEN * i, BUB_LEN) for i in range(nbub)],
        "mem": [int.from_bytes(data[136 + 2 * i:138 + 2 * i], "little", signed=True)
                for i in range(MEM)],
        "maker": _get_str(data, 152, WHO_LEN),
        "made": int.from_bytes(data[164:168], "little"),
        "tags": _get_str(data, 168, TAGS_LEN), "gifted": _get_str(data, 192, WHO_LEN),
        "sig": data[hdr:hdr + sig_len],
        "script": b"" if newer else data[o:o + script_len],
        "frames": [data[o + script_len + FRAME_BYTES * i:o + script_len + FRAME_BYTES * (i + 1)]
                   for i in range(nframes)],
    }


MEM_AT, MEM_LEN = 136, 16      # the 8 int16 memory slots: scripts write them


def message(record):
    """What a record's signature covers: the record without its signature --
    the header with signature length 0, the memory slots 0 and the total not
    counting the signature, then everything after it (script, frames)."""
    record = bytes(record)
    hdr, sig_len = record[1], record[204]
    head = bytearray(record[:hdr])
    head[204] = 0
    head[MEM_AT:MEM_AT + MEM_LEN] = bytes(MEM_LEN)
    head[2:4] = (len(record) - sig_len).to_bytes(2, "little")
    return bytes(head) + record[hdr + sig_len:]


def seal(unsigned, signer=None):
    """An unsigned record (signature length 0) -> the same record signed:
    the 64 bytes put in after the header and counted in both lengths."""
    unsigned = bytes(unsigned)
    if unsigned[204] != 0:
        unsigned = message(unsigned)
    sig = (signer or sign.sign)(message(unsigned))
    hdr = unsigned[1]
    head = bytearray(unsigned[:hdr])
    head[204] = len(sig)
    head[2:4] = (len(unsigned) + len(sig)).to_bytes(2, "little")
    return bytes(head) + sig + unsigned[hdr:]


def verified(record, pub=None):
    """Is this a record the server signed? (pub: 65 raw bytes; ours if None)"""
    record = bytes(record)
    try:
        decode(record)
    except ValueError:
        return False
    sig_len = record[204]
    if sig_len != 64 or record[10] & F_BUILTIN:
        return False
    hdr = record[1]
    return sign.verify(message(record), record[hdr:hdr + sig_len], pub)


# ---- the text filter ---------------------------------------------------------------

# Deliberately minimal: whole words (and their plain plurals and -ing/-er/-ed/-y
# forms) after undoing the commonest letter-for-digit swaps. It keeps the
# obvious out of a children's-shop sprite name; it is not moderation, and
# substrings are not matched so a "Scunthorpe" passes.
BAD_WORDS = frozenset("""
    fuck shit cunt bitch bastard dick cock pussy piss slut whore twat wank
    wanker arse asshole bollocks prick fag faggot nigger nigga retard rape
    nazi hitler porn sex sexy nude boob boobs tits dildo cum jizz
""".split())
_LEET = str.maketrans("013457@$!", "oieastasi")
_SUFFIXES = ("", "s", "es", "ing", "er", "ers", "ed", "y")


def clean(text):
    """True if `text` passes the filter."""
    words = re.findall(r"[a-z]+", (text or "").lower().translate(_LEET))
    for w in words:
        for suf in _SUFFIXES:
            if w.endswith(suf) and w[:len(w) - len(suf)] in BAD_WORDS:
                return False
    return True


# ---- the day: date, season, moon, weather ---------------------------------------------

SYNODIC = 29.530588853
NEW_MOON = 947182440          # 2000-01-06 18:14 UTC
MOON_NAMES = ["new moon", "waxing crescent", "first quarter", "waxing gibbous", "full moon",
              "waning gibbous", "last quarter", "waning crescent"]


def moon(date):
    """(phase name, is it a full-moon day) for a datetime.date, at noon UTC."""
    t = datetime.datetime(date.year, date.month, date.day, 12,
                          tzinfo=datetime.timezone.utc).timestamp()
    age = ((t - NEW_MOON) / 86400.0) % SYNODIC
    name = MOON_NAMES[int(age / SYNODIC * 8 + 0.5) % 8]
    return name, abs(age - SYNODIC / 2) < 0.75


def season(date, south=False):
    """(season, is it the first day of it): by month, the 1st of March, June,
    September and December starting one; turned round below the equator."""
    names = ["winter", "spring", "summer", "autumn"]
    i = (date.month % 12) // 3
    if south:
        i = (i + 2) % 4
    return names[i], date.day == 1 and date.month in (3, 6, 9, 12)


# A POSIX TZ string names a zone's abbreviation and offset, not a place. One
# representative city per (abbreviation, standard offset in hours east of
# UTC, has daylight saving), then per offset alone. UTC0 -- what a device
# with no TZ set has -- is nowhere, and asks for no weather.
CITIES = {
    "honolulu": (21.31, -157.86), "anchorage": (61.22, -149.90),
    "san francisco": (37.77, -122.42), "denver": (39.74, -104.99), "phoenix": (33.45, -112.07),
    "chicago": (41.88, -87.63), "new york": (40.71, -74.01), "halifax": (44.65, -63.57),
    "san juan": (18.47, -66.11), "st johns": (47.56, -52.71), "sao paulo": (-23.55, -46.63),
    "ponta delgada": (37.74, -25.67), "reykjavik": (64.15, -21.94), "london": (51.51, -0.13),
    "dublin": (53.35, -6.26), "lisbon": (38.72, -9.14), "berlin": (52.52, 13.40),
    "lagos": (6.52, 3.38), "athens": (37.98, 23.73), "johannesburg": (-26.20, 28.05),
    "cairo": (30.04, 31.24), "moscow": (55.76, 37.62), "tehran": (35.69, 51.39),
    "dubai": (25.20, 55.27), "karachi": (24.86, 67.01), "delhi": (28.61, 77.21),
    "kathmandu": (27.72, 85.32), "dhaka": (23.81, 90.41), "bangkok": (13.76, 100.50),
    "jakarta": (-6.21, 106.85), "singapore": (1.35, 103.82), "shanghai": (31.23, 121.47),
    "hong kong": (22.32, 114.17), "manila": (14.60, 120.98), "perth": (-31.95, 115.86),
    "tokyo": (35.68, 139.69), "seoul": (37.57, 126.98), "adelaide": (-34.93, 138.60),
    "darwin": (-12.46, 130.84), "sydney": (-33.87, 151.21), "brisbane": (-27.47, 153.03),
    "auckland": (-36.85, 174.76),
}
BY_ABBR = {
    ("HST", -10, False): "honolulu", ("AKST", -9, True): "anchorage",
    ("PST", -8, True): "san francisco", ("MST", -7, True): "denver",
    ("MST", -7, False): "phoenix", ("CST", -6, True): "chicago", ("EST", -5, True): "new york",
    ("AST", -4, True): "halifax", ("AST", -4, False): "san juan", ("NST", -3.5, True): "st johns",
    ("GMT", 0, True): "london", ("IST", 1, True): "dublin", ("WET", 0, True): "lisbon",
    ("CET", 1, True): "berlin", ("WAT", 1, False): "lagos", ("EET", 2, True): "athens",
    ("EET", 2, False): "cairo", ("SAST", 2, False): "johannesburg", ("MSK", 3, False): "moscow",
    ("PKT", 5, False): "karachi", ("IST", 5.5, False): "delhi", ("WIB", 7, False): "jakarta",
    ("CST", 8, False): "shanghai", ("HKT", 8, False): "hong kong", ("PST", 8, False): "manila",
    ("AWST", 8, False): "perth", ("JST", 9, False): "tokyo", ("KST", 9, False): "seoul",
    ("ACST", 9.5, True): "adelaide", ("ACST", 9.5, False): "darwin",
    ("AEST", 10, True): "sydney", ("AEST", 10, False): "brisbane",
    ("NZST", 12, True): "auckland",
}
BY_OFFSET = {
    -10: "honolulu", -9: "anchorage", -8: "san francisco", -7: "denver", -6: "chicago",
    -5: "new york", -4: "halifax", -3.5: "st johns", -3: "sao paulo", -1: "ponta delgada",
    0: "london", 1: "berlin", 2: "athens", 3: "moscow", 3.5: "tehran", 4: "dubai",
    5: "karachi", 5.5: "delhi", 5.75: "kathmandu", 6: "dhaka", 7: "bangkok", 8: "singapore",
    9: "tokyo", 9.5: "adelaide", 10: "sydney", 12: "auckland",
}
_TZ = re.compile(r"^(<[^>]{1,10}>|[A-Za-z]{3,6})([+-]?)(\d{1,2})(?::(\d{2}))?(.*)$")


def city_for_tz(tz):
    """(city, (lat, lon)) for a POSIX TZ string, or None."""
    m = _TZ.match((tz or "").strip())
    if not m:
        return None
    abbr = m.group(1).upper()
    west = int(m.group(3)) + int(m.group(4) or 0) / 60.0
    east = west if m.group(2) == "-" else -west          # POSIX counts west
    east = round(east * 4) / 4
    dst = bool(re.match(r"^(<[^>]+>|[A-Za-z]{3,6})", m.group(5)))
    if abbr in ("UTC", "UCT", "ZULU") or (east == 0 and abbr == "GMT" and not dst):
        return None
    name = BY_ABBR.get((abbr, east, dst)) or BY_ABBR.get((abbr, east, not dst)) \
        or BY_OFFSET.get(east)
    return (name, CITIES[name]) if name else None


WEATHER_URL = ("https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
               "&current=weather_code,temperature_2m")
_weather_cache = {}


def weather_word(code):
    """A WMO weather code -> one word."""
    if code in (0, 1):
        return "sunny"
    if code in (2, 3):
        return "cloudy"
    if code in (45, 48):
        return "foggy"
    if 51 <= code <= 67 or 80 <= code <= 82:
        return "rainy"
    if 71 <= code <= 77 or code in (85, 86):
        return "snowy"
    if code >= 95:
        return "stormy"
    return None


def fetch_weather(lat, lon):
    """(word, celsius) now at a place from open-meteo (no key), or None if
    it cannot be had -- the day goes on without weather."""
    try:
        req = urllib.request.Request(WEATHER_URL % (lat, lon),
                                     headers={"User-Agent": "cardos-server"})
        with urllib.request.urlopen(req, timeout=6) as r:
            cur = json.loads(r.read().decode("utf-8", "replace")).get("current") or {}
        word = weather_word(int(cur.get("weather_code")))
        temp = cur.get("temperature_2m")
        return (word, float(temp) if temp is not None else None) if word else None
    except Exception as e:                              # noqa: BLE001 - quietly skipped
        sys.stderr.write("jar: weather: %s\n" % e)
        return None


def day_facts(date, tz):
    """Everything about the day that steers the stock."""
    place = city_for_tz(tz)
    south = bool(place and place[1][0] < 0)
    s, first = season(date, south)
    m, full = moon(date)
    facts = {"date": date.isoformat(), "season": s, "first_of_season": first, "moon": m,
             "full_moon": full, "city": place[0] if place else None, "weather": None,
             "temp": None}
    if place:
        key = (place[0], facts["date"])
        if key not in _weather_cache:
            _weather_cache[key] = fetch_weather(*place[1])
            if len(_weather_cache) > 512:
                _weather_cache.pop(next(iter(_weather_cache)))
        got = _weather_cache[key]
        if got:
            facts["weather"], facts["temp"] = got
    return facts


# ---- what the device sends ---------------------------------------------------------

FLAVOURS = {
    "mushroom": ["odd", "glowing", "spooky"],
    "berry": ["sweet", "round", "food"],
    "fern": ["soft", "sleepy", "cosy"],
    "flower": ["fancy", "dressed-up"],
    "cactus": ["spiky", "deserty", "tough"],
}
_TAG = re.compile(r"^[a-z][a-z-]{0,15}$")


def parse_request(body):
    """The device's lines -> {"garden": {plant: n}, "shelf": [tags],
    "owned": [names], "tz": str, "hint": str}. Unknown lines and junk are
    dropped; so is a hint that does not pass the filter."""
    out = {"garden": {}, "shelf": [], "owned": [], "tz": "", "hint": ""}
    for line in body.splitlines():
        head, _, rest = line.strip().partition(" ")
        rest = rest.strip()
        if head == "garden":
            for part in rest.split(","):
                k, _, v = part.strip().partition("=")
                k = "fern" if k.strip() == "moss" else k.strip()
                if k in FLAVOURS:
                    try:
                        out["garden"][k] = max(0, min(99, int(v)))
                    except ValueError:
                        pass
        elif head == "shelf":
            for t in rest.split(","):
                t = t.strip().lower()
                if _TAG.match(t) and t not in out["shelf"] and len(out["shelf"]) < 16:
                    out["shelf"].append(t)
        elif head == "owned":
            for n in rest.split(","):
                n = wire.flat(n, NAME_LEN, ascii=True)
                if n and len(out["owned"]) < 48:
                    out["owned"].append(n)
        elif head == "tz":
            out["tz"] = rest[:64]
        elif head == "hint":
            h = wire.flat(rest, HINT_LEN, ascii=True).replace('"', "'")
            if clean(h):
                out["hint"] = h
    return out


def day_tags(req, facts, seed):
    """The short tag list the stock is made from: the strongest flavours of
    the garden and the shelf, then the day's season, weather and moon."""
    rng = random.Random(seed)
    score = {}
    for plant, n in req["garden"].items():
        for w in FLAVOURS[plant]:
            score[w] = score.get(w, 0) + n
    for t in req["shelf"]:
        score[t] = score.get(t, 0) + 1
    ranked = [w for w in score if score[w] > 0]
    rng.shuffle(ranked)                       # ties fall differently each day
    ranked.sort(key=lambda w: -score[w])
    tags = ranked[:3]
    if not tags:                              # nothing grown, nothing shown: a surprise
        tags = rng.sample(sorted({w for ws in FLAVOURS.values() for w in ws}), 2)
    tags.append(facts["season"])
    if facts.get("weather"):
        tags.append(facts["weather"])
    if facts.get("full_moon"):
        tags.append("full-moon")
    elif facts.get("moon") == "new moon":
        tags.append("new-moon")
    return tags[:6]


def record_tags(tags):
    """The tags as the record's 24-byte field: whole tags, comma-separated."""
    out = ""
    for t in tags:
        nxt = t if not out else out + "," + t
        if len(nxt) > TAGS_LEN:
            break
        out = nxt
    return out


# ---- asking Claude for items ------------------------------------------------------------

def item_schema():
    e = enums()
    printable = "^[ -~]+$"
    return {
        "type": "object",
        "required": ["name", "line", "kind", "movement", "speed", "zone", "habits",
                     "bubbles", "palette", "frames"],
        "additionalProperties": False,
        "properties": {
            "name": {"type": "string", "minLength": 1, "maxLength": NAME_LEN, "pattern": printable},
            "line": {"type": "string", "minLength": 1, "maxLength": LINE_LEN, "pattern": printable},
            "kind": {"enum": list(e["kinds"])},
            "movement": {"enum": list(e["moves"])},
            "speed": {"enum": list(e["speeds"])},
            "zone": {"enum": list(e["zones"])},
            "habits": {"type": "array", "maxItems": MAX_HAB, "items": {
                "type": "object", "required": ["when", "do"], "additionalProperties": False,
                "properties": {
                    "when": {"enum": list(e["events"])},
                    "when_arg": {"type": "integer", "minimum": 0, "maximum": 255},
                    "do": {"enum": [a for a in e["actions"] if a != "none"]},
                    "do_arg": {"type": "integer", "minimum": 0, "maximum": 255}}}},
            "bubbles": {"type": "array", "maxItems": MAX_BUB, "items": {
                "type": "string", "minLength": 1, "maxLength": BUB_LEN, "pattern": printable}},
            "palette": {"type": "array", "minItems": 8, "maxItems": 8, "items": {
                "type": "string", "pattern": "^#[0-9a-fA-F]{6}$"}},
            "frames": {"type": "array", "minItems": 1, "maxItems": MAX_FRAMES, "items": {
                "type": "array", "minItems": 16, "maxItems": 16, "items": {
                    "type": "string", "pattern": "^[0-7]{16}$"}}},
            # Phase 2 (step 7): an optional behaviour script in server/jarvm.py's
            # language, compiled and given a simulated day before it ships.
            "script": {"type": "string", "maxLength": SCRIPT_SRC_MAX},
        },
    }


def batch_schema(n, kind=None):
    one = item_schema()
    if kind:
        one = dict(one, properties=dict(one["properties"], kind={"enum": [kind]}))
    return {"type": "object", "required": ["items"], "additionalProperties": False,
            "properties": {"items": {"type": "array", "minItems": 1, "maxItems": n,
                                     "items": one}}}


def _numbered(d):
    return ", ".join("%d %s" % (v, k) for k, v in sorted(d.items(), key=lambda kv_: kv_[1]))


HINT_LEN = 40


KIND_WORDS = {"critter": "a critter (it roams)", "floor": "floor decor (it stands on the soil)",
              "hanging": "hanging decor (it hangs from the lid)"}


def prompt_for(n, tags, facts, owned, special=None, avoid=(), hint="", kind=None):
    e = enums()
    day = "%s, %s, %s" % (facts["date"], facts["season"], facts["moon"])
    if facts.get("weather"):
        day += ", %s%s" % (facts["weather"], "" if facts.get("temp") is None
                           else " and %d C" % round(facts["temp"]))
    if facts.get("city"):
        day += " (near %s)" % facts["city"].title()
    lines = [
        "Jar Factory is a cosy idle game on a tiny 240x135 screen: a glass jar with a "
        "little jam factory built from human junk (thimbles, matchboxes, bottle caps, "
        "twigs), mossy plants, worker critters called mosslings and a snail who carries "
        "the jars out. Its shop sells collectible items the player places in the jar.",
        "",
        ("Invent one new shop item for today. Today's tags: %s. The day: %s." % (
            ", ".join(tags), day)) if n == 1 else
        ("Invent %d new shop items for today. Today's tags: %s. The day: %s." % (
            n, ", ".join(tags), day)),
        ("It should clearly fit one or more of the tags." if n == 1 else
         "Each item should clearly fit one or more of the tags.") + (
            " Make it %s." % KIND_WORDS[kind] if kind else
            " Mix the kinds: about a third critters (they roam), the rest floor decor "
            "(stands on the soil) and hanging decor (hangs from the lid)."),
    ]
    if avoid:
        lines.append("Today's shop already has: %s. Make something different from those."
                     % ", ".join(avoid))
    if special:
        lines.append("Today is special (%s): make %s a rare, special one about it."
                     % (special, "this item" if n == 1 else "the last item"))
    if hint:
        # A word to the shopkeeper. He is a junk dealer who knows people, not a
        # genie: it nudges what turns up, it does not order it.
        lines.append(
            "The shopkeeper is a junk dealer with connections, not a wish-granter. The "
            "player mentioned to him in passing: \"%s\". Treat it as a loose guideline, "
            "the way a dealer would: let this find lean that way, loosely and in the "
            "spirit of the tags and the day -- something he turned up through a contact, "
            "not made to order. Do not make an item that simply is the request, and do "
            "not mention the request in its text." % hint)
    if owned or avoid:
        lines.append("Do not reuse these names: %s." % ", ".join(list(owned) + list(avoid)))
    lines += [
        "",
        "Fields:",
        "- name: up to 12 characters; line: a one-line description, up to 32, playful.",
        "- movement (how it moves), speed, zone (where it likes to be).",
        "- habits: up to 3 'when this, do that' pairs. when: tick (when_arg = every N "
        "quarter-seconds, at least 4), poke (the player selects it), near (when_arg: %s), "
        "shipped (a jar leaves), jam (when_arg 0 either, 1 the belt jams, 2 it is fixed), "
        "gift (a parcel arrives), time (when_arg 0 any, 1 dawn, 2 day, 3 dusk, 4 night), "
        "weather. do: hop (do_arg height in pixels 1-24), walk (do_arg zone: %s), float, "
        "face, stop, frame (do_arg = frame number, from 0), flip, glow (0 off, 1 on, "
        "2 toggle), particle (do_arg: %s), say (do_arg = bubble number, from 0), wait "
        "(do_arg ticks). Hanging decor cannot hop or walk." % (
            _numbered(e["near"]), _numbered(e["zones"]), _numbered(e["particles"])),
        "- bubbles: up to 4 short speech bubbles, up to 12 characters each.",
        "- palette: 8 colours '#rrggbb'. Slot 0 is transparent (its colour is ignored); "
        "choose 7 that suit the item, with a dark outline colour and highlights.",
        "- frames: 1 to 4 frames of 16x16 pixel art, each 16 strings of 16 digits 0-7, "
        "a digit being a palette slot (0 transparent). Frame 0 is the shop picture; extra "
        "frames animate it (a blink, a step, a wobble). Draw a clear, recognisable "
        "silhouette with an outline, filling a good part of the 16x16 grid, using at least "
        "three colours; no single colour should cover most of it. Most items need 1 or 2 "
        "frames; give at most two items 3 or 4, so the whole answer stays under 12000 "
        "characters.",
        "- script (optional, give it to about half the items): a short behaviour script, "
        "at most 12 lines, in the language below. It runs instead of the habits for the "
        "events it handles, so it can count, remember (mem[0]..mem[7]) and choose. Make it "
        "small and charming: react to pokes, the time of day, a jar shipping, a critter "
        "passing. Bubble and frame numbers in scripts count from 1.",
        "Keep everything friendly for all ages. Write the JSON compactly, without "
        "indentation.",
        "",
        "The script language:",
        jarvm.LANGUAGE_GUIDE,
    ]
    return "\n".join(lines)


def check_item(raw, nameset=()):
    """What is wrong with one item Claude gave (a dict as the schema shapes
    it): a list of reasons, empty if nothing. Covers what the schema cannot:
    sprites, the text filter, the recipe's arguments, names already used."""
    e = enums()
    why = []
    errs = ask.validate(raw, item_schema())
    if errs:
        return errs
    texts = [raw["name"], raw["line"]] + list(raw["bubbles"])
    if not all(clean(t) for t in texts):
        why.append("a word the filter refuses")
    if raw["name"].strip().lower() in nameset:
        why.append("the name %r is taken" % raw["name"])
    nframes, nbub = len(raw["frames"]), len(raw["bubbles"])
    for i, frame in enumerate(raw["frames"]):
        px = "".join(frame)
        if len(frame) != 16 or any(len(r) != 16 for r in frame) or set(px) - set("01234567"):
            why.append("frame %d is not 16x16 of palette slots 0-7" % i)
            continue
        solid = [c for c in px if c != "0"]
        if len(solid) < 24:
            why.append("frame %d is (nearly) blank" % i)
            continue
        top = max(solid.count(c) for c in set(solid))
        if top > 0.85 * len(solid):
            why.append("frame %d is too flat: one colour is %d%% of it"
                       % (i, 100 * top // len(solid)))
    hanging = raw["kind"] == "hanging"
    for k, h in enumerate(raw["habits"]):
        ev, ea = h["when"], h.get("when_arg", 0)
        ac, aa = h["do"], h.get("do_arg", 0)
        lim = {"near": max(e["near"].values()), "jam": 2, "time": 4}.get(ev)
        if lim is not None and ea > lim:
            why.append("habit %d: %s takes 0-%d" % (k, ev, lim))
        if ev == "tick" and ea < 4:
            why.append("habit %d: tick every 4 or more" % k)
        if ac == "say" and aa >= nbub:
            why.append("habit %d: no bubble %d" % (k, aa))
        if ac == "frame" and aa >= nframes:
            why.append("habit %d: no frame %d" % (k, aa))
        if ac == "particle" and aa >= len(e["particles"]):
            why.append("habit %d: no particle %d" % (k, aa))
        if ac == "walk" and aa >= len(e["zones"]):
            why.append("habit %d: no zone %d" % (k, aa))
        if ac == "glow" and aa > 2:
            why.append("habit %d: glow takes 0-2" % k)
        if ac == "hop" and aa > 24:
            why.append("habit %d: hop at most 24" % k)
        if hanging and ac in ("hop", "walk"):
            why.append("habit %d: hanging decor cannot %s" % (k, ac))
    return why


def compile_script(raw, log=None):
    """An item's script source -> its bytecode, or b"" when it has none or it
    does not compile and live through a simulated day: the item then ships
    with its recipe alone, which always works -- a failed script is not worth
    losing the item over."""
    src = (raw.get("script") or "").strip()
    if not src:
        return b""
    try:
        return jarvm.prepare_script(src, nbub=len(raw.get("bubbles") or []),
                                    nframes=len(raw.get("frames") or []))
    except jarvm.ScriptError as e:
        if log:
            log("script of %r dropped: %s" % (raw.get("name"), e))
        return b""


def to_item(raw, item_id, made, tags, log=None):
    """A checked item (Claude's dict) -> the dict encode() takes."""
    e = enums()
    return {
        "id": item_id, "kind": e["kinds"][raw["kind"]], "flags": 0,
        "name": raw["name"], "line": raw["line"],
        "pal": [0] + [rgb565(c) for c in raw["palette"][1:]],
        "move": e["moves"][raw["movement"]], "speed": e["speeds"][raw["speed"]],
        "zone": e["zones"][raw["zone"]],
        "habits": [(e["events"][h["when"]], h.get("when_arg", 0),
                    e["actions"][h["do"]], h.get("do_arg", 0)) for h in raw["habits"]],
        "bubbles": list(raw["bubbles"]), "mem": [0] * MEM,
        "maker": MAKER, "made": int(made), "tags": tags, "gifted": "",
        "frames": [pack_frame(f) for f in raw["frames"]],
        "script": compile_script(raw, log),
    }


def generate(chat, person, req, date, store=None, signer=None, now=None, log=None,
             on_tags=None, on_item=None):
    """A day's stock for `person`: (tags, [signed records]). Asks Claude for
    ITEMS (one more on a special day) one item a call, in KIND_PLAN's kinds,
    sealing each as it passes and handing it to on_item(k, record); a call
    that comes to nothing is tried again, MAX_FAILS of them in all. Fewer
    than asked for is a smaller stock, not a failure. Raises if Claude
    cannot be asked at all (the day's limit, no Claude, ...) and nothing
    came of it."""
    st = store or kv.store()
    now = time.time() if now is None else now
    facts = day_facts(date, req.get("tz"))
    tags = day_tags(req, facts, "%s:%s" % (person, facts["date"]))
    special = None
    if facts["full_moon"]:
        special = "a full moon"
    elif facts["first_of_season"]:
        special = "the first day of %s" % facts["season"]
    want = ITEMS + (1 if special else 0)
    owned = [n.lower() for n in req.get("owned", [])]
    hint = req.get("hint", "")
    rtags = record_tags(tags)
    if on_tags:
        on_tags(tags)
    names, records, last_error, fails = [], [], None, 0
    while len(records) < want and fails < MAX_FAILS:
        k = len(records)
        sp = special if special and k == want - 1 else None
        kind = None if sp else KIND_PLAN[k % len(KIND_PLAN)]
        taken = set(owned) | {n.strip().lower() for n in names}
        try:
            got = ask.ask_shape(chat, prompt_for(1, tags, facts, req.get("owned", []), sp, names,
                                                 hint if k in HINTED else "", kind),
                                batch_schema(1, kind), user=person, limit=ask.DAILY,
                                timeout=GEN_TIMEOUT, store=st)
        except ask.RateLimited:
            if records:                                # the day's asks ran out: what came
                break
            raise
        except Exception as e:                         # one item lost, not the day
            last_error = e
            fails += 1
            if log:
                log("item %d: %s" % (k + 1, e))
            continue
        raw = (got.get("items") or [None])[0]
        why = check_item(raw, taken) if isinstance(raw, dict) else ["no item"]
        if why:
            fails += 1
            if log:
                log("refused %r: %s" % (raw.get("name") if isinstance(raw, dict) else raw,
                                        "; ".join(why[:3])))
            continue
        names.append(raw["name"])
        item_id = ID_BASE + st.incr(NS, "next_id")
        rec = seal(encode(to_item(raw, item_id, now, rtags, log)), signer)
        st.put(NS, "own/%d" % item_id, person.encode())
        records.append(rec)
        if on_item:
            on_item(k, rec)
    if not records:
        raise last_error or ValueError("no item passed the checks")
    return tags, records


# ---- the day's stock, kept ---------------------------------------------------------------

_running = set()               # (person, date) generating in this process
_day_lock = threading.Lock()


def _today(now=None):
    return datetime.datetime.fromtimestamp(time.time() if now is None else now,
                                           datetime.timezone.utc).date()


def day_state(person, store=None):
    raw = (store or kv.store()).get(NS, "day/" + person)
    try:
        return json.loads(raw.decode()) if raw else None
    except ValueError:
        return None


def _set_day(person, state, store=None):
    (store or kv.store()).put(NS, "day/" + person, json.dumps(state).encode(), ttl=STOCK_TTL)


def _make_day(chat, person, req, date, store):
    key = (person, date.isoformat())
    made = {"tags": []}

    def on_tags(tags):
        made["tags"] = tags

    def on_item(k, rec):
        # on show the moment it is made: the day stays "pending", with a count
        store.put(NS, "stock/%s/%d" % (person, k), rec, ttl=STOCK_TTL)
        _set_day(person, {"state": "pending", "date": key[1], "tags": made["tags"],
                          "n": k + 1}, store)

    try:
        tags, records = generate(chat, person, req, date, store=store, on_tags=on_tags,
                                 on_item=on_item,
                                 log=lambda s: sys.stderr.write("jar: %s: %s\n" % (person, s)))
        _set_day(person, {"state": "ok", "date": key[1], "tags": tags, "n": len(records)}, store)
        sys.stderr.write("jar: %s: %d items for %s\n" % (person, len(records), key[1]))
    except Exception as e:                              # noqa: BLE001 - reported to the device
        why = wire.flat(str(e) or type(e).__name__, 120)
        _set_day(person, {"state": "error", "date": key[1], "why": why}, store)
        sys.stderr.write("jar: %s: stock failed: %s\n" % (person, why))
    finally:
        with _day_lock:
            _running.discard(key)


def start_day(chat, person, req, now=None, store=None, fresh=False):
    """("pending" | "ok", date): today's stock, started if it is not made or
    making. A failed one is tried again. `fresh` makes a new one even when
    today's is done -- the shop's `r`, for trying things out; it still counts
    against the person's daily asks, so it cannot run away."""
    st = store or kv.store()
    date = _today(now)
    key = (person, date.isoformat())
    with _day_lock:
        cur = day_state(person, st)
        if cur and cur.get("date") == key[1]:
            if cur["state"] == "ok" and not fresh:
                return "ok", key[1]
            if cur["state"] == "pending" and key in _running:
                return "pending", key[1]
        _running.add(key)
        _set_day(person, {"state": "pending", "date": key[1]}, st)
    threading.Thread(target=_make_day, args=(chat, person, req, date, st), daemon=True).start()
    return "pending", key[1]


# ---- friends ----------------------------------------------------------------------------

def _person(name):
    if accounts.enabled():
        return name if name in accounts.users() else None
    return name if name == kv.NOBODY else None


def code_for(person, store=None):
    st = store or kv.store()
    with _day_lock:
        got = st.get(NS, "codeof/" + person)
        if got:
            return got.decode()
        while True:
            code = "".join(secrets.choice(CODE_CHARS) for _ in range(CODE_LEN))
            if st.get(NS, "code/" + code) is None:
                break
        st.put(NS, "code/" + code, person.encode())
        st.put(NS, "codeof/" + person, code.encode())
        return code


def friends_of(person, store=None):
    raw = (store or kv.store()).get(NS, "friends/" + person) or b""
    return [n for n in raw.decode().split("\n") if n]


def _set_friends(person, names, store=None):
    st = store or kv.store()
    if names:
        st.put(NS, "friends/" + person, "\n".join(names).encode())
    else:
        st.delete(NS, "friends/" + person)


def mutual(a, b, store=None):
    return b in friends_of(a, store) and a in friends_of(b, store)


def _display(person):
    for name, display, seen in people.listing():
        if name == person:
            return display, seen
    return person, 0


# ---- routes -------------------------------------------------------------------------

def _chat_ok(h):
    return bool(h.chat and h.chat.claude)


@kv_route
def get_me(h, args):
    """your friend code, your name, your friends"""
    me = kv.me()
    st = kv.store()
    out = ["code %s\n" % code_for(me, st), "name %s\n" % _display(me)[0]]
    rows = {n: (d, s) for n, d, s in people.listing()}
    for f in friends_of(me, st):
        if _person(f) is None:
            continue
        d, s = rows.get(f, (f, 0))
        out.append("friend %s\t%s\t%d\t%s\n" % (f, d, s, "mutual" if mutual(me, f, st) else "waiting"))
    h.text("".join(out))


@kv_route
def post_friend(h, args):
    """add a friend by their code"""
    me = kv.me()
    st = kv.store()
    code = re.sub(r"[\s-]", "", arg(args, "code")).upper()
    if not code:
        raise KVError("code= is a friend code")
    who = st.get(NS, "code/" + code)
    who = who.decode() if who else None
    if not who or _person(who) is None:
        raise NotFound("no one has that code")
    if who == me:
        raise KVError("that is your own code")
    with _day_lock:
        names = friends_of(me, st)
        if who not in names:
            if len(names) >= FRIENDS_MAX:
                raise kv.Full("%d friends at most" % FRIENDS_MAX)
            names.append(who)
            _set_friends(me, names, st)
    h.text("ok %s %s\n" % (who, "mutual" if mutual(me, who, st) else "waiting"))


@kv_route
def post_unfriend(h, args):
    """stop being someone's friend"""
    me = kv.me()
    who = arg(args, "name")
    if not who:
        raise KVError("name= is whom")
    with _day_lock:
        _set_friends(me, [n for n in friends_of(me) if n != who])
    h.text("ok\n")


@kv_route
def post_day(h, args):
    """today's shop stock: start it from your garden, shelf and zone"""
    try:
        body = h.body(4096).decode("utf-8", "replace")
    except ValueError as e:
        raise kv.TooBig(str(e))
    if not _chat_ok(h):
        h.text("error this server runs without Claude\n", 503)
        return
    state, date = start_day(h.chat, kv.me(), parse_request(body),
                            fresh=(args.get("fresh") or ["0"])[0] == "1")
    # The whole answer, as GET gives it: "ok DATE" alone was read by the shop
    # as a stock of no items, and replaced the one it had with nothing.
    h.text(_day_text(day_state(kv.me())) or "pending\n")


def _day_text(cur):
    """What /jar/day answers for a stock with something in it, or None: "ok",
    then "more" while the rest is still being made."""
    if not cur or not cur.get("n") or cur.get("state") not in ("ok", "pending"):
        return None
    return "ok %s\ntags %s\nitems %d\n%s" % (cur["date"], ", ".join(cur["tags"]), cur["n"],
                                              "more\n" if cur["state"] == "pending" else "")


@kv_route
def get_day(h, args):
    """today's shop stock: pending, ok and its tags, or why not"""
    cur = day_state(kv.me())
    if not cur:
        raise NotFound("no stock yet: POST /jar/day")
    if cur["state"] == "ok" and not cur.get("n"):
        h.text("ok %s\ntags %s\nitems 0\n" % (cur["date"], ", ".join(cur["tags"])))
    elif cur["state"] in ("ok", "pending"):
        h.text(_day_text(cur) or "pending\n")
    else:
        h.text("error %s\n" % cur.get("why", "it failed"))


@kv_route
def get_item(h, args):
    """one item of your stock, base64"""
    raw = arg(args, "i")
    if not raw.isdigit():
        raise KVError("i= is an item number, from 0")
    rec = kv.store().get(NS, "stock/%s/%d" % (kv.me(), int(raw)))
    if rec is None:
        raise NotFound("no item %s" % raw)
    h.text(base64.b64encode(rec).decode() + "\n")


def get_pubkey(h, path, args):
    """the key items are signed with (hex)"""
    sign.get_pubkey(h, path, args)


_gift_lock = threading.Lock()


@kv_route
def post_gift(h, args):
    """send an item you own to a friend: body NOTE, then the record in base64"""
    me = kv.me()
    st = kv.store()
    to = arg(args, "to")
    if _person(to) is None:
        raise NotFound("no such person")
    if to == me:
        raise KVError("a gift is for someone else")
    try:
        body = h.body(4096).decode("ascii", "replace")
    except ValueError as e:
        raise kv.TooBig(str(e))
    note, _, b64 = body.partition("\n")
    note = wire.flat(note, NOTE_MAX, ascii=True)
    if not clean(note):
        raise KVError("that note does not pass the filter")
    try:
        rec = base64.b64decode("".join(b64.split()), validate=True)
        decode(rec)
    except ValueError:
        raise KVError("that is not an item")
    if len(rec) != int.from_bytes(rec[2:4], "little"):
        raise KVError("that is not an item")
    if rec[204] == 0 or rec[10] & F_BUILTIN:
        raise KVError("only shop items can be sent")
    if not verified(rec):
        raise KVError("the item's signature does not check")
    item_id = int.from_bytes(rec[4:8], "little")
    with _gift_lock:
        owner = st.get(NS, "own/%d" % item_id)
        if owner is None or owner.decode() != me:
            raise NotAllowed("that item is not yours")
        if not mutual(me, to, st):
            raise NotAllowed("%s and you have not both added each other" % to)
        msg = bytearray(message(rec))
        msg[10] |= F_GIFT
        _put_str(msg, 192, wire.flat(_display(me)[0], WHO_LEN, ascii=True) or me, WHO_LEN)
        out = seal(bytes(msg))
        st.push(to, GIFTS_Q, ("%s\t%s\t%s" % (me, note, base64.b64encode(out).decode())).encode())
        st.put(NS, "own/%d" % item_id, to.encode())
    sys.stderr.write("jar: %s gave No. %04d to %s\n" % (me, item_id, to))
    h.text("ok\n")


@kv_route
def post_thanks(h, args):
    """thank a friend for a gift"""
    me = kv.me()
    st = kv.store()
    to = arg(args, "to")
    raw = arg(args, "id")
    if _person(to) is None:
        raise NotFound("no such person")
    if not raw.isdigit():
        raise KVError("id= is the item's number")
    if not mutual(me, to, st):
        raise NotAllowed("%s and you have not both added each other" % to)
    st.push(to, THANKS_Q, ("%s\t%d" % (me, int(raw))).encode())
    h.text("ok\n")


ROUTES = [
    ("GET", "/jar/me", get_me, "device_or_dash"),
    ("POST", "/jar/friend", post_friend, "device_or_dash"),
    ("POST", "/jar/unfriend", post_unfriend, "device_or_dash"),
    ("POST", "/jar/day", post_day, "device_or_dash"),
    ("GET", "/jar/day", get_day, "device_or_dash"),
    ("GET", "/jar/item", get_item, "device_or_dash"),
    ("GET", "/jar/pubkey", get_pubkey, "device_or_dash"),
    ("POST", "/jar/gift", post_gift, "device_or_dash"),
    ("POST", "/jar/thanks", post_thanks, "device_or_dash"),
]
