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

from . import shopkeep
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
MAX_FAILS = 4                 # calls that may come to nothing before the day settles for less
GEN_TIMEOUT = 900             # one Claude call, one item (a dear one thinks longer)

# A price is chosen before the item is made, and the dearer it is the more
# care it is asked for (and the more effort Claude may spend): a cheap thing
# is a quick sketch, a treasure is lavished on. (name, chance, price range,
# effort, what to ask for). Prices are in the record (byte 207, in fives).
TIERS = (
    ("common", 50, (30, 80), "low",
     "It is a common, cheap find: keep it simple -- one frame, a small clean sprite, "
     "a habit or two, no script."),
    ("everyday", 30, (90, 180), "medium",
     "It is an everyday find: one or two frames, two or three habits; a short script "
     "if it suits it."),
    ("rare", 15, (200, 400), "high",
     "It is a rare find: make it special -- a detailed, carefully drawn sprite with two "
     "or three frames of animation, distinctive habits and a charming script."),
    ("treasure", 5, (450, 900), "high",
     "It is a treasure, the best thing in the shop: lavish care on it -- a rich, "
     "detailed sprite with three or four frames of animation, delightful bubbles and "
     "a script with real personality."),
)


def pick_tier(rng):
    """(name, price, effort, words) for an item about to be made."""
    roll = rng.randrange(sum(t[1] for t in TIERS))
    for name, chance, (lo, hi), effort, words in TIERS:
        if roll < chance:
            return name, rng.randrange(lo // 5, hi // 5 + 1) * 5, effort, words
        roll -= chance
    raise AssertionError("unreachable")


# Plants are sold as seed packets now and then, instead of steering the
# stock (the owner: "i dont want the plants to influence the shop").
SEED_CHANCE = 30              # percent of stocks with a seed packet
SEED_PRICE = {"berry": 25, "fern": 25, "mushroom": 40, "flower": 40, "cactus": 50}
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
    "script_len": (205, 2), "price5": (207, 1),
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
    out[207] = max(0, min(255, int(it.get("price", 0)) // 5))   # 0: the device works one out
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
        "version": data[0], "hdr": hdr, "total": total, "price": data[207] * 5,
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
    out = {"garden": {}, "shelf": [], "owned": [], "tz": "", "hint": "", "bought": [],
           "held": [], "jar": []}
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
        elif head == "jar":
            for n in rest.split(","):
                n = wire.flat(n, NAME_LEN, ascii=True)
                if n and len(out["jar"]) < 16:
                    out["jar"].append(n)
        elif head in ("bought", "held"):
            for part in rest.split(","):
                part = part.strip()
                if part.isdigit() and len(out[head]) < 16:
                    out[head].append(int(part))
        elif head == "hint":
            h = wire.flat(rest, HINT_LEN, ascii=True).replace('"', "'")
            if clean(h):
                out["hint"] = h
        elif head == "finds":                           # a paid commission: more new finds
            try:
                out["finds"] = max(0, min(MAX_FINDS, int(rest)))
            except ValueError:
                pass
        elif head == "ask":                             # ... and what to look for
            a = wire.flat(rest, ASK_LEN, ascii=True).replace('"', "'")
            if clean(a):
                out["ask"] = a
    return out


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


GAME = (
    "Jar Factory is a cosy idle game on a tiny 240x135 screen: a glass jar with a "
    "little jam factory built from human junk (thimbles, matchboxes, bottle caps, "
    "twigs), mossy plants, worker critters called mosslings and a snail who carries "
    "the jars out. Its shop sells odd collectible things the player places in the jar.")


def prompt_for(brief, owned=(), avoid=(), kind=None, tier=None):
    """The prompt for one item, from the shopkeeper's brief (server/shopkeep.py)."""
    e = enums()
    lines = [
        GAME,
        "",
        "Make one new shop item. The shopkeeper's note on it: \"%s\". Take that as the idea "
        "and make it your own; the details are yours." % brief,
    ]
    if kind:
        lines.append("Make it %s." % KIND_WORDS[kind])
    if tier:
        lines.append("It will sell for %d coins. %s" % (tier[1], tier[3]))
    if avoid:
        lines.append("The shop already has today: %s. Make something unlike those."
                     % ", ".join(avoid))
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
        "three colours; no single colour should cover most of it. As many frames as the "
        "care above asks for; keep the whole answer under 12000 characters.",
        "- script (optional; the care above says whether): a short behaviour script, "
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


def to_item(raw, item_id, made, tags, log=None, price=0):
    """A checked item (Claude's dict) -> the dict encode() takes."""
    e = enums()
    return {
        "price": int(price),
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
             on_item=None, want=None, kind_at=0, briefs=None, limit=ask.DAILY, rng=None):
    """New items for `person` (None: for the pool): [signed records]. One
    Claude call an item, each from a brief (the shopkeeper's, or loose sparks
    when there is none), its kind from KIND_PLAN starting at kind_at and its
    price chosen first (pick_tier), which also says how much care it gets.
    Each record is sealed as it passes and handed to on_item(k, record); a
    call that comes to nothing is tried again, MAX_FAILS of them in all.
    Fewer than asked for is fewer, not a failure. Every record is kept
    (rec/ID), to go to the pool later. Raises if Claude cannot be asked at
    all (the day's limit, no Claude, ...) and nothing came of it."""
    st = store or kv.store()
    now = time.time() if now is None else now
    rng = rng or random.Random()
    want = ITEMS if want is None else want
    briefs = list(briefs or [])
    while len(briefs) < want:
        briefs.append(shopkeep.loose_brief(rng))
    owned = [n.lower() for n in req.get("owned", [])]
    names, records, last_error, fails = [], [], None, 0
    tier = None
    while len(records) < want and fails < MAX_FAILS:
        k = len(records)
        kind = KIND_PLAN[(kind_at + k) % len(KIND_PLAN)]
        taken = set(owned) | {n.strip().lower() for n in names}
        if tier is None:                               # chosen first; kept through a retry
            tier = pick_tier(rng)
        try:
            got = ask.ask_shape(chat, prompt_for(briefs[k], req.get("owned", []), names, kind,
                                                 tier),
                                batch_schema(1, kind), user=person or "pool", limit=limit,
                                timeout=GEN_TIMEOUT, store=st, effort=tier[2],
                                model=shopkeep.MODEL, cwd=shopkeep.home())
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
        rec = seal(encode(to_item(raw, item_id, now, record_tags([tier[0], kind]), log,
                                  tier[1])), signer)
        st.put(NS, "rec/%d" % item_id, rec)
        if person:
            st.put(NS, "own/%d" % item_id, person.encode())
        records.append(rec)
        if log:
            log("%s: %s %s, %d coins (%s)" % (raw["name"], tier[0], kind, tier[1], briefs[k]))
        tier = None
        if on_item:
            on_item(k, rec)
    if not records:
        raise last_error or ValueError("no item passed the checks")
    return records


# ---- the pool ------------------------------------------------------------------------------
#
# Every item made goes into one pool shared by everyone, and a shop's stock is
# part pool, part new. An item is in one place at a time -- the pool, one
# person's shop, or someone's things -- which own/ID says: absent, it is in
# the pool; a person's name, it is in their shop or theirs. So every item
# stays unique. The device buys offline and says what it bought (and what it
# is holding) when it next asks for a stock; the rest of the stock it had
# goes back to the pool then. Under _day_lock.

STOCK_SIZE = 8                # a shop's stock: held, then random and Tibbs's picks, then new
NEW_ITEMS = 2                 # new finds in each stock
MAX_FINDS = 5                 # new finds a paid commission may ask for (the shop: f)
ASK_LEN = 100                 # what a commission may say to look for
POOL_SHOWN = 40               # pool items Tibbs is shown to choose from
MAX_HELD = 4                  # items a person may hold in their shop


def _json(st, key, default):
    raw = st.get(NS, key)
    try:
        return json.loads(raw.decode()) if raw else default
    except ValueError:
        return default


def pool_ids(store=None):
    return [int(i) for i in _json(store or kv.store(), "pool", [])]


def _set_pool(st, ids):
    st.put(NS, "pool", json.dumps(sorted(set(ids))).encode())


def shown(person, store=None):
    """The ids in `person`'s shop now, in order."""
    return [int(i) for i in _json(store or kv.store(), "shown/" + person, [])]


def _set_shown(st, person, ids):
    st.put(NS, "shown/" + person, json.dumps(ids).encode())


def to_pool(ids, person=None, store=None):
    """Items back to the pool -- those `person` has in their shop, or (no
    person) new ones for it. Not one that is someone else's."""
    st = store or kv.store()
    pool = set(pool_ids(st))
    for i in ids:
        if st.get(NS, "rec/%d" % i) is None:
            continue
        own = st.get(NS, "own/%d" % i)
        if own is not None and (person is None or own.decode() != person):
            continue
        st.delete(NS, "own/%d" % i)
        pool.add(i)
    _set_pool(st, pool)


def _words(s):
    return set(re.findall(r"[a-z]+", (s or "").lower()))


def pick_from_pool(person, req, n, store=None, rng=None, skip=()):
    """Up to n pool items for `person`'s shop, taken out of the pool: a random
    mix, never a name they own."""
    st = store or kv.store()
    rng = rng or random.Random()
    owned = {o.lower() for o in req.get("owned", [])}
    scored = []
    for i in pool_ids(st):
        rec = st.get(NS, "rec/%d" % i)
        if rec is None:
            continue
        try:
            it = decode(rec)
        except ValueError:
            continue
        if i in skip or it["name"].strip().lower() in owned:
            continue
        scored.append((rng.random(), i))
    scored.sort(reverse=True)
    got = [i for _, i in scored[:n]]
    pool = set(pool_ids(st)) - set(got)
    _set_pool(st, pool)
    for i in got:
        st.put(NS, "own/%d" % i, person.encode())
    return got


def take_from_pool(person, ids, store=None):
    """These pool items into `person`'s shop, those still in the pool."""
    st = store or kv.store()
    pool = set(pool_ids(st))
    got = [i for i in ids if i in pool]
    _set_pool(st, pool - set(got))
    for i in got:
        st.put(NS, "own/%d" % i, person.encode())
    return got


def seed_pool(chat, n, store=None, signer=None, log=None, date=None):
    """n new items straight into the pool, from loose sparks, four at a
    time, counted against nobody's day."""
    st = store or kv.store()
    date = date or _today()
    rng = random.Random()
    made = 0
    while made < n:
        req = parse_request("")
        req["owned"] = [decode(st.get(NS, "rec/%d" % i))["name"] for i in pool_ids(st)
                        if st.get(NS, "rec/%d" % i)]
        try:
            recs = generate(chat, None, req, date, store=st, signer=signer, log=log,
                            want=min(4, n - made), kind_at=made, limit=None, rng=rng)
        except Exception as e:                          # noqa: BLE001 - try another set
            if log:
                log("seed: %s" % e)
            continue
        ids = [decode(r)["id"] for r in recs]
        with _day_lock:
            to_pool(ids, store=st)
        made += len(ids)
        if log:
            log("seed: %d in the pool (%s)" % (len(pool_ids(st)),
                                                ", ".join(decode(r)["name"] for r in recs)))
    return made


# ---- the day's stock, kept ---------------------------------------------------------------

_running = set()               # (person, date) generating in this process
_day_lock = threading.Lock()


RESTOCK_S = 4 * 3600          # a new stock every 4 hours (UTC); paying Tibbs is not waiting


def _stock_key(now=None):
    """Which stock it is now: "YYYYMMDD-N", N the 4-hour slot of the UTC day.
    Ten characters: the shop keeps it in a 12-byte field."""
    t = time.time() if now is None else now
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    return "%s-%d" % (d.strftime("%Y%m%d"), d.hour * 3600 // RESTOCK_S)


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


def _item_note(rec):
    it = decode(rec)
    kinds = {v: k for k, v in enums()["kinds"].items()}
    return "%s (%s, %d coins: %s)" % (it["name"], kinds.get(it["kind"], "item"),
                                      it["price"] or 0, it["line"])


def _make_day(chat, person, req, date, store, batch, base, n_pick, n_new, seed, passed=(),
              skey=None):
    """The rest of a stock, after the `base` (held and random) items already
    on show: Tibbs's turn -- his greeting, his picks from the pool, briefs for
    the new finds -- then his picks, then a new item for each brief."""
    key = (person, skey or _stock_key())
    told = {"say": ""}
    at = [base]
    say_log = lambda s: sys.stderr.write("jar: %s: %s\n" % (person, s))   # noqa: E731

    def state(st_, n):
        s = {"state": st_, "date": key[1], "tags": [], "n": n, "batch": batch, "say": told["say"],
             "seed": seed}
        if st_ == "pending":
            s["req"] = req
        _set_day(person, s, store)

    def show(rec):
        store.put(NS, "stock/%s/%d" % (person, at[0]), rec, ttl=STOCK_TTL)
        with _day_lock:
            _set_shown(store, person, shown(person, store) + [decode(rec)["id"]])
        at[0] += 1
        state("pending", at[0])

    def on_item(k, rec):                                # on show the moment it is made
        show(rec)
        shopkeep.note("New in %s's shop: %s." % (shopkeep.display(person), _item_note(rec)),
                      store)

    try:
        pool = [(i, ) + _pool_view(store, i) for i in pool_ids(store) if i not in passed]
        rng = random.Random()
        rng.shuffle(pool)
        told["say"], mine, briefs = shopkeep.stock_turn(
            chat, person, pool[:POOL_SHOWN], n_pick, n_new, friends_of(person, store),
            store=store, log=say_log, commission=req.get("ask") if req.get("finds") else None)
        with _day_lock:                                 # his picks, if still free
            got = take_from_pool(person, [i for i in mine if i in dict(
                (p[0], 1) for p in pool[:POOL_SHOWN])][:n_pick], store)
        with _day_lock:                                 # what he got wrong: random ones
            got += pick_from_pool(person, req, n_pick - len(got), store, skip=passed)
        state("pending", at[0])
        for i in got:
            rec = store.get(NS, "rec/%d" % i)
            if rec is not None:
                show(rec)
        records = generate(chat, person, req, date, store=store, on_item=on_item, want=n_new,
                           kind_at=batch, briefs=briefs, log=say_log)
        state("ok", at[0])
        sys.stderr.write("jar: %s: %d items for %s (%d his, %d new)\n" % (
            person, at[0], key[1], len(got), len(records)))
    except Exception as e:                              # noqa: BLE001 - reported to the device
        why = wire.flat(str(e) or type(e).__name__, 120)
        if at[0]:                                       # the pool's part is still a stock
            state("ok", at[0])
        else:
            _set_day(person, {"state": "error", "date": key[1], "why": why}, store)
        sys.stderr.write("jar: %s: stock: %s\n" % (person, why))
    finally:
        with _day_lock:
            _running.discard(key)


def _pool_view(st, i):
    """(name, kind, price, line) of a pool item, for Tibbs to choose from."""
    rec = st.get(NS, "rec/%d" % i)
    if rec is None:
        return ("?", "item", 0, "")
    it = decode(rec)
    kinds = {v: k for k, v in enums()["kinds"].items()}
    return (it["name"], kinds.get(it["kind"], "item"), it["price"] or 0, it["line"])


def start_day(chat, person, req, now=None, store=None, fresh=False):
    """("pending" | "ok", date): today's stock, started if it is not made or
    making. A failed one is tried again. `fresh` makes a new one even when
    today's is done -- the shop's `r`. A new stock (STOCK_SIZE) is what the
    person holds from the last one, then pool items at random, then pool
    items Tibbs picks for them, then NEW_ITEMS new finds; what they neither
    bought nor held goes back to the pool."""
    st = store or kv.store()
    date = _today(now)
    key = (person, _stock_key(now))
    with _day_lock:
        cur = day_state(person, st)
        if cur and cur.get("date") == key[1]:
            if cur["state"] == "ok" and not fresh:
                return "ok", key[1]
            if cur["state"] == "pending" and key in _running:
                return "pending", key[1]
    with _day_lock:
        if key in _running:
            return "pending", key[1]
        _running.add(key)
        # A batch number tells the shop one stock from the next on the same
        # day (r makes another), so it can tell "the rest of this one" from
        # "a new one" when it is opened again.
        batch = st.incr(NS, "batch")
        prev = shown(person, st)
        bought = set(req.get("bought", [])) & set(prev)
        who = shopkeep.display(person)
        for i in prev:
            if i in bought and st.get(NS, "rec/%d" % i) is not None:
                shopkeep.note("%s bought %s." % (who, _item_note(st.get(NS, "rec/%d" % i))), st)
        if req.get("jar"):
            shopkeep.note("In %s's jar now: %s." % (who, ", ".join(req["jar"])), st)
        held = [i for i in req.get("held", []) if i in prev and i not in bought][:MAX_HELD]
        passed = [i for i in prev if i not in bought and i not in held]
        to_pool(passed, person, st)
        # A commission -- the player paid Tibbs to go looking -- is more new
        # finds, fewer from the pool; coins are the device's, taken there.
        n_new = max(NEW_ITEMS, min(req.get("finds", 0), MAX_FINDS, STOCK_SIZE - len(held)))
        if req.get("finds"):
            shopkeep.note("%s paid you to go out and find new things%s." % (
                who, (" -- they asked for: \"%s\"" % req["ask"]) if req.get("ask") else ""), st)
        rest = STOCK_SIZE - len(held) - n_new
        n_pick = (rest + 1) // 2                        # Tibbs's choice ...
        picks = pick_from_pool(person, req, rest - n_pick, st, skip=passed)   # ... and chance's
        base = []
        for i in held + picks:
            rec = st.get(NS, "rec/%d" % i)
            if rec is not None:
                st.put(NS, "stock/%s/%d" % (person, len(base)), rec, ttl=STOCK_TTL)
                base.append(i)
        _set_shown(st, person, base)
        # Plants are seed packets now and then, instead of steering the stock.
        rng = random.Random()
        seed = rng.choice(sorted(SEED_PRICE)) if rng.randrange(100) < SEED_CHANCE else ""
        _set_day(person, {"state": "pending", "date": key[1], "tags": [], "n": len(base),
                          "batch": batch, "req": req, "seed": seed}, st)
    threading.Thread(target=_make_day, args=(chat, person, req, date, st, batch, len(base),
                                             n_pick, n_new, seed, set(passed), key[1]),
                     daemon=True).start()
    return "pending", key[1]


def resume_day(chat, person, store=None, now=None):
    """A stock left "pending" by a server that stopped mid-way (a restart, a
    deploy) has nothing making it: start it again, from the request it was
    made from. True if it did."""
    st = store or kv.store()
    cur = day_state(person, st)
    key = (person, _stock_key(now))
    if not cur or cur.get("state") != "pending" or cur.get("date") != key[1]:
        return False
    with _day_lock:
        if key in _running:
            return False
    req = cur.get("req") or parse_request("")
    # what it was showing is held, so the restart keeps it
    req = dict(req, held=shown(person, st)[:MAX_HELD], bought=[])
    start_day(chat, person, req, now=now, store=st, fresh=True)
    return True


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
            if mutual(me, who, st):
                shopkeep.note("%s and %s are friends now." % (shopkeep.display(me),
                                                              shopkeep.display(who)), st)
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
def post_talk(h, args):
    """say something to Tibbs, the shopkeeper; his reply comes later (GET)"""
    try:
        body = h.body(1024).decode("utf-8", "replace")
    except ValueError as e:
        raise kv.TooBig(str(e))
    if not _chat_ok(h):
        h.text("error this server runs without Claude\n", 503)
        return
    try:
        me = kv.me()
        started = shopkeep.say(h.chat, me, body,
                               [f for f in friends_of(me) if mutual(me, f)])
    except ValueError as e:
        raise KVError(str(e))
    h.text("pending\n" if started else "busy he is still answering\n")


@kv_route
def get_talk(h, args):
    """Tibbs: "pending", "ok" or "error WHY", then his line of the day and the talk"""
    h.text(shopkeep.talk_text(kv.me()))


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
    out = "ok %s\ntags %s\nitems %d\nbatch %d\n" % (
        cur["date"], ", ".join(cur.get("tags", [])), cur["n"], cur.get("batch", 0))
    if cur.get("say"):
        out += "say %s\n" % cur["say"]
    if cur.get("seed"):
        out += "seed %s %d\n" % (cur["seed"], SEED_PRICE[cur["seed"]])
    return out + ("more\n" if cur["state"] == "pending" else "")


@kv_route
def get_day(h, args):
    """today's shop stock: pending, ok and its tags, or why not"""
    cur = day_state(kv.me())
    if not cur:
        raise NotFound("no stock yet: POST /jar/day")
    if _chat_ok(h) and resume_day(h.chat, kv.me()):
        sys.stderr.write("jar: %s: a stock left half-made, started again\n" % kv.me())
        cur = day_state(kv.me())
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
    shopkeep.note("%s sent %s %s as a gift%s." % (
        shopkeep.display(me), shopkeep.display(to), _item_note(rec),
        (", with a note: '%s'" % note) if note else ""))
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
    ("POST", "/jar/talk", post_talk, "device_or_dash"),
    ("GET", "/jar/talk", get_talk, "device_or_dash"),
    ("GET", "/jar/day", get_day, "device_or_dash"),
    ("GET", "/jar/item", get_item, "device_or_dash"),
    ("GET", "/jar/pubkey", get_pubkey, "device_or_dash"),
    ("POST", "/jar/gift", post_gift, "device_or_dash"),
    ("POST", "/jar/thanks", post_thanks, "device_or_dash"),
]


def main(argv=None):
    """python -m server.jar seed N [--claude-cli PATH]: N new items straight
    into the pool (CARDOS_STATE is the store, as for the server)."""
    import argparse
    from .chat import ChatService
    ap = argparse.ArgumentParser(prog="python -m server.jar")
    ap.add_argument("what", choices=["seed", "pool"])
    ap.add_argument("n", type=int, nargs="?", default=20)
    ap.add_argument("--claude-cli", default=None)
    args = ap.parse_args(argv)
    if args.what == "pool":
        st = kv.store()
        for i in pool_ids(st):
            it = decode(st.get(NS, "rec/%d" % i))
            print("No.%04d %-12s %s" % (i, it["name"], it["line"]))
        return
    chat = ChatService(claude=args.claude_cli) if args.claude_cli else ChatService()
    seed_pool(chat, args.n, log=lambda s: print(s, flush=True))


if __name__ == "__main__":
    main()
