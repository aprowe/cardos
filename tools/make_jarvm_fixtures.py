#!/usr/bin/env python3
"""Write the cross-check fixtures for Jar Factory's script machine.

Each fixture is a script (test/fixtures/jarvm_NAME.bin, the bytecode) and a
run (test/fixtures/jarvm_NAME.txt): the world it runs in, a list of events,
and -- after a line "--" -- the trace the Python machine (server/jarvm.py)
produced for them. test/test_jarvm.c replays every fixture named in
test/fixtures/jarvm_list.txt through the C machine (apps/jarvm.h) and needs
the same trace, byte for byte.

The .txt, before "--":
    seed N          the random numbers' seed (xorshift, as js_rnd)
    sense K V       sense K reads V (sense 8, random, is rnd() & 255)
    mem A B ... H   the item's memory before the first event
    ev E A          fire event E with argument A
After it, the trace:
    check R         jv_check's answer for the script
    act A ARG       an action, in the order done
    ret R S         the run's result and its step count
    mem A B ... H   the memory after that run

Run it after changing apps/jarvm.def, the machine, or the compiler:
    python tools/make_jarvm_fixtures.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from server import jarvm as V  # noqa: E402

OUT = os.path.join(ROOT, "test", "fixtures")

E = V.EVENTS
OP = V.OP


VERSION2 = """
on hour 18: sound horn; end
on hour: mem[0] = hour; end
on signal 2: throw; signal 3; end
on world ants: shake; boost 20; end
on world: nudge; end
on new: if has(newtraits, shiny) then burst sparkle else burst heart end; end
on berry: if berries > 2 then eat else drop end; end
on bumped: mem[1] = has(neartraits, sleepy) * 10 + has(neartraits, food); end
on near: seek food; fly; home; end
on poke: mem[2] += fed; if mem[2] > 3 then sound bang end; end
"""


def asm(entries, code, fmt=V.FORMAT):
    """Bytes by hand: entries [(event, filter, offset from code start)]."""
    base = 2 + 3 * len(entries)
    out = bytearray([fmt, len(entries)])
    for ev, f, off in entries:
        out += bytes([ev, f, base + off])
    return bytes(out + bytes(code))


ALL_STATEMENTS = """
# every statement and sense, once
on poke:
  say 1; say 4; hop; hop 12; walk to garden; walk dock; float; float 3
  face; stop; frame 2; flip; glow on; glow off; glow toggle
  emit sparkle; emit heart; emit note; emit zzz; emit puff; wait 8
end
on gift:
  mem[0] = x; mem[1] = zone; mem[2] = near; mem[3] = distance
  mem[4] = time; mem[5] = weather; mem[6] = days; mem[7] = gift
end
on shipped:
  mem[0] = random; mem[1] = rand(10); mem[2] = rand(0); mem[3] = rand(-4)
  say rand(3) + 1; frame mem[1] % 3 + 1; hop mem[3] + 2
end
"""

ARITH = """
on poke:
  mem[0] = mem[0] + 1000 * 40          # wraps
  mem[1] = -7 / 2; mem[2] = -7 % 2; mem[3] = 7 / 0; mem[4] = 7 % 0
  mem[5] = (mem[0] - 3) * -2; mem[6] = -32768 / -1
  mem[7] = not (1 < 2 and 3 >= 3 or 0) + (2 != 2) * 5
end
on gift:
  mem[0] = x / mem[7] ; mem[1] = x % -3; mem[2] = -x / 4
  mem[3] = (x == 120) + (x <= 120) * 2 + (x > 119) * 4 + (x < 0) * 8
  mem[4] += 300; mem[5] -= x; mem[6] = -mem[6]
end
"""

BRANCHES = """
on tick every 4:
  emit note
end
on tick:
  if mem[0] < 3 then mem[0] += 1; hop
  elif mem[0] == 3 then mem[0] = 10; say 2
  else mem[0] = 0; flip
  end
end
on near snail: say 1; end
on near: if near == critter or distance < 5 then emit heart else face end
end
on time night: glow on; end
on time: glow off; end
on jam fixed: say 3
end
on jam: if time == night then return end
  emit puff
end
on weather 2: walk to water; end
"""

# Over 64 instructions with no loop: the compiler takes it, the machine stops.
LONG = "on poke:\n" + "mem[0] += 1\n" * 17 + "say 1\nend\n"

EVENTS_ALL = [
    ("poke", 0), ("gift", 0), ("shipped", 0), ("shipped", 0), ("poke", 0), ("gift", 0),
    ("tick", 0), ("tick", 1),
]

FIXTURES = [
    # name, script bytes, seed, senses {k: v}, initial mem, events [(word, arg)]
    ("statements", V.compile_script(ALL_STATEMENTS), 12345,
     {0: 120, 1: 1, 2: 3, 3: 14, 4: 4, 5: 2, 6: 9, 7: 1}, [0] * 8, EVENTS_ALL),
    ("arith", V.compile_script(ARITH), 7,
     {0: 120, 1: 1, 2: 3, 3: 14, 4: 4, 5: 0, 6: 0, 7: 0}, [5, 0, 0, 0, 32700, 0, 0, 0],
     [("poke", 0), ("gift", 0), ("poke", 0), ("gift", 0)]),
    ("branches", V.compile_script(BRANCHES), 99,
     {0: 40, 1: 0, 2: 2, 3: 3, 4: 4, 5: 2, 6: 0, 7: 0}, [0] * 8,
     [("tick", t) for t in range(10)] +
     [("near", 2), ("near", 1), ("near", 3), ("near", 0), ("time", 4), ("time", 1),
      ("jam", 1), ("jam", 2), ("weather", 2), ("weather", 1), ("poke", 0), ("gift", 0)]),
    ("limit", V.compile_script(LONG), 1, {}, [0] * 8, [("poke", 0), ("poke", 0)]),
    # version 2: the new events with their filters, has() (BIT) on bit 15 and
    # beyond, and every new action
    ("v2", V.compile_script(VERSION2, v2=True), 4242,
     {V.SENSES["neartraits"]: -32768, V.SENSES["newtraits"]: 0x0202, V.SENSES["berries"]: 3,
      V.SENSES["fed"]: 2, V.SENSES["hour"]: 18},
     [0] * 8,
     [("hour", 19), ("hour", 3), ("signal", 2), ("signal", 9), ("world", 1), ("world", 2),
      ("new", 0), ("berry", 0), ("bumped", 0), ("near", 1), ("poke", 0), ("poke", 0)]),
    ("clamp", asm([(E["poke"], 0, 0)],
                  [OP["SENSE"], 6, OP["STORE"], 0, OP["SENSE"], 0, OP["STORE"], 1, OP["END"]]),
     3, {6: 40000, 0: -40000}, [0] * 8, [("poke", 0)]),
    # by hand: what a compiler never makes
    ("bit_range", asm([(E["poke"], 0, 0)],
                      [OP["PUSH8"], 7, OP["PUSH8"], 99, OP["BIT"], OP["STORE"], 0,
                       OP["PUSH8"], 0xFF, OP["PUSH8"], 15, OP["BIT"], OP["STORE"], 1,
                       OP["PUSH8"], 5, OP["PUSH8"], 0xFE, OP["BIT"], OP["STORE"], 2,
                       OP["PUSH8"], 5, OP["PUSH8"], 2, OP["BIT"], OP["STORE"], 3, OP["END"]]),
     1, {}, [9] * 8, [("poke", 0)]),
    ("loop", asm([(E["poke"], 0, 0)], [OP["ACTK"], 1, 0, OP["JMP"], 5]),
     1, {}, [0] * 8, [("poke", 0), ("tick", 0)]),
    ("underflow", asm([(E["poke"], 0, 0)], [OP["ACTK"], 7, 0, OP["ADD"], OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("overflow", asm([(E["poke"], 0, 0)], [OP["PUSH8"], 1] * 9 + [OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("badjump", asm([(E["poke"], 0, 0)], [OP["ACTK"], 10, 0, OP["JMP"], 200, OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("backjump", asm([(E["poke"], 0, 0)], [OP["JMP"], 2, OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("unknown_op", asm([(E["poke"], 0, 0)], [OP["ACTK"], 1, 0, 0xEE, OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("bad_slot", asm([(E["poke"], 0, 0)], [OP["LOAD"], 9, OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("bad_action", asm([(E["poke"], 0, 0)], [OP["ACTK"], 60, 0, OP["END"]]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("off_end", asm([(E["poke"], 0, 0)], [OP["ACTK"], 1, 0]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("cut_operand", asm([(E["poke"], 0, 0)], [OP["ACTK"], 1]),
     1, {}, [0] * 8, [("poke", 0)]),
    ("newer", asm([(E["poke"], 0, 0)], [OP["ACTK"], 1, 0, OP["END"]], fmt=2),
     1, {}, [0] * 8, [("poke", 0)]),
    ("too_many_entries", bytes([1, 200]) + bytes(20), 1, {}, [0] * 8, [("poke", 0)]),
    ("empty", b"", 1, {}, [0] * 8, [("poke", 0)]),
]


class FixtureIo(V.Io):
    def __init__(self, seed, senses, trace):
        V.Io.__init__(self)
        self.r = V.xorshift(seed)
        self.senses = senses
        self.trace = trace

    def sense(self, k):
        return self.r() & 255 if k == V.SENSES["random"] else self.senses.get(k, 0)

    def act(self, a, arg):
        self.trace.append("act %d %d" % (a, arg))

    def rnd(self):
        return self.r()


def make(name, script, seed, senses, mem, events):
    inp = ["# %s: written by tools/make_jarvm_fixtures.py -- do not edit" % name,
           "seed %d" % seed]
    for k in sorted(senses):
        inp.append("sense %d %d" % (k, senses[k]))
    inp.append("mem " + " ".join(str(v) for v in mem))
    for word, arg in events:
        inp.append("ev %d %d" % (E[word], arg))
    trace = ["check %d" % V.check(script)]
    io = FixtureIo(seed, senses, trace)
    mem = list(mem)
    for word, arg in events:
        r, steps = V.run(script, E[word], arg, mem, io)
        trace.append("ret %d %d" % (r, steps))
        trace.append("mem " + " ".join(str(v) for v in mem))
    with open(os.path.join(OUT, "jarvm_%s.bin" % name), "wb") as f:
        f.write(script)
    with open(os.path.join(OUT, "jarvm_%s.txt" % name), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(inp + ["--"] + trace) + "\n")
    return trace


def main():
    os.makedirs(OUT, exist_ok=True)
    names = []
    for fx in FIXTURES:
        trace = make(*fx)
        names.append(fx[0])
        print("  jarvm_%-16s %3d bytes, %3d trace lines" % (fx[0], len(fx[1]), len(trace)))
    with open(os.path.join(OUT, "jarvm_list.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(names) + "\n")


if __name__ == "__main__":
    main()
