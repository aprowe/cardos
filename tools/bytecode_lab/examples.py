"""Part 4: the three sandbox behaviours, written in each candidate design and
measured.

  pill bug  crawls; poked or bumped, it curls into a ball and rolls with real
            momentum (friction, bouncing off the glass), stops, uncurls and
            crawls on
  tree      shakes (sways, shows a shake frame, puffs leaves) when a bird
            lands on one of its perches, or when poked
  bird      sits on a perch looking around; now and then swoops in an arc to
            a random perch (or home), lands, maybe says something

Designs:
  today     the language as it is (apps/jarvm.def). It has no momentum, no
            arcs, no perches: these are the closest it can come, noted.
  extended  today's language + motion primitives (lab_ext.def: body, push,
            swoop, wobble, anim; events stopped, landed, perched, hit),
            compiled by the real compiler over the extended table
  table     format 3, the behaviour table (bt.py)
Each code-based design is given in format 1 (today's bytes) and format 2
(dense.py).

    python tools/bytecode_lab/examples.py [--json out.json]
"""
import json
import sys

import bt
import dense
import labvm
from isa import V

X = labvm.X

TODAY = {
    "pillbug": """
on poke:
  frame 3; sound boing; mem[0] = 12
  if x < 120 then walk to dock else walk to garden end
end
on bumped:
  frame 3; sound boing; mem[0] = 12; walk to anywhere
end
on tick every 2:
  if mem[0] > 0 then
    mem[0] -= 1
    if mem[0] % 2 == 0 then frame 3 else frame 4 end
    if mem[0] == 0 then stop; frame 1; say 1 end
  end
end
""",
    "tree": """
on signal 7:
  frame 2; emit puff; mem[0] = 6
end
on poke:
  frame 2; mem[0] = 4
end
on tick:
  if mem[0] > 0 then
    mem[0] -= 1
    if mem[0] % 2 == 0 then frame 2 else frame 3 end
    if mem[0] == 0 then frame 1 end
  end
end
""",
    "bird": """
on tick every 4:
  if mem[0] == 0 then
    if rand(40) == 0 then flip end
    if rand(30) == 0 then fly; sound whoosh; mem[0] = 1; mem[1] = 12 + rand(12) end
  else
    mem[1] -= 1
    if mem[1] <= 0 then home; mem[0] = 0; signal 7; if rand(2) == 0 then say 1 end end
  end
end
on poke:
  sound chirp; fly; mem[0] = 1; mem[1] = 8
end
""",
}

TODAY_LOST = {
    "pillbug": "no momentum or bounce: 'rolling' is a walk to a zone with the frames swapped",
    "tree": "shakes on a signal number the bird must agree on, not on a landing",
    "bird": "flies about and goes home; cannot pick a perch or arc to it; the tree learns by signal",
}

EXTENDED = {
    "pillbug": """
on poke:
  frame 3; sound boing; body roll; push 48; mem[0] = 1
end
on bumped:
  frame 3; body roll; push 32; mem[0] = 1
end
on stopped:
  if mem[0] == 1 then mem[0] = 2; mem[1] = 0 end
end
on tick every 4:
  if mem[0] == 2 then
    mem[1] += 1
    if mem[1] == 2 then frame 4; say 1
    elif mem[1] == 3 then frame 1; body crawl; mem[0] = 0 end
  end
end
on hit: sound plop
end
""",
    "tree": """
on perched:
  wobble 6; frame 2; emit puff; mem[0] = 6
end
on poke:
  wobble 4; frame 2; mem[0] = 4
end
on tick:
  if mem[0] > 0 then
    mem[0] -= 1
    if mem[0] == 0 then frame 1 end
  end
end
""",
    "bird": """
on tick every 4:
  if mem[0] == 0 then
    if rand(40) == 0 then flip end
    if rand(30) == 0 then swoop perch; anim 22; sound whoosh; mem[0] = 1
    elif rand(80) == 0 then swoop nest; anim 22; mem[0] = 1 end
  end
end
on landed:
  mem[0] = 0; frame 1
  if rand(2) == 0 then say 1 end
end
on poke:
  sound chirp; swoop perch; anim 22; mem[0] = 1
end
""",
}

S = X.GROUPS["sound"]
BT = {
    "pillbug": bt.encode({
        "preset": 1,
        "globals": [("poke", 0, "roll", None), ("bumped", 0, "roll", None),
                    ("hit", 0, None, ("sound", S["plop"]))],
        "states": [
            {"name": "crawl", "motion": ("crawl", 0), "anim": (0, 2, 3)},
            {"name": "roll", "motion": ("roll", 20), "anim": (2, 1, 0), "enter": [("sound", S["boing"])],
             "on": [("done", 0, "rest")]},
            {"name": "rest", "motion": ("still", 0), "anim": (2, 1, 0), "on": [("after", 8, "uncurl")]},
            {"name": "uncurl", "motion": ("still", 0), "anim": (3, 1, 0), "enter": [("say", 0)],
             "on": [("after", 4, "crawl")]},
        ]}),
    "tree": bt.encode({
        "perches": [(-8, 22), (7, 28)],
        "globals": [("perched", 0, "shake", None), ("poke", 0, "shake", None)],
        "states": [
            {"name": "idle", "motion": ("still", 0), "anim": (0, 1, 0)},
            {"name": "shake", "motion": ("sway", 3), "anim": (1, 2, 1),
             "enter": [("emit", X.GROUPS["particle"]["puff"])], "on": [("after", 6, "idle")]},
        ]}),
    "bird": bt.encode({
        "preset": 2,
        "globals": [("poke", 0, "fly", ("sound", S["chirp"]))],
        "states": [
            {"name": "sit", "motion": ("perched", 1), "anim": (0, 1, 0),
             "on": [("chance", 8, "fly"), ("chance", 3, "gohome")]},
            {"name": "fly", "motion": ("arc", 1), "anim": (1, 2, 1), "enter": [("sound", S["whoosh"])],
             "on": [("done", 0, "land")]},
            {"name": "gohome", "motion": ("arc", 0), "anim": (1, 2, 1), "on": [("done", 0, "land")]},
            {"name": "land", "motion": ("perched", 0), "anim": (0, 1, 0), "enter": [("say", 0)],
             "on": [("after", 4, "sit")]},
        ]}),
}


def sizes():
    rows = []
    for name in ("pillbug", "tree", "bird"):
        dense.use_table(V)
        t1 = V.compile_script(TODAY[name], v2=True)
        t2 = dense.encode(t1)
        dense.use_table(X)
        e1 = X.compile_script(EXTENDED[name], v2=True)
        e2 = dense.encode(e1)
        dense.use_table(V)
        b = BT[name]
        assert not bt.check(b), bt.check(b)
        rows.append({"example": name, "today_f1": len(t1), "today_f2": len(t2), "ext_f1": len(e1),
                     "ext_f2": len(e2), "table": len(b),
                     "today_lines": len([l for l in TODAY[name].splitlines() if l.strip()]),
                     "ext_lines": len([l for l in EXTENDED[name].splitlines() if l.strip()]),
                     "today_lost": TODAY_LOST[name]})
    return rows


def main():
    rows = sizes()
    print("%-8s %9s %9s %9s %9s %7s" % ("", "today f1", "today f2", "ext f1", "ext f2", "table"))
    for r in rows:
        print("%-8s %9d %9d %9d %9d %7d" % (r["example"], r["today_f1"], r["today_f2"], r["ext_f1"],
                                            r["ext_f2"], r["table"]))
    tot = {k: sum(r[k] for r in rows) for k in ("today_f1", "today_f2", "ext_f1", "ext_f2", "table")}
    print("%-8s %9d %9d %9d %9d %7d" % ("all", tot["today_f1"], tot["today_f2"], tot["ext_f1"],
                                        tot["ext_f2"], tot["table"]))
    if len(sys.argv) > 2 and sys.argv[1] == "--json":
        with open(sys.argv[2], "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
