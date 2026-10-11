"""Jar Factory's scripts on the server: server/jarvm.py -- the table read
from apps/jarvm.def, the compiler and its errors, the machine, and the
simulated day. The C machine is held to the same traces by
test/test_jarvm.c, from the fixtures checked here to be current."""

import importlib.util
import os
import tempfile
import unittest

from server import jarvm as V

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
E, A, S, OP = V.EVENTS, V.ACTIONS, V.SENSES, V.OP


class Rec(V.Io):
    """A world for tests: fixed senses, a fixed random stream, a trace."""

    def __init__(self, senses=None, seed=1):
        V.Io.__init__(self)
        self.senses = senses or {}
        self.r = V.xorshift(seed)
        self.trace = []

    def sense(self, k):
        return self.senses.get(k, 0)

    def act(self, a, arg):
        self.trace.append((V.ACTION_NAME[a], arg))

    def rnd(self):
        return self.r()


def go(src, event="poke", arg=0, mem=None, senses=None, nbub=4):
    code = V.compile_script(src, nbub=nbub, v2=True)
    io = Rec(senses)
    mem = mem if mem is not None else [0] * 8
    r, steps = V.run(code, E[event], arg, mem, io)
    return r, io.trace, mem, steps


class TableTest(unittest.TestCase):
    def test_the_table_is_read_from_the_def(self):
        with open(V.DEF_PATH, encoding="utf-8") as f:
            text = f.read()
        lines = [l for l in text.splitlines() if l.startswith("JV_OP(")]
        self.assertEqual(len(lines), V.NOPS)
        self.assertEqual([o.code for o in V.OPS], list(range(V.NOPS)))
        for name in ("END", "PUSH8", "JZ", "ACT", "ACTK", "SENSE", "RAND"):
            self.assertIn(name, V.OP)
        self.assertEqual(V.OPS[V.OP["ADD"]].pops, 2)
        self.assertEqual(V.OPS[V.OP["ACTK"]].nops, 2)
        self.assertEqual(V.NACTIONS, 24)
        self.assertEqual(V.NSENSES, 19)
        self.assertEqual((V.V1_EVENTS, V.V1_ACTIONS, V.V1_SENSES), (8, 12, 9))
        self.assertEqual(V.GROUPS["zone"]["water"], 3)

    def test_a_line_it_cannot_read_is_an_error_not_a_skip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.def")
            with open(p, "w", encoding="utf-8") as f:
                f.write('JV_OP( 0, END, 0, 0, 0, "ok")\nJV_OP(1, BROKEN)\n')
            with self.assertRaises(ValueError):
                V._load_def(p)
            with open(p, "w", encoding="utf-8") as f:
                f.write('JV_OP( 1, END, 0, 0, 0, "not from 0")\n')
            with self.assertRaises(ValueError):
                V._load_def(p)


class StatementTest(unittest.TestCase):
    def test_every_action_statement(self):
        cases = [
            ("say 1", ("say", 0)), ("say 4", ("say", 3)), ("hop", ("hop", 0)),
            ("hop 12", ("hop", 12)), ("walk to garden", ("walk", 0)), ("walk dock", ("walk", 2)),
            ("float", ("float", 0)), ("float 3", ("float", 3)), ("face", ("face", 0)),
            ("stop", ("stop", 0)), ("frame 2", ("frame", 1)), ("flip", ("flip", 0)),
            ("glow on", ("glow", 1)), ("glow off", ("glow", 0)), ("glow toggle", ("glow", 2)),
            ("emit sparkle", ("emit", 0)), ("emit puff", ("emit", 4)), ("wait 8", ("wait", 8)),
            ("hop 300", ("hop", 300)), ("wait -1", ("wait", -1)),
            ("sound horn", ("sound", 5)), ("burst sparkle", ("burst", 0)), ("throw", ("throw", 0)),
            ("eat", ("eat", 0)), ("drop", ("drop", 0)), ("signal 3", ("signal", 3)),
            ("seek food", ("seek", 0)), ("fly", ("fly", 0)), ("home", ("home", 0)),
            ("boost", ("boost", 0)), ("boost 20", ("boost", 20)), ("nudge", ("nudge", 0)),
            ("shake", ("shake", 0)),
        ]
        for stmt, want in cases:
            r, trace, _m, _s = go("on poke: %s; end" % stmt)
            self.assertEqual(r, V.DONE, stmt)
            self.assertEqual(trace, [want], stmt)
        self.assertEqual(set(w for _s, (w, _a) in cases), set(A) - {"nothing"})

    def test_memory_statements_and_senses(self):
        senses = {S["x"]: 120, S["zone"]: 1, S["near"]: 2, S["distance"]: 9, S["time"]: 4,
                  S["weather"]: 3, S["days"]: 6, S["gift"]: 1}
        src = """on gift:
          mem[0] = x; mem[1] = zone; mem[2] = near; mem[3] = distance
          mem[4] = time; mem[5] = weather; mem[6] = days; mem[7] = gift
        end"""
        r, _t, mem, _s = go(src, "gift", senses=senses)
        self.assertEqual(r, V.DONE)
        self.assertEqual(mem, [120, 1, 2, 9, 4, 3, 6, 1])
        r, _t, mem, _s = go("on poke: mem[2] += 5; mem[3] -= 2; mem[4] = mem[2] * 3; end",
                            mem=[0, 0, 10, 10, 0, 0, 0, 0])
        self.assertEqual(mem[2:5], [15, 8, 45])

    def test_names_compare_with_senses(self):
        src = """on tick:
          if near == snail and distance < 10 then say 1
          elif time == night or zone == water then say 2
          else say 3
          end
        end"""
        self.assertEqual(go(src, "tick", senses={S["near"]: 2, S["distance"]: 3})[1], [("say", 0)])
        self.assertEqual(go(src, "tick", senses={S["near"]: 2, S["distance"]: 30})[1], [("say", 2)])
        self.assertEqual(go(src, "tick", senses={S["time"]: 4})[1], [("say", 1)])
        self.assertEqual(go(src, "tick", senses={S["zone"]: 3})[1], [("say", 1)])

    def test_return_stops_and_handles(self):
        r, trace, _m, steps = go("on poke: hop; return; flip; end")
        self.assertEqual((r, trace), (V.DONE, [("hop", 0)]))
        self.assertEqual(steps, 2)

    def test_rand_and_computed_arguments(self):
        r, trace, mem, _s = go("on poke: mem[0] = rand(10); say rand(2) + 1; frame mem[0] % 2 + 1; end")
        self.assertEqual(r, V.DONE)
        self.assertTrue(0 <= mem[0] < 10)
        self.assertEqual(trace[0][0], "say")
        self.assertIn(trace[0][1], (0, 1))
        self.assertEqual(trace[1], ("frame", mem[0] % 2))
        self.assertEqual(go("on poke: mem[0] = rand(0) + rand(-3); end")[2][0], 0)

    def test_arithmetic_is_the_machines(self):
        # folded at compile time and done at run time, the same answers
        src = "on poke: mem[0] = -7 / 2; mem[1] = -7 %% 2; mem[2] = 7 / 0; mem[3] = 30000 + 30000; " \
              "mem[4] = mem[%d] / 2; mem[5] = mem[%d] %% 2; mem[6] = -32768 / -1; end"
        r, _t, mem, _s = go(src % (7, 7), mem=[0] * 7 + [-7])
        self.assertEqual(mem[:7], [-3, -1, 0, -5536, -3, -1, -32768])

    def test_comments_newlines_and_case(self):
        src = "# a comment\nON POKE:   # another\n\n  Say 1\n\n  HOP\nEND\n"
        self.assertEqual(go(src)[1], [("say", 0), ("hop", 0)])

    def test_one_handler_runs_the_narrowest_first(self):
        src = """
        on near: say 1; end
        on near snail: say 2; end
        on tick: flip; end
        on tick every 4: hop; end
        on time night: glow on; end
        on jam fixed: emit note; end
        on weather 2: walk to water; end
        on shipped: emit heart; end
        """
        code = V.compile_script(src)
        io = Rec()
        mem = [0] * 8
        for ev, arg in (("near", 2), ("near", 1), ("tick", 4), ("tick", 5), ("time", 4),
                        ("time", 1), ("jam", 2), ("jam", 1), ("weather", 2), ("weather", 1),
                        ("shipped", 0), ("gift", 0), ("poke", 0)):
            V.run(code, E[ev], arg, mem, io)
        self.assertEqual(io.trace, [("say", 1), ("say", 0), ("hop", 0), ("flip", 0), ("glow", 1),
                                    ("emit", 2), ("walk", 3), ("emit", 1)])


class ErrorTest(unittest.TestCase):
    def err(self, src, line, *words, **kw):
        with self.assertRaises(V.ScriptError) as cm:
            V.compile_script(src, **kw)
        e = cm.exception
        self.assertEqual(e.line, line, str(e))
        for w in words:
            self.assertIn(w, str(e))
        if line:
            self.assertTrue(str(e).startswith("line %d: " % line), str(e))
        return str(e)

    def test_errors_name_the_line_and_what_was_expected(self):
        self.err("on poke:\n  say 1\n", 3, "end")
        self.err("on poke:\n  if x > 3 say 1 end\nend", 2, "'then'")
        self.err("on poke:\n  if x = 3 then say 1 end\nend", 2, "==")
        self.err("on poke:\n  dance\nend", 2, "unknown statement 'dance'", "say", "hop")
        self.err("on wiggle: hop; end", 1, "expected an event", "poke")
        self.err("on poke hop; end", 1, "':'")
        self.err("on poke:\n\n  walk to moon\nend", 3, "zone", "garden")
        self.err("on poke: emit glitter; end", 1, "particle", "sparkle")
        self.err("on poke: glow bright; end", 1, "on, off or toggle", "bright")
        self.err("on poke: say 3; end", 1, "bubbles 1..2", nbub=2)
        self.err("on poke: say 0; end", 1, "bubbles")
        self.err("on poke: frame 5; end", 1, "frames")
        self.err("on poke: mem[8] = 1; end", 1, "slot 0..7")
        self.err("on poke: mem[0] = 40000; end", 1, "too big")
        self.err("on poke: face 3; end", 1, "takes nothing")
        self.err("on poke: wait; end", 1, "needs a number")
        self.err("on poke: hop\non tick: flip; end", 2, "'end' to close")
        self.err("on poke: hop; end\non poke: flip; end", 2, "second 'on poke'", "line 1")
        self.err("on near snail: hop; end\non near snail: flip; end", 2, "second")
        self.err("on tick every 0: hop; end", 1, "1..255")
        self.err("on poke: hop 1 2; end", 1, "';'")
        self.err("on poke: else hop; end", 1, "without an 'if'")
        self.err("on poke: mem[0] = (1 + 2; end", 1, "')'")
        self.err("on poke: mem[0] = 1 < 2 < 3; end", 1, "';'")
        self.err("", 1, "no handlers")
        self.err("on poke: say 1; end!", 1, "unexpected character")

    def test_a_script_too_big_is_refused(self):
        src = "on poke:\n" + "mem[0] = mem[1] + 1000\n" * 40 + "end\n"
        self.err(src, 0, "256")


class MachineTest(unittest.TestCase):
    def asm(self, code, ev="poke", fmt=V.FORMAT):
        return bytes([fmt, 1, E[ev], 0, 5]) + bytes(code)

    def test_step_limit(self):
        src = "on poke:\n" + "mem[0] += 1\n" * 17 + "say 1\nend\n"
        r, trace, mem, steps = go(src)
        self.assertEqual((r, steps, trace), (V.LIMIT, V.STEPS, []))
        self.assertEqual(mem[0], 16)             # what it did before it stopped stays done
        r, steps = V.run(self.asm([OP["ACTK"], A["flip"], 0, OP["JMP"], 5]), E["poke"], 0, [0] * 8, Rec())
        self.assertEqual((r, steps), (V.LIMIT, V.STEPS))

    def test_faults_stop_safely(self):
        bad = {
            "underflow": [OP["ADD"], OP["END"]],
            "overflow": [OP["PUSH8"], 1] * 9 + [OP["END"]],
            "far jump": [OP["JMP"], 250, OP["END"]],
            "into the header": [OP["JMP"], 1, OP["END"]],
            "unknown": [0xEE, OP["END"]],
            "slot": [OP["STORE"], 8, OP["END"]],
            "sense": [OP["SENSE"], 99, OP["END"]],
            "action": [OP["ACTK"], 99, 0, OP["END"]],
            "cut": [OP["PUSH16"], 1],
            "off the end": [OP["ACTK"], A["hop"], 0],
        }
        for name, code in bad.items():
            mem = [0] * 8
            r, _s = V.run(self.asm(code), E["poke"], 0, mem, Rec())
            self.assertEqual(r, V.FAULT, name)
            self.assertEqual(mem, [0] * 8, name)
        accepted = {name for name, code in bad.items() if V.check(self.asm(code)) == 0}
        self.assertEqual(accepted, {"underflow", "overflow", "off the end"})

    def test_newer_format_is_ignored(self):
        s = self.asm([OP["ACTK"], A["hop"], 0, OP["END"]], fmt=V.FORMAT + 1)
        self.assertEqual(V.check(s), V.IGNORE)
        self.assertEqual(V.run(s, E["poke"], 0, [0] * 8, Rec()), (V.IGNORE, 0))
        self.assertEqual(V.run(b"", E["poke"], 0, [0] * 8, Rec()), (V.IGNORE, 0))

    def test_memory_persists_across_events(self):
        code = V.compile_script("on poke: mem[0] += 1; if mem[0] > 2 then say 1; mem[0] = 0 end; end")
        mem, io = [0] * 8, Rec()
        for _ in range(7):
            V.run(code, E["poke"], 0, mem, io)
        self.assertEqual(io.trace, [("say", 0), ("say", 0)])
        self.assertEqual(mem[0], 1)

    def test_values_wrap_and_senses_clamp(self):
        code = V.compile_script("on poke: mem[0] = days; mem[1] = x; mem[2] = mem[0] + 1; end")
        mem = [0] * 8
        V.run(code, E["poke"], 0, mem, Rec({S["days"]: 99999, S["x"]: -99999}))
        self.assertEqual(mem[:3], [32767, -32768, -32768])

    def test_disassemble_reads_back(self):
        text = V.disassemble(V.compile_script("on poke: say 1; end"))
        self.assertIn("on poke", text)
        self.assertIn("ACTK say 0", text)


GOOD = [
    "on poke: say 1; hop; end",
    "on tick: if rand(100) < 5 then emit sparkle end\nend",
    "on near critter: mem[0] = mem[0] + 1; if mem[0] > 3 then say 2; mem[0] = 0 end\nend",
    "on time night: glow on\nend\non time dawn: glow off; end",
    "on shipped: if rand(3) == 0 then hop 10 end; end",
]


class DayTest(unittest.TestCase):
    def test_good_scripts_live_through_a_day(self):
        for src in GOOD:
            rep = V.simulate_day(V.compile_script(src))
            self.assertTrue(rep.ok, (src, rep.problems))
            self.assertGreater(rep.visible, 0, src)
            self.assertLessEqual(rep.max_steps, V.STEPS)
            self.assertEqual(rep.events["tick"], 24 * 120)

    def test_the_guide_example_lives_through_a_day(self):
        ex = V.LANGUAGE_GUIDE.split("EXAMPLE\n", 1)[1]
        self.assertTrue(V.simulate_day(V.prepare_script(ex)).ok)

    def test_a_script_that_loops_is_rejected(self):
        loop = bytes([V.FORMAT, 1, E["tick"], 0, 5, OP["ACTK"], A["hop"], 0, OP["JMP"], 5])
        rep = V.simulate_day(loop)
        self.assertFalse(rep.ok)
        self.assertIn("without reaching its end", rep.problems[0])
        src = "on poke:\n" + "mem[0] += 1\n" * 17 + "say 1\nend\n"
        with self.assertRaises(V.ScriptError) as cm:
            V.prepare_script(src)
        self.assertIn("on poke: ran 64 instructions", str(cm.exception))

    def test_a_script_that_faults_is_rejected(self):
        bad = bytes([V.FORMAT, 1, E["shipped"], 0, 5, OP["ADD"], OP["END"]])
        rep = V.simulate_day(bad)
        self.assertFalse(rep.ok)
        self.assertIn("on shipped: the machine faulted", rep.problems[0])
        self.assertFalse(V.simulate_day(b"\x02\x00").ok)

    def test_a_script_that_never_acts_is_rejected(self):
        for src in ("on poke: mem[0] = 1; end",
                    "on tick: wait 4; end",
                    "on weather 9: hop; end",                    # never fires in the day
                    "on tick: if rand(10) > 20 then hop end\nend"):  # never true
            rep = V.simulate_day(V.compile_script(src))
            self.assertFalse(rep.ok, src)
            self.assertIn("never did anything visible", rep.problems[0])

    def test_a_bubble_the_item_lacks_is_rejected(self):
        code = V.compile_script("on poke: say mem[0] + 3; end")
        rep = V.simulate_day(code, nbub=2)
        self.assertFalse(rep.ok)
        self.assertIn("'say 3', but the item has 2 bubbles", rep.problems[0])
        with self.assertRaises(V.ScriptError):
            V.prepare_script("on poke: say 3; end", nbub=2)

    def test_prepare_gives_bytes_the_machine_takes(self):
        code = V.prepare_script(GOOD[2], nbub=2)
        self.assertIsInstance(code, bytes)
        self.assertEqual(V.check(code), 0)
        self.assertLessEqual(len(code), 256)


class GuideTest(unittest.TestCase):
    def test_the_guide_names_every_word(self):
        g = V.LANGUAGE_GUIDE_V2
        for word in list(A) + list(S) + list(E) + list(V.GROUPS["trait"]) + list(V.GROUPS["sound"]):
            if word != "nothing":
                self.assertIn(word, g)
        self.assertLess(len(g), 7000)
        v1 = V.LANGUAGE_GUIDE_V1
        for word in ("burst", "throw", "on hour", "has(", "neartraits"):
            self.assertNotIn(word, v1)                       # nothing a device cannot run yet
        self.assertLess(len(v1), 4000)


class VersionTwoTest(unittest.TestCase):
    def test_new_events_narrow(self):
        src = ("on hour 18: sound horn; end\non hour: say 1; end\n"
               "on signal 2: throw; end\non world ants: shake; end\n"
               "on new: if has(newtraits, sweet) then hop 10 end; end\n"
               "on berry: eat; end\non bumped: say 2; end")
        self.assertEqual(go(src, "hour", 19)[1], [("sound", V.GROUPS["sound"]["horn"])])  # 18 + 1
        self.assertEqual(go(src, "hour", 3)[1], [("say", 0)])
        self.assertEqual(go(src, "signal", 2)[1], [("throw", 0)])
        self.assertEqual(go(src, "signal", 3)[1], [])
        self.assertEqual(go(src, "world", V.GROUPS["world"]["ants"])[1], [("shake", 0)])
        sweet = 1 << V.GROUPS["trait"]["sweet"]
        self.assertEqual(go(src, "new", senses={S["newtraits"]: sweet})[1], [("hop", 10)])
        self.assertEqual(go(src, "new", senses={S["newtraits"]: 1})[1], [])
        self.assertEqual(go(src, "bumped")[1], [("say", 1)])

    def test_has_reads_a_bit_and_bit_fifteen(self):
        sleepy = V.GROUPS["trait"]["sleepy"]
        self.assertEqual(sleepy, 15)
        src = "on poke: if has(neartraits, sleepy) then say 1 else say 2 end; end"
        self.assertEqual(go(src, senses={S["neartraits"]: -32768})[1], [("say", 0)])  # bit 15
        self.assertEqual(go(src, senses={S["neartraits"]: 0x7FFF})[1], [("say", 1)])
        with self.assertRaises(V.ScriptError):
            V.compile_script("on poke: if has(traits, purple) then hop end; end", v2=True)

    def test_filters_are_checked(self):
        for bad in ("on hour 24: hop; end", "on signal 0: hop; end", "on world rain: hop; end"):
            with self.assertRaises(V.ScriptError, msg=bad):
                V.compile_script(bad, v2=True)

    def test_version_one_refuses_what_version_two_adds(self):
        with self.assertRaises(V.ScriptError) as e:
            V.compile_script("on poke: burst sparkle; end", v2=False)
        self.assertIn("burst", str(e.exception))
        with self.assertRaises(V.ScriptError):
            V.compile_script("on berry: hop; end", v2=False)
        with self.assertRaises(V.ScriptError):
            V.compile_script("on poke: if berries > 0 then hop end; end", v2=False)
        code = V.compile_script("on poke: hop; say 1; end", v2=False)
        self.assertEqual(V.v2_words(code), [])

    def test_a_name_means_one_number(self):
        self.assertEqual(V.CONSTS["water"], V.GROUPS["zone"]["water"])
        self.assertEqual(V.GROUPS["trait"]["wet"], 6)

    def test_an_interactive_item_lives_through_its_day(self):
        src = ("on poke:\n  burst sparkle; sound bang; say 1\nend\n"
               "on near:\n  if has(neartraits, food) and fed < 5 then seek food; eat end\nend\n"
               "on berry: eat; drop; end\non world ants: shake; signal 2; end")
        rep = V.simulate_day(V.compile_script(src, 2, 1, v2=True), 2, 1)
        self.assertTrue(rep.ok, rep.problems)
        self.assertGreater(rep.actions.get("eat", 0), 0)
        self.assertGreater(rep.actions.get("burst", 0), 0)
        self.assertEqual(rep.events.get("world"), 2)


def _fixtures_module():
    path = os.path.join(ROOT, "tools", "make_jarvm_fixtures.py")
    spec = importlib.util.spec_from_file_location("make_jarvm_fixtures", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FixtureTest(unittest.TestCase):
    def test_the_fixtures_are_current(self):
        """The C test replays these; they must be what this machine says now
        (python tools/make_jarvm_fixtures.py rewrites them)."""
        mod = _fixtures_module()
        out = os.path.join(ROOT, "test", "fixtures")
        with open(os.path.join(out, "jarvm_list.txt"), encoding="utf-8") as f:
            self.assertEqual(f.read().split(), [fx[0] for fx in mod.FIXTURES])
        for name, script, seed, senses, mem, events in mod.FIXTURES:
            with open(os.path.join(out, "jarvm_%s.bin" % name), "rb") as f:
                self.assertEqual(f.read(), script, name)
            trace = ["check %d" % V.check(script)]
            io = mod.FixtureIo(seed, senses, trace)
            m = list(mem)
            for word, arg in events:
                r, steps = V.run(script, E[word], arg, m, io)
                trace.append("ret %d %d" % (r, steps))
                trace.append("mem " + " ".join(str(v) for v in m))
            with open(os.path.join(out, "jarvm_%s.txt" % name), encoding="utf-8") as f:
                want = f.read().replace("\r\n", "\n").split("\n--\n", 1)[1]
            self.assertEqual("\n".join(trace) + "\n", want, name)


if __name__ == "__main__":
    unittest.main()
