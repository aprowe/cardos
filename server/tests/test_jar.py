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
from server import accounts, app, ask, jar, kv, people, sign
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

    def __call__(self, chat, prompt, schema, **kw):
        with self.lock:
            n = schema["properties"]["items"]["maxItems"]
            self.calls.append((n, prompt, kw))
            if self.batches:
                b = self.batches.pop(0)
                if isinstance(b, Exception):
                    raise b
                return {"items": b}
            out = []
            for _ in range(n):
                self.serial += 1
                out.append(raw_item("Thing %d" % self.serial))
            return {"items": out}


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
        self.assertEqual(len(table), 25)
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
    for name, data in (("jar_item_signed.bin", rec), ("jar_item_message.bin", jar.message(rec)),
                       ("jar_pubkey.bin", test_pub())):
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
        facts = {"season": "autumn", "weather": "rainy", "full_moon": True, "moon": "full moon"}
        tags = jar.day_tags(req, facts, "x")
        self.assertEqual(tags[0], "spooky")                    # 4 from mushrooms, 1 shelf
        self.assertEqual(set(tags[1:3]), {"odd", "glowing"})
        self.assertEqual(tags[3:], ["autumn", "rainy", "full-moon"])
        self.assertEqual(jar.record_tags(tags), ",".join(tags[:3]))
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
        self.date = datetime.date(2026, 10, 9)                 # no full moon, no new season

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
        tags, recs = self.run_with(stub)
        self.assertEqual(len(recs), 8)
        self.assertEqual(len(stub.calls), 1)
        n, prompt, kw = stub.calls[0]
        self.assertEqual(n, 8)
        self.assertIn("rainy", prompt)
        self.assertIn("cosy", prompt)
        self.assertIn("Moth", prompt)                          # owned: not again
        self.assertEqual(kw["user"], "alex")
        self.assertEqual(tags[0], "cosy")                      # 3 ferns and the shelf
        self.assertEqual(set(tags[1:3]), {"soft", "sleepy"})
        self.assertEqual(tags[3:], ["autumn", "rainy"])
        ids = set()
        st = kv.store()
        for rec in recs:
            it = jar.decode(rec)
            self.assertTrue(jar.verified(rec))
            self.assertEqual(it["maker"], "Jar Works")
            self.assertEqual(it["made"], 1791590400)
            self.assertEqual(it["tags"], jar.record_tags(tags))
            self.assertGreater(it["id"], jar.ID_BASE)
            self.assertEqual(st.get(jar.NS, "own/%d" % it["id"]), b"alex")
            ids.add(it["id"])
        self.assertEqual(len(ids), 8)

    def test_failures_are_asked_for_again(self):
        flat = [r.replace("3", "2").replace("7", "2").replace("1", "2") for r in sprite()]
        first = [raw_item("Good %d" % i) for i in range(6)] + \
                [raw_item("Flat", frames=[flat]), raw_item("Damn Shit")]
        stub = Stub(first)
        tags, recs = self.run_with(stub)
        self.assertEqual(len(recs), 8)
        self.assertEqual([c[0] for c in stub.calls], [8, 2])     # only the two again
        names = [jar.decode(r)["name"] for r in recs]
        self.assertNotIn("Flat", names)
        self.assertNotIn("Damn Shit", names)
        self.assertIn("Good 0", stub.calls[1][1])                # not those names again

    def test_bounded_and_fewer_is_fine(self):
        bad = [raw_item("Bad", frames=[["0" * 16] * 16])]
        stub = Stub([raw_item("One")] + bad * 7, bad * 4, ask.Invalid("did not fit"))
        tags, recs = self.run_with(stub)
        self.assertEqual(len(recs), 1)
        self.assertEqual(len(stub.calls), jar.MAX_ROUNDS)

    def test_nothing_at_all_is_an_error(self):
        stub = Stub(ask.Invalid("x"), ask.Invalid("y"), ask.Invalid("z"))
        with self.assertRaises(ask.Invalid):
            self.run_with(stub)

    def test_a_full_moon_adds_one(self):
        self.date = datetime.date(2026, 10, 26)
        stub = Stub()
        tags, recs = self.run_with(stub)
        self.assertEqual(len(recs), 9)
        self.assertIn("special", stub.calls[0][1])
        self.assertIn("full-moon", tags)


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
        self.assertIn(body, ("pending\n",))
        deadline = time.time() + 10
        while time.time() < deadline:
            s, body = self.req("GET", "/jar/day", tok)
            if body != "pending\n":
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
        today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
        lines = body.splitlines()
        self.assertEqual(lines[0], "ok " + today)
        self.assertTrue(lines[1].startswith("tags odd, "), lines[1])
        n = int(lines[2].split()[1])
        self.assertIn(n, (8, 9))
        # once made, a POST says so and makes nothing more
        self.assertEqual(self.req("POST", "/jar/day", self.alex, ""), (200, "ok %s\n" % today))
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

    def test_fresh_makes_a_new_stock_the_same_day(self):
        # The shop's r, for trying things out: a new batch even though today's
        # is made. Without it a POST makes nothing more.
        self.make_day(self.alex)
        first = jar.decode(self.item(self.alex, 0))["id"]
        self.assertEqual(self.req("POST", "/jar/day?fresh=1", self.alex, ""), (200, "pending\n"))
        for _ in range(200):
            s, body = self.req("GET", "/jar/day", self.alex)
            if not body.startswith("pending"):
                break
            time.sleep(0.02)
        self.assertTrue(body.startswith("ok "), body)
        self.assertNotEqual(jar.decode(self.item(self.alex, 0))["id"], first)

    def test_a_failed_day_says_why_and_a_post_tries_again(self):
        with mock.patch("server.ask.ask_shape", Stub(*[ask.Invalid("did not fit")] * 3)):
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
