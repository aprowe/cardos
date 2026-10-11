"""server/jar.py: Jar Factory's stock, friends and gifts.

    python -m server.tests.test_jar
    python -m server.tests.test_jar --fixtures    rewrite test/fixtures/jar_*.bin

The fixtures are for the device's host test: a record signed with a fixed
test key, exactly what was signed, and the key's public point. ECDSA here
is randomised, so rewriting them changes the signature bytes (and only
those); the test below checks whatever is there.
"""
import base64
import datetime
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest import mock
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import accounts, app, ask, jar, kv, people, shopkeep, sign
from server import chat as chatmod

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURES = os.path.join(ROOT, "test", "fixtures")


# ---- a fixed key, for the fixtures --------------------------------------------------

def test_key():
    from cryptography.hazmat.primitives.asymmetric import ec
    n = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    d = int.from_bytes(hashlib.sha256(b"jar factory test key").digest(), "big") % (n - 1) + 1
    return ec.derive_private_key(d, ec.SECP256R1())


def test_signer(data):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    r, s = decode_dss_signature(test_key().sign(bytes(data), ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def test_pub():
    from cryptography.hazmat.primitives import serialization
    return test_key().public_key().public_bytes(serialization.Encoding.X962,
                                                serialization.PublicFormat.UncompressedPoint)


# ---- items as Claude would give them ------------------------------------------------------

def sprite(seed=0, colours=3):
    """A 16x16 blob: an outline (1), a body striped in colours 2.., eyes (7)."""
    rows = []
    for y in range(16):
        row = ""
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 8.5) ** 2
            if d > 49:
                row += "0"
            elif d > 36:
                row += "1"
            elif y == 6 and x in (5, 10):
                row += "7"
            else:
                row += str(2 + (x + y + seed) % max(1, colours - 1))
        rows.append(row)
    return rows


def raw_item(name="Glowcap", **kw):
    it = {
        "name": name, "line": "A mushroom that hums at night",
        "kind": "critter", "movement": "hops", "speed": "slow", "zone": "garden",
        "habits": [{"when": "poke", "do": "say", "do_arg": 1},
                   {"when": "time", "when_arg": 4, "do": "glow", "do_arg": 1},
                   {"when": "tick", "when_arg": 40, "do": "frame", "do_arg": 1}],
        "bubbles": ["hm?", "glowy!"],
        "palette": ["#000000", "#1a1020", "#c04070", "#f0a0c0", "#80e0a0", "#ffd166",
                    "#40a0ff", "#ffffff"],
        "frames": [sprite(0), sprite(1)],
    }
    it.update(kw)
    return it


class Stub:
    """ask.ask_shape, standing in: each call answers with the next batch
    given, or (when it runs out) as many good items as the schema asks for."""

    def __init__(self, *batches):
        self.batches = list(batches)
        self.calls = []
        self.lock = threading.Lock()
        self.serial = 0
        self.beats, self.talks = [], []

    def __call__(self, chat, prompt, schema, **kw):
        with self.lock:
            props = schema["properties"]
            if "briefs" in props:                       # Tibbs's beat
                self.beats.append(prompt)
                n = props["briefs"]["maxItems"]
                return {"line": "A crate came in.",
                        "briefs": ["find %d of the day" % (i + 1) for i in range(n)]}
            if "reply" in props:                        # Tibbs talking
                self.talks.append(prompt)
                return {"reply": "Hm, I know a fellow."}
            n = props["items"]["maxItems"]
            self.calls.append((n, prompt, kw))
            if self.batches:
                b = self.batches.pop(0)
                if isinstance(b, Exception):
                    raise b
                return {"items": b}
            out = []
            kinds = schema["properties"]["items"]["items"]["properties"]["kind"]["enum"]
            for _ in range(n):
                self.serial += 1
                out.append(raw_item("Thing %d" % self.serial, kind=kinds[0] if len(kinds) == 1
                                    else "critter"))
            return {"items": out}


class FakeTibbs:
    """shopkeep._ask, standing in: one session ("s1"), a stock turn answered
    with JSON (his first n picks from what he is shown), talk with words."""

    def __init__(self, *answers):
        self.answers = list(answers)
        self.prompts, self.sessions = [], []
        self.lock = threading.Lock()
        self.stock = ""

    def __call__(self, chat, prompt, session, effort):
        with self.lock:
            self.prompts.append(prompt)
            self.sessions.append(session)
            if "needs filling" in prompt:
                self.stock = prompt
            if self.answers:
                a = self.answers.pop(0)
                if isinstance(a, Exception):
                    raise a
                return a, "s1"
            if "needs filling" in prompt or "not the JSON" in prompt:
                prompt = self.stock                      # a retry: the request it was for
                m = re.search(r"Pick (\d+) of those", prompt)
                n = int(m.group(1)) if m else 0
                ids = [int(x) for x in re.findall(r"^  (\d+): ", prompt, re.M)][:n]
                briefs = ["find %d of the day" % (i + 1)
                          for i in range(len(re.findall(r"find \d+[^\n]*? sparks", prompt)))]
                return json.dumps({"line": "A crate came in.", "picks": ids,
                                   "briefs": briefs}), "s1"
            return "Hm, I know a fellow.", "s1"


def header_table():
    """The layout table in apps/jaritem.h's opening comment: [(off, size, field)]."""
    with open(jar.HEADER, encoding="utf-8") as f:
        src = f.read()
    rows = []
    for m in re.finditer(r"^ \*\s+(\d+)\s+(\d+)\s+(\S.*)$", src, re.M):
        rows.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return rows


def ji_px(px, i):
    """jaritem.h's ji_px, line for line."""
    bit = i * 3
    b, sh = bit >> 3, bit & 7
    v = px[b]
    if sh > 5:
        v |= px[b + 1] << 8
    return (v >> sh) & 7


# ---- the record --------------------------------------------------------------------------

class Record(unittest.TestCase):

    def test_layout_is_the_headers(self):
        table = header_table()
        self.assertEqual(len(table), 32)
        ours = sorted(jar.OFF.values())
        self.assertEqual([(o, s) for o, s, _ in table], ours)
        with open(jar.HEADER, encoding="utf-8") as f:
            defs = dict(re.findall(r"#define\s+(JI_\w+)\s+(\d+)", f.read()))
        want = {"JI_VERSION": jar.VERSION, "JI_HDR": jar.HDR, "JI_MAX": jar.REC_MAX,
                "JI_FRAME_BYTES": jar.FRAME_BYTES, "JI_MAX_FRAMES": jar.MAX_FRAMES,
                "JI_NAME": jar.NAME_LEN, "JI_LINE": jar.LINE_LEN, "JI_BUB": jar.BUB_LEN,
                "JI_MAX_BUB": jar.MAX_BUB, "JI_MAX_HAB": jar.MAX_HAB, "JI_MEM": jar.MEM,
                "JI_WHO": jar.WHO_LEN, "JI_TAGS": jar.TAGS_LEN, "JI_SIG_MAX": jar.SIG_MAX,
                "JI_SCRIPT_MAX": jar.SCRIPT_MAX}
        self.assertEqual({k: int(defs[k]) for k in want}, want)

    def test_enums_come_from_the_header(self):
        e = jar.enums()
        self.assertEqual(e["kinds"], {"floor": 0, "hanging": 1, "critter": 2})
        self.assertEqual(e["actions"]["say"], 10)
        self.assertEqual(e["events"]["weather"], 7)
        self.assertEqual(list(e["zones"]), ["garden", "works", "dock", "water", "high", "anywhere"])

    def test_frames_pack_as_ji_px_reads_them(self):
        rng = random.Random(7)
        for _ in range(20):
            rows = ["".join(str(rng.randrange(8)) for _ in range(16)) for _ in range(16)]
            data = jar.pack_frame(rows)
            self.assertEqual(len(data), 96)
            flat = [int(c) for c in "".join(rows)]
            self.assertEqual([ji_px(data, i) for i in range(256)], flat)
            self.assertEqual(jar.unpack_frame(data), flat)

    def test_frames_pack_as_make_jar_art_does(self):
        spec = importlib.util.spec_from_file_location(
            "make_jar_art", os.path.join(ROOT, "tools", "make_jar_art.py"))
        mja = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mja)
        rows = sprite(3)
        letters = "abcdefg"
        art = ["".join("." if c == "0" else letters[int(c) - 1] for c in r) for r in rows]
        self.assertEqual(mja.pack([art], 16, 16, letters, "t")[0], jar.pack_frame(rows))
        self.assertEqual(mja.rgb565((0xc0, 0x40, 0x70)), jar.rgb565("#c04070"))

    def by_hand(self):
        """A record made byte by byte from the header's table, and the item
        dict that should encode to it."""
        frames = [jar.pack_frame(sprite(0)), jar.pack_frame(sprite(2))]
        sig = bytes(range(64))
        script = b"\x01\x02\x03"
        b = bytearray(208)
        b[0], b[1] = 1, 208
        total = 208 + 64 + 3 + 192
        b[2:4] = total.to_bytes(2, "little")
        b[4:8] = (4321).to_bytes(4, "little")
        b[8], b[9], b[10], b[11] = 2, 2, 1, 2
        b[12:12 + 7] = b"Glowcap"
        b[24:24 + 9] = b"hums softly"[:9]
        pal = [0, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0xFFFF]
        for i, v in enumerate(pal):
            b[56 + 2 * i:58 + 2 * i] = v.to_bytes(2, "little")
        b[72], b[73], b[74], b[75] = 1, 0, 3, 2
        b[76:80] = bytes([1, 0, 10, 1])
        b[80:84] = bytes([6, 4, 8, 1])
        b[88:91] = b"hm?"
        b[100:106] = b"glowy!"
        b[136:138] = (0xFFFF).to_bytes(2, "little")        # memory[0] = -1
        b[152:161] = b"Jar Works"
        b[164:168] = (1791504000).to_bytes(4, "little")
        b[168:177] = b"odd,glowy"
        b[192:195] = b"sam"
        b[204] = 64
        b[205:207] = (3).to_bytes(2, "little")
        rec = bytes(b) + sig + script + frames[0] + frames[1]
        item = {"id": 4321, "kind": 2, "flags": 1, "name": "Glowcap", "line": "hums soft",
                "pal": pal, "move": 1, "speed": 0, "zone": 3,
                "habits": [(1, 0, 10, 1), (6, 4, 8, 1)], "bubbles": ["hm?", "glowy!"],
                "mem": [-1, 0, 0, 0, 0, 0, 0, 0], "maker": "Jar Works", "made": 1791504000,
                "tags": "odd,glowy", "gifted": "sam", "sig": sig, "script": script,
                "frames": frames}
        return rec, item

    def test_encode_is_the_hand_built_record(self):
        rec, item = self.by_hand()
        self.assertEqual(jar.encode(item), rec)

    def test_decode_round_trips(self):
        rec, item = self.by_hand()
        got = jar.decode(rec)
        for k, v in item.items():
            self.assertEqual(got[k], v, k)
        self.assertEqual(jar.encode(got), rec)

    def test_decode_refuses_what_jitem_decode_refuses(self):
        rec, _ = self.by_hand()
        for bad, why in ((rec[:100], "-1"), (rec[:-1], "-1"),
                         (rec[:9] + b"\x05" + rec[10:], "-3"),          # 5 frames
                         (rec[:204] + b"\x40\x04\x00" + rec[207:], "-2")):   # script 4
            with self.assertRaises(ValueError) as cm:
                jar.decode(bad)
            self.assertTrue(str(cm.exception).startswith(why), (why, cm.exception))
        b = bytearray(rec)
        b[76] = 99                                           # an event not known here
        self.assertEqual(len(jar.decode(bytes(b))["habits"]), 1)


class Signing(unittest.TestCase):

    def test_seal_and_message(self):
        item = jar.to_item(raw_item(), 777, 1791504000, "odd,glowing")
        unsigned = jar.encode(item)
        rec = jar.seal(unsigned, test_signer)
        self.assertEqual(len(rec), len(unsigned) + 64)
        self.assertEqual(rec[204], 64)
        self.assertEqual(int.from_bytes(rec[2:4], "little"), len(rec))
        self.assertEqual(jar.message(rec), unsigned)
        self.assertEqual(jar.decode(rec)["frames"], item["frames"])
        self.assertTrue(jar.verified(rec, test_pub()))
        bad = bytearray(rec)
        bad[-1] ^= 1
        self.assertFalse(jar.verified(bytes(bad), test_pub()))

    def test_an_item_ships_its_script_compiled_or_none(self):
        # Step 7: a script that compiles and lives through its simulated day
        # goes into the record; one that does not is dropped and the item
        # ships on its recipe.
        from server import jarvm
        good = raw_item("Hummer", script="on poke: say 1; hop\nend\non time night: glow on\nend")
        rec = jar.encode(jar.to_item(good, 101, 1791504000, "cosy"))
        code = jar.decode(rec)["script"]
        self.assertTrue(code)
        self.assertEqual(code, jarvm.prepare_script(good["script"], nbub=2, nframes=2))
        logged = []
        bad = raw_item("Mumbler", script="on poke: say then then")
        rec = jar.encode(jar.to_item(bad, 102, 1791504000, "cosy", logged.append))
        self.assertEqual(jar.decode(rec)["script"], b"")
        self.assertTrue(logged and "Mumbler" in logged[0])
        self.assertEqual(jar.decode(jar.encode(jar.to_item(raw_item("Plain"), 103, 1791504000,
                                                           "cosy")))["script"], b"")
        self.assertIn("script", jar.item_schema()["properties"])
        self.assertIn(jarvm.LANGUAGE_GUIDE[:40],
                      jar.prompt_for(8, ["cosy"], {"date": "d", "season": "s", "moon": "m"}, []))

    def test_memory_is_not_signed_but_everything_else_is(self):
        # Scripts write the memory slots on the device; a gift must still
        # verify afterwards. Anything else changed must not.
        raw = raw_item("Memo", line="remembers")
        rec = jar.seal(jar.encode(jar.to_item(raw, 77, 1791504000, "cosy")), test_signer)
        mem = bytearray(rec)
        mem[136:152] = bytes(range(1, 17))
        self.assertTrue(jar.verified(bytes(mem), test_pub()))
        renamed = bytearray(rec)
        renamed[12] ^= 1
        self.assertFalse(jar.verified(bytes(renamed), test_pub()))

    def test_the_longer_header(self):
        raw = raw_item("Cupboard")
        item = jar.to_item(raw, 90, 1791504000, "big")
        plain = jar.encode(item)
        self.assertEqual(plain[1], 208)                       # a thing: as before, byte for byte
        item.update(role=1, scale=2, surface=3, voice=5, pdx=-5, pdy=3)
        rec = jar.encode(item)
        self.assertEqual(rec[1], 224)
        self.assertEqual(len(rec), len(plain) + 16)
        self.assertEqual(rec[208:215], bytes([1, 2, 3, 0, 0xFB, 3, 5]))
        self.assertEqual(rec[224:], plain[208:])               # what follows is the same
        it = jar.decode(rec)
        self.assertEqual((it["role"], it["scale"], it["surface"], it["voice"], it["pdx"], it["pdy"]),
                         (1, 2, 3, 5, -5, 3))
        self.assertEqual(jar.encode(it), rec)
        self.assertEqual(jar.decode(plain)["role"], 0)
        sealed = jar.seal(rec, test_signer)
        self.assertTrue(jar.verified(sealed, test_pub()))
        self.assertEqual(sealed[224:288], sealed[224:224 + sealed[204]])
        bad = bytearray(sealed)
        bad[209] = 1                                           # the header is signed, all of it
        self.assertFalse(jar.verified(bytes(bad), test_pub()))
        with self.assertRaises(ValueError):
            jar.encode(dict(item, role=7))
        with open(os.path.join(FIXTURES, "jar_item_ext.bin"), "rb") as f:
            fx = f.read()
        self.assertTrue(jar.verified(fx, test_pub()))
        self.assertEqual(jar.decode(fx)["part"], 1)

    def test_fixtures(self):
        with open(os.path.join(FIXTURES, "jar_item_signed.bin"), "rb") as f:
            rec = f.read()
        with open(os.path.join(FIXTURES, "jar_item_message.bin"), "rb") as f:
            msg = f.read()
        with open(os.path.join(FIXTURES, "jar_pubkey.bin"), "rb") as f:
            pub = f.read()
        self.assertEqual(pub, test_pub())
        self.assertEqual(len(pub), 65)
        self.assertEqual(jar.message(rec), msg)
        # the extraction, spelled out the way a device does it: the memory
        # slots (136..151) zeroed, which the fixture's are not
        self.assertNotEqual(rec[136:152], bytes(16))
        self.assertEqual(msg[136:152], bytes(16))
        self.assertEqual(msg[:136], rec[:2] + (len(rec) - 64).to_bytes(2, "little") + rec[4:136])
        self.assertEqual(msg[152:204], rec[152:204])
        self.assertEqual(msg[204], 0)
        self.assertEqual(msg[205:208], rec[205:208])
        self.assertEqual(msg[208:], rec[208 + 64:])
        self.assertTrue(sign.verify(msg, rec[208:272], pub))
        self.assertTrue(jar.verified(rec, pub))
        self.assertEqual(jar.decode(rec)["name"], "Fixture")


def write_fixtures():
    raw = raw_item("Fixture", line="Made for the device's tests")
    item = jar.to_item(raw, 4242, 1791504000, "odd,glowing,autumn")
    unsigned = bytearray(jar.encode(item))
    # memory a script wrote, which the signature must not cover
    unsigned[136:152] = (1).to_bytes(2, "little") * 3 + (0xFFFF).to_bytes(2, "little") + bytes(8)
    rec = jar.seal(bytes(unsigned), test_signer)
    os.makedirs(FIXTURES, exist_ok=True)
    # a birdhouse: furniture, twice the size, its last frame the bird
    raw2 = raw_item("Birdhouse", line="A house for a small bird")
    raw2["frames"] = [raw2["frames"][0], raw2["frames"][0]]
    ext = jar.to_item(raw2, 4343, 1791504000, "cosy,music")
    ext.update(role=1, scale=2, surface=4, part=1, pdx=-6, pdy=-9, voice=2)
    rec2 = jar.seal(jar.encode(ext), test_signer)
    for name, data in (("jar_item_signed.bin", rec), ("jar_item_message.bin", jar.message(rec)),
                       ("jar_pubkey.bin", test_pub()), ("jar_item_ext.bin", rec2),
                       ("jar_item_ext_message.bin", jar.message(rec2))):
        with open(os.path.join(FIXTURES, name), "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (name, len(data)))


# ---- checking and making a day's stock ----------------------------------------------------

class Checks(unittest.TestCase):

    def test_good(self):
        self.assertEqual(jar.check_item(raw_item()), [])

    def test_flat_sprite(self):
        flat = [r.replace("3", "2").replace("7", "2").replace("1", "2") for r in sprite()]
        why = jar.check_item(raw_item(frames=[flat]))
        self.assertTrue(any("too flat" in w for w in why), why)

    def test_blank_sprite(self):
        why = jar.check_item(raw_item(frames=[["0" * 16] * 16]))
        self.assertTrue(any("blank" in w for w in why), why)

    def test_bad_word(self):
        for field in ({"name": "Shit Snail"}, {"line": "a fucking mushroom"},
                      {"bubbles": ["hi", "b1tch"]}):
            why = jar.check_item(raw_item(**field))
            self.assertIn("a word the filter refuses", why, field)
        self.assertTrue(jar.clean("Scunthorpe snail"))

    def test_recipe_options(self):
        def why(*habits, **kw):
            return jar.check_item(raw_item(habits=list(habits), **kw))
        self.assertTrue(why({"when": "poke", "do": "say", "do_arg": 2}))       # 2 bubbles
        self.assertTrue(why({"when": "poke", "do": "frame", "do_arg": 2}))     # 2 frames
        self.assertTrue(why({"when": "near", "when_arg": 9, "do": "hop"}))
        self.assertTrue(why({"when": "tick", "when_arg": 1, "do": "hop"}))
        self.assertTrue(why({"when": "poke", "do": "particle", "do_arg": 5}))
        self.assertTrue(why({"when": "poke", "do": "hop"}, kind="hanging"))
        self.assertTrue(why({"when": "poke", "do": "dance"}))                  # schema
        self.assertEqual(why({"when": "near", "when_arg": 2, "do": "particle", "do_arg": 1}), [])

    def test_names_taken(self):
        self.assertTrue(jar.check_item(raw_item("Glowcap"), {"glowcap"}))

    def test_request_and_tags(self):
        req = jar.parse_request("garden mushroom=4,flower=2,weed=9,cactus=x\n"
                                "shelf spooky,Bad Tag,cosy\nowned Snail\tking,Moth\n"
                                "tz PST8PDT,M3.2.0,M11.1.0\nnonsense here\n")
        self.assertEqual(req["garden"], {"mushroom": 4, "flower": 2})
        self.assertEqual(req["shelf"], ["spooky", "cosy"])
        self.assertEqual(req["owned"], ["Snail king", "Moth"])
        self.assertEqual(req["tz"], "PST8PDT,M3.2.0,M11.1.0")
        self.assertEqual(req["hint"], "")

    def test_the_jar_is_in_the_request(self):
        req = jar.parse_request("jar Moon Moth,Kettle Imp,%s\n" % ("x" * 30))
        self.assertEqual(req["jar"][:2], ["Moon Moth", "Kettle Imp"])
        self.assertLessEqual(len(req["jar"][2]), jar.NAME_LEN)
        self.assertLessEqual(len(jar.record_tags(["a" * 30])), 24)

    def test_the_day(self):
        self.assertEqual(jar.moon(datetime.date(2026, 10, 26))[1], True)   # a full moon
        self.assertEqual(jar.moon(datetime.date(2026, 10, 10))[0], "new moon")
        self.assertEqual(jar.season(datetime.date(2026, 3, 1)), ("spring", True))
        self.assertEqual(jar.season(datetime.date(2026, 7, 4), south=True), ("winter", False))
        self.assertEqual(jar.city_for_tz("PST8PDT,M3.2.0,M11.1.0")[0], "san francisco")
        self.assertEqual(jar.city_for_tz("AEST-10AEDT,M10.1.0,M4.1.0/3")[0], "sydney")
        self.assertEqual(jar.city_for_tz("IST-5:30")[0], "delhi")
        self.assertEqual(jar.city_for_tz("CST-8")[0], "shanghai")
        self.assertEqual(jar.city_for_tz("<+0545>-5:45")[0], "kathmandu")
        self.assertIsNone(jar.city_for_tz("UTC0"))
        self.assertIsNone(jar.city_for_tz(""))
        self.assertEqual(jar.weather_word(63), "rainy")
        with mock.patch("server.jar.fetch_weather", return_value=None):
            jar._weather_cache.clear()
            facts = jar.day_facts(datetime.date(2026, 10, 9), "GMT0BST,M3.5.0/1,M10.5.0")
        self.assertEqual(facts["city"], "london")
        self.assertIsNone(facts["weather"])                    # unreachable: quietly none


class Generate(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        sign.forget_cached()
        jar._weather_cache.clear()
        p = mock.patch("server.jar.fetch_weather", return_value=("rainy", 11.0))
        p.start()
        self.addCleanup(p.stop)
        self.req = jar.parse_request("garden fern=3\nshelf cosy\nowned Moth\ntz CET-1CEST\n")
        self.date = datetime.date(2026, 10, 9)

    def tearDown(self):
        kv.close_all()
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_with(self, stub):
        p = mock.patch("server.ask.ask_shape", stub)
        p.start()
        self.addCleanup(p.stop)
        return jar.generate(None, "alex", self.req, self.date, now=1791590400)

    def test_a_good_batch(self):
        stub = Stub()
        got = []
        p = mock.patch("server.ask.ask_shape", stub)
        p.start()
        self.addCleanup(p.stop)
        briefs = ["a brass beetle that ticks", "a felt cloud"]
        recs = jar.generate(None, "alex", self.req, self.date, now=1791590400,
                            on_item=lambda k, rec: got.append((k, rec)), want=4, briefs=briefs)
        self.assertEqual(len(recs), 4)
        self.assertEqual(got, list(enumerate(recs)))           # each as it was made
        self.assertEqual([c[0] for c in stub.calls], [1] * 4)   # one item a call
        n, prompt, kw = stub.calls[0]
        self.assertIn('"a brass beetle that ticks"', prompt)    # Tibbs's brief
        self.assertIn('"a felt cloud"', stub.calls[1][1])
        self.assertIn("a find suggested by", stub.calls[2][1])  # no brief: loose sparks
        self.assertIn("Make it a critter", prompt)              # its kind, given
        self.assertIn("Thing 1", stub.calls[1][1])              # what is already made
        self.assertIn("Moth", prompt)                          # owned: not again
        steer = prompt.split("The script language:")[0]    # the guide names every weather
        for word in ("tags", "autumn", "rainy", "fern", "shelf"):
            self.assertNotIn(word, steer)                       # nothing steers it but the brief
        self.assertEqual(kw["user"], "alex")
        self.assertIn("It will sell for", prompt)              # a price, chosen first
        ids = set()
        st = kv.store()
        for rec in recs:
            it = jar.decode(rec)
            self.assertTrue(jar.verified(rec))
            self.assertEqual(it["maker"], "Jar Works")
            self.assertEqual(it["made"], 1791590400)
            self.assertGreater(it["id"], jar.ID_BASE)
            self.assertEqual(st.get(jar.NS, "own/%d" % it["id"]), b"alex")
            ids.add(it["id"])
        self.assertEqual(len(ids), 4)
        e = jar.enums()["kinds"]
        self.assertEqual([jar.decode(r)["kind"] for r in recs], [e[k] for k in jar.KIND_PLAN[:4]])

    def test_failures_are_asked_for_again(self):
        flat = [r.replace("3", "2").replace("7", "2").replace("1", "2") for r in sprite()]
        stub = Stub([raw_item("Good 0")], [raw_item("Flat", frames=[flat])],
                    [raw_item("Damn Shit")])
        recs = self.run_with(stub)
        self.assertEqual(len(recs), 8)
        self.assertEqual(len(stub.calls), 10)                    # the two again
        names = [jar.decode(r)["name"] for r in recs]
        self.assertNotIn("Flat", names)
        self.assertNotIn("Damn Shit", names)
        self.assertIn("Good 0", stub.calls[3][1])                # not those names again

    def test_bounded_and_fewer_is_fine(self):
        bad = [raw_item("Bad", frames=[["0" * 16] * 16])]
        stub = Stub([raw_item("One")], bad, bad, ask.Invalid("did not fit"), bad)
        recs = self.run_with(stub)
        self.assertEqual(len(recs), 1)
        self.assertEqual(len(stub.calls), 1 + jar.MAX_FAILS)

    def test_out_of_asks_keeps_what_came(self):
        stub = Stub([raw_item("One")], [raw_item("Two")], ask.RateLimited("the day's limit"))
        recs = self.run_with(stub)
        self.assertEqual(len(recs), 2)

    def test_nothing_at_all_is_an_error(self):
        stub = Stub(*[ask.Invalid("x")] * jar.MAX_FAILS)
        with self.assertRaises(ask.Invalid):
            self.run_with(stub)

    def test_prices_are_chosen_first_and_dear_ones_get_more_care(self):
        rng = random.Random(3)
        seen = {}
        for _ in range(2000):
            name, price, effort, words = jar.pick_tier(rng)
            seen.setdefault(name, []).append(price)
            self.assertEqual(price % 5, 0)
        self.assertEqual(set(seen), {t[0] for t in jar.TIERS})
        self.assertGreater(len(seen["common"]), len(seen["rare"]) * 2)
        self.assertGreater(min(seen["treasure"]), max(seen["common"]))
        stub = Stub()
        recs = self.run_with(stub)
        for (n, prompt, kw), rec in zip(stub.calls, recs):
            price = jar.decode(rec)["price"]
            tier = [t for t in jar.TIERS if t[2][0] <= price <= t[2][1]][0]
            self.assertIn("sell for %d coins" % price, prompt)
            self.assertEqual(kw["effort"], tier[3])
            self.assertTrue(jar.verified(rec))                  # the price is signed too

    def test_seed_fills_the_pool_for_nobody(self):
        stub = Stub()
        p = mock.patch("server.ask.ask_shape", stub)
        p.start()
        self.addCleanup(p.stop)
        self.assertEqual(jar.seed_pool(None, 6, date=self.date), 6)
        st = kv.store()
        ids = jar.pool_ids(st)
        self.assertEqual(len(ids), 6)
        for i in ids:
            self.assertIsNone(st.get(jar.NS, "own/%d" % i))
            self.assertTrue(jar.verified(st.get(jar.NS, "rec/%d" % i)))
        self.assertTrue(all(c[2]["limit"] is None for c in stub.calls))   # nobody's day
        got = jar.pick_from_pool("kit", self.req, 2, st)
        self.assertEqual(len(got), 2)
        self.assertEqual(len(jar.pool_ids(st)), 4)
        self.assertEqual(st.get(jar.NS, "own/%d" % got[0]), b"kit")


# ---- the shopkeeper ------------------------------------------------------------------------

class Shopkeeper(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        self.tibbs = FakeTibbs()
        p = mock.patch("server.shopkeep._ask", self.tibbs)
        p.start()
        self.addCleanup(p.stop)

    def tearDown(self):
        kv.close_all()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_one_session_shared_by_everyone_and_news_goes_first(self):
        # One Tibbs: alex's turn starts his session, sam's turn continues it,
        # so what alex said is what sam's Tibbs remembers.
        self.assertEqual(shopkeep.turn(None, "hello", "alex"), "Hm, I know a fellow.")
        shopkeep.note("alex bought Cork Owl (critter, 80 coins: hoots).")
        shopkeep.turn(None, "what did alex buy?", "sam")
        self.assertEqual(self.tibbs.sessions, [None, "s1"])
        self.assertTrue(self.tibbs.prompts[1].startswith("[Since you last spoke"))
        self.assertIn("alex bought Cork Owl", self.tibbs.prompts[1])
        shopkeep.turn(None, "and?", "sam")
        self.assertNotIn("Since you last spoke", self.tibbs.prompts[2])   # told once
        self.assertTrue(os.path.isdir(shopkeep.home()))      # not the repository

    def test_a_lost_session_starts_again_from_his_notebook(self):
        shopkeep.turn(None, "my name is Alex and I collect spoons", "alex")
        self.tibbs.answers.append(chatmod.ClaudeError("No conversation found"))
        self.assertEqual(shopkeep.turn(None, "still there?", "alex"), "Hm, I know a fellow.")
        self.assertEqual(self.tibbs.sessions[-1], None)
        self.assertIn("picking up where you left off", self.tibbs.prompts[-1])
        self.assertIn("I collect spoons", self.tibbs.prompts[-1])     # what was said, kept
        self.assertTrue(self.tibbs.prompts[-1].endswith("still there?"))

    def test_every_exchange_is_kept(self):
        shopkeep.turn(None, "hello", "alex")
        shopkeep.turn(None, "hi Tibbs", "sam")
        rows = shopkeep.history(10)
        self.assertEqual([(r["person"], r["said"], r["reply"]) for r in rows],
                         [("alex", "hello", "Hm, I know a fellow."),
                          ("sam", "hi Tibbs", "Hm, I know a fellow.")])
        self.assertEqual(shopkeep.history(1)[0]["person"], "sam")
        files = os.listdir(os.path.join(shopkeep.home(), "history"))
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].endswith(".jsonl"))
        # a month later: a new file, and history reads across both
        shopkeep.history_add("talk", "kit", "hullo", "Ah, Kit.", now=time.time() + 40 * 86400)
        self.assertEqual([r["person"] for r in shopkeep.history(3)], ["alex", "sam", "kit"])

    def test_a_long_session_is_replaced_by_a_seeded_one(self):
        with mock.patch("server.shopkeep.ROTATE_AT", 3):
            for i in range(3):
                shopkeep.turn(None, "turn %d" % i, "alex")
            self.assertEqual(self.tibbs.sessions, [None, "s1", "s1"])
            shopkeep.turn(None, "turn 3", "alex")
            self.assertEqual(self.tibbs.sessions[-1], None)          # fresh ...
            self.assertIn("picking up where you left off", self.tibbs.prompts[-1])
            self.assertIn("turn 2", self.tibbs.prompts[-1])          # ... and seeded
            shopkeep.turn(None, "turn 4", "alex")
            self.assertEqual(self.tibbs.sessions[-1], "s1")          # and resumed after

    def test_his_notebook_is_brought_up_to_date(self):
        shopkeep.turn(None, "I am Alex, I like spoons", "alex")
        shopkeep.turn(None, "I am Sam, Alex's sister", "sam")
        asked = []

        def note(chat, prompt):
            asked.append(prompt)
            return json.dumps({"shop": "Two regulars.", "players": {
                "alex": "Likes spoons.", "sam": "Alex's sister."}})

        with mock.patch("server.shopkeep._note_ask", note):
            self.assertTrue(shopkeep.update_notebook(None))
            self.assertFalse(shopkeep.update_notebook(None))         # nothing new since
        self.assertIn("I like spoons", asked[0])
        nb = shopkeep.notebook()
        self.assertEqual(nb["players"]["alex"], "Likes spoons.")
        self.assertEqual(nb["shop"], "Two regulars.")
        # a bad answer keeps the old notebook
        shopkeep.turn(None, "anything new?", "alex")
        with mock.patch("server.shopkeep._note_ask", lambda c, p: "not json"):
            self.assertFalse(shopkeep.update_notebook(None))
        self.assertEqual(shopkeep.notebook()["players"]["sam"], "Alex's sister.")
        # and a fresh session opens with it
        self.assertIn("Likes spoons.", shopkeep.seed())

    def test_the_notebook_is_kept_every_so_many_turns(self):
        ran = []
        with mock.patch("server.shopkeep.NOTE_EVERY", 2), \
                mock.patch("server.shopkeep.update_notebook", lambda c, s: ran.append(1)):
            shopkeep.turn(object(), "a", "alex")
            self.assertEqual(ran, [])
            shopkeep.turn(object(), "b", "alex")
            for _ in range(100):
                if ran:
                    break
                time.sleep(0.01)
            self.assertEqual(ran, [1])

    def test_a_bribe(self):
        rng = random.Random(3)
        self.assertEqual({jar.pick_tier(rng)[0] for _ in range(400)},
                         {"common", "everyday", "rare", "treasure"})
        self.assertNotIn("common", {jar.pick_tier(rng, 150)[0] for _ in range(400)})
        self.assertEqual({jar.pick_tier(rng, 500)[0] for _ in range(400)}, {"rare", "treasure"})
        self.assertEqual(jar.parse_request("finds 8\nbribe 99999\nask owls\n")["bribe"],
                         jar.BRIBE_MAX)
        p = shopkeep.stock_prompt("alex", [], 0, 2, random.Random(1), False,
                                  commission="owls", bribe=300)
        self.assertIn("slipped you 300 coins", p)
        self.assertIn("every find answers it", p)

    def test_a_turn_that_fails_keeps_the_news(self):
        shopkeep.note("sam and alex are friends now.")
        self.tibbs.answers.append(chatmod.ClaudeError("no answer", timed_out=True))
        with self.assertRaises(chatmod.ClaudeError):
            shopkeep.turn(None, "hello", "alex")
        shopkeep.turn(None, "hello again", "alex")
        self.assertIn("friends now", self.tibbs.prompts[-1])

    def test_a_stock_turn(self):
        pool = [(101, "Cork Owl", "critter", 80, "hoots"), (102, "Tin Kite", "hanging", 45, "flaps"),
                (103, "Wax Frog", "critter", 60, "melts a bit")]
        line, picks, briefs = shopkeep.stock_turn(None, "alex", pool, 2, 2)
        self.assertEqual(line, "A crate came in.")
        self.assertEqual(picks, [101, 102])
        self.assertEqual(briefs, ["find 1 of the day", "find 2 of the day"])
        p = self.tibbs.prompts[0]
        self.assertIn("101: Cork Owl, critter, 80 coins -- hoots", p)
        self.assertIn("Today, by chance:", p)                # the dice are the code's
        self.assertIn("find 2 sparks:", p)
        k = shopkeep.stock_prompt("alex", pool, 2, 2, random.Random(1), False,
                                  kinds=["critter", "hanging"])
        self.assertIn("find 1, a critter that roams the jar, sparks:", k)   # the kind it will be
        self.assertIn("find 2, decor that hangs from the lid, sparks:", k)
        self.assertIn("Do not just match what they bought", p)
        self.assertIn("A crate came in.", shopkeep.talk_text("alex"))   # his greeting, kept
        a = shopkeep.stock_prompt("alex", pool, 2, 2, random.Random(1), True)
        b = shopkeep.stock_prompt("alex", pool, 2, 2, random.Random(2), False)
        self.assertIn("find 1 is what your contacts turned up", a)   # the coin
        self.assertIn("None of today's finds is for anything", b)

    def test_a_stock_turn_asks_again_for_the_json_then_gives_up(self):
        self.tibbs.answers.append("Ah, hello! Lovely day.")      # words, not JSON
        line, picks, briefs = shopkeep.stock_turn(None, "alex", [], 0, 2)
        self.assertIn("not the JSON", self.tibbs.prompts[1])
        self.assertEqual(line, "A crate came in.")
        self.tibbs.answers += ["nope", "still nope"]
        line, picks, briefs = shopkeep.stock_turn(None, "alex", [], 0, 2)
        self.assertIn("note on the door", line)
        self.assertEqual((picks, len(briefs)), ([], 2))

    def test_a_brief_a_little_long_is_trimmed_not_refused(self):
        long = "x" * (shopkeep.BRIEF_LEN + 20)
        self.tibbs.answers.append(json.dumps({"line": "Hm.", "picks": [], "briefs": [long]}))
        line, picks, briefs = shopkeep.stock_turn(None, "alex", [], 0, 1)
        self.assertEqual(len(briefs[0]), shopkeep.BRIEF_LEN)

    def test_talking(self):
        self.assertTrue(shopkeep.say(None, "alex", "got any red things?", ["sam"]))
        for _ in range(200):
            if shopkeep.talk_state("alex")["state"] != "pending":
                break
            time.sleep(0.01)
        text = shopkeep.talk_text("alex")
        self.assertEqual(text.splitlines()[0], "ok")
        self.assertIn("me\tgot any red things?", text)
        self.assertIn("him\tHm, I know a fellow.", text)
        self.assertIn("got any red things?", self.tibbs.prompts[0])
        self.assertIn("Their friends in the shop: sam.", self.tibbs.prompts[0])


# ---- the routes, with accounts -----------------------------------------------------------

class Routes(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        sign.forget_cached()
        accounts.migrate("alex", "alex-password", "alex-token")
        accounts.add_user("sam", "sam-password")
        accounts.add_user("kit", "kit-password")
        _, self.sam = accounts.add_device("sam", "hers")
        _, self.kit = accounts.add_device("kit", "theirs")
        self.alex = "alex-token"
        people._last.clear()
        jar._weather_cache.clear()
        for target, value in (("server.jar.fetch_weather", None), ("server.ask.ask_shape", Stub())):
            p = mock.patch(target, return_value=value) if value is None else mock.patch(target, value)
            p.start()
            self.addCleanup(p.stop)
        self.tibbs = FakeTibbs()
        p = mock.patch("server.shopkeep._ask", self.tibbs)
        p.start()
        self.addCleanup(p.stop)
        jar.seed_pool(None, 12)                             # a pool to choose from
        app.Handler.chat = chatmod.ChatService(claude="stub", token="alex-token")
        app.Handler.store = None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.base = "http://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        deadline = time.time() + 5
        while jar._running and time.time() < deadline:
            time.sleep(0.02)
        kv.close_all()
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def req(self, method, path, tok, body=None):
        if body is None and method == "POST":
            body = b""
        if isinstance(body, str):
            body = body.encode()
        h = {"Authorization": "Bearer " + tok} if tok else {}
        r = urllib.request.Request(self.base + path, method=method, data=body, headers=h)
        try:
            resp = urllib.request.urlopen(r, timeout=20)
        except urllib.error.HTTPError as e:
            resp = e
        return (resp.status if hasattr(resp, "status") else resp.code), resp.read().decode()

    def test_sky(self):
        sky = {"word": "rainy", "temp": 11.4, "cloud": 90, "rise": 431, "set": 1105}
        jar._sky_cache.clear()
        with mock.patch("server.jar.fetch_sky", return_value=sky) as f:
            s, body = self.req("GET", "/jar/sky?tz=PST8PDT,M3.2.0,M11.1.0", self.alex)
            self.assertEqual(s, 200)
            self.assertEqual(body, "sky rainy 4\nrise 431\nset 1105\ntemp 11\ncloud 90\n")
            self.req("GET", "/jar/sky?tz=PST8PDT,M3.2.0,M11.1.0", self.sam)
            self.assertEqual(f.call_count, 1)               # one city, one half hour: asked once
            self.assertEqual(self.req("GET", "/jar/sky?tz=UTC0", self.alex)[1], "sky none 0\n")
        self.assertEqual(self.req("GET", "/jar/sky?tz=UTC0", None)[0], 403)

    def me(self, tok):
        s, body = self.req("GET", "/jar/me", tok)
        self.assertEqual(s, 200)
        lines = body.splitlines()
        friends = {}
        for ln in lines[2:]:
            head, _, rest = ln.partition(" ")
            self.assertEqual(head, "friend")
            f = rest.split("\t")
            friends[f[0]] = f[1:]
        return lines[0].split(" ", 1)[1], lines[1].split(" ", 1)[1], friends

    def befriend(self, a, b):
        code_b = self.me(b)[0]
        return self.req("POST", "/jar/friend?code=" + code_b, a)

    def make_day(self, tok):
        s, body = self.req("POST", "/jar/day", tok, "garden mushroom=2\nshelf odd\nowned \ntz UTC0\n")
        self.assertEqual(s, 200)
        self.assertTrue(body == "pending\n" or body.endswith("more\n"), body)
        deadline = time.time() + 10
        while time.time() < deadline:
            s, body = self.req("GET", "/jar/day", tok)
            if body != "pending\n" and not body.endswith("more\n"):
                break
            time.sleep(0.02)
        return s, body

    def item(self, tok, i=0):
        s, body = self.req("GET", "/jar/item?i=%d" % i, tok)
        self.assertEqual(s, 200, body)
        return base64.b64decode(body.strip())

    def test_day_pending_then_ok(self):
        s, body = self.req("GET", "/jar/day", self.alex)
        self.assertEqual((s, body), (404, "error no stock yet: POST /jar/day\n"))
        s, body = self.make_day(self.alex)
        lines = body.splitlines()
        self.assertEqual(lines[0], "ok " + jar._stock_key())
        self.assertEqual(lines[1], "tags ")                     # no tags any more
        n = int(lines[2].split()[1])
        self.assertEqual(n, 8)                                  # 3 random, 3 his, 2 new
        self.assertTrue(lines[3].startswith("batch "), lines[3])
        self.assertEqual(lines[4], "say A crate came in.")      # Tibbs's line of the day
        self.assertTrue(all(l.startswith("seed ") for l in lines[5:]), lines)
        # once made, a POST says so, with the whole answer GET gives ("ok
        # DATE" alone emptied the shop), and makes nothing more
        self.assertEqual(self.req("POST", "/jar/day", self.alex, ""), (200, body))
        s, pub = self.req("GET", "/jar/pubkey", self.alex)
        self.assertEqual(pub, self.req("GET", "/sign/pubkey", None)[1])
        self.assertEqual(len(pub.strip()), 130)
        for i in range(n):
            rec = self.item(self.alex, i)
            self.assertTrue(jar.verified(rec, bytes.fromhex(pub.strip())))
        self.assertEqual(self.req("GET", "/jar/item?i=%d" % n, self.alex)[0], 404)
        self.assertEqual(self.req("GET", "/jar/item?i=x", self.alex)[0], 400)
        self.assertEqual(self.req("GET", "/jar/item?i=0", self.sam)[0], 404)   # sam's own: none
        self.assertEqual(self.req("GET", "/jar/day", None)[0], 403)

    def test_items_show_as_they_are_made(self):
        # One item a call: the first is fetchable while the rest are being
        # made, and the day says "more" until it is whole.
        gate, first = threading.Event(), threading.Event()
        inner = Stub()
        seeded = len(jar.pool_ids())

        def slow(chat, prompt, schema, **kw):
            if len(inner.calls) > 0 and seeded:
                first.set()
                gate.wait(10)
            return inner(chat, prompt, schema, **kw)

        with mock.patch("server.ask.ask_shape", slow):
            self.req("POST", "/jar/day", self.alex, "")
            self.assertTrue(first.wait(10))
            s, body = self.req("GET", "/jar/day", self.alex)
            # 2 at random, 2 he chose, the first new one; the second is being made
            self.assertTrue(body.startswith("ok ") and "\nitems 5\nbatch " in body
                            and body.endswith("\nmore\n"), body)
            self.assertEqual(self.req("POST", "/jar/day", self.alex, ""), (200, body))
            self.item(self.alex, 4)
            self.assertEqual(self.req("GET", "/jar/item?i=5", self.alex)[0], 404)
            gate.set()
            for _ in range(500):
                s, body = self.req("GET", "/jar/day", self.alex)
                if not body.endswith("more\n"):
                    break
                time.sleep(0.02)
        self.assertIn("\nitems 8\n", body)
        self.assertFalse(body.endswith("more\n"), body)

    def wait_day(self, tok):
        for _ in range(500):
            s, body = self.req("GET", "/jar/day", tok)
            if body != "pending\n" and not body.endswith("more\n"):
                return body
            time.sleep(0.02)
        self.fail("the stock never finished: " + body)

    def ids(self, tok, n):
        return [jar.decode(self.item(tok, i))["id"] for i in range(n)]

    def test_the_pool_turns_over_and_items_stay_unique(self):
        # Alex's stock is 2 random and 2 Tibbs's from the pool, and 4 new.
        # Alex buys one and holds one; the next stock keeps the held one
        # first, the bought one is Alex's for good, and the rest go back to
        # the pool -- but not straight back to Alex. Sam's shop takes from the
        # pool too, and never what is in Alex's.
        st = kv.store()
        self.make_day(self.alex)
        first = self.ids(self.alex, 8)
        self.assertEqual(len(jar.pool_ids(st)), 12 - 4)
        self.assertIn("Pick 2 of those", self.tibbs.prompts[0])
        s, body = self.req("POST", "/jar/day?fresh=1", self.alex,
                           "bought %d\nheld %d\n" % (first[0], first[1]))
        body = self.wait_day(self.alex)
        self.assertIn("\nitems 8\n", body)
        second = self.ids(self.alex, 8)
        self.assertEqual(second[0], first[1])                 # held: still there, first
        self.assertEqual(st.get(jar.NS, "own/%d" % first[0]), b"alex")
        self.assertTrue(set(first[2:]) <= set(jar.pool_ids(st)))   # passed on: the pool
        self.assertTrue(set(second[1:]).isdisjoint(first))    # none straight back
        self.assertIn("bought", self.tibbs.prompts[1])        # news for Tibbs
        s, body = self.req("POST", "/jar/day", self.sam, "")
        self.wait_day(self.sam)
        sams = self.ids(self.sam, 8)
        for i in sams:
            self.assertEqual(st.get(jar.NS, "own/%d" % i), b"sam")
        self.assertTrue(set(sams).isdisjoint(second))         # never two shops at once
        # a bought id that was never in your shop is not taken as bought
        self.req("POST", "/jar/day?fresh=1", self.sam, "bought %d\n" % second[0])
        self.wait_day(self.sam)
        self.assertEqual(st.get(jar.NS, "own/%d" % second[0]), b"alex")

    def test_an_admin_is_not_held_to_the_days_limit(self):
        st = kv.store()
        for _ in range(3):
            ask.take_turn("alex", 2, st)                      # alex is the admin here
        with self.assertRaises(ask.RateLimited):
            for _ in range(3):
                ask.take_turn("sam", 2, st)

    def test_a_new_stock_every_four_hours(self):
        t0 = datetime.datetime(2026, 10, 10, 3, 59, tzinfo=datetime.timezone.utc).timestamp()
        self.assertEqual(jar._stock_key(t0), "20261010-0")
        self.assertEqual(jar._stock_key(t0 + 60), "20261010-1")          # 04:00: a new one
        self.assertEqual(jar._stock_key(t0 + 20 * 3600 + 60), "20261011-0")
        self.assertLessEqual(len(jar._stock_key(t0)), 11)                 # the shop's 12 bytes
        st = kv.store()
        self.assertEqual(jar.start_day(None, "alex", jar.parse_request(""), now=t0, store=st)[0],
                         "pending")
        deadline = time.time() + 10
        while jar._running and time.time() < deadline:
            time.sleep(0.02)
        self.assertEqual(jar.start_day(None, "alex", jar.parse_request(""), now=t0 + 30,
                                       store=st), ("ok", "20261010-0"))  # the same slot
        self.assertEqual(jar.start_day(None, "alex", jar.parse_request(""), now=t0 + 60,
                                       store=st)[0], "pending")          # the next: made anew

    def test_a_paid_commission_is_more_new_finds(self):
        # The shop's f: the player paid Tibbs to go looking. Five new finds,
        # the rest from the pool; his prompt says what was asked, and the
        # news that they paid reaches him.
        self.make_day(self.alex)
        made = len(self.tibbs.prompts)
        s, body = self.req("POST", "/jar/day?fresh=1", self.alex, "finds 5\nask brass birds\n")
        body = self.wait_day(self.alex)
        self.assertIn("\nitems 8\n", body)
        p = self.tibbs.prompts[made]
        self.assertIn("paid you to go out looking, and asked for: \"brass birds\"", p)
        self.assertIn("Pick 2 of those", p)                    # 8 - 5 new: 2 his, 1 random
        self.assertEqual(len(re.findall(r"find \d+[^\n]*? sparks", p)), 5)
        self.assertIn("paid you to go out and find new things", p)   # the news
        # too many asked for is the most allowed; a word that fails the filter is dropped
        req = jar.parse_request("finds 99\nask shit birds\n")
        self.assertEqual((req["finds"], req.get("ask")), (jar.MAX_FINDS, None))

    def test_a_deal_struck_in_conversation(self):
        st = kv.store()
        self.make_day(self.alex)
        first = self.ids(self.alex, 8)
        self.req("POST", "/jar/day?fresh=1", self.alex, "bought %d\n" % first[0])
        self.wait_day(self.alex)
        owl = first[0]                                   # alex owns it now
        # He takes 300 coins and the owl, gives 50, and goes looking for brass birds.
        deal = {"take": 300, "give": 50, "trade_in": [owl],
                "commission": {"ask": "brass birds", "finds": 6}}
        self.tibbs.answers.append("Done, and done.\nDEAL: " + json.dumps(deal))
        s, body = self.req("POST", "/jar/talk", self.alex, "coins 500\nhere, 300 for brass birds")
        self.assertEqual(body, "pending\n")
        for _ in range(200):
            body = self.req("GET", "/jar/talk?tx=0", self.alex)[1]
            if body.startswith("ok"):
                break
            time.sleep(0.02)
        self.assertIn("him\tDone, and done.", body)
        self.assertNotIn("DEAL", body)                           # the tag never reaches them
        self.assertIn("They have 500 coins, and own: %d:" % owl, self.tibbs.prompts[-1])
        self.assertIn("tx 1 lose %d" % owl, body)
        self.assertIn("tx 2 pay 300", body)
        self.assertIn("tx 3 get 50", body)
        self.assertNotIn("tx 1 ", self.req("GET", "/jar/talk?tx=1", self.alex)[1])
        self.assertNotIn("tx ", self.req("GET", "/jar/talk", self.alex)[1])   # an old device
        self.assertIn(owl, jar.pool_ids(st))                    # the owl is his to sell again
        # the shop is told to ask for a fresh stock, and it is the commission
        self.assertTrue(self.req("GET", "/jar/day", self.alex)[1].endswith("deal\n"))
        self.req("POST", "/jar/day?fresh=1", self.alex, "")
        body = self.wait_day(self.alex)
        self.assertNotIn("deal\n", body)
        self.assertIn("brass birds", self.tibbs.stock)
        self.assertIn("slipped you 300 coins", self.tibbs.stock)

    def test_a_deal_they_cannot_keep_falls_through(self):
        deal = {"take": 900, "commission": {"ask": "gold", "finds": 8}}
        self.tibbs.answers.append("Deal!\nDEAL: " + json.dumps(deal))
        self.req("POST", "/jar/talk", self.alex, "coins 100\nI'll give you 900")
        for _ in range(200):
            body = self.req("GET", "/jar/talk?tx=0", self.alex)[1]
            if body.startswith("ok"):
                break
            time.sleep(0.02)
        self.assertNotIn("tx ", body)
        self.assertIsNone(jar.pending_deal("alex"))
        # an item that is not theirs cannot be traded either
        self.assertIn("do not own", jar.make_deal("alex", {"trade_in": [123456]}))
        # and a device that does not say its coins gets no deals at all
        self.tibbs.answers.append("Sure.\nDEAL: {\"give\": 40}")
        self.req("POST", "/jar/talk", self.alex, "hello")
        for _ in range(200):
            body = self.req("GET", "/jar/talk?tx=0", self.alex)[1]
            if body.startswith("ok") and "Sure" in body:
                break
            time.sleep(0.02)
        self.assertNotIn("tx ", body)
        self.assertNotIn("strike a deal", self.tibbs.prompts[-1])

    def test_talking_to_tibbs_over_http(self):
        self.befriend(self.alex, self.sam)
        self.befriend(self.sam, self.alex)
        self.assertEqual(self.req("POST", "/jar/talk", self.alex, "how is sam?"), (200, "pending\n"))
        for _ in range(200):
            s, body = self.req("GET", "/jar/talk", self.alex)
            if not body.startswith("pending"):
                break
            time.sleep(0.02)
        self.assertIn("him\tHm, I know a fellow.", body)
        self.assertIn("Their friends in the shop: sam.", self.tibbs.prompts[-1])
        self.assertIn("friends now", self.tibbs.prompts[-1])   # the news reached him
        self.assertEqual(self.req("GET", "/jar/talk", None)[0], 403)

    def test_a_stock_half_made_by_a_server_that_stopped_starts_again(self):
        self.make_day(self.alex)
        st = kv.store()
        cur = jar.day_state("alex", st)
        jar._set_day("alex", dict(cur, state="pending", req=jar.parse_request("")), st)
        body = self.wait_day(self.alex)                      # a GET notices, and restarts
        self.assertTrue(body.startswith("ok "), body)
        self.assertGreater(int(re.search(r"batch (\d+)", body).group(1)), cur["batch"])

    def test_fresh_makes_a_new_stock_the_same_day(self):
        # The shop's r, for trying things out: a new batch even though today's
        # is made. Without it a POST makes nothing more.
        self.make_day(self.alex)
        first = jar.decode(self.item(self.alex, 0))["id"]
        s, body = self.req("POST", "/jar/day?fresh=1", self.alex, "")
        self.assertTrue(body == "pending\n" or body.endswith("more\n"), body)
        for _ in range(200):
            s, body = self.req("GET", "/jar/day", self.alex)
            if not body.startswith("pending") and not body.endswith("more\n"):
                break
            time.sleep(0.02)
        self.assertTrue(body.startswith("ok "), body)
        self.assertNotEqual(jar.decode(self.item(self.alex, 0))["id"], first)

    def test_a_failed_day_says_why_and_a_post_tries_again(self):
        jar._set_pool(kv.store(), [])                       # nothing to fall back on
        with mock.patch("server.ask.ask_shape", Stub(*[ask.Invalid("did not fit")] * jar.MAX_FAILS)):
            s, body = self.make_day(self.alex)
        self.assertEqual(body, "error did not fit\n")
        s, body = self.make_day(self.alex)
        self.assertTrue(body.startswith("ok "), body)

    def test_without_claude(self):
        app.Handler.chat.claude = None
        self.assertEqual(self.req("POST", "/jar/day", self.alex, "")[0], 503)

    def test_friends(self):
        code, name, friends = self.me(self.alex)
        self.assertRegex(code, "^[%s]{6}$" % jar.CODE_CHARS)
        self.assertEqual(self.me(self.alex)[0], code)                  # stable
        self.assertEqual((name, friends), ("alex", {}))
        self.assertEqual(self.req("POST", "/jar/friend?code=" + code, self.alex)[0], 400)
        self.assertEqual(self.req("POST", "/jar/friend?code=ZZZZZZ", self.alex)[0], 404)
        self.assertEqual(self.befriend(self.alex, self.sam), (200, "ok sam waiting\n"))
        self.req("POST", "/people/name", self.sam, "Sammy")
        f = self.me(self.alex)[2]
        self.assertEqual(f["sam"][0], "Sammy")
        self.assertGreater(int(f["sam"][1]), 0)
        self.assertEqual(f["sam"][2], "waiting")
        self.assertEqual(self.me(self.sam)[2], {})                    # sam has added no one
        sam_code = self.me(self.sam)[0].lower()
        self.assertEqual(self.req("POST", "/jar/friend?code=" + self.me(self.alex)[0], self.sam),
                         (200, "ok alex mutual\n"))
        self.assertEqual(self.me(self.alex)[2]["sam"][2], "mutual")
        self.assertEqual(self.req("POST", "/jar/friend?code=" + sam_code[:3] + "-" + sam_code[3:],
                                  self.alex), (200, "ok sam mutual\n"))   # again; loosely typed
        self.assertEqual(list(self.me(self.alex)[2]), ["sam"])
        self.assertEqual(self.req("POST", "/jar/unfriend?name=sam", self.alex), (200, "ok\n"))
        self.assertEqual(self.me(self.alex)[2], {})
        self.assertEqual(self.me(self.sam)[2]["alex"][2], "waiting")

    def gift(self, frm, to, rec, note="for you"):
        return self.req("POST", "/jar/gift?to=" + to, frm,
                        "%s\n%s\n" % (note, base64.b64encode(rec).decode()))

    def test_gift(self):
        self.befriend(self.alex, self.sam)
        self.make_day(self.alex)
        rec = self.item(self.alex, 0)
        item_id = jar.decode(rec)["id"]
        # not friends both ways yet
        self.assertEqual(self.gift(self.alex, "sam", rec)[0], 403)
        self.befriend(self.sam, self.alex)
        self.assertEqual(self.gift(self.alex, "sam", rec, "for you\tsam"), (200, "ok\n"))
        self.assertEqual(kv.store().get(jar.NS, "own/%d" % item_id), b"sam")
        s, body = self.req("GET", "/q/peek?q=jar.gifts", self.sam)
        head, _, rest = body.partition("\n")
        mid, frm, at, size = head.split("\t")
        self.assertEqual(frm, "-")                                    # the server pushed it
        sender, note, b64 = rest[:int(size)].split("\t")
        self.assertEqual((sender, note), ("alex", "for you sam"))
        got = base64.b64decode(b64)
        it = jar.decode(got)
        self.assertTrue(jar.verified(got))
        self.assertEqual(it["flags"] & jar.F_GIFT, jar.F_GIFT)
        self.assertEqual(it["gifted"], "alex")
        self.assertEqual(it["id"], item_id)
        self.assertEqual(it["frames"], jar.decode(rec)["frames"])
        # alex has it no more
        self.assertEqual(self.gift(self.alex, "sam", rec), (403, "error that item is not yours\n"))
        # sam can send it back, re-signed record and all
        self.assertEqual(self.gift(self.sam, "alex", got), (200, "ok\n"))
        self.assertEqual(kv.store().get(jar.NS, "own/%d" % item_id), b"alex")
        self.assertEqual(self.req("POST", "/q/ack?q=jar.gifts&upto=" + mid, self.sam), (200, "ok 1\n"))

    def test_gift_refused(self):
        self.befriend(self.alex, self.sam)
        self.befriend(self.sam, self.alex)
        self.make_day(self.alex)
        rec = self.item(self.alex, 1)
        # tampered: a pixel changed
        bad = bytearray(rec)
        bad[-5] ^= 0x10
        self.assertEqual(self.gift(self.alex, "sam", bytes(bad)),
                         (400, "error the item's signature does not check\n"))
        # a built-in: unsigned
        builtin = jar.encode(dict(jar.decode(rec), sig=b"", flags=jar.F_BUILTIN, id=3))
        self.assertEqual(self.gift(self.alex, "sam", builtin),
                         (400, "error only shop items can be sent\n"))
        unsigned = jar.message(rec)
        self.assertEqual(self.gift(self.alex, "sam", unsigned)[0], 400)
        # not the sender's: kit is nobody's friend, and sam does not own it
        self.assertEqual(self.gift(self.sam, "alex", rec), (403, "error that item is not yours\n"))
        self.assertEqual(self.gift(self.alex, "kit", rec)[0], 403)
        self.assertEqual(self.gift(self.alex, "alex", rec)[0], 400)
        self.assertEqual(self.gift(self.alex, "nobody", rec)[0], 404)
        self.assertEqual(self.req("POST", "/jar/gift?to=sam", self.alex, "hi\nnot base64!\n")[0], 400)
        self.assertEqual(self.gift(self.alex, "sam", rec, "you bitch")[0], 400)
        self.assertEqual(self.req("GET", "/q/len?q=jar.gifts", self.sam)[1], "0\n")
        self.assertEqual(kv.store().get(jar.NS, "own/%d" % jar.decode(rec)["id"]), b"alex")

    def test_thanks(self):
        self.assertEqual(self.req("POST", "/jar/thanks?to=alex&id=123", self.sam)[0], 403)
        self.befriend(self.alex, self.sam)
        self.befriend(self.sam, self.alex)
        self.assertEqual(self.req("POST", "/jar/thanks?to=alex&id=123", self.sam), (200, "ok\n"))
        s, body = self.req("GET", "/q/peek?q=jar.thanks", self.alex)
        head, _, rest = body.partition("\n")
        self.assertEqual(head.split("\t")[1], "-")
        self.assertEqual(rest, "sam\t123\n")
        self.assertEqual(self.req("POST", "/jar/thanks?to=alex&id=x", self.sam)[0], 400)
        self.assertEqual(self.req("POST", "/jar/thanks?to=ghost&id=1", self.sam)[0], 404)


if __name__ == "__main__":
    if "--fixtures" in sys.argv:
        write_fixtures()
    else:
        unittest.main()
