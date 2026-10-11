"""Jar Factory's item scripts on the server: the compiler, the machine, and
the simulated day a script must survive before it is delivered.

The design is docs/superpowers/specs/2026-10-09-jar-factory-design.md
("Item behaviour", and "Scripts: the language" at the end). In short:

* The instruction table is apps/jarvm.def, read here at import -- the same
  lines apps/jarvm.h builds the device's tables from. Nothing is retyped.
* `compile_script(source)` turns the readable language into bytecode, at
  most 256 bytes, or raises ScriptError naming the line and what was
  expected, so a generator can hand the message back and try again.
* `run(script, event, arg, mem, io)` is the device's machine, step for step:
  same results, same step counts, same actions in the same order
  (test/test_jarvm.c replays fixtures this module writes and must agree).
* `simulate_day(script)` fires a compressed day of events at a script and
  says whether it never faulted, never hit the step limit, and did
  something visible.
* `prepare_script(source, ...)` is the one call generation needs: compile,
  check, simulate; the bytes, or ScriptError with every problem.
* `LANGUAGE_GUIDE` is the language in a page, for the generation prompt.
"""

import os
import re

DEF_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "apps", "jarvm.def")

# ---- the table, from apps/jarvm.def -----------------------------------------

_OP_RE = re.compile(r'^JV_OP\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\d)\s*,\s*(\d)\s*,\s*(\d)\s*,'
                    r'\s*"([^"]*)"\s*\)\s*$')
_EVENT_RE = re.compile(r'^JV_EVENT\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
                       r'\s*"([^"]*)"\s*\)\s*$')
_ACTION_RE = re.compile(r'^JV_ACTION\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
                        r'\s*"([^"]*)"\s*\)\s*$')
_SENSE_RE = re.compile(r'^JV_SENSE\(\s*(\d+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,'
                       r'\s*"([^"]*)"\s*\)\s*$')
_NAME_RE = re.compile(r'^JV_NAME\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\)\s*$')


class Op:
    def __init__(self, code, name, nops, pops, pushes, doc):
        self.code, self.name, self.nops = code, name, nops
        self.pops, self.pushes, self.doc = pops, pushes, doc

    def __repr__(self):
        return "Op(%d, %s)" % (self.code, self.name)


def _load_def(path=DEF_PATH):
    ops, events, actions, senses, names = [], [], [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            m = _OP_RE.match(line)
            if m:
                ops.append(Op(int(m.group(1)), m.group(2), int(m.group(3)),
                              int(m.group(4)), int(m.group(5)), m.group(6)))
                continue
            m = _EVENT_RE.match(line)
            if m:
                events.append((int(m.group(1)), m.group(2), m.group(4), m.group(5)))
                continue
            m = _ACTION_RE.match(line)
            if m:
                actions.append((int(m.group(1)), m.group(2), m.group(4), m.group(5)))
                continue
            m = _SENSE_RE.match(line)
            if m:
                senses.append((int(m.group(1)), m.group(2), m.group(4)))
                continue
            m = _NAME_RE.match(line)
            if m:
                names.append((m.group(1), m.group(2), int(m.group(3))))
                continue
            if line.startswith("JV_"):
                raise ValueError("%s: a line this reader does not understand: %s" % (path, line))
    for what, rows in (("JV_OP", [o.code for o in ops]), ("JV_EVENT", [e[0] for e in events]),
                       ("JV_ACTION", [a[0] for a in actions]), ("JV_SENSE", [s[0] for s in senses])):
        if rows != list(range(len(rows))):
            raise ValueError("%s: %s numbers must be 0, 1, 2 ... in order" % (path, what))
    return ops, events, actions, senses, names


OPS, _EVENTS, _ACTIONS, _SENSES, _NAMES = _load_def()
OP = {o.name: o.code for o in OPS}
NOPS = len(OPS)
NACTIONS = len(_ACTIONS)
NSENSES = len(_SENSES)

EVENTS = {name: num for num, name, _f, _d in _EVENTS}           # word -> number
EVENT_FILTER = {name: filt for _n, name, filt, _d in _EVENTS}   # word -> filter kind
EVENT_NAME = {num: name for num, name, _f, _d in _EVENTS}
ACTIONS = {name: num for num, name, _k, _d in _ACTIONS}
ACTION_ARG = {name: kind for _n, name, kind, _d in _ACTIONS}
ACTION_NAME = {num: name for num, name, _k, _d in _ACTIONS}
SENSES = {name: num for num, name, _d in _SENSES}
GROUPS = {}                                                      # group -> {word: value}
for _g, _w, _v in _NAMES:
    GROUPS.setdefault(_g, {})[_w] = _v
# Names usable in any expression. The glow words (on, off, toggle) are only
# what follows `glow`: `on` also starts a handler.
CONSTS = {}
for _g, _words in GROUPS.items():
    if _g == "glow":
        continue
    for _w, _v in _words.items():
        if CONSTS.get(_w, _v) != _v:
            raise ValueError("jarvm.def: %r means two numbers (%s)" % (_w, _g))
        CONSTS[_w] = _v

# Version 2 of the language (2026-10-11: sounds, bursts, throwing, berries,
# signals, traits, world events) runs only on a jar.capp built with it. Until
# devices have that, generation stays inside version 1: what v1 had is the
# first V1_EVENTS events, V1_ACTIONS actions, V1_SENSES senses, and no BIT.
# CARDOS_JAR_V2=1 lets v2 out.
V2 = os.environ.get("CARDOS_JAR_V2") == "1"
V1_EVENTS, V1_ACTIONS, V1_SENSES = 8, 12, 9

FORMAT = 1
STACK = 8
STEPS = 64
MAX_ENTRIES = 16
SCRIPT_MAX = 256
MEM = 8
MAX_BUB = 4
MAX_FRAMES = 4

DONE, NONE, LIMIT, FAULT, IGNORE = 0, 1, -1, -2, -3
RESULT_NAME = {DONE: "done", NONE: "none", LIMIT: "limit", FAULT: "fault", IGNORE: "ignore"}

TICK = EVENTS["tick"]
INVISIBLE = {ACTIONS["nothing"], ACTIONS["wait"], ACTIONS["stop"], ACTIONS["signal"]}

_KEYWORDS = {"on", "end", "if", "then", "elif", "else", "and", "or", "not", "mem",
             "rand", "has", "return", "every", "to"}
for _w in list(SENSES) + list(CONSTS) + list(ACTIONS):
    if _w in _KEYWORDS:
        raise ValueError("jarvm.def: %r is a keyword of the language" % _w)
for _w in CONSTS:
    if _w in SENSES:
        raise ValueError("jarvm.def: %r is both a sense and a name" % _w)


def w16(v):
    """A value as the machine keeps it: 16 bits, two's complement."""
    return ((int(v) + 0x8000) & 0xFFFF) - 0x8000


def _cdiv(a, b):
    """C's division, toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def _cmod(a, b):
    """C's remainder: the sign of a."""
    return a - _cdiv(a, b) * b


# ---- the machine ------------------------------------------------------------

def _code0(s):
    n = len(s)
    if n < 2 or n > SCRIPT_MAX or s[0] != FORMAT or s[1] > MAX_ENTRIES:
        return -1
    c = 2 + 3 * s[1]
    return c if c <= n else -1


def check(s):
    """apps/jarvm.h's jv_check: 0, or IGNORE."""
    s = bytes(s)
    n, c = len(s), _code0(s)
    if c < 0:
        return IGNORE
    starts = set()
    pc = c
    while pc < n:
        op = s[pc]
        if op >= NOPS:
            return IGNORE
        k = OPS[op].nops
        if pc + k >= n:
            return IGNORE
        if op in (OP["LOAD"], OP["STORE"]) and s[pc + 1] >= MEM:
            return IGNORE
        if op == OP["SENSE"] and s[pc + 1] >= NSENSES:
            return IGNORE
        if op in (OP["ACT"], OP["ACTK"]) and s[pc + 1] >= NACTIONS:
            return IGNORE
        starts.add(pc)
        pc += 1 + k
    for pc in sorted(starts):
        if s[pc] in (OP["JMP"], OP["JZ"]) and s[pc + 1] not in starts:
            return IGNORE
    for e in range(s[1]):
        if s[4 + 3 * e] not in starts:
            return IGNORE
    return 0


def entry(s, ev, arg):
    """The code offset for (ev, arg), or -1: jv_entry."""
    for e in range(s[1]):
        t = s[2 + 3 * e: 5 + 3 * e]
        if t[0] != ev:
            continue
        if ev == TICK:
            if t[1] > 1 and _cmod(arg, t[1]):
                continue
        elif t[1] and t[1] != arg:
            continue
        return t[2]
    return -1


class Io:
    """What a script may reach. Subclass or pass callables."""

    def __init__(self, sense=None, act=None, rnd=None):
        self._sense, self._act, self._rnd = sense, act, rnd

    def sense(self, k):
        return self._sense(k) if self._sense else 0

    def act(self, a, arg):
        if self._act:
            self._act(a, arg)

    def rnd(self):
        return self._rnd() if self._rnd else 0


def run(s, ev, arg, mem, io):
    """apps/jarvm.h's jv_run, exactly. Returns (result, steps); `mem` (a list
    of 8 ints) is changed in place."""
    s = bytes(s)
    n, c = len(s), _code0(s)
    if c < 0:
        return IGNORE, 0
    pc = entry(s, ev, arg)
    if pc < 0:
        return NONE, 0
    st, steps = [], 0
    for i in range(STEPS):
        if pc < c or pc >= n or s[pc] >= NOPS:
            return FAULT, steps
        op = s[pc]
        o = OPS[op]
        if pc + o.nops >= n or len(st) < o.pops or len(st) - o.pops + o.pushes > STACK:
            return FAULT, steps
        a = s[pc + 1] if o.nops else 0
        b = s[pc + 2] if o.nops == 2 else 0
        pc += 1 + o.nops
        x = y = 0
        if o.pops == 2:
            y = st.pop()
        if o.pops:
            x = st.pop()
        steps = i + 1
        name = o.name
        if name == "END":
            return DONE, steps
        elif name == "PUSH8":
            x = a - 256 if a > 127 else a
        elif name == "PUSH16":
            x = w16(a | (b << 8))
        elif name == "LOAD":
            if a >= MEM:
                return FAULT, steps
            x = mem[a]
        elif name == "STORE":
            if a >= MEM:
                return FAULT, steps
            mem[a] = x
        elif name == "SENSE":
            if a >= NSENSES:
                return FAULT, steps
            x = max(-32768, min(32767, int(io.sense(a))))
        elif name == "RAND":
            x = (io.rnd() & 0xFFFFFFFF) % x if x > 0 else 0
        elif name == "ADD":
            x = x + y
        elif name == "SUB":
            x = x - y
        elif name == "MUL":
            x = x * y
        elif name == "DIV":
            x = _cdiv(x, y) if y else 0
        elif name == "MOD":
            x = _cmod(x, y) if y else 0
        elif name == "NEG":
            x = -x
        elif name == "EQ":
            x = int(x == y)
        elif name == "NE":
            x = int(x != y)
        elif name == "LT":
            x = int(x < y)
        elif name == "LE":
            x = int(x <= y)
        elif name == "GT":
            x = int(x > y)
        elif name == "GE":
            x = int(x >= y)
        elif name == "NOT":
            x = int(not x)
        elif name == "AND":
            x = int(bool(x) and bool(y))
        elif name == "OR":
            x = int(bool(x) or bool(y))
        elif name == "BIT":
            x = (x >> y) & 1 if 0 <= y <= 15 else 0
        elif name == "JMP":
            pc = a
        elif name == "JZ":
            if not x:
                pc = a
        elif name in ("ACT", "ACTK"):
            if a >= NACTIONS:
                return FAULT, steps
            io.act(a, b if name == "ACTK" else x)
        else:                                   # a new op in the .def, not here yet
            raise NotImplementedError("jarvm.py does not run %s" % name)
        if o.pushes:
            st.append(w16(x))
    return LIMIT, steps


def disassemble(s):
    """The bytes as text, one instruction a line, for people and tests."""
    s = bytes(s)
    c = _code0(s)
    if c < 0:
        return "not a script (format %r)" % (s[:1],)
    out = ["format %d, %d entries" % (s[0], s[1])]
    for e in range(s[1]):
        ev, f, off = s[2 + 3 * e: 5 + 3 * e]
        out.append("  on %s %d -> %d" % (EVENT_NAME.get(ev, "?%d" % ev), f, off))
    pc = c
    while pc < len(s):
        op = s[pc]
        if op >= NOPS:
            out.append("%4d  ?? %d" % (pc, op))
            pc += 1
            continue
        o = OPS[op]
        args = list(s[pc + 1: pc + 1 + o.nops])
        txt = o.name
        if o.name in ("ACT", "ACTK") and args:
            txt += " " + ACTION_NAME.get(args[0], "?%d" % args[0])
            args = args[1:]
        elif o.name == "SENSE" and args:
            txt += " " + _SENSES[args[0]][1] if args[0] < NSENSES else " ?"
            args = []
        out.append("%4d  %s%s" % (pc, txt, "".join(" %d" % v for v in args)))
        pc += 1 + o.nops
    return "\n".join(out)


# ---- the compiler -----------------------------------------------------------

class ScriptError(Exception):
    """A script that cannot be delivered. `line` is 1-based, 0 when the
    problem is the whole script (its size, or what the simulated day found);
    str() is the message to hand back to whoever wrote it."""

    def __init__(self, line, msg):
        self.line, self.msg = line, msg
        super().__init__(("line %d: %s" % (line, msg)) if line else msg)


_TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+) | (?P<comment>\#[^\n]*) | (?P<nl>\n) |
    (?P<num>\d+) | (?P<name>[A-Za-z_][A-Za-z_0-9]*) |
    (?P<op>==|!=|<=|>=|\+=|-=|[-+*/%<>=()\[\]:;,])
""", re.X)


def _tokens(src):
    toks, line, pos = [], 1, 0
    while pos < len(src):
        m = _TOKEN_RE.match(src, pos)
        if not m:
            raise ScriptError(line, "unexpected character %r" % src[pos])
        kind, text = m.lastgroup, m.group()
        pos = m.end()
        if kind in ("ws", "comment"):
            continue
        if kind == "nl":
            toks.append(("sep", "newline", line))
            line += 1
            continue
        if kind == "op" and text == ";":
            toks.append(("sep", ";", line))
            continue
        if kind == "name":
            text = text.lower()
        toks.append((kind, text, line))
    toks.append(("eof", "end of script", line))
    return toks


def _show(tok):
    kind, text, _l = tok
    if kind == "sep":
        return "the end of the line" if text == "newline" else "';'"
    if kind == "eof":
        return "the end of the script"
    return "'%s'" % text


def _fold(op, a, b=None):
    """Constant arithmetic, as the machine would do it."""
    if op == "neg":
        return w16(-a)
    if op == "not":
        return int(not a)
    return {
        "+": lambda: w16(a + b), "-": lambda: w16(a - b), "*": lambda: w16(a * b),
        "/": lambda: w16(_cdiv(a, b)) if b else 0, "%": lambda: w16(_cmod(a, b)) if b else 0,
        "==": lambda: int(a == b), "!=": lambda: int(a != b), "<": lambda: int(a < b),
        "<=": lambda: int(a <= b), ">": lambda: int(a > b), ">=": lambda: int(a >= b),
        "and": lambda: int(bool(a) and bool(b)), "or": lambda: int(bool(a) or bool(b)),
        "has": lambda: (a >> b) & 1 if 0 <= b <= 15 else 0,
    }[op]()


_BINOP = {"+": "ADD", "-": "SUB", "*": "MUL", "/": "DIV", "%": "MOD", "==": "EQ",
          "!=": "NE", "<": "LT", "<=": "LE", ">": "GT", ">=": "GE", "and": "AND", "or": "OR",
          "has": "BIT"}

_STMT_END = ("end", "else", "elif")


class _Parser:
    def __init__(self, src, nbub, nframes):
        self.t = _tokens(src)
        self.i = 0
        self.nbub = nbub
        self.nframes = nframes

    # -- tokens
    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def next(self):
        tok = self.t[self.i]
        if tok[0] != "eof":
            self.i += 1
        return tok

    def line(self):
        return self.peek()[2]

    def is_(self, text, kind=None):
        tok = self.peek()
        return tok[1] == text and (kind is None or tok[0] == kind) and tok[0] != "sep"

    def expect(self, text, what=None):
        tok = self.peek()
        if tok[1] != text or tok[0] == "sep":
            raise ScriptError(tok[2], "expected %s, found %s" % (what or "'%s'" % text, _show(tok)))
        return self.next()

    def skip_seps(self):
        while self.peek()[0] == "sep":
            self.next()

    def at_stmt_end(self):
        kind, text, _l = self.peek()
        return kind in ("sep", "eof") or (kind == "name" and text in _STMT_END)

    # -- the script
    def script(self):
        handlers = []
        self.skip_seps()
        while self.peek()[0] != "eof":
            handlers.append(self.handler())
            self.skip_seps()
        if not handlers:
            raise ScriptError(1, "the script has no handlers; start one with 'on poke:' or 'on tick:'")
        return handlers

    def handler(self):
        line = self.line()
        if not self.is_("on", "name"):
            raise ScriptError(line, "expected 'on EVENT:' to start a handler, found %s"
                              % _show(self.peek()))
        self.next()
        tok = self.next()
        if tok[0] != "name" or tok[1] not in EVENTS:
            raise ScriptError(tok[2], "expected an event after 'on' (%s), found %s"
                              % (", ".join(EVENTS), _show(tok)))
        ev = tok[1]
        filt = self.filter(ev)
        self.expect(":", "':' after 'on %s'" % ev)
        body = self.block(("end",))
        self.expect("end", "'end' to close 'on %s' (from line %d)" % (ev, line))
        return (EVENTS[ev], filt, body, line, ev)

    def filter(self, ev):
        kind = EVENT_FILTER[ev]
        tok = self.peek()
        if tok[1] == ":" or tok[0] != "name" and tok[0] != "num":
            return 0
        if kind == "every":
            if tok[1] != "every":
                raise ScriptError(tok[2], "after 'on tick' write ':' or 'every N:', found %s" % _show(tok))
            self.next()
            n = self.next()
            if n[0] != "num" or not 1 <= int(n[1]) <= 255:
                raise ScriptError(n[2], "expected a number of ticks 1..255 after 'every', found %s" % _show(n))
            return int(n[1])
        if kind == "hour":
            self.next()
            if tok[0] != "num" or not 0 <= int(tok[1]) <= 23:
                raise ScriptError(tok[2], "expected an hour 0..23 or ':' after 'on hour', found %s"
                                  % _show(tok))
            return int(tok[1]) + 1                      # 0 is "any": the hour is kept + 1
        if kind == "count":
            self.next()
            if tok[0] != "num" or not 1 <= int(tok[1]) <= 255:
                raise ScriptError(tok[2], "expected a number 1..255 or ':' after 'on %s', found %s"
                                  % (ev, _show(tok)))
            return int(tok[1])
        if kind in GROUPS:
            self.next()
            if kind == "weather" and tok[0] == "num":       # the tag itself, as before it had names
                if not 1 <= int(tok[1]) <= 255:
                    raise ScriptError(tok[2], "expected a weather tag 1..255 or ':', found %s" % _show(tok))
                return int(tok[1])
            if tok[1] not in GROUPS[kind]:
                raise ScriptError(tok[2], "after 'on %s' write ':' or one of %s, found %s"
                                  % (ev, ", ".join(GROUPS[kind]), _show(tok)))
            return GROUPS[kind][tok[1]]
        raise ScriptError(tok[2], "'on %s' takes nothing before ':', found %s" % (ev, _show(tok)))

    def block(self, enders):
        stmts = []
        while True:
            self.skip_seps()
            kind, text, line = self.peek()
            if kind == "eof":
                raise ScriptError(line, "the script ended inside a block: expected 'end'")
            if kind == "name" and text in enders:
                return stmts
            stmts.append(self.statement())
            if not self.at_stmt_end():
                raise ScriptError(self.line(), "expected ';' or a new line after the statement, found %s"
                                  % _show(self.peek()))

    def statement(self):
        kind, text, line = self.next()
        if kind == "name" and text == "if":
            return self.if_stmt(line)
        if kind == "name" and text == "return":
            return ("return", line)
        if kind == "name" and text == "mem":
            slot = self.slot()
            tok = self.next()
            if tok[1] not in ("=", "+=", "-="):
                raise ScriptError(tok[2], "expected '=' after mem[%d], found %s" % (slot, _show(tok)))
            return ("store", slot, tok[1], self.expr(), line)
        if kind == "name" and text in ACTIONS:
            return self.action(text, line)
        if kind == "name" and text == "on":
            raise ScriptError(line, "expected 'end' to close the handler before the next 'on'")
        if kind == "name" and text in ("else", "elif", "then"):
            raise ScriptError(line, "'%s' without an 'if'" % text)
        raise ScriptError(line, "unknown statement %s; statements are %s, mem[N] = ..., if ... then ... end, return"
                          % (_show((kind, text, line)), ", ".join(ACTIONS)))

    def if_stmt(self, line):
        arms = []
        cond = self.expr()
        if self.is_("=", "op"):
            raise ScriptError(self.line(), "use '==' to compare, not '='")
        self.expect("then", "'then' after the condition")
        arms.append((cond, self.block(("elif", "else", "end"))))
        other = None
        while True:
            tok = self.next()
            if tok[1] == "elif":
                cond = self.expr()
                self.expect("then", "'then' after the condition")
                arms.append((cond, self.block(("elif", "else", "end"))))
            elif tok[1] == "else":
                other = self.block(("end",))
                self.expect("end", "'end' to close the 'if' from line %d" % line)
                break
            else:
                break                               # 'end'
        return ("if", arms, other, line)

    def slot(self):
        self.expect("[", "'[' after 'mem'")
        tok = self.next()
        if tok[0] != "num" or int(tok[1]) >= MEM:
            raise ScriptError(tok[2], "expected a memory slot 0..7 in mem[...], found %s" % _show(tok))
        self.expect("]")
        return int(tok[1])

    def action(self, word, line):
        kind = ACTION_ARG[word]
        a = ACTIONS[word]
        if kind == "none":
            if not self.at_stmt_end():
                raise ScriptError(self.line(), "'%s' takes nothing after it, found %s" % (word, _show(self.peek())))
            return ("act", a, ("num", 0), line)
        if kind == "opt":
            if self.at_stmt_end():
                return ("act", a, ("num", 0), line)
            return ("act", a, self.expr(), line)
        if kind == "num":
            if self.at_stmt_end():
                raise ScriptError(line, "'%s' needs a number after it" % word)
            return ("act", a, self.expr(), line)
        if kind in ("bubble", "frame"):
            if self.at_stmt_end():
                raise ScriptError(line, "'%s' needs a number after it, counted from 1" % word)
            e = self.expr()
            top = (self.nbub if kind == "bubble" else self.nframes) or (MAX_BUB if kind == "bubble" else MAX_FRAMES)
            if e[0] == "num":
                if not 1 <= e[1] <= top:
                    raise ScriptError(line, "'%s %d': this item has %s 1..%d"
                                      % (word, e[1], "bubbles" if kind == "bubble" else "frames", top))
                return ("act", a, ("num", e[1] - 1), line)
            return ("act", a, ("bin", "-", e, ("num", 1)), line)
        # a name from a group: walk (to) ZONE, emit PARTICLE, glow on|off|toggle
        if word == "walk" and self.is_("to", "name"):
            self.next()
        tok = self.next()
        if tok[0] != "name" or tok[1] not in GROUPS[kind]:
            raise ScriptError(tok[2], "expected %s after '%s' (%s), found %s"
                              % ("a " + kind if kind != "glow" else "on, off or toggle", word,
                                 ", ".join(GROUPS[kind]), _show(tok)))
        return ("act", a, ("num", GROUPS[kind][tok[1]]), line)

    # -- expressions
    def expr(self):
        return self.binary(0)

    _LEVELS = [("or",), ("and",), None, ("==", "!=", "<", "<=", ">", ">="), ("+", "-"), ("*", "/", "%")]

    def binary(self, level):
        if level == 2:                              # not
            if self.is_("not", "name"):
                self.next()
                return self.mk1("not", self.binary(2))
            return self.binary(3)
        if level == len(self._LEVELS):
            return self.unary()
        ops = self._LEVELS[level]
        left = self.binary(level + 1)
        while self.peek()[1] in ops and self.peek()[0] in ("op", "name"):
            op = self.next()[1]
            right = self.binary(level + 1)
            left = self.mk2(op, left, right)
            if level == 3:                          # comparisons do not chain
                break
        return left

    def mk1(self, op, e):
        if e[0] == "num":
            return ("num", _fold(op, e[1]))
        return (op, e)

    def mk2(self, op, a, b):
        if a[0] == "num" and b[0] == "num":
            return ("num", _fold(op, a[1], b[1]))
        return ("bin", op, a, b)

    def unary(self):
        if self.is_("-", "op"):
            self.next()
            kind, text, line = self.peek()
            if kind == "num" and int(text) == 32768:   # the one number only negative
                self.next()
                return ("num", -32768)
            return self.mk1("neg", self.unary())
        return self.atom()

    def atom(self):
        kind, text, line = self.next()
        if kind == "num":
            v = int(text)
            if v > 32767:
                raise ScriptError(line, "%s is too big: numbers are -32768..32767" % text)
            return ("num", v)
        if kind == "op" and text == "(":
            e = self.expr()
            self.expect(")", "')'")
            return e
        if kind == "name":
            if text == "mem":
                return ("mem", self.slot())
            if text == "rand":
                self.expect("(", "'(' after 'rand'")
                e = self.expr()
                self.expect(")", "')' to close rand(...)")
                return ("rand", e)
            if text == "has":
                self.expect("(", "'(' after 'has'")
                e = self.expr()
                self.expect(",", "',' between has(TRAITS, TRAIT)")
                tok = self.next()
                if tok[0] != "name" or tok[1] not in GROUPS["trait"]:
                    raise ScriptError(tok[2], "expected a trait (%s) in has(..., TRAIT), found %s"
                                      % (", ".join(GROUPS["trait"]), _show(tok)))
                self.expect(")", "')' to close has(...)")
                return self.mk2("has", e, ("num", GROUPS["trait"][tok[1]]))
            if text in SENSES:
                return ("sense", SENSES[text])
            if text in CONSTS:
                return ("num", CONSTS[text])
            if text in ("true", "yes"):
                return ("num", 1)
            if text in ("false", "no"):
                return ("num", 0)
        raise ScriptError(line, "expected a number, a sense (%s), mem[N], rand(N), has(X, TRAIT) or a name, found %s"
                          % (", ".join(SENSES), _show((kind, text, line))))


class _Gen:
    def __init__(self, base):
        self.base = base
        self.code = bytearray()

    def at(self):
        return self.base + len(self.code)

    def emit(self, name, *args):
        self.code.append(OP[name])
        for v in args:
            self.code.append(v & 0xFF)

    def push(self, v):
        if -128 <= v <= 127:
            self.emit("PUSH8", v)
        else:
            self.emit("PUSH16", v & 0xFF, (v >> 8) & 0xFF)

    def jump(self, name):
        self.emit(name, 0)
        return len(self.code) - 1                   # where to patch

    def patch(self, where, target):
        if target > 255:
            raise ScriptError(0, "the script is too long: over %d bytes" % SCRIPT_MAX)
        self.code[where] = target

    def expr(self, e):
        k = e[0]
        if k == "num":
            self.push(e[1])
        elif k == "mem":
            self.emit("LOAD", e[1])
        elif k == "sense":
            self.emit("SENSE", e[1])
        elif k == "rand":
            self.expr(e[1])
            self.emit("RAND")
        elif k == "neg":
            self.expr(e[1])
            self.emit("NEG")
        elif k == "not":
            self.expr(e[1])
            self.emit("NOT")
        else:
            self.expr(e[2])
            self.expr(e[3])
            self.emit(_BINOP[e[1]])

    def stmts(self, body):
        for s in body:
            self.stmt(s)

    def stmt(self, s):
        k = s[0]
        if k == "return":
            self.emit("END")
        elif k == "store":
            _k, slot, op, e, _l = s
            if op == "=":
                self.expr(e)
            else:
                self.emit("LOAD", slot)
                self.expr(e)
                self.emit("ADD" if op == "+=" else "SUB")
            self.emit("STORE", slot)
        elif k == "act":
            _k, a, e, _l = s
            if e[0] == "num" and 0 <= e[1] <= 255:
                self.emit("ACTK", a, e[1])
            else:
                self.expr(e)
                self.emit("ACT", a)
        elif k == "if":
            _k, arms, other, _l = s
            ends = []
            for n, (cond, body) in enumerate(arms):
                last = n == len(arms) - 1 and other is None
                self.expr(cond)
                skip = self.jump("JZ")
                self.stmts(body)
                if not last:
                    ends.append(self.jump("JMP"))
                self.patch(skip, self.at())
            if other is not None:
                self.stmts(other)
            for w in ends:
                self.patch(w, self.at())


def v2_words(code):
    """What in compiled `code` needs language version 2: [word], empty if none."""
    out = []
    n = code[1]
    for e in range(n):
        if code[2 + 3 * e] >= V1_EVENTS:
            out.append("on " + EVENT_NAME[code[2 + 3 * e]])
    pc = 2 + 3 * n
    while pc < len(code):
        op = OPS[code[pc]]
        if op.name == "BIT":
            out.append("has()")
        elif op.name in ("ACT", "ACTK") and code[pc + 1] >= V1_ACTIONS:
            out.append(ACTION_NAME[code[pc + 1]])
        elif op.name == "SENSE" and code[pc + 1] >= V1_SENSES:
            out.append(_SENSES[code[pc + 1]][1])
        pc += 1 + op.nops
    return list(dict.fromkeys(out))


def compile_script(src, nbub=None, nframes=None, v2=None):
    """The readable language into bytecode (bytes). `nbub` and `nframes`, when
    given, are the item's, and `say N` / `frame N` are checked against them.
    Without v2 (the default is V2), a word of version 2 is refused.
    Raises ScriptError(line, message)."""
    p = _Parser(src, nbub, nframes)
    handlers = p.script()
    seen = {}
    for ev, filt, _body, line, word in handlers:
        if (ev, filt) in seen:
            raise ScriptError(line, "a second 'on %s%s' handler (the first is on line %d); "
                              "one per event" % (word, " ..." if filt else "", seen[(ev, filt)]))
        seen[(ev, filt)] = line
    if len(handlers) > MAX_ENTRIES:
        raise ScriptError(0, "%d handlers; at most %d" % (len(handlers), MAX_ENTRIES))
    # A narrower handler is found first: filtered before unfiltered, a longer
    # tick period before a shorter one.
    order = sorted(range(len(handlers)),
                   key=lambda k: (handlers[k][0], handlers[k][1] == 0, -handlers[k][1], k))
    base = 2 + 3 * len(handlers)
    g = _Gen(base)
    starts = {}
    for k in order:
        starts[k] = g.at()
        g.stmts(handlers[k][2])
        g.emit("END")
    out = bytearray([FORMAT, len(handlers)])
    for k in order:
        if starts[k] > 255:
            raise ScriptError(0, "the script is too long: over %d bytes" % SCRIPT_MAX)
        out += bytes([handlers[k][0], handlers[k][1], starts[k]])
    out += g.code
    if len(out) > SCRIPT_MAX:
        raise ScriptError(0, "the script is %d bytes; at most %d -- make it shorter" % (len(out), SCRIPT_MAX))
    if check(out) != 0:                             # a compiler bug, never the writer's
        raise AssertionError("compiled a script the machine would ignore:\n" + disassemble(out))
    code = bytes(out)
    if not (V2 if v2 is None else v2):
        newer = v2_words(code)
        if newer:
            raise ScriptError(0, "%s: not in this version of the language" % ", ".join(newer))
    return code


# ---- the simulated day ------------------------------------------------------

def xorshift(seed):
    """The jar's random numbers (js_rnd in apps/jarsim.h)."""
    state = [seed & 0xFFFFFFFF]

    def rnd():
        x = state[0] or 0x9E3779B9
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        state[0] = x
        return x
    return rnd


class DayReport:
    def __init__(self):
        self.ok = True
        self.problems = []
        self.runs = 0           # entries run
        self.events = {}        # event word -> times fired
        self.actions = {}       # action word -> times done
        self.visible = 0
        self.max_steps = 0
        self.mem = [0] * MEM

    def problem(self, msg):
        if msg not in self.problems:
            self.problems.append(msg)
        self.ok = False

    def __repr__(self):
        return "DayReport(ok=%r, problems=%r, visible=%d, max_steps=%d)" % (
            self.ok, self.problems, self.visible, self.max_steps)


_ZONE_X = [(0, 78, 0), (78, 98, 3), (98, 178, 1), (178, 240, 2)]  # js_zone_of


def simulate_day(script, nbub=MAX_BUB, nframes=MAX_FRAMES, seed=1, ticks_per_hour=120,
                 gift=False, days=3):
    """Fire a day of events at a compiled script -- compressed: each hour is
    `ticks_per_hour` ticks, with nears, shipped jars, a jam and its fix,
    pokes, a gift, a weather change and dawn, day, dusk and night among
    them -- and report. The script fails the day if it faults, hits the step
    limit, says a bubble the item does not have, or does nothing visible."""
    rep = DayReport()
    if check(script) != 0:
        rep.problem("the bytecode is not a script this machine would run")
        return rep
    rnd = xorshift(seed)
    world = {"x": 120, "near": 1, "dist": 40, "time": 0, "weather": 0, "hour": 0, "berries": 2,
             "fed": 0, "world": 0, "neartraits": 0, "newtraits": 0, "stuck": 0, "sulking": 0}
    mem = [0] * MEM
    current = [None]

    def sense(k):
        name = _SENSES[k][1]
        if name == "x":
            return world["x"]
        if name == "zone":
            return next(z for lo, hi, z in _ZONE_X if lo <= world["x"] < hi)
        if name == "near":
            return world["near"]
        if name == "distance":
            return world["dist"]
        if name == "time":
            return world["time"]
        if name == "weather":
            return world["weather"]
        if name == "days":
            return days
        if name == "gift":
            return int(gift)
        if name == "random":
            return rnd() & 255
        if name in world:
            return world[name]
        if name == "traits":
            return 0x22                                 # shiny, light: something to test
        if name == "playing":
            return 1
        return 0

    def act(a, arg):
        word = ACTION_NAME[a]
        rep.actions[word] = rep.actions.get(word, 0) + 1
        if a not in INVISIBLE:
            rep.visible += 1
        if word == "say" and not 0 <= arg < nbub:
            rep.problem("on %s: 'say %d', but the item has %d bubble%s"
                        % (current[0], arg + 1, nbub, "" if nbub == 1 else "s"))
        if word == "walk":
            lo, hi = {0: (8, 80), 1: (100, 176), 2: (180, 224), 3: (78, 96)}.get(arg, (10, 226))
            world["x"] = lo + rnd() % (hi - lo + 1)
        if word == "eat" and world["berries"] > 0:
            world["berries"] -= 1
            world["fed"] += 1
        if word == "sound" and not 1 <= arg <= len(GROUPS["sound"]):
            rep.problem("on %s: 'sound %d' is not a sound" % (current[0], arg))
        if word == "signal" and not 1 <= arg <= 255:
            rep.problem("on %s: 'signal %d': a signal is 1..255" % (current[0], arg))

    io = Io(sense, act, rnd)

    def fire(word, arg):
        ev = EVENTS[word]
        current[0] = word
        rep.events[word] = rep.events.get(word, 0) + 1
        r, steps = run(script, ev, arg, mem, io)
        rep.max_steps = max(rep.max_steps, steps)
        if r == NONE:
            return
        rep.runs += 1
        if r == LIMIT:
            rep.problem("on %s: ran %d instructions without reaching its end (too long, or a loop)"
                        % (word, STEPS))
        elif r == FAULT:
            rep.problem("on %s: the machine faulted (stack or jump)" % word)
        elif r == IGNORE:
            rep.problem("the script was ignored by the machine")

    phases = {5: 1, 8: 2, 17: 3, 20: 4}             # dawn, day, dusk, night (js_phase_of)
    tick = 0
    for hour in range(24):
        world["hour"] = hour
        fire("hour", hour + 1)
        if hour in phases:
            world["time"] = phases[hour]
            fire("time", phases[hour])
        if hour == 11:
            world["newtraits"] = (1 << GROUPS["trait"]["food"]) | (1 << GROUPS["trait"]["sweet"])
            fire("new", 0)
        if hour == 15:
            world["world"] = GROUPS["world"]["ants"]
            fire("world", world["world"])
        if hour == 16:
            world["world"] = 0
        if hour == 18:
            world["world"] = GROUPS["world"]["visitor"]
            fire("world", world["world"])
            world["world"] = 0
        if hour == 7:
            world["weather"] = GROUPS["weather"]["rainy"]
            fire("weather", world["weather"])
        if hour == 12:
            fire("gift", 0)
        for t in range(ticks_per_hour):
            fire("tick", tick)
            tick += 1
            if t % 30 == 7:
                world["near"] = 1 + rnd() % 4
                world["dist"] = rnd() % 17
                world["neartraits"] = (rnd() & 0xFFFF) if world["near"] >= 3 else 0
                fire("near", world["near"])
            if t % 45 == 30:
                world["berries"] += 1
                fire("berry", 0)
            if t % 60 == 41:
                fire("signal", 1 + rnd() % 3)
            if hour == 14 and t == 12:
                fire("bumped", 0)
            else:
                world["dist"] = 17 + rnd() % 60
            if t % 40 == 20:
                fire("shipped", 0)
            if hour == 10 and t == 50:
                fire("jam", 1)
            if hour == 10 and t == 66:
                fire("jam", 2)
            if hour in (9, 13, 21) and t == 90:
                fire("poke", 0)
    if rep.ok and rep.visible == 0:
        rep.problem("it never did anything visible in a whole day: no hop, walk, float, face, "
                    "frame, flip, glow, emit or say ever ran")
    rep.mem = list(mem)
    return rep


def prepare_script(src, nbub=MAX_BUB, nframes=MAX_FRAMES, v2=None):
    """What generation calls: compile, check, and live through a day. The
    bytecode, or ScriptError whose message lists every problem."""
    code = compile_script(src, nbub, nframes, v2)
    rep = simulate_day(code, nbub, nframes)
    if not rep.ok:
        raise ScriptError(0, "the script failed its simulated day: " + "; ".join(rep.problems))
    return code


# ---- the guide, for the generation prompt -----------------------------------

def _guide(v2=True):
    acts = []
    for _n, word, kind, doc in _ACTIONS:
        if word == "nothing" or (not v2 and _n >= V1_ACTIONS):
            continue
        form = {
            "none": word, "opt": "%s [N]" % word, "num": "%s N" % word,
            "bubble": "%s N" % word, "frame": "%s N" % word,
            "zone": "%s to ZONE" % word, "particle": "%s PARTICLE" % word,
            "glow": "%s on|off|toggle" % word, "sound": "%s SOUND" % word,
            "trait": "%s TRAIT" % word,
        }[kind]
        acts.append("  %-18s %s" % (form, doc))
    evs = []
    for _n, word, filt, doc in _EVENTS:
        if not v2 and _n >= V1_EVENTS:
            continue
        form = {"none": "on %s:" % word, "every": "on tick: / on tick every N:",
                "hour": "on %s: / on %s 0..23:" % (word, word),
                "count": "on %s: / on %s N:" % (word, word)}.get(
            filt, "on %s: / on %s %s:" % (word, word, "|".join(GROUPS.get(filt, {}))))
        evs.append("  %-34s %s" % (form, doc))
    sens = ["  %-10s %s" % (word, doc) for _n, word, doc in _SENSES if v2 or _n < V1_SENSES]
    if v2:
        names = ("; world events " + ", ".join(GROUPS["world"]) + "; sounds " +
                 ", ".join(GROUPS["sound"]) + "; traits " + ", ".join(GROUPS["trait"]) + ".")
        extra = ["Traits are what an item is (its record lists its own); scripts react to traits,",
                 "not to names, so an item meets things made after it:",
                 "  on near: if has(neartraits, food) then seek food; eat end; end"]
        has = ["EXPRESSIONS: numbers -32768..32767, mem[K], rand(N) (0..N-1), has(X, TRAIT)",
               "(1 if the traits X include TRAIT), the senses"]
    else:
        names, extra = ".", []
        has = ["EXPRESSIONS: numbers -32768..32767, mem[K], rand(N) (0..N-1), the senses"]
    return "\n".join([
        "JAR SCRIPT -- a tiny language for one item's behaviour. Compiled to at most",
        "256 bytes; every handler run may take at most 64 instructions.",
        "",
        "A script is handlers. Each starts 'on EVENT:' and ends 'end'; statements",
        "are separated by new lines or ';'. One handler runs per event; an event",
        "with no handler is left to the item's recipe. # starts a comment.",
        "",
        "EVENTS",
    ] + evs + [
        "",
        "STATEMENTS (actions; N is any expression)",
    ] + acts + [
        "  mem[K] = EXPR      remember a number; K is 0..7, kept while the item lives",
        "  mem[K] += EXPR     (and -=)",
        "  if C then ... elif C then ... else ... end",
        "  return             stop here",
        "",
    ] + has + [
        "below, + - * / % (whole numbers), == != < <= > >=, and or not, ( ).",
        "Names for numbers: zones " + ", ".join(GROUPS["zone"]) + "; particles " +
        ", ".join(GROUPS["particle"]) + "; kinds " + ", ".join(GROUPS["near"]) +
        "; times " + ", ".join(GROUPS["time"]) + "; weathers " + ", ".join(GROUPS["weather"]) +
        " (the real weather where the owner lives; 0 when not known)" + names,
    ] + extra + [
        "",
        "SENSES",
    ] + sens + [
        "",
        "There are no loops. Bubbles and frames count from 1. Keep handlers short:",
        "on tick runs 4 times a second, so act there only now and then (rand).",
        "A script must do something visible within a day.",
        "",
        "EXAMPLE",
        "  on poke:",
        "    say 1; hop",
        "  end",
        "  on tick:",
        "    if rand(100) < 3 then emit sparkle end",
        "  end",
        "  on near snail:",
        "    mem[0] += 1",
        "    if mem[0] > 3 then say 2; mem[0] = 0 end",
        "  end",
        "  on time night:",
        "    glow on",
        "  end",
        "  on time dawn: glow off; end",
    ])


LANGUAGE_GUIDE_V1 = _guide(False)
LANGUAGE_GUIDE_V2 = _guide(True)
LANGUAGE_GUIDE = LANGUAGE_GUIDE_V2 if V2 else LANGUAGE_GUIDE_V1
