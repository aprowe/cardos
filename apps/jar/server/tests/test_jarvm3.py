"""JarScript 3 on the server (apps/jar/server/jarvm3.py): the compiler, the
machine, the simulated hour, and the fixtures the C machine replays.

    python -m pytest apps/jar/server/tests/test_jarvm3.py
    python -m apps.jar.server.tests.test_jarvm3
"""

import glob
import importlib.util
import os
import re
import unittest

from apps.jar.server import jarvm3 as J

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))


def _tool():
    spec = importlib.util.spec_from_file_location(
        "make_jarvm3_fixtures", os.path.join(ROOT, "apps", "jar", "tools", "make_jarvm3_fixtures.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(src, ticks=1, nframes=1, others=(), events=(), seed=1, **kw):
    """Compile src into slot 0 of a jar, run, return (jar, log lines, says)."""
    logs, says = [], []
    jar = J.Jar(seed=seed, log=lambda s, t: logs.append(t),
                say=lambda s, t, n: says.append((t, n)), **kw)
    jar.put(0, 100 * 16, 112 * 16, 16, 16, 1, 0, nframes, J.compile_script(src, nframes))
    for slot, x, traits in others:
        jar.put(slot, x * 16, 112 * 16, 16, 16, 1, traits, 1)
    for slot in range(J.SLOTS):
        if jar.items[slot]:
            jar.place(slot)
    for t in range(1, ticks + 1):
        for when, ev, arg, other in events:
            if when == t:
                jar.event(0, J.EVENTS[ev], arg, other)
        jar.tick()
    return jar, logs, says


def values(expr):
    """What `log EXPR` prints, one tick in."""
    _jar, logs, _ = run("state s:\n  log %s\nend\n" % expr)
    return logs[0]


class TestTable(unittest.TestCase):
    def test_def_is_read(self):
        self.assertEqual(J.OP["HALT"], 0)
        self.assertEqual(J.OP["ACT"], len(J.OPS) - 1)
        self.assertEqual(len(J.GROUPS["trait"]), 32)
        self.assertEqual(J.GROUPS["trait"]["hunter"], 31)
        for g in ("trait", "sound", "time", "weather", "jam", "world", "zone", "glow"):
            self.assertIn(g, J.GROUPS)
        self.assertEqual(J.FUNCS["has"], (17, 2, 2))
        self.assertEqual(J.SENSE_KIND[J.SENSES["x"]], "rw")
        self.assertEqual(J.OPS[J.OP["JMP"]].kinds, ("u16",))
        self.assertEqual(J.OPS[J.OP["PUSH16"]].kinds, ("s16",))
        self.assertEqual(J.OPS[J.OP["CALL"]].kinds, ("u8", "u8"))

    def test_tiers(self):
        self.assertEqual(J.TIERS, {"none": 0, "small": 512, "full": 1024})
        self.assertEqual(J.tier_limit("small"), 512)
        with self.assertRaises(ValueError):
            J.tier_limit("huge")

    def test_ang_table(self):
        self.assertEqual(J.ANG[0], 0)
        self.assertEqual(J.ANG[64], 16384)
        self.assertEqual(J.ANG[192], -16384)
        self.assertEqual(J.ANG[32], 11585)
        self.assertEqual(sum(J.ANG), 0)
        # the C machine's copy is the same table
        src = open(os.path.join(ROOT, "apps", "jar", "jv3.h"), encoding="utf-8").read()
        m = re.search(r"J3_ANG\[256\] = \{([^}]*)\}", src)
        self.assertEqual([int(v) for v in m.group(1).replace("\n", " ").split(",") if v.strip()], J.ANG)

    def test_sin_table(self):
        self.assertEqual(J.SIN[0], 0)
        self.assertEqual(J.SIN[64], 16)
        self.assertEqual(J.SIN[192], -16)
        self.assertEqual(J.SIN[21], 8)
        self.assertEqual(sum(J.SIN), 0)

    def test_signal_hash(self):
        # FNV-1a 32 over the ASCII, % 255 + 1
        h = 2166136261
        for c in b"landed":
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        self.assertEqual(J.fnv_signal("landed"), h % 255 + 1)
        self.assertTrue(1 <= J.fnv_signal("x") <= 255)

    def test_is_v3(self):
        self.assertTrue(J.is_v3(J.compile_script("state s:\nend", 1)))
        self.assertFalse(J.is_v3(b"\x01\x00"))
        self.assertFalse(J.is_v3(b""))


class TestLayout(unittest.TestCase):
    def test_small_script_by_hand(self):
        b = J.compile_script("var a = 1.5\nstate s:\n  a = 2\nend\non poke: go s end\n", 1)
        want = bytes([3, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0,   # header
                      0x18, 0x00,                                 # a = 1.5 (raw 24)
                      25, 0,                                      # state s at 25
                      0, 0, 255, 0, 0, 30, 0,                     # on poke, any state, code 30
                      2, 2, 5, 0, 0,                              # PUSH8 2; STV 0; HALT
                      31, 0, 1])                                  # GO 0; RET
        self.assertEqual(b, want)

    def test_tables(self):
        src = ("traits bird\nscale 2\nlayer -2\nbody 5\nsolid 1, 2, 3, 4\nperch 4, 5\n"
               "anim fly = a b a @ 12\nstate s:\n  say \"hi \" + x\nend\n"
               "on near bug within 30: log \"n\" end\non every 0.5s: log 1 end\n"
               "on signal ping: log 2 end\non chime 5: log 3 end\non time night: log 4 end\n")
        b = J.compile_script(src, 2, ["a", "b"])
        s = J.Script(b)
        self.assertEqual(s.flags, 1)
        self.assertEqual(s.layer, -2)
        self.assertEqual(s.body, 5)
        self.assertEqual(s.solid, (1, 2, 3, 4))
        self.assertEqual(s.perches, [(4, 5)])
        self.assertEqual(s.anims, [(12, [0, 1, 0])])
        self.assertEqual(s.strings, [b"hi \x01", b"n"])
        hs = [(J.EVENT_NAME[e], a, st, p) for e, a, st, p, _c in s.handlers]
        self.assertEqual(hs, [("near", J.TRAITS["bug"] + 1, 255, 30), ("every", 0, 255, 10),
                              ("signal", J.fnv_signal("ping"), 255, 0), ("chime", 0, 255, 5),
                              ("time", 4, 255, 0)])
        self.assertIsNone(J.check(b, 2))
        self.assertEqual(J.script_traits(src), ["bird"])

    def test_state_handlers_come_first(self):
        b = J.compile_script("state a:\n  on poke: log 1 end\nend\nstate b:\n  on tick: log 2 end\n"
                             "end\non poke: log 3 end\n", 1)
        s = J.Script(b)
        self.assertEqual([(J.EVENT_NAME[e], st) for e, _a, st, _p, _c in s.handlers],
                         [("poke", 0), ("tick", 1), ("poke", 255)])

    def test_jumps_are_absolute(self):
        b = J.compile_script("state s:\n  if x > 1 then\n    log 1\n  end\nend\n", 1)
        s = J.Script(b)
        txt = J.disassemble(b)
        jz = re.search(r"JZ (\d+)", txt)
        self.assertTrue(int(jz.group(1)) > s.code_start)
        self.assertIsNone(J.check(b))

    def test_strings_pooled_and_log_without_text(self):
        b = J.compile_script('state s:\n  say "a"\n  say "a"\n  log x, y\nend\n', 1)
        s = J.Script(b)
        self.assertEqual(s.strings, [b"a"])
        self.assertIn("LOG 255 2", J.disassemble(b))


class TestCompileErrors(unittest.TestCase):
    def err(self, src, nframes=1, names=None, limit=1024):
        with self.assertRaises(J.ScriptError) as cm:
            J.compile_script(src, nframes, names, limit)
        return cm.exception

    def test_lines(self):
        e = self.err("state s:\n  wait 1\n  jump\nend\n")
        self.assertEqual(e.line, 3)
        self.assertIn("'jump' is not a statement", str(e))
        self.assertTrue(str(e).startswith("line 3:"))
        self.assertEqual(self.err("state s:\n  x = 1\n").line, 1)       # no end
        self.assertEqual(self.err("on poke:\n  wait 1\nend\n").line, 2)  # wait in a handler
        self.assertIn("takes time", str(self.err("on poke:\n  move to 1, 2\nend\n")))
        self.assertEqual(self.err('state s:\n\n  x = "a"\nend\n').line, 3)
        self.assertIn("only be said or logged", str(self.err('state s:\n  x = "a"\nend\n')))
        self.assertIn("is not a trait", str(self.err("traits cat\n")))
        self.assertIn("is not a trait", str(self.err("state s:\n  x = find(cat)\nend\n")))
        self.assertIn("is not a trait", str(self.err("on near cat: log 1 end\n")))
        self.assertIn("can be read but not set", str(self.err("state s:\n  speed = 1\nend\n")))
        self.assertIn("unknown name", str(self.err("state s:\n  x = zz\nend\n")))
        self.assertIn("already a word", str(self.err("var home\n")))
        self.assertIn("already a word", str(self.err("state fly:\nend\n")))
        self.assertIn("no state", str(self.err("state s:\n  go t\nend\n")))
        self.assertIn("'=' sets a value", str(self.err("state s:\n  if x = 1 then x = 2 end\nend\n")))
        self.assertIn("a number can only end in s or ms", str(self.err("state s:\n  wait 3m\nend\n")))
        self.assertIn("no closing", str(self.err('state s:\n  say "hi\nend\n')))
        self.assertIn("out of range", str(self.err("state s:\n  x = 5000\nend\n")))
        self.assertIn("characters", str(self.err('state s:\n  say "' + "a" * 25 + '"\nend\n')))
        self.assertIn("not a sound", str(self.err("state s:\n  sound meow\nend\n")))

    def test_frames(self):
        self.assertIn("no frame called", str(self.err("anim a = x y @ 4\n", 2, ["p", "q"])))
        self.assertIn("there is no frame 3", str(self.err("anim a = 1 3 @ 4\n", 2)))
        self.assertIn("frame should be 1..2", str(self.err("state s:\n  frame 3\nend\n", 2)))
        self.assertIn("at most 8", str(self.err("anim a = 1 1 1 1 1 1 1 1 1 @ 4\n", 1)))
        J.compile_script("anim a = 1 2 1 2 1 2 1 2 @ 4\nstate s:\n  frame b\nend\n", 2, ["a", "b"])
        with self.assertRaises(ValueError):
            J.compile_script("", 5)

    def test_limits(self):
        e = self.err("var " + ", ".join("v%d" % i for i in range(9)) + "\n")
        self.assertIn("at most 8", str(e))
        e = self.err("".join("on near bug within %d: log 1 end\n" % (10 + i) for i in range(5)))
        self.assertIn("near handlers", str(e))
        e = self.err("".join("perch %d, 1\n" % i for i in range(5)))
        self.assertIn("perches", str(e))
        e = self.err("on poke: log 1 end\non poke: log 2 end\n")
        self.assertIn("a second", str(e))
        deep = "state s:\n  x = " + "(1 + " * 17 + "1" + ")" * 17 + "\nend\n"
        self.assertIn("on the stack", str(self.err(deep)))
        J.compile_script(deep, 1, check_stack=False)

    def test_size_limit(self):
        src = "state s:\n" + "".join("  x = x + %d\n" % i for i in range(1, 120)) + "end\n"
        b = J.compile_script(src, 1)
        self.assertGreater(len(b), 512)
        e = self.err(src, limit=512)
        self.assertIn("the script is %d bytes; at most 512" % len(b), str(e))
        self.assertIn("no script", str(self.err("state s:\nend\n", limit=0)))
        big = "state s:\n" + "".join("  x = x + %d\n" % i for i in range(1, 300)) + "end\n"
        self.assertIn("at most 1024", str(self.err(big)))


class TestNumbers(unittest.TestCase):
    def test_format(self):
        for raw, s in [(16, "1"), (-32, "-2"), (24, "1.5"), (1, "0.06"), (-36, "-2.25"),
                       (15, "0.94"), (8, "0.5"), (0, "0"), (32767, "2047.94"), (-1, "-0.06"),
                       (18, "1.13")]:
            self.assertEqual(J.fmt_num(raw), s, raw)

    def test_literals_round_half_away(self):
        self.assertEqual(J.rha(J.Fraction(1, 32) * 16), 1)
        self.assertEqual(values("0.03125, -0.03125, 0.1, 300ms, 2s"), "0.06 -0.06 0.13 0.31 2")

    def test_arithmetic(self):
        self.assertEqual(values("2000 + 100, -2000 - 100, -(-2048), 2047 * 2"),
                         "2047.94 -2048 2047.94 2047.94")
        self.assertEqual(values("-7 / 2, 7 / -2, -1 / 3, 5 / 0, 5 % 0"), "-3.5 -3.5 -0.31 0 0")
        self.assertEqual(values("-7 % 3, 7 % -3, -7.5 % 2, 0.1 * 0.1, 3.5 * -2.25"),
                         "2 -2 0.5 0 -7.88")
        self.assertEqual(values("-0.0625 * 1"), "-0.06")
        self.assertEqual(values("-0.0625 * 0.0625"), "-0.06")         # floor(-1/16) raw -1
        self.assertEqual(values("1 and 2, 0 or 0, not 3, 2 >= 2, 1 != 1"), "1 0 0 1 0")

    def test_functions(self):
        self.assertEqual(values("round(2.5), round(-2.5), floor(-0.5), abs(-2048), sign(-0.1)"),
                         "3 -3 -1 2047.94 -1")
        self.assertEqual(values("clamp(5, 1, 3), clamp(0, 3, 1), min(-1, 2), max(-1, 2)"), "3 3 -1 2")
        self.assertEqual(values("sqrt(4), sqrt(2), sqrt(-1), sin(90), cos(0), cos(180), sin(-90)"),
                         "2 1.38 0 1 1 -1 -1")
        jar = J.Jar(seed=1)
        a = jar.rnd()
        x = a
        self.assertEqual(a, x)
        jar2 = J.Jar(seed=0)
        self.assertNotEqual(jar2.rnd(), 0)

    def test_random_ranges(self):
        logs = run("state s:\n  loop\n    log random(1, 3), random(0.5, 1), random(4, 2), "
                   "chance(0), chance(100)\n    wait\n  end\nend\n", ticks=60)[1]
        seen = [[float(v) for v in l.split()] for l in logs]
        self.assertTrue(all(1 <= r[0] <= 3 and r[0] == int(r[0]) for r in seen))
        self.assertTrue(all(0.5 <= r[1] <= 1 for r in seen))
        self.assertTrue(all(2 <= r[2] <= 4 for r in seen))
        self.assertTrue(all(r[3] == 0 and r[4] == 1 for r in seen))
        self.assertEqual({r[0] for r in seen}, {1, 2, 3})

    def test_angles(self):
        self.assertEqual(values("angleto(2)"), "0")                   # nobody
        self.assertEqual(run("state s:\n  log angleto(2), angleto(3), dist(2), find(bird), "
                             "find(bird, 30), count(bird), has(2, bird)\nend\n",
                             others=[(1, 140, 1 << J.TRAITS["bird"]),
                                     (2, 60, 0)])[1][0],
                         "0 180 40 2 0 1 1")      # straight left: angleto's own fine table (D24)
        _jar, logs, _ = run("state s:\n  angle = -90\n  log angle\n  angle = 361\n  log angle\nend\n")
        self.assertEqual(logs, ["270", "0"])


class TestAngleto(unittest.TestCase):
    def test_compass_is_exact(self):
        jar = J.Jar(seed=1)
        jar.put(0, 120 * 16, 80 * 16, script=J.compile_script("state s:\n  wait 100\nend\n", 1))
        d = 40
        dirs = [(d, 0), (d, d), (0, d), (-d, d), (-d, 0), (-d, -d), (0, -d), (d, -d)]
        for i, (dx, dy) in enumerate(dirs):
            jar.put(1 + i, (120 + dx) * 16, (80 + dy) * 16)
        for s in range(9):
            jar.place(s)
        k = J.FUNCS["angleto"][0]
        got = [jar._call(jar.items[0], k, [(2 + i) * 16]) for i in range(8)]
        self.assertEqual(got, [step * 45 // 2 for step in range(0, 256, 32)])
        self.assertEqual(got, [deg * 16 for deg in range(0, 360, 45)])

    def test_far_apart_does_not_overflow(self):
        jar = J.Jar(seed=1)
        jar.put(0, -32768, 32767, script=J.compile_script("state s:\n  wait 100\nend\n", 1))
        jar.put(1, 32767, -32768)
        jar.place(0)
        jar.place(1)
        # dx 65535, dy -65535 (centres): up and right, in int32 on the device
        self.assertEqual(jar._call(jar.items[0], J.FUNCS["angleto"][0], [2 * 16]), 315 * 16)


class TestMachine(unittest.TestCase):
    def test_wait_counts_ticks(self):
        _jar, logs, _ = run("state s:\n  log time\n  wait 0.25\n  log time\n  wait\n  log time\nend\n",
                            ticks=10)
        # time is 0 in tick 1; 5 ticks later (tick 6) 5 ticks = 0.25 s; then 6 ticks
        # = 0.3 s, which is 4.8 raw, kept as 4 (0.25)
        self.assertEqual(logs, ["0", "0.25", "0.25"])

    def test_handler_go_is_pending_and_body_go_is_at_once(self):
        _jar, logs, _ = run("state a:\n  log \"a\"\n  wait 10\nend\nstate b:\n  log \"b\"\n  go c\nend\n"
                            "state c:\n  log \"c\"\n  wait 10\nend\non poke: go b; log \"no\" end\n"
                            "on signal x: go a end\n",
                            ticks=4, events=[(2, "poke", 0, 0)])
        self.assertEqual(logs, ["a", "b", "c"])

    def test_last_pending_go_wins(self):
        jar, logs, _ = run("state a:\n  wait 10\nend\nstate b:\n  log \"b\"\nend\nstate c:\n  log \"c\"\n"
                           "end\non poke: go b end\non gift: go c end\n",
                           ticks=3, events=[(2, "poke", 0, 0), (2, "gift", 0, 0)])
        self.assertEqual(logs, ["c"])

    def test_four_gos_a_tick(self):
        jar, _l, _ = run("var n\nstate a:\n  n += 1\n  go a\nend\n", ticks=3)
        self.assertEqual(jar.items[0].mem[0], 12 * 16)

    def test_events_wait_a_tick(self):
        logs = []
        jar = J.Jar(seed=1, log=lambda s, t: logs.append((jar.ticks, s, t)))
        a = J.compile_script("state s:\n  signal hi\n  signal hi to 2\nend\n", 1)
        b = J.compile_script("on signal hi: log other end\n", 1)
        jar.put(1, 80 * 16, 112 * 16, script=b)
        jar.put(0, 50 * 16, 112 * 16, script=a)
        jar.put(2, 90 * 16, 112 * 16, script=a)
        for s in (0, 1, 2):
            jar.place(s)
        jar.tick()
        self.assertEqual(logs, [])
        jar.tick()
        self.assertEqual(sorted(logs), [(1, 1, "1"), (1, 1, "1"), (1, 1, "3"), (1, 1, "3")])

    def test_near_once_per_arrival(self):
        logs = []
        jar = J.Jar(seed=1, log=lambda s, t: logs.append(t))
        jar.put(0, 100 * 16, 112 * 16, script=J.compile_script("on near bug within 20: log other end", 1))
        jar.put(1, 200 * 16, 112 * 16, traits=1 << J.TRAITS["bug"])
        jar.place(0)
        jar.place(1)
        for x in (200, 110, 110, 105, 200, 115, 115):
            jar.items[1].x = x * 16
            jar.tick()
        self.assertEqual(logs, ["2", "2"])

    def test_near_goes_to_the_handler_that_saw_it(self):
        # one trait, two radii: the state's within 22, the top level's within 48
        logs = []
        jar = J.Jar(seed=1, log=lambda s, t: logs.append(t))
        jar.put(0, 100 * 16, 112 * 16, script=J.compile_script(
            "state s:\n  on near bug within 22: log \"close\" end\n  wait 100\nend\n"
            "on near bug within 48: log \"far\" end\n", 1))
        jar.put(1, 200 * 16, 112 * 16, traits=1 << J.TRAITS["bug"])
        jar.place(0)
        jar.place(1)
        seen = []
        for x in (200, 140, 140, 110, 110, 200, 200, 105, 105, 105):
            jar.items[1].x = x * 16
            logs.clear()
            jar.tick()
            seen.append(sorted(logs))
        # an arrival is heard the tick after it is seen; at 105 both see it
        self.assertEqual(seen, [[], [], ["far"], [], ["close"], [], [], [], ["close", "far"], []])

    def test_unstarted_handlers_are_dropped_not_faults(self):
        # 25 steps a poke, 50 a tick: two run, the rest of each 8 cannot start
        faults = []
        src = "var m\nstate s:\n  wait 100\nend\non poke:\n" + "  m += 1\n" * 6 + "end\n"
        jar, _l, _ = run(src, ticks=6,
                         events=[(t, "poke", 0, 0) for t in (2, 3, 4) for _ in range(8 if t < 4 else 4)],
                         fault=lambda s, pc: faults.append(pc))
        self.assertEqual(faults, [])
        self.assertFalse(jar.items[0].stopped)
        self.assertEqual(jar.items[0].faults, 0)
        self.assertEqual(jar.items[0].mem[0], 3 * 2 * 6 * 16)

    def test_every_and_chime(self):
        _jar, logs, _ = run("on every 0.2s: log \"e\" end\non chime 5: log \"c5\" end\n"
                            "on chime: log \"c15\" end\nstate s:\n  frame 1\nend\n",
                            ticks=9, events=[(1, "chime", 10, 0), (1, "chime", 15, 0),
                                             (1, "chime", 7, 0)])
        self.assertEqual(logs, ["c5", "c5", "e", "e"])

    def test_handler_budget_is_a_fault(self):
        faults = []
        jar, _l, _ = run("var n\non poke:\n  while true\n    n += 1\n  end\nend\nstate s:\n  wait 100\nend\n",
                         ticks=12, events=[(t, "poke", 0, 0) for t in range(2, 12)],
                         fault=lambda s, pc: faults.append(pc))
        self.assertEqual(len(faults), 8)
        self.assertTrue(jar.items[0].stopped)

    def test_body_runs_out_and_carries_on(self):
        jar, _l, _ = run("var n\nstate s:\n  loop\n    n += 1\n  end\nend\n", ticks=3)
        it = jar.items[0]
        self.assertTrue(it.starved)
        self.assertEqual(it.faults, 0)
        self.assertGreater(it.mem[0], 0)

    def test_stack_overflow_faults_the_body(self):
        src = "state s:\n  x = " + "(1 + " * 17 + "1" + ")" * 17 + "\n  log 1\nend\n"
        b = J.compile_script(src, 1, check_stack=False)
        self.assertIsNotNone(J.check(b) is None and "ok")
        faults = []
        jar = J.Jar(fault=lambda s, pc: faults.append(pc))
        jar.put(0, 1600, 1792, script=b)
        jar.place(0)
        jar.tick()
        self.assertEqual(len(faults), 1)
        self.assertTrue(jar.items[0].halted)

    def test_vars_start_from_memory(self):
        b = J.compile_script("var a = 3, b = -1\nstate s:\nend\n", 1)
        jar = J.Jar()
        jar.put(0, 0, 0, script=b)
        jar.place(0)
        self.assertEqual(jar.items[0].mem[:3], [48, -16, 0])
        jar.put(1, 0, 0, script=b, mem=[5, 0, 0, 0, 0, 0, 0, 0])
        jar.place(1)
        self.assertEqual(jar.items[1].mem[:2], [5, 0])

    def test_move_lands_and_says(self):
        jar, logs, says = run('state s:\n  move to 50, 100 speed 50\n  say "there " + x\nend\n'
                              "on land: log \"land\", other end\n", ticks=40)
        self.assertEqual(logs, ["land 0"])
        self.assertEqual(says, [("there 50", 40)])
        self.assertEqual((jar.items[0].x, jar.items[0].y), (800, 1600))

    def test_physics_falls_and_rests(self):
        jar, logs, _ = run("state s:\n  physics on\n  wait until grounded and speed < 1\n"
                           "  log \"rest\", y\nend\non hit: log \"hit\" end\n", ticks=80)
        it = jar.items[0]
        it.y = it.y
        self.assertEqual(logs[-1], "rest 113")      # radius 7, centre at y - 8: feet 1 px down

    def test_bad_scripts_are_refused(self):
        good = J.compile_script("state s:\n  wait 1\nend\non poke: log 1 end\n", 1)
        self.assertIsNone(J.check(good))
        self.assertIsNotNone(J.check(good[:-1]))
        self.assertIsNotNone(J.check(b"\x01" + good[1:]))
        bad = bytearray(good)
        bad[-3] = 99                                  # an opcode that is not one
        self.assertIsNotNone(J.check(bytes(bad)))
        s = J.Script(good)
        h = s.handlers[0][4]
        bad = bytearray(good)
        bad[h] = J.OP["WAIT"]                         # time in a handler
        self.assertIsNotNone(J.check(bytes(bad)))
        jar = J.Jar()
        jar.put(0, 0, 0, script=bytes(bad))
        self.assertIsNone(jar.items[0].s)             # idle, never run
        self.assertIn("not a JarScript 3", J.disassemble(b"\x01"))


class TestSandboxScenarios(unittest.TestCase):
    # what the device's limits refuse in the sandbox's hand-built scenes
    KNOWN = {"Clover": "perches", "Water lily": "fps"}

    def test_every_scenario_script(self):
        tool = _tool()
        ok, failed = [], {}
        for f in sorted(glob.glob(os.path.join(ROOT, "apps", "jar", "tools", "jarsandbox", "scenarios", "*.js"))):
            text = open(f, encoding="utf-8").read()
            for m in re.finditer(r'\{\s*name:\s*"([^"]+)",\s*w:\s*(\d+),\s*h:\s*(\d+)(.*?)'
                                 r'script:\s*`(.*?)`', text, re.S):
                frames = re.findall(r"\{\s*name:\s*['\"](\w+)['\"],\s*rows", m.group(4))
                src, names = tool.adapt_sandbox(m.group(5), frames)
                try:
                    J.compile_script(src, len(names) or 1, names)
                    ok.append(m.group(1))
                except J.ScriptError as e:
                    failed[m.group(1)] = str(e)
        for name, why in failed.items():
            self.assertIn(name, self.KNOWN, "%s: %s" % (name, why))
            self.assertIn(self.KNOWN[name], why)
        self.assertGreaterEqual(len(ok), 30)


class TestSimulate(unittest.TestCase):
    def test_sane_script_passes(self):
        b = J.compile_script("traits critter\nanim a = 1 2 @ 4\nstate s:\n  play a\n  wait random(1, 3)\n"
                             "  walk random(-20, 20)\nend\non poke: say \"hi\" end\n", 2)
        self.assertEqual(J.simulate(b, 2, traits=["critter"]), [])

    def test_never_waits(self):
        b = J.compile_script("var n\nstate s:\n  frame 1\n  loop\n    n += 1\n  end\nend\n", 1)
        p = J.simulate(b, 1)
        self.assertTrue(any("never waits" in x for x in p), p)

    def test_faults(self):
        b = J.compile_script("var n\non poke:\n  while true\n    n += 1\n  end\nend\nstate s:\n"
                             "  loop\n    hop\n    wait 2\n  end\nend\n", 1)
        p = J.simulate(b, 1)
        self.assertTrue(p and "faulted" in p[0], p)

    def test_nothing_visible(self):
        b = J.compile_script("var n\nstate s:\n  loop\n    n += 1\n    wait 1\n  end\nend\n", 1)
        p = J.simulate(b, 1)
        self.assertTrue(any("nothing visible" in x for x in p), p)

    def test_prepare_script(self):
        b = J.prepare_script("state s:\n  loop\n    hop\n    wait 3\n  end\nend\n", 1, tier="small")
        self.assertTrue(J.is_v3(b))
        with self.assertRaises(J.ScriptError) as cm:
            J.prepare_script("var n\nstate s:\n  loop\n    n += 1\n    wait 1\n  end\nend\n", 1)
        self.assertIn("simulated hour", str(cm.exception))
        with self.assertRaises(J.ScriptError):
            J.prepare_script("state s:\nend\n", 1, tier="none")

    def test_guide(self):
        g = J.LANGUAGE_GUIDE_V3
        self.assertIn("up to 4 pictures (frames)", g)
        for w in J.TRAITS:
            self.assertIn(w, g)
        for w in J.ACTIONS:
            self.assertIn(w, g)


class TestFixtures(unittest.TestCase):
    def test_deterministic_and_current(self):
        tool = _tool()
        a = tool.generate()
        b = tool.generate()
        self.assertEqual(a, b)
        names = {n for n in a if n.endswith(".world")}
        self.assertGreaterEqual(len(names), 10)
        out = os.path.join(ROOT, "apps", "jar", "test", "fixtures")
        for n, data in a.items():
            p = os.path.join(out, n)
            self.assertTrue(os.path.exists(p), "%s is missing: run apps/jar/tools/make_jarvm3_fixtures.py" % n)
            with open(p, "rb") as f:
                self.assertEqual(f.read(), data, "%s is stale: run apps/jar/tools/make_jarvm3_fixtures.py" % n)

    def test_world_files_replay(self):
        tool = _tool()
        out = os.path.join(ROOT, "apps", "jar", "test", "fixtures")
        for p in sorted(glob.glob(os.path.join(out, "jv3_*.world"))):
            with open(p) as f:
                world = f.read()
            got = tool.run_world(world, lambda n: open(os.path.join(out, n), "rb").read())
            with open(p[:-6] + ".trace") as f:
                self.assertEqual(got, f.read(), p)


if __name__ == "__main__":
    unittest.main()
