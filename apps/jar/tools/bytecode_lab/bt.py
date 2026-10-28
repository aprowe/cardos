"""Candidate format 3: a behaviour table. A small state machine as data, whose
states name a parameterised motion primitive (the physics, arcs and perches
live in the firmware), an animation, a few one-byte actions on entry, and
2-byte transitions. No code, no loops: the device walks a table.

LAYOUT
  byte 0   format 3
  byte 1   bits 0-2 states-1 (1..8), bits 3-4 perch points (0..3),
           bits 5-7 global transitions (0..7)
  byte 2   bits 0-3 body preset (a 16-row table in flash: gravity, friction,
           bounce, top speed, arc apex), bits 4-7 spare
  perches  2 bytes each: dx (signed px from its middle), dy (px above its foot)
  globals  3 bytes each: [trigger:5 | next:3] [arg] [one ACT1 action, 0xFF none]
           (checked in every state: poke, bumped, ...)
  states   byte m: bits 0-2 motion, bits 3-7 its parameter
           byte a: bits 0-1 first frame, 2-3 frames-1, 4-7 ticks a frame (0 still)
           byte n: bits 0-3 actions on entry, bits 4-7 transitions
           actions: ACT1 bytes (format 2's one-byte table)
           transitions: [trigger:5 | next:3] [arg]       next 7 = stay
TRIGGERS  0..17 the events (lab numbering: tick .. hit), 18 AFTER arg ticks,
          19 DONE (the motion finished: stopped or landed), 20 CHANCE arg/256
          a tick
MOTIONS   0 still, 1 crawl (in zone param), 2 roll (impulse param*8 px/s),
          3 arc (to target param: 0 nest, 1 a random perch, 2 friend),
          4 perched (param bit 0: look around), 5 sway (amplitude), 6 hang,
          7 float

At most one transition fires per item per tick, and every trigger waits for
time or an event (AFTER 0 is refused), so a table cannot loop: the work an
item costs a tick is bounded by its size, with no step counter.
"""
import dense
import labvm

X = labvm.X
TRIG = dict(X.EVENTS)
TRIG.update({"after": 18, "done": 19, "chance": 20})
MOTION = {"still": 0, "crawl": 1, "roll": 2, "arc": 3, "perched": 4, "sway": 5, "hang": 6, "float": 7}
STAY = 7
# body presets (flash): gravity 1/16 px/tick^2, friction /256 a tick, bounce /256, top speed, apex px
PRESETS = [(0, 0, 0, 0, 0), (12, 5, 150, 96, 0), (0, 0, 0, 48, 24), (6, 30, 60, 32, 0)]


def act1(word, arg=0):
    dense.use_table(X)
    return dense.ACT1[(X.ACTIONS[word], arg)]


def encode(spec):
    """spec: {"preset": k, "perches": [(dx, dy)], "globals": [(trigger, arg, next, act)],
    "states": [{"motion": (name, param), "anim": (first, count, period),
                "enter": [(word, arg)], "on": [(trigger, arg, next)]}]}"""
    st = spec["states"]
    names = [s["name"] for s in st]
    out = bytearray([3, (len(st) - 1) | (len(spec.get("perches", [])) << 3)
                     | (len(spec.get("globals", [])) << 5), spec.get("preset", 0)])
    for dx, dy in spec.get("perches", []):
        out += bytes([dx & 0xFF, dy])
    for trig, arg, nxt, act in spec.get("globals", []):
        out += bytes([TRIG[trig] | (_next(nxt, names) << 5), arg,
                      0xFF if act is None else act1(*act)])
    for s in st:
        mname, param = s["motion"]
        f0, n, per = s.get("anim", (0, 1, 0))
        out += bytes([MOTION[mname] | (param << 3), f0 | ((n - 1) << 2) | (per << 4),
                      len(s.get("enter", [])) | (len(s.get("on", [])) << 4)])
        out += bytes(act1(*a) for a in s.get("enter", []))
        for trig, arg, nxt in s.get("on", []):
            out += bytes([TRIG[trig] | (_next(nxt, names) << 5), arg])
    return bytes(out)


def _next(nxt, names):
    return STAY if nxt is None else names.index(nxt)


def decode(b):
    ns, npch, ng = (b[1] & 7) + 1, (b[1] >> 3) & 3, b[1] >> 5
    p = 3
    perches = []
    for _ in range(npch):
        perches.append((b[p] - 256 if b[p] > 127 else b[p], b[p + 1]))
        p += 2
    glob = []
    for _ in range(ng):
        glob.append((b[p] & 31, b[p + 1], b[p] >> 5, None if b[p + 2] == 0xFF else b[p + 2]))
        p += 3
    states = []
    for _ in range(ns):
        m, a, n = b[p], b[p + 1], b[p + 2]
        p += 3
        enter = list(b[p:p + (n & 15)])
        p += n & 15
        on = []
        for _ in range(n >> 4):
            on.append((b[p] & 31, b[p + 1], b[p] >> 5))
            p += 2
        states.append({"motion": (m & 7, m >> 3), "anim": (a & 3, ((a >> 2) & 3) + 1, a >> 4),
                       "enter": enter, "on": on})
    assert p == len(b), (p, len(b))
    return {"preset": b[2] & 15, "perches": perches, "globals": glob, "states": states}


def check(b):
    """What the server would refuse: a bad next state, AFTER 0, an action out
    of the table, an unreachable state. [] when fine."""
    d = decode(b)
    bad = []
    n = len(d["states"])
    seen, todo = {0}, [0]
    edges = {i: [nx for _t, _a, nx in s["on"]] for i, s in enumerate(d["states"])}
    gl = [nx for _t, _a, nx, _x in d["globals"]]
    while todo:
        i = todo.pop()
        for nx in edges[i] + gl:
            if nx != STAY and nx < n and nx not in seen:
                seen.add(nx)
                todo.append(nx)
    for i, s in enumerate(d["states"]):
        for t, a, nx in s["on"]:
            if nx != STAY and nx >= n:
                bad.append("state %d: goes to state %d of %d" % (i, nx, n))
            if t == TRIG["after"] and a == 0:
                bad.append("state %d: after 0 would loop" % i)
            if t > 20:
                bad.append("state %d: trigger %d" % (i, t))
        for x in s["enter"]:
            if x >= len(dense.ACT1_TABLE):
                bad.append("state %d: action %d" % (i, x))
    for i in range(n):
        if i not in seen:
            bad.append("state %d is never reached" % i)
    return bad
