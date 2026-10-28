"""JarScript 3 on the server: the compiler, the machine, the simulated hour,
the language guide.

The semantics are docs/superpowers/specs/2026-10-14-jarscript3-design.md; the
table is apps/jar/jarvm3.def, read here at import with regular expressions
(the same lines apps/jar/jv3.h builds its tables from). The C machine
(apps/jar/jv3.h) and this one must agree byte for byte: tools/
make_jarvm3_fixtures.py runs fixture worlds here and writes traces that
apps/jar/test/test_jv3.c replays.

Public:
  compile_script(src, nframes, frame_names=None, limit=1024) -> bytes
  script_traits(src) -> [trait words]
  tier_limit(tier), TIERS
  check(script, nframes=None) -> None or the reason it would not run
  Jar (the machine), simulate(...), prepare_script(...)
  disassemble(bytes), is_v3(bytes), LANGUAGE_GUIDE_V3

Decisions where the spec was loose (each is also where the code does it):
  D1  Events: every event queued during tick T (host events between ticks
      excepted) is handled at T+1. The queues are snapshotted for every item
      at the very start of a tick, before Near -- so `near` found in step 1
      is handled next tick, like land/hit/bump/signal.
  D2  A queue entry is {ev, arg (int16), other}. hit and bump carry the
      impact (raw) in arg; their handlers have arg 0, so they still match.
      `impact` is set from arg for hit/bump, else 0; `other` from the entry.
      tick and every handlers run with other = impact = 0. A full queue (8)
      drops the new entry.
  D3  Handler match: same event, state == current or 255, and arg 0 or equal
      -- except chime: the event's arg is the minutes since open and a
      handler matches when arg % param == 0 -- and near: a near event's arg
      is the index + 1 of the near handler (in the handler table) whose
      radius saw the arrival, and only that handler runs it (while it is
      still active; else the event is dropped). So two near handlers on one
      trait with different radii each hear only their own arrivals. The
      entry is the same size: the trait is not carried (the handler has it).
  D4  Faults are detected before the instruction has any effect (stack
      needs checked first); the faulting instruction costs its step. A
      handler that starts and runs out of budget mid-way is a fault too,
      reported with the pc it stopped at. One that cannot start because the
      tick's budget is already spent (budget <= 0 when it would run) is not
      run and not a fault: its event is dropped silently (other and impact
      are not set; an every counter that reached 0 still reloads). At 8
      faults the item stops at once for the tick.
  D5  AWAIT and JZY that yield refund their own step; the condition's steps
      are spent.
  D6  wait: body sets wait = N; in step 2f, wait > 0 is decremented and the
      body runs only once it is 0, so it resumes N ticks later.
  D7  A body GO enters at once and keeps running; the 4th GO in one tick
      enters the state and ends the body's run for the tick.
  D8  Entering a state reloads only that state's own every counters (any-
      state ones keep counting) and clears that state's near masks. Only
      active every handlers count down.
  D9  An item argument (FIELD, dist, has, toward, move to ITEM, signal to)
      is handle = raw >> 4, valid when 1..24 and the slot is occupied; an
      invalid one reads 0 (dist, angleto 0; has 0; toward and signal do
      nothing; move to it goes to the ground).
  D10 Perches: `move to perch T` -> perches of other items with trait T;
      no T (or `rim`, operand 0) -> the four rim spots only (16,22) (32,22)
      (208,22) (224,22). A spot is free when no item holds it (my own is not
      free). Order: items by slot, perch index, then rim; one draw picks.
      Spot x = owner.x - w*s*8 + px*s*16, y = owner.y - h*s*16 + py*s*16
      (raw; no half-pixel). On arrival the perch is taken even if another
      item took it meanwhile.
  D11 Paths: cx = (x0+x1)>>1, cy = ((y0+y1)>>1) - 2*arc. Points by de
      Casteljau with t in /4096 and >> 12. Length: 13 points at
      t = i*4096//12, summing isqrt. duration = max(3, ticks(trunc(len*16 /
      max(80, speed)))) in 32 bits; step = ceil(4096 / dur). Ease s =
      ((t*t >> 12) * (12288 - 2t)) >> 12. A move/walk/hop sets vx = vy = 0
      and faces the target (sign of x1 - x0; 0 keeps); physics is not turned
      off but does not step while a mover runs. Target clamp: x to
      [w*s*8, 3840 - w*s*8], y to [128, ground(x)]. move to ground: x =
      14 + r % 213 px. hop: x1 = clamp(x+dx, half+32, 3840-half-32), y1 = y,
      arc = H, speed 40.
  D12 walk: per tick max(1, |speed| / 20) raw; walls at half+32 and
      3840-half-32; a solid blocks when the item's vertical span overlaps it
      (y > sy + 8 and y - hs < sy + sh) and the new span [nx-half, nx+half]
      overlaps it while the old did not; standing above the ground (|y -
      ground| > 16) on a solid's top, x leaving the top stops it. y unchanged.
  D13 Physics: radius = body*16 if declared else max(16, min(w,h)*s*8 -
      16). Order: soil, left glass, right glass, lid, then solids (items by
      slot, then ledges). Reflection by bounce: v' = -trunc(v*bounce/1600).
      Solids: closest point; inside -> normal up, cy = top - r; else pushed
      out by trunc(d*(r-dist)/dist); vn = trunc(v.n/dist); if vn < 0:
      k = trunc(vn*(1600+bounce)/1600), v -= trunc(k*n/dist); a "floor"
      normal (5*ny < -3*dist) zeroes |vy| < 192 and grounds. touching mask:
      slot bits 0..23, ledge i bit 24+i. Roll: angle += trunc(dx*163/(4r))
      where dx = trunc(vx/20). Spin (physics off): angle += trunc(spin/450).
  D14 Not physics: grounded = no perch, no mover, and |y - ground(x)| <= 16
      or on a solid's top (|y - sy| <= 16, x within the top +-16).
  D15 Anims: loop ticks wrap at 20*n; frame = frames[(ticks*fps//20) % n];
      once: ticks counts until (ticks*fps//20) >= n, then the last frame and
      done. PLAY sets frame = first entry unless the same anim is already
      looping. FRAMEE: idx = round(N) - 1; out of range does nothing at all.
      FRAME/anim entries >= nframes are refused by check (and ignored).
  D16 say: template string, 0x01 = a value goes here (values in order);
      ticks = max(6, ticks(T)); text cut to 24 after filling. log: string
      255 = the values joined by one space; log text is never cut.
      Numbers: whole -> "%d"; else hundredths rounded half away from zero,
      "%d.%02d" with one trailing zero removed, sign in front.
  D17 emit: n = clamp(N >> 4, 0, 12); the host is told only when n > 0.
  D18 x/y written by a script leave any perch (the mover keeps going).
      bounce is clamped 0..1600 raw, friction to >= 0.
  D19 statetime counts in step 3 (saturates at 65535 ticks; the sense at
      32767). The jar's tick count rises at the end of a tick, so `time` is
      0 during tick 1. ACT's argument is raw (sound pop -> 16).
  D20 round(): computed in 32 bits, then saturated (round(2047.9375) ->
      32767 raw). clamp(v, lo, hi) = lo if v < lo, else hi if v > hi, else v.
  D21 xorshift32 with state 0 replaced by 0x9E3779B9 on the first draw
      (as js_rnd does).
  D22 The item's scale is the host's (the record); the script's flag is
      only advice. Unscripted items have x, y, w, h, scale and traits only.
  D23 Stopped items (8 faults) neither run nor move; they are still traced.
  D24 angleto has its own table, ANG[k] = round(sin(2 pi k / 256) * 16384)
      (halves away from zero), not the 1/16 SIN that sin()/cos() return. dx
      and dy (raw centre differences) are clamped to -65535..65535; the
      answer is the step k 0..255 maximising dx * ANG[(k+64) & 255] + dy *
      ANG[k], ties to the lowest k, as degrees raw. Each product is under
      2^30 and the sum is at most |(dx, dy)| * 16385 < 2^31, so the C
      machine does it in int32 with no 64-bit maths.
"""

import math
import os
import re
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
DEF_PATH = os.path.join(ROOT, "apps", "jar", "jarvm3.def")

# ---- the table ----------------------------------------------------------------

_RES = {
    "OP": re.compile(r'^J3_OP\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "EVENT": re.compile(r'^J3_EVENT\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "SENSE": re.compile(r'^J3_SENSE\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "FUNC": re.compile(r'^J3_FUNC\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "FIELD": re.compile(r'^J3_FIELD\(\s*(\d+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "TARGET": re.compile(r'^J3_TARGET\(\s*(\d+)\s*,\s*(\w+)\s*\)\s*$'),
    "ACTION": re.compile(r'^J3_ACTION\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)\s*$'),
    "PARTICLE": re.compile(r'^J3_PARTICLE\(\s*(\d+)\s*,\s*(\w+)\s*\)\s*$'),
    "NAME": re.compile(r'^J3_NAME\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)\s*$'),
}


def _load_def(path=DEF_PATH):
    rows = {k: [] for k in _RES}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("J3_"):
                continue
            for k, rx in _RES.items():
                m = rx.match(line)
                if m:
                    rows[k].append(m.groups())
                    break
            else:
                raise ValueError("%s: a line this reader does not understand: %s" % (path, line))
    for k in ("OP", "EVENT", "SENSE", "FUNC", "FIELD", "TARGET", "ACTION", "PARTICLE"):
        nums = [int(r[0]) for r in rows[k]]
        if nums != list(range(len(nums))):
            raise ValueError("%s: J3_%s numbers must be 0, 1, 2 ... in order" % (path, k))
    return rows


_ROWS = _load_def()


class Op:
    def __init__(self, code, name, nbytes, doc):
        self.code, self.name, self.nbytes, self.doc = code, name, nbytes, doc
        if nbytes == 0:
            self.kinds = ()
        elif nbytes == 1:
            self.kinds = ("s8",) if name == "PUSH8" else ("u8",)
        elif "signed 16-bit" in doc:
            self.kinds = ("s16",)
        elif "u16" in doc:
            self.kinds = ("u16",)
        else:
            self.kinds = ("u8",) * nbytes

    def __repr__(self):
        return "Op(%d, %s)" % (self.code, self.name)


OPS = [Op(int(c), n, int(b), d) for c, n, b, d in _ROWS["OP"]]
OP = {o.name: o.code for o in OPS}
EVENTS = {n: int(c) for c, n, _f, _d in _ROWS["EVENT"]}
EVENT_NAME = {v: k for k, v in EVENTS.items()}
EVENT_FILTER = {n: f for _c, n, f, _d in _ROWS["EVENT"]}
EVENT_DOC = {n: d for _c, n, _f, d in _ROWS["EVENT"]}
SENSES = {n: int(c) for c, n, _k, _d in _ROWS["SENSE"]}
SENSE_NAME = {v: k for k, v in SENSES.items()}
SENSE_KIND = {int(c): k for c, _n, k, _d in _ROWS["SENSE"]}
SENSE_DOC = {n: d for _c, n, _k, d in _ROWS["SENSE"]}
FUNCS = {n: (int(c), int(lo), int(hi)) for c, n, lo, hi, _d in _ROWS["FUNC"]}
FUNC_NAME = {v[0]: k for k, v in FUNCS.items()}
FUNC_DOC = {n: d for _c, n, _l, _h, d in _ROWS["FUNC"]}
FIELDS = {n: int(c) for c, n, _d in _ROWS["FIELD"]}
FIELD_NAME = {v: k for k, v in FIELDS.items()}
TARGETS = {n: int(c) for c, n in _ROWS["TARGET"]}
TARGET_NAME = {v: k for k, v in TARGETS.items()}
ACTIONS = {n: int(c) for c, n, _a, _d in _ROWS["ACTION"]}
ACTION_NAME = {v: k for k, v in ACTIONS.items()}
ACTION_ARG = {n: a for _c, n, a, _d in _ROWS["ACTION"]}
ACTION_DOC = {n: d for _c, n, _a, d in _ROWS["ACTION"]}
PARTICLES = {n: int(c) for c, n in _ROWS["PARTICLE"]}
PARTICLE_NAME = {v: k for k, v in PARTICLES.items()}
GROUPS = {}
for _g, _w, _v in _ROWS["NAME"]:
    GROUPS.setdefault(_g, {})[_w] = int(_v)
TRAITS = GROUPS["trait"]
TRAIT_NAME = {v: k for k, v in TRAITS.items()}
NAMES = {}                       # every group word usable in an expression
for _g, _words in GROUPS.items():
    for _w, _v in _words.items():
        if NAMES.get(_w, _v) != _v:
            raise ValueError("jarvm3.def: %r means two numbers" % _w)
        NAMES[_w] = _v

FORMAT = 3
MAX_BYTES = 1024
MAX_VARS, MAX_STATES, MAX_HANDLERS, MAX_ANIMS = 8, 16, 24, 8
MAX_PERCHES, MAX_STRINGS, MAX_STR = 4, 16, 24
MAX_NEAR, MAX_EVERY, MAX_FRAMES, MAX_ANIM_LEN = 4, 6, 4, 8
STACK = 16
QUEUE = 8
SLOTS = 24
MAX_FAULTS = 8
BUDGET_FULL, BUDGET_SMALL = 100, 50
ANY = 255
LOG_NONE = 255
SLOT_MARK = 0x01
TIERS = {"none": 0, "small": 512, "full": 1024}
RIM = [(16, 22), (32, 22), (208, 22), (224, 22)]
JAR_W, JAR_H, SOIL = 240, 135, 112

CONSTS = {"left": -1, "right": 1, "true": 1, "false": 0, "none": 0}

KEYWORDS = {"traits", "var", "anim", "perch", "solid", "body", "layer", "scale", "state", "on",
            "every", "end", "if", "then", "elif", "else", "while", "do", "repeat", "times", "loop",
            "wait", "until", "go", "frame", "play", "once", "stop", "physics", "roll", "say", "log",
            "emit", "sway", "shake", "face", "toward", "move", "to", "walk", "hop", "speed", "arc",
            "signal", "return", "within", "and", "or", "not", "rim", "fps", "ground"}
RESERVED = (KEYWORDS | set(SENSES) | set(FUNCS) | set(CONSTS) | set(NAMES) | set(ACTIONS)
            | set(PARTICLES))
WRITABLE = {k for k, v in SENSE_KIND.items() if v == "rw"}

# The stack effect of each op: (pops, pushes); None = depends on operands.
_EFFECT = {
    "HALT": (0, 0), "RET": (0, 0), "PUSH8": (0, 1), "PUSH16": (0, 1), "LDV": (0, 1),
    "STV": (1, 0), "LDS": (0, 1), "STP": (1, 0), "NEG": (1, 1), "NOT": (1, 1),
    "FIELD": (1, 1), "JMP": (0, 0), "JZ": (1, 0), "JZY": (1, 0), "RPT": (1, 1), "WAIT": (1, 0),
    "AWAIT": (0, 0), "GO": (0, 0), "FRAME": (0, 0), "FRAMEE": (1, 0), "PLAY": (0, 0),
    "STOP": (0, 0), "FLAG": (0, 0), "EMIT": (1, 0), "SWAY": (2, 0), "SHAKE": (1, 0),
    "FACE": (1, 0), "TOWARD": (1, 0), "WALK": (2, 0), "HOP": (2, 0),
}
for _n in ("ADD", "SUB", "MUL", "DIV", "MOD", "EQ", "NE", "LT", "LE", "GT", "GE", "AND", "OR"):
    _EFFECT[_n] = (2, 1)
for _o in OPS:
    if _o.name not in _EFFECT and _o.name not in ("CALL", "SAY", "LOG", "MOVE", "SIGNAL", "ACT"):
        raise ValueError("jarvm3.def: no stack effect known for %s" % _o.name)


def _effect(name, a, b):
    if name == "CALL":
        return b, 1
    if name == "SAY":
        return 1 + b, 0
    if name == "LOG":
        return b, 0
    if name == "MOVE":
        return {0: 4, 1: 3}.get(a, 2), 0
    if name in ("SIGNAL", "ACT"):
        return (1 if b else 0), 0
    return _EFFECT[name]


# ---- numbers --------------------------------------------------------------------

def sat(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def s16(v):
    return ((v + 0x8000) & 0xFFFF) - 0x8000


def tdiv(a, b):
    """C's division: toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def rha(fr):
    """Round a Fraction (or int) half away from zero."""
    fr = Fraction(fr)
    n = abs(fr)
    r = int(n + Fraction(1, 2))         # floor(n + 1/2)
    return r if fr >= 0 else -r


def ticks_of(raw):
    """Seconds (raw) to ticks: (raw * 20 + 8) >> 4, floored."""
    return (raw * 20 + 8) >> 4


def isqrt32(v):
    return math.isqrt(v & 0xFFFFFFFF)


def _sin_table():
    out = []
    for k in range(256):
        v = math.sin(2 * math.pi * k / 256) * 16
        out.append(int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1))
    return out


SIN = _sin_table()


def _ang_table():
    out = []
    for k in range(256):
        v = math.sin(2 * math.pi * k / 256) * 16384
        out.append(int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1))
    return out


ANG = _ang_table()   # angleto's table (D24); sin()/cos() keep SIN


def deg_to_step(raw):
    return (raw * 256 // 5760) % 256


def step_to_deg(step):
    return (step * 45) >> 1


def fmt_num(raw):
    if raw % 16 == 0:
        return "%d" % (raw // 16)
    h = (abs(raw) * 25 + 2) // 4                 # hundredths, half away from zero
    s = "%d.%02d" % (h // 100, h % 100)
    if s.endswith("0"):
        s = s[:-1]
    return ("-" if raw < 0 else "") + s


def fnv_signal(name):
    h = 2166136261
    for c in name.encode("ascii"):
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h % 255 + 1


def tier_limit(tier):
    if tier not in TIERS:
        raise ValueError("no script tier %r (tiers: %s)" % (tier, ", ".join(TIERS)))
    return TIERS[tier]


def is_v3(script):
    return bool(script) and script[0] == FORMAT


# ---- errors and the lexer -------------------------------------------------------

class ScriptError(Exception):
    """A script that cannot be delivered. `line` is 1-based, 0 for the whole
    script; str() is the message to hand back to whoever wrote it."""

    def __init__(self, msg, line=0):
        self.msg, self.line = msg, line
        super().__init__(("line %d: %s" % (line, msg)) if line else msg)


class Tok:
    __slots__ = ("t", "v", "line")

    def __init__(self, t, v, line):
        self.t, self.v, self.line = t, v, line


def _lex(src):
    toks = []
    lines = src.split("\n")
    for ln, s in enumerate(lines):
        L = ln + 1
        i = 0
        while i < len(s):
            c = s[i]
            if c in " \t\r":
                i += 1
                continue
            if c == "#":
                break
            nxt = s[i + 1] if i + 1 < len(s) else ""
            if c.isdigit() or (c == "." and nxt.isdigit()):
                j = i
                while j < len(s) and (s[j].isdigit() or s[j] == "."):
                    j += 1
                txt = s[i:j]
                if txt.count(".") > 1:
                    raise ScriptError("'%s' is not a number" % txt, L)
                v = Fraction(txt)
                after = s[j:j + 3]
                if after.startswith("ms") and not (after[2:3].isalnum() or after[2:3] == "_"):
                    v /= 1000
                    j += 2
                elif after.startswith("s") and not (after[1:2].isalnum() or after[1:2] == "_"):
                    j += 1
                elif after[:1].isalpha() or after[:1] == "_":
                    w = re.match(r"[\w.]+", s[i:]).group(0)
                    raise ScriptError("'%s' -- a number can only end in s or ms" % w, L)
                toks.append(Tok("num", v, L))
                i = j
                continue
            if c.isalpha() or c == "_":
                j = i
                while j < len(s) and (s[j].isalnum() or s[j] == "_"):
                    j += 1
                toks.append(Tok("id", s[i:j], L))
                i = j
                continue
            if c == '"':
                j = i + 1
                while j < len(s) and s[j] != '"':
                    j += 1
                if j >= len(s):
                    raise ScriptError('this text has no closing "', L)
                toks.append(Tok("str", s[i + 1:j], L))
                i = j + 1
                continue
            two = s[i:i + 2]
            if two in ("==", "!=", "<=", ">=", "+=", "-=", "*=", "/="):
                toks.append(Tok("op", two, L))
                i += 2
                continue
            if c in "+-*/%<>=(),:;.@":
                toks.append(Tok("op", c, L))
                i += 1
                continue
            raise ScriptError("unexpected character '%s'" % c, L)
        toks.append(Tok("nl", None, L))
    toks.append(Tok("eof", None, len(lines)))
    return toks


# ---- the parser (a port of apps/jar/tools/jarsandbox/jarscript3.js) ----------------------

def _desc(k):
    if k.t == "nl":
        return "the end of the line"
    if k.t == "eof":
        return "the end of the script"
    if k.t == "str":
        return '"%s"' % k.v
    if k.t == "num":
        return "'%s'" % _numtxt(k.v)
    return "'%s'" % k.v


def _numtxt(v):
    return str(v.numerator) if v.denominator == 1 else str(float(v))


class _Parser:
    def __init__(self, src):
        self.T = _lex(src)
        self.p = 0
        self.prog = {"traits": [], "vars": [], "anims": {}, "perches": [], "solid": None,
                     "body": None, "layer": None, "scale": 1, "states": [], "handlers": []}

    # tokens
    def k(self):
        return self.T[self.p]

    def is_(self, t, v=None):
        k = self.T[self.p]
        return k.t == t and (v is None or k.v == v)

    def isid(self, v=None):
        return self.is_("id", v)

    def isop(self, v=None):
        return self.is_("op", v)

    def fail(self, msg, k=None):
        raise ScriptError(msg, (k or self.T[self.p]).line)

    def expect(self, t, v=None, what=None):
        if not self.is_(t, v):
            self.fail("expected %s, found %s" % (what or ("'%s'" % v if v else t), _desc(self.k())))
        tok = self.T[self.p]
        self.p += 1
        return tok

    def skip_sep(self):
        while self.is_("nl") or self.isop(";"):
            self.p += 1

    def at_end(self):
        return (self.is_("nl") or self.isop(";") or self.is_("eof") or self.isid("end")
                or self.isid("else") or self.isid("elif"))

    def end_stmt(self):
        if self.is_("nl") or self.isop(";"):
            self.p += 1
            return
        if self.at_end():
            return
        self.fail("unexpected %s -- the statement should end here" % _desc(self.k()))

    def num_lit(self):
        neg = False
        if self.isop("-"):
            neg = True
            self.p += 1
        k = self.expect("num", None, "a number")
        return (-k.v if neg else k.v), k

    # expressions
    def expr(self):
        return self.e_or()

    def e_or(self):
        l = self.e_and()
        while self.isid("or"):
            line = self.T[self.p].line
            self.p += 1
            l = {"k": "bin", "op": "or", "l": l, "r": self.e_and(), "line": line}
        return l

    def e_and(self):
        l = self.e_not()
        while self.isid("and"):
            line = self.T[self.p].line
            self.p += 1
            l = {"k": "bin", "op": "and", "l": l, "r": self.e_not(), "line": line}
        return l

    def e_not(self):
        if self.isid("not"):
            line = self.T[self.p].line
            self.p += 1
            return {"k": "not", "e": self.e_not(), "line": line}
        return self.e_cmp()

    def e_cmp(self):
        l = self.e_add()
        if self.is_("op") and self.k().v in ("==", "!=", "<", "<=", ">", ">="):
            k = self.T[self.p]
            self.p += 1
            l = {"k": "bin", "op": k.v, "l": l, "r": self.e_add(), "line": k.line}
        elif self.isop("="):
            self.fail("'=' sets a value; to compare, use '=='")
        return l

    def e_add(self):
        l = self.e_mul()
        while self.isop("+") or self.isop("-"):
            k = self.T[self.p]
            self.p += 1
            l = {"k": "bin", "op": k.v, "l": l, "r": self.e_mul(), "line": k.line}
        return l

    def e_mul(self):
        l = self.e_un()
        while self.isop("*") or self.isop("/") or self.isop("%"):
            k = self.T[self.p]
            self.p += 1
            l = {"k": "bin", "op": k.v, "l": l, "r": self.e_un(), "line": k.line}
        return l

    def e_un(self):
        if self.isop("-"):
            line = self.T[self.p].line
            self.p += 1
            return {"k": "neg", "e": self.e_un(), "line": line}
        return self.e_post()

    def e_post(self):
        e = self.e_prim()
        while self.isop("."):
            self.p += 1
            f = self.expect("id", None, 'a field name after "."')
            e = {"k": "field", "e": e, "f": f.v, "line": f.line}
        return e

    def e_prim(self):
        k = self.k()
        if k.t == "num":
            self.p += 1
            return {"k": "num", "v": k.v, "line": k.line}
        if k.t == "str":
            self.p += 1
            return {"k": "str", "v": k.v, "line": k.line}
        if self.isop("("):
            self.p += 1
            e = self.expr()
            self.expect("op", ")", "')'")
            return e
        if k.t == "id":
            if k.v in ("end", "then", "else", "elif", "do"):
                self.fail("expected a value, found %s" % _desc(k))
            self.p += 1
            if self.isop("("):
                self.p += 1
                args = []
                if not self.isop(")"):
                    args.append(self.expr())
                    while self.isop(","):
                        self.p += 1
                        args.append(self.expr())
                self.expect("op", ")", "')' to close %s(" % k.v)
                return {"k": "call", "name": k.v, "args": args, "line": k.line}
            return {"k": "id", "v": k.v, "line": k.line}
        self.fail("expected a value, found %s" % _desc(k))

    # statements
    def then_tok(self):
        if self.isid("then") or self.isop(":"):
            self.p += 1
        elif not self.is_("nl"):
            self.fail("expected 'then' or ':' after the condition, found %s" % _desc(self.k()))

    def opt(self):
        return None if self.at_end() else self.expr()

    def close_end(self, what, line):
        if not self.isid("end"):
            self.fail("expected 'end' to close the %s from line %d, found %s"
                      % (what, line, _desc(self.k())))
        self.p += 1
        self.end_stmt()

    def block(self, open_line, what, st=None):
        out = []
        while True:
            self.skip_sep()
            if self.is_("eof"):
                raise ScriptError("the %s that starts on line %d has no 'end' -- or an if/loop "
                                  "inside it is missing its own" % (what, open_line), open_line)
            if self.isid("end") or self.isid("else") or self.isid("elif"):
                break
            if st is not None and (self.isid("on") or self.isid("every")):
                st["handlers"].append(self.handler())
                continue
            if st is not None and self.isid("state"):
                self.fail("state '%s' (line %d) needs an 'end' before the next state"
                          % (st["name"], open_line))
            out.append(self.stmt())
        return out

    def handler(self):
        k = self.T[self.p]
        self.p += 1
        h = {"ev": None, "word": None, "within": None, "every": None, "chime": None,
             "line": k.line}
        if k.v == "every":
            h["ev"] = "every"
            h["every"], _ = self.num_lit()
        else:
            e = self.expect("id", None, "an event name")
            ev = e.v
            if ev not in EVENTS:
                self.fail("'%s' is not an event -- events are %s" % (ev, ", ".join(EVENTS)), e)
            h["ev"] = ev
            filt = EVENT_FILTER[ev]
            if filt == "every":
                h["every"], _ = self.num_lit()
            elif filt == "signal":
                h["word"] = self.expect("id", None, "the signal name, e.g. on signal landed:").v
            elif filt == "near":
                t = self.expect("id", None, "a trait, e.g. on near bird:")
                h["word"] = t.v
                h["wline"] = t.line
                if self.isid("within"):
                    self.p += 1
                    h["within"], _ = self.num_lit()
            elif filt == "chime":
                if self.is_("num"):
                    h["chime"], _ = self.num_lit()
            elif filt in GROUPS:
                if self.isid() and not self.isid("end"):
                    t = self.T[self.p]
                    if t.v not in GROUPS[filt]:
                        self.fail("'%s' is not a %s -- try %s" % (t.v, filt, ", ".join(GROUPS[filt])), t)
                    h["word"] = t.v
                    self.p += 1
        if h["every"] is not None and h["every"] <= 0:
            self.fail("every needs a time above zero", k)
        if self.isop(":"):
            self.p += 1
        what = "'on %s' handler" % h["ev"]
        h["body"] = self.block(k.line, what)
        self.close_end(what, k.line)
        return h

    def stmt(self):
        k = self.k()
        line = k.line
        if k.t != "id":
            self.fail("a statement can't start with %s" % _desc(k))
        w = k.v
        nx = self.T[self.p + 1]
        if nx.t == "op" and nx.v in ("=", "+=", "-=", "*=", "/="):
            self.p += 2
            e = self.expr()
            self.end_stmt()
            return {"k": "assign", "name": w, "op": nx.v, "e": e, "line": line}
        self.p += 1
        s = None
        if w == "if":
            arms = []
            c = self.expr()
            self.then_tok()
            arms.append((c, self.block(line, "if")))
            els = None
            while True:
                if self.isid("elif"):
                    self.p += 1
                    c = self.expr()
                    self.then_tok()
                    arms.append((c, self.block(line, "if")))
                elif self.isid("else"):
                    self.p += 1
                    if self.isop(":"):
                        self.p += 1
                    els = self.block(line, "if")
                    break
                else:
                    break
            self.close_end("if", line)
            return {"k": "if", "arms": arms, "els": els, "line": line}
        if w == "while":
            c = self.expr()
            if self.isop(":") or self.isid("do"):
                self.p += 1
            b = self.block(line, "while")
            self.close_end("while", line)
            return {"k": "while", "c": c, "b": b, "line": line}
        if w == "repeat":
            n = self.expr()
            if self.isid("times"):
                self.p += 1
            if self.isop(":"):
                self.p += 1
            b = self.block(line, "repeat")
            self.close_end("repeat", line)
            return {"k": "repeat", "n": n, "b": b, "line": line}
        if w == "loop":
            if self.isop(":"):
                self.p += 1
            b = self.block(line, "loop")
            self.close_end("loop", line)
            return {"k": "loop", "b": b, "line": line}
        if w == "wait":
            if self.isid("until"):
                self.p += 1
                s = {"k": "waituntil", "c": self.expr()}
            else:
                s = {"k": "wait", "e": self.opt()}
        elif w == "go":
            s = {"k": "go", "name": self.expect("id", None, "a state name after go").v}
        elif w == "frame":
            s = {"k": "frame", "e": self.expr()}
        elif w == "play":
            n = self.expect("id", None, "an anim or frame name").v
            once = False
            if self.isid("once"):
                self.p += 1
                once = True
            elif self.isid("loop"):
                self.p += 1
            s = {"k": "play", "name": n, "once": once}
        elif w == "stop":
            s = {"k": "stop"}
        elif w in ("physics", "roll"):
            v = self.expect("id", None, "'on' or 'off'").v
            if v not in ("on", "off", "true", "false"):
                self.fail("%s takes 'on' or 'off', not '%s'" % (w, v))
            s = {"k": "flag", "name": w, "v": 1 if v in ("on", "true") else 0}
        elif w == "say":
            s = {"k": "say", "e": self.expr(), "d": self.opt()}
        elif w == "log":
            es = [self.expr()]
            while self.isop(","):
                self.p += 1
                es.append(self.expr())
            s = {"k": "log", "es": es}
        elif w == "emit":
            kind = self.expect("id", None, "a particle kind").v
            if kind not in PARTICLES:
                self.fail("'%s' is not a particle -- try %s" % (kind, ", ".join(PARTICLES)))
            s = {"k": "emit", "kind": kind, "n": self.opt()}
        elif w == "sway":
            s = {"k": "sway", "a": self.expr(), "d": self.opt()}
        elif w == "shake":
            s = {"k": "shake", "d": self.opt()}
        elif w == "face":
            if self.isid("toward"):
                self.p += 1
                s = {"k": "toward", "e": self.expr()}
            else:
                s = {"k": "face", "e": self.expr()}
        elif w == "move":
            self.expect("id", "to", "'to' after move")
            if self.isid("perch"):
                self.p += 1
                trait = None
                if self.is_("id") and self.k().v not in ("speed", "arc"):
                    trait = self.T[self.p]
                    self.p += 1
                tgt = {"k": "perch", "trait": trait}
            elif self.isid("ground"):
                self.p += 1
                tgt = {"k": "ground"}
            else:
                a = self.expr()
                if self.isop(","):
                    self.p += 1
                    tgt = {"k": "xy", "a": a, "b": self.expr()}
                else:
                    tgt = {"k": "item", "a": a}
            speed = arc = None
            while self.isid("speed") or self.isid("arc"):
                o = self.T[self.p].v
                self.p += 1
                if o == "speed":
                    speed = self.expr()
                else:
                    arc = self.expr()
            s = {"k": "move", "tgt": tgt, "speed": speed, "arc": arc}
        elif w == "walk":
            dx = self.expr()
            speed = None
            if self.isid("speed"):
                self.p += 1
                speed = self.expr()
            s = {"k": "walk", "dx": dx, "speed": speed}
        elif w == "hop":
            dx = self.opt()
            h = self.opt() if dx is not None else None
            s = {"k": "hop", "dx": dx, "h": h}
        elif w == "signal":
            n = self.expect("id", None, "a signal name").v
            to = None
            if self.isid("to"):
                self.p += 1
                to = self.expr()
            s = {"k": "signal", "name": n, "to": to}
        elif w == "return":
            s = {"k": "return"}
        elif w in ("gravity", "bounce", "friction", "spin"):
            s = {"k": "assign", "name": w, "op": "=", "e": self.expr()}
        elif w in ACTIONS:
            kind = ACTION_ARG[w]
            if kind == "none":
                s = {"k": "act", "name": w, "arg": None}
            elif kind == "opt":
                s = {"k": "act", "name": w, "arg": self.opt()}
            else:
                t = self.expect("id", None, "a %s name" % kind)
                if t.v not in GROUPS[kind]:
                    self.fail("'%s' is not a %s -- try %s" % (t.v, kind, ", ".join(GROUPS[kind])), t)
                s = {"k": "act", "name": w, "arg": {"k": "num", "v": Fraction(GROUPS[kind][t.v]),
                                                    "line": t.line}}
        else:
            if w == "state":
                self.fail("'state' only starts a new state at the top level -- is an 'end' "
                          "missing above?", k)
            if w == "on":
                self.fail("handlers ('on ...') go at the top level or directly inside a state, "
                          "not inside another block", k)
            hint = ""
            if w in SENSES or any(v["name"] == w for v in self.prog["vars"]):
                hint = " -- did you mean '%s = ...'?" % w
            self.fail("'%s' is not a statement%s" % (w, hint), k)
        s["line"] = line
        self.end_stmt()
        return s

    # top level
    def script(self):
        P = self.prog
        while True:
            self.skip_sep()
            if self.is_("eof"):
                break
            k = self.k()
            if k.t != "id":
                self.fail("expected traits, var, anim, perch, state or on at the top level, "
                          "found %s" % _desc(k))
            v = k.v
            if v == "traits":
                self.p += 1
                while not self.at_end():
                    if self.isop(","):
                        self.p += 1
                        continue
                    t = self.expect("id", None, "a trait word")
                    if t.v not in TRAITS:
                        self.fail("'%s' is not a trait -- traits are %s" % (t.v, ", ".join(TRAITS)), t)
                    if t.v not in P["traits"]:
                        P["traits"].append(t.v)
                self.end_stmt()
            elif v == "var":
                self.p += 1
                while True:
                    n = self.expect("id", None, "a variable name")
                    init = Fraction(0)
                    if self.isop("="):
                        self.p += 1
                        init, _ = self.num_lit()
                    P["vars"].append({"name": n.v, "init": init, "line": n.line})
                    if self.isop(","):
                        self.p += 1
                        continue
                    break
                self.end_stmt()
            elif v == "anim":
                self.p += 1
                n = self.expect("id", None, "an anim name")
                self.expect("op", "=", "'=' then frame names")
                fr = []
                while not self.at_end() and not self.isop("@"):
                    if self.isop(","):
                        self.p += 1
                        continue
                    f = self.T[self.p]
                    self.p += 1
                    if f.t not in ("id", "num"):
                        self.fail("expected a frame name, found %s" % _desc(f), f)
                    fr.append(f)
                fps = Fraction(8)
                if self.isop("@"):
                    self.p += 1
                    fps, _ = self.num_lit()
                    if self.isid("fps"):
                        self.p += 1
                if not fr:
                    self.fail("anim '%s' lists no frames" % n.v, n)
                if n.v in P["anims"]:
                    self.fail("anim '%s' is declared twice" % n.v, n)
                P["anims"][n.v] = {"frames": fr, "fps": fps, "line": n.line}
                self.end_stmt()
            elif v == "perch":
                self.p += 1
                x, _ = self.num_lit()
                self.expect("op", ",", "',' between x and y")
                y, _ = self.num_lit()
                P["perches"].append({"x": x, "y": y, "line": k.line})
                self.end_stmt()
            elif v == "solid":
                self.p += 1
                a = [self.num_lit()[0]]
                for _i in range(3):
                    self.expect("op", ",", "solid x, y, w, h")
                    a.append(self.num_lit()[0])
                P["solid"] = (a, k.line)
                self.end_stmt()
            elif v == "body":
                self.p += 1
                P["body"] = (self.num_lit()[0], k.line)
                self.end_stmt()
            elif v == "layer":
                self.p += 1
                P["layer"] = (self.num_lit()[0], k.line)
                self.end_stmt()
            elif v == "scale":
                self.p += 1
                sc, _ = self.num_lit()
                if sc not in (1, 2):
                    self.fail("scale is 1 or 2", k)
                P["scale"] = int(sc)
                self.end_stmt()
            elif v == "state":
                self.p += 1
                n = self.expect("id", None, "a state name")
                if self.isop(":"):
                    self.p += 1
                st = {"name": n.v, "line": n.line, "body": [], "handlers": []}
                st["body"] = self.block(n.line, "state '%s'" % n.v, st)
                self.close_end("state '%s'" % n.v, n.line)
                P["states"].append(st)
            elif v in ("on", "every"):
                P["handlers"].append(self.handler())
            else:
                self.fail("'%s' can't start a line at the top level -- expected traits, var, anim, "
                          "perch, solid, body, layer, scale, state or on" % v)
        return P


def script_traits(src):
    """The traits a script declares (`traits a, b`), in order."""
    return list(_Parser(src).script()["traits"])


# ---- the compiler -----------------------------------------------------------------

def _whole(v, what, line, lo, hi):
    if Fraction(v).denominator != 1:
        raise ScriptError("%s takes a whole number, not %s" % (what, _numtxt(Fraction(v))), line)
    v = int(v)
    if not lo <= v <= hi:
        raise ScriptError("%s should be %d..%d, not %d" % (what, lo, hi, v), line)
    return v


def _raw(v, line):
    r = rha(Fraction(v) * 16)
    if not -32768 <= r <= 32767:
        raise ScriptError("%s is out of range: numbers are -2048 .. 2047.9375" % _numtxt(Fraction(v)), line)
    return r


class _Gen:
    """Code for one body or handler, into a shared buffer; jumps are code-
    relative until the end, when the base is added."""

    def __init__(self, c, buf, relocs):
        self.c, self.buf, self.relocs = c, buf, relocs
        self.depth = 0
        self.thread = False

    def at(self):
        return len(self.buf)

    def emit(self, name, *ops, line=0):
        o = OPS[OP[name]]
        pops, pushes = _effect(name, ops[0] if ops else 0, ops[1] if len(ops) > 1 else 0)
        self.depth -= pops
        self.depth += pushes
        if self.depth > self.c.maxdepth:
            self.c.maxdepth = self.depth
            self.c.maxline = line
        start = len(self.buf)
        self.buf.append(o.code)
        vals = list(ops)
        for kind in o.kinds:
            v = vals.pop(0)
            if kind == "s8":
                self.buf.append(v & 255)
            elif kind == "u8":
                self.buf.append(v & 255)
            else:
                if kind == "u16":
                    self.relocs.append(len(self.buf))
                self.buf += bytes([v & 255, (v >> 8) & 255])
        return start

    def patch(self, at, target):
        self.buf[at + 1] = target & 255
        self.buf[at + 2] = (target >> 8) & 255

    def push(self, raw):
        if raw % 16 == 0 and -128 <= raw // 16 <= 127:
            self.emit("PUSH8", raw // 16)
        else:
            self.emit("PUSH16", raw & 0xFFFF)

    # expressions
    def ex(self, e):
        c = self.c
        k = e["k"]
        L = e["line"]
        if k == "num":
            self.push(_raw(e["v"], L))
        elif k == "str":
            raise ScriptError('text ("%s") can only be said or logged: say "..." / log "..."'
                              % e["v"], L)
        elif k == "id":
            v = e["v"]
            if v in c.state_idx:
                self.push(c.state_idx[v] * 16)
            elif v in c.var_idx:
                self.emit("LDV", c.var_idx[v], line=L)
            elif v in CONSTS:
                self.push(CONSTS[v] * 16)
            elif v in NAMES:
                self.push(NAMES[v] * 16)
            elif v in SENSES:
                self.emit("LDS", SENSES[v], line=L)
            elif v in c.frame_names:
                raise ScriptError("'%s' is a frame -- use it after 'frame' or 'play'" % v, L)
            else:
                raise ScriptError("unknown name '%s' -- not a variable, sense, state or name "
                                  "(declare it with 'var %s')" % (v, v), L)
        elif k == "bin":
            self.ex(e["l"])
            self.ex(e["r"])
            self.emit(_BINOP[e["op"]], line=L)
        elif k == "neg":
            if e["e"]["k"] == "num":
                self.push(_raw(-e["e"]["v"], L))
            else:
                self.ex(e["e"])
                self.emit("NEG", line=L)
        elif k == "not":
            self.ex(e["e"])
            self.emit("NOT", line=L)
        elif k == "field":
            if e["f"] not in FIELDS:
                raise ScriptError("'.%s' is not a field -- items have %s"
                                  % (e["f"], " ".join("." + f for f in FIELDS)), L)
            self.ex(e["e"])
            self.emit("FIELD", FIELDS[e["f"]], line=L)
        elif k == "call":
            name = e["name"]
            if name not in FUNCS:
                raise ScriptError("'%s()' is not a function -- try %s" % (name, ", ".join(FUNCS)), L)
            num, lo, hi = FUNCS[name]
            n = len(e["args"])
            if not lo <= n <= hi:
                want = str(lo) if lo == hi else "%d to %d" % (lo, hi)
                raise ScriptError("%s() takes %s value%s, got %d" % (name, want, "s" if hi > 1 else "", n), L)
            trait_at = {"find": 0, "count": 0, "has": 1}.get(name)
            for i, a in enumerate(e["args"]):
                if i == trait_at and a["k"] == "id" and a["v"] not in c.var_idx:
                    if a["v"] not in TRAITS:
                        raise ScriptError("'%s' is not a trait -- traits are %s"
                                          % (a["v"], ", ".join(TRAITS)), a["line"])
                    self.push(TRAITS[a["v"]] * 16)
                else:
                    self.ex(a)
            self.emit("CALL", num, n, line=L)
        else:
            raise ScriptError("cannot compile this expression", L)

    def need_thread(self, s, what):
        if not self.thread:
            raise ScriptError("'%s' takes time, so it only works in a state's body -- a handler "
                              "must finish within one tick. Put it in a state and 'go' there." % what,
                              s["line"])

    def template(self, exprs, sep):
        """say/log text: (string index or None, values). A string literal
        anywhere in a `+` chain makes the chain text."""
        parts = []
        for i, e in enumerate(exprs):
            if i:
                parts.append(sep)
            parts += _flatten(e)
        if not any(isinstance(p, str) and p != sep for p in parts) and sep == " ":
            for p in parts:
                if not isinstance(p, str):
                    self.ex(p)
            return None, sum(1 for p in parts if not isinstance(p, str))
        text = ""
        n = 0
        for p in parts:
            if isinstance(p, str):
                text += p
            else:
                self.ex(p)
                text += chr(SLOT_MARK)
                n += 1
        line = exprs[0]["line"]
        for ch in text:
            if ch != chr(SLOT_MARK) and not 32 <= ord(ch) <= 126:
                raise ScriptError("text may use plain ASCII only, not %r" % ch, line)
        if len(text) > MAX_STR:
            raise ScriptError('the text "%s" is %d characters with its values; at most %d'
                              % (text.replace(chr(SLOT_MARK), "#"), len(text), MAX_STR), line)
        return self.c.string(text.encode("ascii"), line), n

    # statements
    def stmts(self, body):
        for s in body:
            self.st(s)

    def st(self, s):
        c = self.c
        k = s["k"]
        L = s["line"]
        if k == "assign":
            name = s["name"]
            is_var = name in c.var_idx
            if not is_var and (name not in SENSES or SENSES[name] not in WRITABLE):
                if name in SENSES:
                    raise ScriptError("'%s' can be read but not set" % name, L)
                raise ScriptError("unknown variable '%s' -- declare it at the top with 'var %s'"
                                  % (name, name), L)
            if s["op"] != "=":
                if is_var:
                    self.emit("LDV", c.var_idx[name], line=L)
                else:
                    self.emit("LDS", SENSES[name], line=L)
            self.ex(s["e"])
            if s["op"] != "=":
                self.emit(_BINOP[s["op"][0]], line=L)
            if is_var:
                self.emit("STV", c.var_idx[name], line=L)
            else:
                self.emit("STP", SENSES[name], line=L)
        elif k == "if":
            ends = []
            for cond, body in s["arms"]:
                self.ex(cond)
                jz = self.emit("JZ", 0, line=L)
                self.stmts(body)
                ends.append(self.emit("JMP", 0, line=L))
                self.patch(jz, self.at())
            if s["els"]:
                self.stmts(s["els"])
            for j in ends:
                self.patch(j, self.at())
        elif k == "while":
            top = self.at()
            self.ex(s["c"])
            jz = self.emit("JZ", 0, line=L)
            self.stmts(s["b"])
            self.emit("JMP", top, line=L)
            self.patch(jz, self.at())
        elif k == "repeat":
            self.ex(s["n"])
            top = self.emit("RPT", 0, line=L)
            self.stmts(s["b"])
            self.emit("JMP", top, line=L)
            self.patch(top, self.at())
            self.depth -= 1                          # RPT pops the count on the way out
        elif k == "loop":
            top = self.at()
            self.stmts(s["b"])
            self.emit("JMP", top, line=L)
        elif k == "wait":
            self.need_thread(s, "wait")
            if s["e"] is not None:
                self.ex(s["e"])
            else:
                self.push(0)
            self.emit("WAIT", line=L)
        elif k == "waituntil":
            self.need_thread(s, "wait until")
            top = self.at()
            self.ex(s["c"])
            self.emit("JZY", top, line=L)
        elif k == "go":
            if s["name"] not in c.state_idx:
                raise ScriptError("there is no state '%s' (states: %s)"
                                  % (s["name"], ", ".join(c.state_names)), L)
            self.emit("GO", c.state_idx[s["name"]], line=L)
        elif k == "frame":
            e = s["e"]
            if e["k"] == "id" and e["v"] in c.frame_names:
                self.emit("FRAME", c.frame_names.index(e["v"]), line=L)
            elif e["k"] == "num":
                n = _whole(e["v"], "frame", L, 1, c.nframes)
                self.emit("FRAME", n - 1, line=L)
            elif e["k"] == "id" and e["v"] not in c.var_idx and e["v"] not in SENSES:
                raise ScriptError("no frame called '%s' (frames: %s)"
                                  % (e["v"], ", ".join(c.frame_names) or "1..%d" % c.nframes), L)
            else:
                self.ex(e)
                self.emit("FRAMEE", line=L)
        elif k == "play":
            name = s["name"]
            if name in c.anim_idx:
                self.emit("PLAY", c.anim_idx[name], 1 if s["once"] else 0, line=L)
                if s["once"] and self.thread:
                    self.emit("AWAIT", line=L)
            elif name in c.frame_names:
                self.emit("FRAME", c.frame_names.index(name), line=L)
            else:
                raise ScriptError("no anim or frame called '%s' -- declare it with 'anim %s = "
                                  "frame1 frame2 @ 8'" % (name, name), L)
        elif k == "stop":
            self.emit("STOP", line=L)
        elif k == "flag":
            self.emit("FLAG", 0 if s["name"] == "physics" else 1, s["v"], line=L)
        elif k == "say":
            idx, n = self.template([s["e"]], "")
            if s["d"] is not None:
                self.ex(s["d"])
            else:
                self.push(32)
            self.emit("SAY", idx, n, line=L)
        elif k == "log":
            idx, n = self.template(s["es"], " ")
            self.emit("LOG", LOG_NONE if idx is None else idx, n, line=L)
        elif k == "emit":
            if s["n"] is not None:
                self.ex(s["n"])
            else:
                self.push(16)
            self.emit("EMIT", PARTICLES[s["kind"]], line=L)
        elif k == "sway":
            self.ex(s["a"])
            if s["d"] is not None:
                self.ex(s["d"])
            else:
                self.push(16)
            self.emit("SWAY", line=L)
        elif k == "shake":
            if s["d"] is not None:
                self.ex(s["d"])
            else:
                self.push(_raw(Fraction(2, 5), L))
            self.emit("SHAKE", line=L)
        elif k == "face":
            self.ex(s["e"])
            self.emit("FACE", line=L)
        elif k == "toward":
            self.ex(s["e"])
            self.emit("TOWARD", line=L)
        elif k == "move":
            self.need_thread(s, "move")
            t = s["tgt"]
            trait1 = 0
            if t["k"] == "xy":
                self.ex(t["a"])
                self.ex(t["b"])
            elif t["k"] == "item":
                self.ex(t["a"])
            elif t["k"] == "perch" and t["trait"] is not None:
                tw = t["trait"]
                if tw.v == "rim":
                    trait1 = 0
                elif tw.v in TRAITS:
                    trait1 = TRAITS[tw.v] + 1
                else:
                    raise ScriptError("'%s' is not a trait (or rim) -- traits are %s"
                                      % (tw.v, ", ".join(TRAITS)), tw.line)
            if s["speed"] is not None:
                self.ex(s["speed"])
            else:
                self.push(60 * 16)
            if s["arc"] is not None:
                self.ex(s["arc"])
            else:
                self.push(0)
            self.emit("MOVE", TARGETS[t["k"]], trait1, line=L)
            self.emit("AWAIT", line=L)
        elif k == "walk":
            self.need_thread(s, "walk")
            self.ex(s["dx"])
            if s["speed"] is not None:
                self.ex(s["speed"])
            else:
                self.push(10 * 16)
            self.emit("WALK", line=L)
            self.emit("AWAIT", line=L)
        elif k == "hop":
            self.need_thread(s, "hop")
            if s["dx"] is not None:
                self.ex(s["dx"])
            else:
                self.push(0)
            if s["h"] is not None:
                self.ex(s["h"])
            else:
                self.push(5 * 16)
            self.emit("HOP", line=L)
            self.emit("AWAIT", line=L)
        elif k == "signal":
            if s["to"] is not None:
                self.ex(s["to"])
            self.emit("SIGNAL", fnv_signal(s["name"]), 1 if s["to"] is not None else 0, line=L)
        elif k == "return":
            self.emit("RET", line=L)
        elif k == "act":
            if s["arg"] is not None:
                self.ex(s["arg"])
            self.emit("ACT", ACTIONS[s["name"]], 1 if s["arg"] is not None else 0, line=L)
        else:
            raise ScriptError("cannot compile this statement", L)


_BINOP = {"+": "ADD", "-": "SUB", "*": "MUL", "/": "DIV", "%": "MOD", "==": "EQ", "!=": "NE",
          "<": "LT", "<=": "LE", ">": "GT", ">=": "GE", "and": "AND", "or": "OR"}


def _is_text(e):
    return e["k"] == "str" or (e["k"] == "bin" and e["op"] == "+" and
                               (_is_text(e["l"]) or e["r"]["k"] == "str"))


def _flatten(e):
    """A say/log expression as parts: str for text, an expression for a value.
    Left-associative like JavaScript: "a" + 1 + 2 is "a12", 1 + 2 + "a" is "3a"."""
    if e["k"] == "str":
        return [e["v"]]
    if _is_text(e):
        r = e["r"]
        return _flatten(e["l"]) + ([r["v"]] if r["k"] == "str" else [r])
    return [e]


class _Compiler:
    def __init__(self, nframes, frame_names):
        self.nframes = nframes
        self.frame_names = list(frame_names or [])
        self.strings = []
        self.maxdepth = 0
        self.maxline = 0

    def string(self, b, line):
        if b in self.strings:
            return self.strings.index(b)
        if len(self.strings) >= MAX_STRINGS:
            raise ScriptError("more than %d different texts in say and log" % MAX_STRINGS, line)
        self.strings.append(b)
        return len(self.strings) - 1


def compile_script(src, nframes, frame_names=None, limit=MAX_BYTES, check_stack=True):
    """The readable language into JarScript 3 bytes, or ScriptError(msg, line).
    `nframes` is the item's picture count (1..4); `frame_names` optionally
    names them. `limit` is the tier's byte limit. `check_stack=False` lets
    an expression deeper than the machine's stack through (fixtures only)."""
    if not 1 <= nframes <= MAX_FRAMES:
        raise ValueError("nframes is 1..%d" % MAX_FRAMES)
    if limit <= 0:
        raise ScriptError("this item takes no script")
    P = _Parser(src).script()
    c = _Compiler(nframes, frame_names)
    if len(c.frame_names) > nframes:
        raise ValueError("more frame names than frames")
    if len(P["vars"]) > MAX_VARS:
        raise ScriptError("%d variables -- an item keeps at most %d" % (len(P["vars"]), MAX_VARS),
                          P["vars"][MAX_VARS]["line"])
    c.var_idx = {}
    for i, v in enumerate(P["vars"]):
        if v["name"] in c.var_idx:
            raise ScriptError("variable '%s' is declared twice" % v["name"], v["line"])
        if v["name"] in RESERVED or v["name"] in c.frame_names:
            raise ScriptError("'%s' is already a word in the language -- pick another name"
                              % v["name"], v["line"])
        c.var_idx[v["name"]] = i
    if not P["states"]:
        P["states"].append({"name": "main", "line": 1, "body": [], "handlers": []})
    if len(P["states"]) > MAX_STATES:
        raise ScriptError("%d states; at most %d" % (len(P["states"]), MAX_STATES),
                          P["states"][MAX_STATES]["line"])
    c.state_idx = {}
    for i, st in enumerate(P["states"]):
        n = st["name"]
        if n in c.state_idx:
            raise ScriptError("state '%s' is declared twice" % n, st["line"])
        if n in RESERVED or n in c.var_idx:
            raise ScriptError("'%s' is already a word in the language -- pick another state name" % n,
                              st["line"])
        c.state_idx[n] = i
    c.state_names = [st["name"] for st in P["states"]]
    if len(P["anims"]) > MAX_ANIMS:
        raise ScriptError("%d anims; at most %d" % (len(P["anims"]), MAX_ANIMS))
    c.anim_idx = {}
    anims = []
    for name, a in P["anims"].items():
        if len(a["frames"]) > MAX_ANIM_LEN:
            raise ScriptError("anim '%s' lists %d frames; at most %d" % (name, len(a["frames"]), MAX_ANIM_LEN),
                              a["line"])
        idx = []
        for f in a["frames"]:
            if f.t == "num":
                if f.v.denominator != 1 or not 1 <= f.v <= nframes:
                    raise ScriptError("anim '%s': there is no frame %s (frames are 1..%d)"
                                      % (name, _numtxt(f.v), nframes), f.line)
                idx.append(int(f.v) - 1)
            else:
                if f.v not in c.frame_names:
                    raise ScriptError("anim '%s': no frame called '%s' (frames: %s)"
                                      % (name, f.v, ", ".join(c.frame_names) or "1..%d" % nframes),
                                      f.line)
                idx.append(c.frame_names.index(f.v))
        fps = a["fps"]
        if fps.denominator != 1 or not 1 <= fps <= 30:
            raise ScriptError("anim '%s': fps should be a whole 1..30" % name, a["line"])
        c.anim_idx[name] = len(anims)
        anims.append((int(fps), idx))
    if len(P["perches"]) > MAX_PERCHES:
        raise ScriptError("%d perches; at most %d" % (len(P["perches"]), MAX_PERCHES),
                          P["perches"][MAX_PERCHES]["line"])
    perches = [(_whole(p["x"], "perch x", p["line"], 0, 255), _whole(p["y"], "perch y", p["line"], 0, 255))
               for p in P["perches"]]
    solid = (0, 0, 0, 0)
    if P["solid"]:
        a, line = P["solid"]
        solid = tuple(_whole(v, "solid", line, 0, 255) for v in a)
    body = _whole(P["body"][0], "body", P["body"][1], 0, 255) if P["body"] else 0
    layer = _whole(P["layer"][0], "layer", P["layer"][1], -128, 127) if P["layer"] else 1

    # handlers: state handlers first (state order), then any-state; source order
    hs = []
    for i, st in enumerate(P["states"]):
        hs += [(h, i) for h in st["handlers"]]
    hs += [(h, ANY) for h in P["handlers"]]
    if len(hs) > MAX_HANDLERS:
        raise ScriptError("%d handlers; at most %d" % (len(hs), MAX_HANDLERS), hs[MAX_HANDLERS][0]["line"])
    table = []
    nnear = nevery = 0
    for h, st in hs:
        ev = h["ev"]
        filt = EVENT_FILTER[ev]
        arg = param = 0
        L = h["line"]
        if filt == "every":
            param = max(1, ticks_of(_raw(h["every"], L)))
            if param > 65535:
                raise ScriptError("every: at most 3276 s", L)
            nevery += 1
        elif filt == "near":
            if h["word"] not in TRAITS:
                raise ScriptError("'%s' is not a trait -- traits are %s" % (h["word"], ", ".join(TRAITS)), L)
            arg = TRAITS[h["word"]] + 1
            param = 24 if h["within"] is None else rha(h["within"])
            if not 1 <= param <= 1000:
                raise ScriptError("within: 1..1000 px", L)
            nnear += 1
        elif filt == "signal":
            arg = fnv_signal(h["word"])
        elif filt == "chime":
            param = 15 if h["chime"] is None else _whole(h["chime"], "chime", L, 1, 15)
        elif filt in GROUPS:
            arg = GROUPS[filt][h["word"]] if h["word"] else 0
        key = (ev, arg, st, param)
        for other in table:
            if other["key"] == key:
                raise ScriptError("a second '%s' handler like the one on line %d; one is enough"
                                  % (ev, other["line"]), L)
        table.append({"key": key, "ev": EVENTS[ev], "arg": arg, "state": st, "param": param,
                      "h": h, "line": L})
    if nnear > MAX_NEAR:
        raise ScriptError("%d near handlers; at most %d" % (nnear, MAX_NEAR))
    if nevery > MAX_EVERY:
        raise ScriptError("%d every handlers; at most %d" % (nevery, MAX_EVERY))

    buf = bytearray()
    relocs = []
    g = _Gen(c, buf, relocs)
    body_at = []
    for st in P["states"]:
        g.thread, g.depth = True, 0
        body_at.append(g.at())
        g.stmts(st["body"])
        g.emit("HALT")
    for t in table:
        g.thread, g.depth = False, 0
        t["code"] = g.at()
        g.stmts(t["h"]["body"])
        g.emit("RET")
    if check_stack and c.maxdepth > STACK:
        raise ScriptError("this needs %d values on the stack at once; the machine has %d -- split the "
                          "expression or un-nest the repeats" % (c.maxdepth, STACK), c.maxline)

    head = bytearray([FORMAT, 1 if P["scale"] == 2 else 0, len(P["vars"]), len(P["states"]),
                      len(table), len(anims), len(perches), len(c.strings), layer & 255, body])
    head += bytes(solid)
    for v in P["vars"]:
        r = _raw(v["init"], v["line"]) & 0xFFFF
        head += bytes([r & 255, r >> 8])
    nst = len(P["states"])
    tables = bytearray()
    for t in table:
        tables += bytes([t["ev"], t["arg"], t["state"]]) + bytes(2) + bytes(2)
    for fps, idx in anims:
        tables += bytes([fps, len(idx)] + idx)
    for x, y in perches:
        tables += bytes([x, y])
    for s in c.strings:
        tables += bytes([len(s)]) + s
    base = len(head) + 2 * nst + len(tables)
    total = base + len(buf)
    if total > min(limit, MAX_BYTES):
        raise ScriptError("the script is %d bytes; at most %d -- make it shorter" % (total, min(limit, MAX_BYTES)))
    for at in relocs:
        v = buf[at] | (buf[at + 1] << 8)
        v += base
        buf[at], buf[at + 1] = v & 255, v >> 8
    out = bytearray(head)
    for a in body_at:
        out += bytes([(a + base) & 255, (a + base) >> 8])
    tpos = len(out)
    out += tables
    for i, t in enumerate(table):
        p = tpos + 7 * i
        code = t["code"] + base
        out[p + 3:p + 7] = bytes([t["param"] & 255, t["param"] >> 8, code & 255, code >> 8])
    out += buf
    out = bytes(out)
    why = check(out, nframes)
    if why:                                          # a compiler bug, never the writer's
        raise AssertionError("compiled a script the machine would refuse (%s):\n%s"
                             % (why, disassemble(out)))
    return out


# ---- decoding and the check ---------------------------------------------------------

class Script:
    """A script's bytes, decoded once (the device reads them in place)."""

    def __init__(self, b):
        b = bytes(b)
        self.b = b
        if len(b) < 14:
            raise ValueError("too short")
        if b[0] != FORMAT:
            raise ValueError("not format 3")
        if len(b) > MAX_BYTES:
            raise ValueError("over %d bytes" % MAX_BYTES)
        self.flags = b[1]
        self.nvars, self.nstates, self.nhandlers = b[2], b[3], b[4]
        self.nanims, self.nperches, self.nstrings = b[5], b[6], b[7]
        self.layer = b[8] - 256 if b[8] > 127 else b[8]
        self.body = b[9]
        self.solid = tuple(b[10:14])
        p = 14
        need = p + 2 * self.nvars + 2 * self.nstates + 7 * self.nhandlers
        if need > len(b):
            raise ValueError("tables run past the end")
        self.inits = []
        for _i in range(self.nvars):
            self.inits.append(s16(b[p] | b[p + 1] << 8))
            p += 2
        self.states = []
        for _i in range(self.nstates):
            self.states.append(b[p] | b[p + 1] << 8)
            p += 2
        self.handlers = []
        for _i in range(self.nhandlers):
            self.handlers.append((b[p], b[p + 1], b[p + 2], b[p + 3] | b[p + 4] << 8,
                                  b[p + 5] | b[p + 6] << 8))
            p += 7
        self.anims = []
        for _i in range(self.nanims):
            if p + 2 > len(b):
                raise ValueError("anims run past the end")
            fps, n = b[p], b[p + 1]
            if p + 2 + n > len(b):
                raise ValueError("anims run past the end")
            self.anims.append((fps, list(b[p + 2:p + 2 + n])))
            p += 2 + n
        self.perches = []
        for _i in range(self.nperches):
            if p + 2 > len(b):
                raise ValueError("perches run past the end")
            self.perches.append((b[p], b[p + 1]))
            p += 2
        self.strings = []
        for _i in range(self.nstrings):
            if p + 1 > len(b) or p + 1 + b[p] > len(b):
                raise ValueError("strings run past the end")
            self.strings.append(b[p + 1:p + 1 + b[p]])
            p += 1 + b[p]
        self.code_start = p
        self.full = len(b) > 512
        self.near = [i for i, h in enumerate(self.handlers) if h[0] == EVENTS["near"]]
        self.every = [i for i, h in enumerate(self.handlers) if h[0] == EVENTS["every"]]


def _decode_at(b, pc):
    """(op, operands, next pc) or None when it runs off the end."""
    if pc >= len(b) or b[pc] >= len(OPS):
        return None
    o = OPS[b[pc]]
    if pc + 1 + o.nbytes > len(b):
        return None
    vals = []
    q = pc + 1
    for kind in o.kinds:
        if kind == "s8":
            vals.append(b[q] - 256 if b[q] > 127 else b[q])
            q += 1
        elif kind == "u8":
            vals.append(b[q])
            q += 1
        elif kind == "u16":
            vals.append(b[q] | b[q + 1] << 8)
            q += 2
        else:
            vals.append(s16(b[q] | b[q + 1] << 8))
            q += 2
    return o, vals, q


_TIMED = {"WAIT", "AWAIT", "JZY"}


def check(script, nframes=None):
    """None if the machine would run this script, else why not (jv3_check)."""
    try:
        s = Script(script)
    except ValueError as e:
        return str(e)
    b = s.b
    if s.flags & ~1:
        return "unknown flags"
    if s.nvars > MAX_VARS or not 1 <= s.nstates <= MAX_STATES or s.nhandlers > MAX_HANDLERS:
        return "counts out of range"
    if s.nanims > MAX_ANIMS or s.nperches > MAX_PERCHES or s.nstrings > MAX_STRINGS:
        return "counts out of range"
    nf = MAX_FRAMES if nframes is None else nframes
    for fps, fr in s.anims:
        if not 1 <= fps <= 30 or not 1 <= len(fr) <= MAX_ANIM_LEN or any(f >= nf for f in fr):
            return "a bad anim"
    if any(len(x) > MAX_STR for x in s.strings):
        return "a string over %d" % MAX_STR
    if s.code_start >= len(b):
        return "no code"
    # every instruction from the code start
    starts = set()
    pc = s.code_start
    while pc < len(b):
        d = _decode_at(b, pc)
        if d is None:
            return "a bad instruction at %d" % pc
        starts.add(pc)
        pc = d[2]
    marks = {}
    for i, st in enumerate(s.strings):
        marks[i] = st.count(SLOT_MARK)
    nnear = nevery = 0
    entries = [(off, True) for off in s.states]
    for ev, arg, state, param, code in s.handlers:
        if ev >= len(EVENTS) or (state != ANY and state >= s.nstates):
            return "a bad handler"
        f = EVENT_FILTER[EVENT_NAME[ev]]
        if f == "every":
            nevery += 1
            if param < 1:
                return "an every handler with no period"
        if f == "near":
            nnear += 1
            if not 1 <= arg <= 32 or param < 1:
                return "a bad near handler"
        if f == "chime" and not 1 <= param <= 15:
            return "a bad chime handler"
        entries.append((code, False))
    if nnear > MAX_NEAR or nevery > MAX_EVERY:
        return "too many near or every handlers"
    for entry, thread in entries:
        if entry not in starts:
            return "an entry off an instruction"
        seen = set()
        todo = [entry]
        while todo:
            pc = todo.pop()
            if pc in seen:
                continue
            seen.add(pc)
            if pc not in starts:
                return "runs off the end at %d" % pc
            o, v, nxt = _decode_at(b, pc)
            n = o.name
            if not thread and n in _TIMED:
                return "a handler takes time at %d" % pc
            bad = ((n in ("LDV", "STV") and v[0] >= s.nvars)
                   or (n == "LDS" and v[0] >= len(SENSES))
                   or (n == "STP" and v[0] not in WRITABLE)
                   or (n == "FIELD" and v[0] >= len(FIELDS))
                   or (n == "CALL" and (v[0] >= len(FUNCS)
                                        or not FUNCS[FUNC_NAME[v[0]]][1] <= v[1] <= FUNCS[FUNC_NAME[v[0]]][2]))
                   or (n == "GO" and v[0] >= s.nstates)
                   or (n == "FRAME" and v[0] >= nf)
                   or (n == "PLAY" and (v[0] >= s.nanims or v[1] > 1))
                   or (n == "FLAG" and (v[0] > 1 or v[1] > 1))
                   or (n == "SAY" and (v[0] >= s.nstrings or v[1] != marks[v[0]]))
                   or (n == "LOG" and ((v[0] != LOG_NONE and (v[0] >= s.nstrings or v[1] != marks[v[0]]))
                                       or v[1] > STACK))
                   or (n == "EMIT" and v[0] >= len(PARTICLES))
                   or (n == "MOVE" and (v[0] >= len(TARGETS) or v[1] > 32))
                   or (n == "SIGNAL" and (not 1 <= v[0] <= 255 or v[1] > 1))
                   or (n == "ACT" and (v[0] >= len(ACTIONS) or v[1] > 1)))
            if bad:
                return "an operand out of range at %d (%s)" % (pc, n)
            if n in ("JMP", "JZ", "JZY", "RPT"):
                if v[0] not in starts:
                    return "a jump off an instruction at %d" % pc
                todo.append(v[0])
            if n in ("HALT", "RET", "JMP"):
                continue
            if n == "GO" and not thread:
                continue
            todo.append(nxt)
    return None


def disassemble(script):
    """The bytes as text: tables, then one instruction a line."""
    try:
        s = Script(script)
    except ValueError as e:
        return "not a JarScript 3 script: %s" % e
    out = ["format 3, %d bytes, %s budget, scale %d, layer %d, body %d, solid %s"
           % (len(s.b), "full" if s.full else "small", 2 if s.flags & 1 else 1, s.layer, s.body,
              s.solid)]
    out.append("vars %s" % (s.inits,))
    for i, off in enumerate(s.states):
        out.append("state %d at %d" % (i, off))
    for ev, arg, st, param, code in s.handlers:
        out.append("on %s arg %d state %s param %d at %d"
                   % (EVENT_NAME.get(ev, ev), arg, "any" if st == ANY else st, param, code))
    for i, (fps, fr) in enumerate(s.anims):
        out.append("anim %d: %s @ %d" % (i, fr, fps))
    for i, p in enumerate(s.perches):
        out.append("perch %d: %s" % (i, p))
    for i, t in enumerate(s.strings):
        out.append("string %d: %r" % (i, t.decode("ascii", "replace").replace(chr(SLOT_MARK), "{}")))
    pc = s.code_start
    while pc < len(s.b):
        d = _decode_at(s.b, pc)
        if d is None:
            out.append("%4d  ?? %d" % (pc, s.b[pc]))
            pc += 1
            continue
        o, v, nxt = d
        txt = o.name
        if o.name in ("LDS", "STP") and v[0] < len(SENSES):
            txt += " " + SENSE_NAME[v[0]]
        elif o.name == "CALL" and v[0] < len(FUNCS):
            txt += " %s/%d" % (FUNC_NAME[v[0]], v[1])
        elif o.name == "FIELD" and v[0] < len(FIELDS):
            txt += " ." + FIELD_NAME[v[0]]
        elif o.name == "ACT" and v[0] < len(ACTIONS):
            txt += " %s %d" % (ACTION_NAME[v[0]], v[1])
        elif o.name == "EMIT" and v[0] < len(PARTICLES):
            txt += " " + PARTICLE_NAME[v[0]]
        elif o.name == "MOVE" and v[0] < len(TARGETS):
            txt += " %s %d" % (TARGET_NAME[v[0]], v[1])
        elif o.name == "PUSH16":
            txt += " %d (%s)" % (v[0], fmt_num(v[0]))
        else:
            txt += "".join(" %d" % x for x in v)
        out.append("%4d  %s" % (pc, txt))
        pc = nxt
    return "\n".join(out)


# ---- the machine --------------------------------------------------------------------

def _noop(*_a):
    return None


class Item:
    """One slot of the jar: what the host knows (x, y, w, h, scale, traits)
    and, for a scripted item, the machine's state (Jv3Item)."""

    def __init__(self, slot, x, y, w, h, scale, traits, nframes, script, mem):
        self.slot, self.x, self.y, self.w, self.h = slot, x, y, w, h
        self.scale, self.traits, self.nframes = scale, traits & 0xFFFFFFFF, nframes
        self.s = None
        if script:
            if check(script, nframes) is None:
                self.s = Script(script)
        self.mem = list(mem) if mem else [0] * 8
        self.vx = self.vy = 0
        self.placed = False


class Jar:
    """The machine for a whole jar: 24 slots, one generator, one clock.

    Host callbacks (all optional):
      ground(x) -> the soil's y (raw) under x (raw); default 112 px
      sense(slot, k) -> a jar sense's raw value
      act(slot, k, arg), say(slot, text, ticks), emit(slot, kind, n),
      log(slot, text), fault(slot, pc)
    """

    def __init__(self, seed=1, ground=None, sense=None, act=None, say=None, emit=None, log=None,
                 fault=None, max_bodies=4, ledges=()):
        self.rng = seed & 0xFFFFFFFF
        self.ground = ground or (lambda x: SOIL * 16)
        self.sense_cb = sense or (lambda slot, k: -16 if SENSE_NAME[k] == "hour" else 0)
        self.act_cb, self.say_cb = act or _noop, say or _noop
        self.emit_cb, self.log_cb, self.fault_cb = emit or _noop, log or _noop, fault or _noop
        self.max_bodies = max_bodies
        self.ledges = [tuple(l) for l in ledges][:8]
        self.items = [None] * SLOTS
        self.ticks = 0

    # -- the host's side
    def rnd(self):
        x = self.rng or 0x9E3779B9
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.rng = x
        return x

    def put(self, slot, x, y, w=16, h=16, scale=1, traits=0, nframes=1, script=None, mem=None):
        it = Item(slot, x, y, w, h, scale, traits, nframes, script, mem)
        self.items[slot] = it
        return it

    def remove(self, slot):
        self.items[slot] = None
        for o in self.items:
            if o and o.s and o.perch and o.perch[0] == slot:
                o.perch = None

    def place(self, slot):
        it = self.items[slot]
        it.placed = True
        if not it.s:
            return
        s = it.s
        if not any(it.mem):
            for i, v in enumerate(s.inits):
                it.mem[i] = v
        it.stack = []
        it.state = 0
        it.pc = s.states[0]
        it.st_ticks = 0
        it.halted = False
        it.faults = 0
        it.stopped = False
        it.wait = 0
        it.anim = None
        it.anim_ticks = 0
        it.anim_once = False
        it.anim_done = False
        it.frame = 0
        it.facing = 1
        it.path = None
        it.walk = None
        it.perch = None
        it.physics = False
        it.roll = False
        it.vx = it.vy = 0
        it.gravity, it.bounce, it.friction = 160 * 16, 40 * 16, 60 * 16
        it.spin = 0
        it.angle = 0
        it.grounded = False
        it.blocked = False
        it.touch = 0
        it.other = 0
        it.impact = 0
        it.queue = []
        it.snap = []
        it.near_masks = [0] * len(s.near)
        it.every = [s.handlers[i][3] for i in s.every]
        it.say_ticks = 0
        it.sway_ticks = 0
        it.shake_ticks = 0
        it.starved = False

    def event(self, slot, ev, arg=0, other=0):
        it = self.items[slot]
        if it and it.s and not it.stopped:
            if len(it.queue) < QUEUE:
                it.queue.append((ev, arg, other))

    def scripted(self):
        return [it for it in self.items if it and it.s and it.placed]

    # -- geometry
    def _valid(self, raw):
        h = raw >> 4
        if 1 <= h <= SLOTS and self.items[h - 1]:
            return self.items[h - 1]
        return None

    @staticmethod
    def _centre(it):
        return it.x, it.y - it.h * it.scale * 8

    def _dist(self, a, b):
        ax, ay = self._centre(a)
        bx, by = self._centre(b)
        dx, dy = bx - ax, by - ay
        return isqrt32(dx * dx + dy * dy)

    def _solid(self, o):
        if not (o.s and o.s.solid[2]):
            return None
        sc = o.scale
        left = o.x - o.w * sc * 8
        top = o.y - o.h * sc * 16
        sx, sy, sw, sh = o.s.solid
        return (left + sx * sc * 16, top + sy * sc * 16, sw * sc * 16, sh * sc * 16)

    def _solids(self, me):
        out = []
        for o in self.items:
            if o and o is not me:
                b = self._solid(o)
                if b:
                    out.append(b + (o.slot, 1 << o.slot))
        for i, l in enumerate(self.ledges):
            out.append(tuple(l) + (None, 1 << (24 + i)))
        return out

    def _on_solid_top(self, it):
        for sx, sy, sw, sh, _o, _b in self._solids(it):
            if abs(it.y - sy) <= 16 and sx - 16 <= it.x <= sx + sw + 16:
                return (sx, sy, sw, sh)
        return None

    def _spot_pos(self, spot):
        owner, i = spot
        if owner < 0:
            return RIM[i][0] * 16, RIM[i][1] * 16
        o = self.items[owner]
        if not o or not o.s or i >= len(o.s.perches):
            return None
        sc = o.scale
        px, py = o.s.perches[i]
        return o.x - o.w * sc * 8 + px * sc * 16, o.y - o.h * sc * 16 + py * sc * 16

    def _queue(self, it, ev, arg, other):
        if it.s and not it.stopped and len(it.queue) < QUEUE:
            it.queue.append((ev, arg, other))

    # -- senses
    def _lds(self, it, k):
        name = SENSE_NAME[k]
        if SENSE_KIND[k] == "jar":
            return sat(int(self.sense_cb(it.slot, k)))
        if name == "x":
            return it.x
        if name == "y":
            return it.y
        if name == "vx":
            return it.vx
        if name == "vy":
            return it.vy
        if name == "angle":
            return step_to_deg(it.angle)
        if name == "spin":
            return it.spin
        if name == "gravity":
            return it.gravity
        if name == "bounce":
            return it.bounce
        if name == "friction":
            return it.friction
        if name == "speed":
            return sat(isqrt32(it.vx * it.vx + it.vy * it.vy))
        if name == "grounded":
            return 16 if it.grounded else 0
        if name == "blocked":
            return 16 if it.blocked else 0
        if name == "onperch":
            return 16 if it.perch else 0
        if name == "perched":
            return (it.perch[0] + 1) * 16 if it.perch and it.perch[0] >= 0 else 0
        if name == "facing":
            return 16 * it.facing
        if name == "other":
            return it.other * 16
        if name == "impact":
            return it.impact
        if name == "me":
            return (it.slot + 1) * 16
        if name == "state":
            return it.state * 16
        if name == "statetime":
            return sat(it.st_ticks * 16 // 20)
        if name == "frame":
            return (it.frame + 1) * 16
        if name == "time":
            return (self.ticks % 20480) * 16 // 20
        if name == "physics":
            return 16 if it.physics else 0
        if name == "soil":
            return SOIL * 16
        if name == "jarw":
            return JAR_W * 16
        if name == "jarh":
            return JAR_H * 16
        return 0

    def _stp(self, it, k, v):
        name = SENSE_NAME[k]
        if name in ("x", "y"):
            setattr(it, name, v)
            it.perch = None
        elif name == "angle":
            it.angle = deg_to_step(v)
        elif name == "bounce":
            it.bounce = max(0, min(1600, v))
        elif name == "friction":
            it.friction = max(0, v)
        else:
            setattr(it, name, v)

    # -- functions
    def _call(self, it, k, a):
        name = FUNC_NAME[k]
        if name == "random":
            lo, hi = a
            if hi < lo:
                lo, hi = hi, lo
            r = self.rnd()
            if lo % 16 == 0 and hi % 16 == 0:
                span = (hi - lo) // 16
                return sat(lo + 16 * (r % (span + 1)))
            return sat(lo + r % (hi - lo + 1))
        if name == "chance":
            return 16 if (self.rnd() % 1600) < a[0] else 0
        if name == "pick":
            return a[self.rnd() % len(a)]
        if name == "abs":
            return sat(abs(a[0]))
        if name == "min":
            return min(a)
        if name == "max":
            return max(a)
        if name == "sign":
            return 16 if a[0] > 0 else -16 if a[0] < 0 else 0
        if name == "clamp":
            v, lo, hi = a
            return lo if v < lo else hi if v > hi else v
        if name == "round":
            v = a[0]
            r = ((abs(v) + 8) >> 4) << 4
            return sat(r if v >= 0 else -r)
        if name == "floor":
            return (a[0] >> 4) << 4
        if name == "sqrt":
            return 0 if a[0] <= 0 else math.isqrt(a[0] * 16)
        if name == "sin":
            return SIN[deg_to_step(a[0])]
        if name == "cos":
            return SIN[(deg_to_step(a[0]) + 64) & 255]
        if name == "dist":
            mx, my = self._centre(it)
            if len(a) == 2:
                dx, dy = a[0] - mx, a[1] - my
            else:
                o = self._valid(a[0])
                if not o:
                    return 0
                ox, oy = self._centre(o)
                dx, dy = ox - mx, oy - my
            return sat(isqrt32(dx * dx + dy * dy))
        if name == "angleto":
            o = self._valid(a[0])
            if not o:
                return 0
            mx, my = self._centre(it)
            ox, oy = self._centre(o)
            dx = max(-65535, min(65535, ox - mx))
            dy = max(-65535, min(65535, oy - my))
            best, bk = None, 0
            for kk in range(256):
                v = dx * ANG[(kk + 64) & 255] + dy * ANG[kk]
                if best is None or v > best:
                    best, bk = v, kk
            return step_to_deg(bk)
        if name in ("find", "count"):
            t = a[0] >> 4
            if not 0 <= t <= 31:
                return 0
            bit = 1 << t
            if name == "count":
                return 16 * sum(1 for o in self.items if o and o is not it and o.traits & bit)
            lim = a[1] if len(a) > 1 else None
            best, bd = 0, None
            for o in self.items:
                if not o or o is it or not o.traits & bit:
                    continue
                d = self._dist(it, o)
                if lim is not None and d > lim:
                    continue
                if bd is None or d < bd:
                    bd, best = d, o.slot + 1
            return best * 16
        if name == "has":
            o = self._valid(a[0])
            t = a[1] >> 4
            return 16 if o and 0 <= t <= 31 and o.traits & (1 << t) else 0
        return 0

    def _field(self, o, k):
        name = FIELD_NAME[k]
        if name == "x":
            return o.x
        if name == "y":
            return o.y
        if name == "vx":
            return o.vx
        if name == "vy":
            return o.vy
        if name == "w":
            return sat(o.w * o.scale * 16)
        if name == "h":
            return sat(o.h * o.scale * 16)
        if name == "speed":
            return sat(isqrt32(o.vx * o.vx + o.vy * o.vy))
        return (o.slot + 1) * 16

    def _fill(self, it, k, vals):
        if k == LOG_NONE:
            return " ".join(fmt_num(v) for v in vals)
        t = it.s.strings[k]
        out = []
        i = 0
        for ch in t:
            if ch == SLOT_MARK:
                out.append(fmt_num(vals[i]) if i < len(vals) else "")
                i += 1
            else:
                out.append(chr(ch))
        return "".join(out)

    # -- movement starts
    def _start_path(self, it, x1, y1, speed, arc, spot, hop):
        x0, y0 = it.x, it.y
        cx = (x0 + x1) >> 1
        cy = ((y0 + y1) >> 1) - 2 * arc
        length = 0
        px, py = x0, y0
        for i in range(1, 13):
            x, y = _bez(x0, y0, cx, cy, x1, y1, i * 4096 // 12)
            dx, dy = x - px, y - py
            length += isqrt32(dx * dx + dy * dy)
            px, py = x, y
        spd = max(80, speed)
        dur = max(3, ticks_of(tdiv(length * 16, spd)))
        it.path = {"x0": x0, "y0": y0, "x1": x1, "y1": y1, "cx": cx, "cy": cy, "t": 0,
                   "step": (4096 + dur - 1) // dur, "spot": spot, "hop": hop}
        it.walk = None
        it.vx = it.vy = 0
        if x1 > x0:
            it.facing = 1
        elif x1 < x0:
            it.facing = -1

    def _move(self, it, kind, trait1, args):
        sc = it.scale
        half = it.w * sc * 8
        spot = None
        tx = ty = None
        if kind == 0:
            tx, ty = args[0], args[1]
        elif kind == 1:
            o = self._valid(args[0])
            if o:
                tx, ty = o.x, o.y - o.h * o.scale * 16
        elif kind == 2:
            spots = []
            if trait1:
                bit = 1 << (trait1 - 1)
                for o in self.items:
                    if o and o is not it and o.traits & bit and o.s:
                        spots += [(o.slot, i) for i in range(len(o.s.perches))]
            else:
                spots = [(-1, i) for i in range(len(RIM))]
            held = {x.perch for x in self.items if x and x.s and x.placed and x.perch}
            free = [sp for sp in spots if sp not in held]
            if free:
                spot = free[self.rnd() % len(free)]
                tx, ty = self._spot_pos(spot)
        if tx is None:
            tx = (14 + self.rnd() % 213) * 16
            ty = self.ground(tx)
        tx = max(half, min(JAR_W * 16 - half, tx))
        ty = max(128, min(self.ground(tx), ty))
        it.perch = None
        self._start_path(it, tx, ty, args[-2], args[-1], spot, False)

    # -- running code
    def _exec(self, it, pc, stack, budget, thread):
        """Run from pc. Returns (outcome, pc, used, arg); outcomes: 'end'
        (handler done), 'go', 'halt', 'yield', 'fault', 'budget'."""
        b = it.s.b
        used = 0
        while True:
            if used >= budget:
                return "budget", pc, used, None
            d = _decode_at(b, pc)
            o, v, nxt = d
            n = o.name
            pops, pushes = _effect(n, v[0] if v else 0, v[1] if len(v) > 1 else 0)
            if len(stack) < pops or len(stack) - pops + pushes > STACK:
                used += 1
                return "fault", pc, used, None
            used += 1
            if n == "HALT":
                return ("halt" if thread else "end"), pc, used, None
            if n == "RET":
                return ("yield" if thread else "end"), nxt, used, None
            if n == "PUSH8":
                stack.append(v[0] * 16)
            elif n == "PUSH16":
                stack.append(v[0])
            elif n == "LDV":
                stack.append(it.mem[v[0]])
            elif n == "STV":
                it.mem[v[0]] = stack.pop()
            elif n == "LDS":
                stack.append(self._lds(it, v[0]))
            elif n == "STP":
                self._stp(it, v[0], stack.pop())
            elif pops == 2 and pushes == 1 and n in _ARITH:
                y = stack.pop()
                x = stack.pop()
                stack.append(_ARITH[n](x, y))
            elif n == "NEG":
                stack.append(sat(-stack.pop()))
            elif n == "NOT":
                stack.append(0 if stack.pop() else 16)
            elif n == "FIELD":
                ob = self._valid(stack.pop())
                stack.append(self._field(ob, v[0]) if ob else 0)
            elif n == "CALL":
                k, cnt = v
                args = stack[len(stack) - cnt:]
                del stack[len(stack) - cnt:]
                stack.append(self._call(it, k, args))
            elif n == "JMP":
                pc = v[0]
                continue
            elif n == "JZ":
                if stack.pop() == 0:
                    pc = v[0]
                    continue
            elif n == "JZY":
                if stack.pop() == 0:
                    return "yield", v[0], used - 1, None
            elif n == "RPT":
                if stack[-1] <= 0:
                    stack.pop()
                    pc = v[0]
                    continue
                stack[-1] = sat(stack[-1] - 16)
            elif n == "WAIT":
                it.wait = max(1, ticks_of(stack.pop()))
                return "yield", nxt, used, None
            elif n == "AWAIT":
                if it.path or it.walk or (it.anim is not None and it.anim_once and not it.anim_done):
                    return "yield", pc, used - 1, None
            elif n == "GO":
                return "go", nxt, used, v[0]
            elif n == "FRAME":
                if v[0] < it.nframes:
                    it.frame = v[0]
                    it.anim = None
            elif n == "FRAMEE":
                x = stack.pop()
                r = ((abs(x) + 8) >> 4) * (1 if x >= 0 else -1)
                if 1 <= r <= it.nframes:
                    it.frame = r - 1
                    it.anim = None
            elif n == "PLAY":
                k, once = v
                if not (not once and it.anim == k and not it.anim_once):
                    it.anim, it.anim_once, it.anim_done, it.anim_ticks = k, bool(once), False, 0
                    f = it.s.anims[k][1][0]
                    if f < it.nframes:
                        it.frame = f
            elif n == "STOP":
                it.anim = None
            elif n == "FLAG":
                k, on = v
                if k == 0:
                    if on and not it.physics:
                        nb = sum(1 for x in self.items if x and x.s and x.placed and x.physics)
                        if nb < self.max_bodies:
                            it.physics = True
                            it.perch = None
                            it.path = None
                            it.walk = None
                    elif not on:
                        it.physics = False
                else:
                    it.roll = bool(on)
            elif n == "SAY":
                k, cnt = v
                secs = stack.pop()
                vals = stack[len(stack) - cnt:]
                del stack[len(stack) - cnt:]
                text = self._fill(it, k, vals)[:MAX_STR]
                it.say_ticks = max(6, ticks_of(secs))
                self.say_cb(it.slot, text, it.say_ticks)
            elif n == "LOG":
                k, cnt = v
                vals = stack[len(stack) - cnt:]
                del stack[len(stack) - cnt:]
                self.log_cb(it.slot, self._fill(it, k, vals))
            elif n == "EMIT":
                cnt = max(0, min(12, stack.pop() >> 4))
                if cnt > 0:
                    self.emit_cb(it.slot, v[0], cnt)
            elif n == "SWAY":
                secs = stack.pop()
                stack.pop()
                it.sway_ticks = max(1, ticks_of(secs))
            elif n == "SHAKE":
                it.shake_ticks = max(0, ticks_of(stack.pop()))
            elif n == "FACE":
                x = stack.pop()
                if x > 0:
                    it.facing = 1
                elif x < 0:
                    it.facing = -1
            elif n == "TOWARD":
                ob = self._valid(stack.pop())
                if ob:
                    if ob.x > it.x:
                        it.facing = 1
                    elif ob.x < it.x:
                        it.facing = -1
            elif n == "MOVE":
                kind, trait1 = v
                arc = stack.pop()
                speed = stack.pop()
                if kind == 0:
                    y = stack.pop()
                    x = stack.pop()
                    args = [x, y, speed, arc]
                elif kind == 1:
                    args = [stack.pop(), speed, arc]
                else:
                    args = [speed, arc]
                self._move(it, kind, trait1, args)
            elif n == "WALK":
                speed = stack.pop()
                dx = stack.pop()
                it.blocked = False
                it.perch = None
                it.path = None
                it.walk = None
                it.vx = it.vy = 0
                if abs(dx) >= 8:
                    it.walk = {"left": abs(dx), "dir": 1 if dx > 0 else -1,
                               "step": max(1, abs(speed) // 20)}
                    it.facing = 1 if dx > 0 else -1
            elif n == "HOP":
                hgt = stack.pop()
                dx = stack.pop()
                half = it.w * it.scale * 8
                if it.perch:
                    dx = 0
                x1 = max(half + 32, min(JAR_W * 16 - half - 32, it.x + dx))
                keep = it.perch
                self._start_path(it, x1, it.y, 40 * 16, hgt, None, True)
                it.perch = keep
            elif n == "SIGNAL":
                k, to = v
                if to:
                    ob = self._valid(stack.pop())
                    if ob:
                        self._queue(ob, EVENTS["signal"], k, it.slot + 1)
                else:
                    for ob in self.items:
                        if ob and ob is not it and ob.s and ob.placed:
                            self._queue(ob, EVENTS["signal"], k, it.slot + 1)
            elif n == "ACT":
                k, has = v
                arg = stack.pop() if has else 0
                self.act_cb(it.slot, k, arg)
            pc = nxt

    def _fault(self, it, pc):
        it.faults += 1
        self.fault_cb(it.slot, pc)
        if it.faults >= MAX_FAULTS:
            it.stopped = True

    def _enter(self, it, k):
        s = it.s
        it.state = k
        it.pc = s.states[k]
        it.stack = []
        it.wait = 0
        it.halted = False
        it.path = None
        it.walk = None
        it.st_ticks = 0
        for j, hi in enumerate(s.every):
            if s.handlers[hi][2] == k:
                it.every[j] = s.handlers[hi][3]
        for j, hi in enumerate(s.near):
            if s.handlers[hi][2] == k:
                it.near_masks[j] = 0

    def _active(self, it, h):
        return h[2] == ANY or h[2] == it.state

    def _match(self, it, ev, arg):
        if ev == EVENTS["near"]:              # D3: the handler that saw it, only
            hs = it.s.handlers
            if 1 <= arg <= len(hs) and hs[arg - 1][0] == ev and self._active(it, hs[arg - 1]):
                return hs[arg - 1]
            return None
        for h in it.s.handlers:
            if h[0] != ev or not self._active(it, h):
                continue
            if ev == EVENTS["chime"]:
                if arg > 0 and arg % h[3] == 0:
                    return h
            elif h[1] == 0 or h[1] == arg:
                return h
        return None

    def _handler(self, it, h, other, impact, budget):
        """Run one handler; returns (used, pending state or None, ok). With
        no budget left it does not start: dropped, no fault (D4)."""
        if budget <= 0:
            return 0, None, True
        it.other, it.impact = other, impact
        out, pc, used, arg = self._exec(it, h[4], [], budget, False)
        if out in ("fault", "budget"):
            self._fault(it, pc)
            return used, None, False
        return used, (arg if out == "go" else None), True

    # -- one tick
    def tick(self):
        items = self.scripted()
        for it in items:
            it.snap, it.queue = it.queue, []
        for it in items:
            if not it.stopped:
                self._near(it)
        for it in items:
            if not it.stopped:
                self._run(it)
        for it in items:
            if not it.stopped:
                self._step(it)
        self.ticks += 1

    def _near(self, it):
        s = it.s
        for j, hi in enumerate(s.near):
            h = s.handlers[hi]
            if not self._active(it, h):
                it.near_masks[j] = 0
                continue
            bit = 1 << (h[1] - 1)
            r = h[3] * 16
            mask = 0
            for o in self.items:
                if o and o is not it and o.traits & bit and self._dist(it, o) <= r:
                    mask |= 1 << o.slot
            new = mask & ~it.near_masks[j]
            for slot in range(SLOTS):
                if new >> slot & 1:
                    self._queue(it, EVENTS["near"], hi + 1, slot + 1)
            it.near_masks[j] = mask

    def _run(self, it):
        s = it.s
        budget = BUDGET_FULL if s.full else BUDGET_SMALL
        pending = None
        it.starved = False
        for ev, arg, other in it.snap:
            h = self._match(it, ev, arg)
            if not h:
                continue
            impact = arg if ev in (EVENTS["hit"], EVENTS["bump"]) else 0
            used, go, _ok = self._handler(it, h, other, impact, budget)
            budget -= used
            if go is not None:
                pending = go
            if it.stopped:
                return
        h = self._match(it, EVENTS["tick"], 0)
        if h:
            used, go, _ok = self._handler(it, h, 0, 0, budget)
            budget -= used
            if go is not None:
                pending = go
            if it.stopped:
                return
        for j, hi in enumerate(s.every):
            h = s.handlers[hi]
            if not self._active(it, h):
                continue
            it.every[j] -= 1
            if it.every[j] > 0:
                continue
            it.every[j] = h[3]
            used, go, _ok = self._handler(it, h, 0, 0, budget)
            budget -= used
            if go is not None:
                pending = go
            if it.stopped:
                return
        if pending is not None:
            self._enter(it, pending)
        if it.halted:
            return
        if it.wait > 0:
            it.wait -= 1
            if it.wait > 0:
                return
        gos = 0
        while True:
            out, pc, used, arg = self._exec(it, it.pc, it.stack, budget, True)
            budget -= used
            it.pc = pc
            if out == "go":
                self._enter(it, arg)
                gos += 1
                if gos >= 4:
                    break
                continue
            if out == "halt":
                it.halted = True
            elif out == "fault":
                it.halted = True
                self._fault(it, pc)
            elif out == "budget":
                it.starved = True
            break

    def _step(self, it):
        sc = it.scale
        hs = it.h * sc * 16
        half = it.w * sc * 8
        moved_by_mover = False
        if it.path:
            p = it.path
            p["t"] = min(4096, p["t"] + p["step"])
            t = p["t"]
            s = (((t * t) >> 12) * (12288 - 2 * t)) >> 12
            it.x, it.y = _bez(p["x0"], p["y0"], p["cx"], p["cy"], p["x1"], p["y1"], s)
            moved_by_mover = True
            if t >= 4096:
                it.x, it.y = p["x1"], p["y1"]
                it.path = None
                if p["spot"]:
                    it.perch = p["spot"]
                if not p["hop"]:
                    sp = p["spot"]
                    self._queue(it, EVENTS["land"], 0, sp[0] + 1 if sp and sp[0] >= 0 else 0)
        elif it.walk:
            w = it.walk
            dd = min(w["left"], w["step"])
            nx = it.x + w["dir"] * dd
            stop = False
            lo, hi = half + 32, JAR_W * 16 - half - 32
            if nx < lo or nx > hi:
                nx = max(lo, min(hi, nx))
                stop = True
            for sx, sy, sw, sh, _o, _b in self._solids(it):
                if (it.y > sy + 8 and it.y - hs < sy + sh and nx + half > sx and nx - half < sx + sw
                        and not (it.x + half > sx and it.x - half < sx + sw)):
                    stop = True
                    nx = it.x
            if abs(it.y - self.ground(it.x)) > 16:
                sup = self._on_solid_top(it)
                if sup and (nx < sup[0] or nx > sup[0] + sup[2]):
                    stop = True
                    nx = it.x
            it.x = sat(nx)
            w["left"] -= dd
            if stop:
                it.blocked = True
                it.walk = None
            elif w["left"] <= 0:
                it.walk = None
            moved_by_mover = True
        elif it.physics:
            self._physics(it, hs)
        if not it.physics or moved_by_mover:
            if not it.physics:
                it.touch = 0
            if it.spin:
                it.angle = (it.angle + tdiv(it.spin, 450)) & 255
        if it.perch and not it.path:
            pos = self._spot_pos(it.perch)
            if pos:
                it.x, it.y = pos
            else:
                it.perch = None
        if not it.physics:
            it.grounded = (not it.perch and not it.path and not it.walk and
                           (abs(it.y - self.ground(it.x)) <= 16 or self._on_solid_top(it) is not None))
        if it.anim is not None:
            fps, fr = it.s.anims[it.anim]
            n = len(fr)
            if it.anim_once:
                if not it.anim_done:
                    it.anim_ticks += 1
                    k = it.anim_ticks * fps // 20
                    if k >= n:
                        f = fr[-1]
                        it.anim_done = True
                    else:
                        f = fr[k]
                    if f < it.nframes:
                        it.frame = f
            else:
                it.anim_ticks = (it.anim_ticks + 1) % (20 * n)
                f = fr[(it.anim_ticks * fps // 20) % n]
                if f < it.nframes:
                    it.frame = f
        if it.st_ticks < 65535:
            it.st_ticks += 1
        if it.say_ticks:
            it.say_ticks -= 1
        if it.sway_ticks:
            it.sway_ticks -= 1
        if it.shake_ticks:
            it.shake_ticks -= 1

    def _physics(self, it, hs):
        sc = it.scale
        r = it.s.body * 16 if it.s.body else max(16, min(it.w, it.h) * sc * 8 - 16)
        e = it.bounce
        vx, vy = it.vx, sat(it.vy + tdiv(it.gravity, 20))
        if it.grounded and it.friction > 0:
            f = tdiv(it.friction, 20)
            if abs(vx) <= f:
                vx = 0
            else:
                vx -= f if vx > 0 else -f
        dxm = tdiv(vx, 20)
        cx = it.x + dxm
        cy = it.y - hs // 2 + tdiv(vy, 20)
        grounded = False
        hits = []
        soil = self.ground(cx)
        if cy + r > soil:
            cy = soil - r
            if vy > 0:
                hits.append(vy)
                vy = -tdiv(vy * e, 1600)
                if abs(vy) < 192:
                    vy = 0
            grounded = True
        if cx - r < 48:
            cx = 48 + r
            if vx < 0:
                hits.append(-vx)
                vx = tdiv(-vx * e, 1600)
        if cx + r > (JAR_W - 3) * 16:
            cx = (JAR_W - 3) * 16 - r
            if vx > 0:
                hits.append(vx)
                vx = -tdiv(vx * e, 1600)
        if cy - r < 128:
            cy = 128 + r
            if vy < 0:
                hits.append(-vy)
                vy = tdiv(-vy * e, 1600)
        for imp in hits:
            if imp > 240:
                self._queue(it, EVENTS["hit"], imp, 0)
        touch = 0
        for sx, sy, sw, sh, owner, bit in self._solids(it):
            nx = max(sx, min(sx + sw, cx))
            ny = max(sy, min(sy + sh, cy))
            dx, dy = cx - nx, cy - ny
            d2 = dx * dx + dy * dy
            if d2 >= r * r:
                continue
            d = math.isqrt(d2)
            if d == 0:
                ndx, ndy, nd = 0, -1, 1
                cy = sy - r
            else:
                ndx, ndy, nd = dx, dy, d
                cx += tdiv(dx * (r - d), d)
                cy += tdiv(dy * (r - d), d)
            vn = tdiv(vx * ndx + vy * ndy, nd)
            floor = 5 * ndy < -3 * nd
            if vn < 0:
                k = tdiv(vn * (1600 + e), 1600)
                vx = sat(vx - tdiv(k * ndx, nd))
                vy = sat(vy - tdiv(k * ndy, nd))
                if floor and abs(vy) < 192:
                    vy = 0
                if -vn > 160 and not it.touch & bit:
                    self._queue(it, EVENTS["bump"], sat(-vn), owner + 1 if owner is not None else 0)
                    if owner is not None:
                        self._queue(self.items[owner], EVENTS["bump"], sat(-vn), it.slot + 1)
            if floor:
                grounded = True
            touch |= bit
        it.touch = touch
        it.grounded = grounded
        it.x = sat(cx)
        it.y = sat(cy + hs // 2)
        it.vx, it.vy = sat(vx), sat(vy)
        if it.roll:
            it.angle = (it.angle + tdiv(dxm * 163, r * 4)) & 255


def _bez(x0, y0, cx, cy, x1, y1, s):
    ax = x0 + (((cx - x0) * s) >> 12)
    ay = y0 + (((cy - y0) * s) >> 12)
    bx = cx + (((x1 - cx) * s) >> 12)
    by = cy + (((y1 - cy) * s) >> 12)
    return ax + (((bx - ax) * s) >> 12), ay + (((by - ay) * s) >> 12)


def _div(a, b):
    return 0 if b == 0 else sat(tdiv(a * 16, b))


def _mod(a, b):
    return 0 if b == 0 else a - b * (a // b)


_ARITH = {
    "ADD": lambda a, b: sat(a + b), "SUB": lambda a, b: sat(a - b),
    "MUL": lambda a, b: sat((a * b) >> 4), "DIV": _div, "MOD": _mod,
    "EQ": lambda a, b: 16 if a == b else 0, "NE": lambda a, b: 16 if a != b else 0,
    "LT": lambda a, b: 16 if a < b else 0, "LE": lambda a, b: 16 if a <= b else 0,
    "GT": lambda a, b: 16 if a > b else 0, "GE": lambda a, b: 16 if a >= b else 0,
    "AND": lambda a, b: 16 if a and b else 0, "OR": lambda a, b: 16 if a or b else 0,
}


# ---- the simulated hour ---------------------------------------------------------------

def _traits_mask(traits):
    if isinstance(traits, int):
        return traits
    m = 0
    for t in traits:
        m |= 1 << TRAITS[t]
    return m


def _where(s, pc):
    """Which body or handler a pc is in (the nearest entry at or below it)."""
    entries = [(off, "state %d's body" % i) for i, off in enumerate(s.states)]
    entries += [(h[4], "the 'on %s' handler" % EVENT_NAME[h[0]]) for h in s.handlers]
    best = None
    for off, name in entries:
        if off <= pc and (best is None or off > best[0]):
            best = (off, name)
    return best[1] if best else "the code"


def simulate(script, nframes, w=16, h=16, scale=1, traits=0, hours=1, seed=1):
    """Run the item for `hours` simulated hours in a small world and list
    what is wrong with it (empty: it passes). The world: flat soil, a
    neighbour that wanders (critter, bird, prey) and one that stays (plant,
    roost, wood), pokes every few minutes, the item's own signals now and
    then, and the jar's events (time, weather, chime, world, berry, new,
    gift, shipped, jam) at 20 ticks a second."""
    why = check(script, nframes)
    if why:
        return ["the bytecode is not a script this machine would run: %s" % why]
    s = Script(script)
    faults = []
    seen = {"visible": False}

    def on_fault(slot, pc):
        if slot == 0:
            faults.append(pc)

    def vis(slot, *_a):
        if slot == 0:
            seen["visible"] = True

    jar_state = {"hour": 9, "daytime": 2, "weather": 1, "world": 0, "berries": 2, "stuck": 0}

    def sense(slot, k):
        name = SENSE_NAME[k]
        if name in jar_state:
            return jar_state[name] * 16
        if name in ("playing",):
            return 16
        if name == "days":
            return 3 * 16
        return 0

    jar = Jar(seed=seed, sense=sense, say=vis, emit=vis, fault=on_fault, max_bodies=4)
    T = TRAITS
    jar.put(0, 120 * 16, SOIL * 16, w, h, scale, _traits_mask(traits), nframes, script)
    jar.put(1, 60 * 16, SOIL * 16, 16, 16, 1, (1 << T["critter"]) | (1 << T["bird"]) | (1 << T["prey"]), 1)
    jar.put(2, 190 * 16, SOIL * 16, 16, 32, 1, (1 << T["plant"]) | (1 << T["roost"]) | (1 << T["wood"]), 1)
    for slot in range(3):
        jar.place(slot)
    me = jar.items[0]
    signals = sorted({hh[1] for hh in s.handlers if hh[0] == EVENTS["signal"] and hh[1]})
    E = EVENTS
    total = hours * 72000
    last = (me.x, me.y, me.frame)
    starved = 0
    problems = []
    nsig = 0
    for t in range(total):
        sec, tk = divmod(t, 20)
        if tk == 0:
            minute, sc_ = divmod(sec, 60)
            if sc_ == 0 and minute:
                jar.event(0, E["chime"], minute)
                q = minute % 60
                if q in (0, 15, 30, 45):
                    jar_state["daytime"] = 1 + q // 15
                    jar_state["hour"] = (9 + minute // 15) % 24
                    jar.event(0, E["time"], jar_state["daytime"])
                if q == 20:
                    jar_state["weather"] = GROUPS["weather"]["rainy"]
                    jar.event(0, E["weather"], jar_state["weather"])
                if q == 30:
                    jar_state["world"] = GROUPS["world"]["ants"]
                    jar.event(0, E["world"], jar_state["world"])
                if q == 33:
                    jar_state["world"] = 0
                if q == 10:
                    jar_state["stuck"] = 1
                    jar.event(0, E["jam"], 1)
                if q == 12:
                    jar_state["stuck"] = 0
                    jar.event(0, E["jam"], 2)
                if q == 25:
                    jar.event(0, E["new"], 0, 2)
                if q == 50:
                    jar.event(0, E["gift"], 0)
                if minute % 4 == 2:
                    jar.event(0, E["shipped"], 0)
                if minute % 5 == 4:
                    jar_state["berries"] += 1
                    jar.event(0, E["berry"], 0)
            if sec % 170 == 37:
                jar.event(0, E["poke"], 0)
            if sec % 23 == 11:                      # the wanderer moves (the host moves it)
                nb = jar.items[1]
                nb.x = (20 + jar.rnd() % 200) * 16
            if signals and sec % 97 == 50:
                jar.event(0, E["signal"], signals[nsig % len(signals)], 2)
                nsig += 1
        jar.tick()
        if me.starved:
            starved += 1
            if starved == 40:
                problems.append("%s never waits: it ran out of its %d steps every tick for 2 s -- "
                                "a loop needs a wait, move, walk, hop or 'play ... once'"
                                % (_where(s, me.pc), BUDGET_FULL if s.full else BUDGET_SMALL))
                break
        else:
            starved = 0
        now = (me.x, me.y, me.frame)
        if now != last:
            seen["visible"] = True
        last = now
        if me.stopped:
            break
    if faults:
        problems.insert(0, "the script faulted %d time%s (first in %s, at %d: a stack too deep or a "
                        "handler that ran out of its %d steps)"
                        % (len(faults), "" if len(faults) == 1 else "s", _where(s, faults[0]), faults[0],
                           BUDGET_FULL if s.full else BUDGET_SMALL))
    if me.stopped:
        problems.append("the script was stopped after %d faults" % MAX_FAULTS)
    if not seen["visible"] and not me.stopped:
        problems.append("nothing visible happened in a whole hour: it never moved, changed frame, "
                        "said anything or gave off particles")
    return problems


def prepare_script(src, nframes, frame_names=None, tier="full", w=16, h=16, scale=1):
    """What generation calls: compile at the tier's limit, then live through
    a simulated hour. The bytes, or ScriptError with the first problem."""
    limit = tier_limit(tier)
    code = compile_script(src, nframes, frame_names, limit)
    traits = script_traits(src)
    problems = simulate(code, nframes, w, h, scale, traits)
    if problems:
        raise ScriptError("the script failed its simulated hour: " + problems[0])
    return code


# ---- the guide ---------------------------------------------------------------------------

def _guide():
    evs = []
    for name, num in EVENTS.items():
        f = EVENT_FILTER[name]
        form = {"none": "on %s:" % name, "every": "on every 2s:", "near": "on near TRAIT [within R]:",
                "signal": "on signal NAME:", "chime": "on chime [N]:"}.get(
            f, "on %s [%s]:" % (name, "|".join(GROUPS.get(f, {}))))
        evs.append("  %-32s %s" % (form, EVENT_DOC[name]))
    acts = []
    for name in ACTIONS:
        kind = ACTION_ARG[name]
        form = {"none": name, "opt": "%s [N]" % name, "sound": "sound NAME",
                "glow": "glow on|off|toggle"}[kind]
        acts.append("  %-20s %s" % (form, ACTION_DOC[name]))
    sens = ["  %-10s %s" % (n, SENSE_DOC[n]) for n in SENSES]
    funcs = ["  %-10s %s" % (n, FUNC_DOC[n]) for n in FUNCS]
    return "\n".join([
        "JARSCRIPT 3 -- the behaviour of one item in the jar. Compiled on the server to at",
        "most the byte limit you are given (small 512, full 1024; the limit counts",
        "everything: tables, texts and code). The item has up to 4 pictures (frames),",
        "16x16, named or numbered from 1.",
        "",
        "SHAPE. Declarations, then states and handlers. The first state is where the item",
        "starts. A state's body runs as a little thread: it may take time (wait, move,",
        "walk, hop, play ... once) and starts again from the top on `go`. A handler",
        "(`on poke:`) runs to its end within one tick and may `go` to a state; inside a",
        "state it listens only in that state and wins over a top-level one. Blocks close",
        "with `end`; `:` and `then` are optional; `;` separates statements; `#` comments.",
        "",
        "  traits A, B",
        "  var count = 0",
        "  anim idle = f1 f2 @ 4",
        "  state rest:",
        "    play idle",
        "    wait random(2, 5)",
        "    go wander",
        "  end",
        "  state wander:",
        "    walk random(-30, 30)",
        "    go rest",
        "  end",
        "  on poke: count += 1; go wander end",
        "",
        "UNITS. Jar pixels, 240 x 135, y grows down, the soil at y 112; x is my centre,",
        "y my feet. Velocities px/s, angles degrees (0 right, 90 down), times seconds",
        "(2s, 300ms or a bare number). 20 ticks a second. Numbers are kept to 1/16,",
        "-2048 .. 2047.9; + - saturate; x / 0 and x % 0 give 0. Items are numbers",
        "(handles; 0 is nobody); a field of nobody reads 0.",
        "",
        "DECLARATIONS",
        "  traits A, B            what this item is; others find it by trait, never by name",
        "  var a, b = 3           up to 8 numbers, kept while the item lives",
        "  anim NAME = f1 f2 .. @ FPS   up to 8 entries of its frames (repeats allowed), fps 1..30",
        "  perch X, Y             up to 4 spots (sprite px) where others may land",
        "  solid X, Y, W, H       one box others' bodies collide with and stand on",
        "  body R                 my physics radius in px",
        "  layer N                draw order (default 1)",
        "  scale 1|2              draw at twice the size",
        "  state NAME: ... end    a mode",
        "  on EVENT: ... end      a handler",
        "",
        "TRAITS (the only words for what things are): " + ", ".join(TRAITS),
        "",
        "EVENTS",
    ] + evs + [
        "",
        "STATEMENTS",
        "  name = expr  (+= -= *= /=)   a var, or x y vx vy angle spin gravity bounce friction",
        "  if C then .. elif C then .. else .. end;  while C .. end;  repeat N .. end;  loop .. end",
        "  wait T | wait | wait until C   [body only] seconds, one tick, or until C holds",
        "  go STATE                       switch state; its body starts from the top",
        "  frame NAME|N                   a still frame;  play ANIM [once];  stop",
        "  physics on|off; roll on|off    gravity, bounces, friction; rolling turns it",
        "  move to X, Y | perch [TRAIT|rim] | ground | ITEM [speed S] [arc A]  [body only]",
        "                                 a curved path (default speed 60); ends with `on land`",
        "  walk DX [speed S]              [body only] along the ground; stops at glass and solids",
        "  hop [DX [H]]                   [body only] a small jump",
        "  face V | face toward ITEM      look left (negative) or right",
        '  say "TEXT" [T]                 a bubble, at most 24 characters; join values with +:',
        '                                 say "x" + n (text only in say and log)',
        "  log A, B ...                   a line in the server's log",
        "  emit KIND [N]                  particles: " + ", ".join(PARTICLES) + " (N 0..12)",
        "  sway AMP [T]; shake [T]        a drawn bend or jitter",
        "  signal NAME [to ITEM]          wakes `on signal NAME` in one item or every other",
        "  return                         leave a handler",
    ] + acts + [
        "",
        "EXPRESSIONS: + - * / %, == != < <= > >=, and or not, ( ), ITEM.x .y .vx .vy .w .h",
        ".id .speed, left (-1), right (1), true, false, none, a state's name (its number),",
        "and these names: times " + ", ".join(GROUPS["time"]) + "; weathers " +
        ", ".join(GROUPS["weather"]) + "; jam " + ", ".join(GROUPS["jam"]) + "; world events " +
        ", ".join(GROUPS["world"]) + "; zones " + ", ".join(GROUPS["zone"]) + "; sounds " +
        ", ".join(GROUPS["sound"]) + ".",
        "",
        "FUNCTIONS",
    ] + funcs + [
        "",
        "SENSES",
    ] + sens + [
        "",
        "LIMITS: 100 steps a tick for a full script, 50 for a small one, shared by handlers",
        "and the body; a handler that runs out mid-way is a fault (one with no steps left to",
        "start is skipped), a body that runs out carries on next tick. 8 faults stop the item for good. 8 vars, 16 states, 24 handlers (4 near,",
        "6 every), 8 anims, 16 different texts, a 16-value stack. A script must do something",
        "visible within an hour: move, change frame, say or emit.",
    ])


LANGUAGE_GUIDE_V3 = _guide()
