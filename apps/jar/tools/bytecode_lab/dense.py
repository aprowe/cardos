"""Candidate format 2 ("dense"): today's machine, a better byte code.

Same stack machine, same senses/actions/memory, same compiler front end --
only the encoding changes. Built by re-encoding format-1 bytecode, so every
number here is for exactly the scripts the server compiled.

HEADER
  byte 0  format (2), byte 1 entries
  entry   1 byte: bits 0-4 event, bits 5-7 filter 0..6 inline (7: a filter
          byte follows), then 1 byte: the handler's code offset
  A handler ends where the next handler's code begins (or the script ends):
  reaching that point is END, so the compiler drops each handler's last END.

OPCODES (one byte; operands follow)
  0x00-0x1A  format 1's ops, unchanged, except JMP/JZ take a forward
             offset from the end of the instruction instead of an absolute
             byte (relocatable: a dictionary entry can be copied anywhere)
  0x20-0x3F  PUSHS: push op-0x24 (-4..27) -- no operand
  0x40-0x47  LOAD k     0x48-0x4F  STORE k
  0x50-0x6F  SENSE k (32 senses)
  0x70-0x75  JN<cmp> rel  -- pop b, a; jump unless a <cmp> b  (EQ NE LT LE GT GE)
  0x76       JNBIT rel  -- pop k, a; jump unless bit k of a
  0x77       JNZ rel    -- pop a; jump if a != 0 (NOT; JZ)
  0x78-0x7F  ADDM k, s8 -- mem[k] += s8 (LOAD k; PUSH c; ADD|SUB; STORE k)
  0x80-0xFF  ACT1 i     -- action ACT1_TABLE[i] with its fixed argument
"""
import isa
from isa import V

FORMAT2 = 2
CMPS = ["EQ", "NE", "LT", "LE", "GT", "GE"]


def _act1_table():
    """The 128 one-byte actions: (action word, wire argument). Built from the
    enumerations in jarvm.def plus round numbers -- not tuned to the corpus."""
    t = []
    A = V.ACTION_ARG
    for word, kind in A.items():
        if kind == "none":
            t.append((word, 0))
        elif kind in ("bubble", "frame"):
            t += [(word, g) for g in range(4)]
        elif kind in ("zone", "particle", "glow", "sound", "trait"):
            t += [(word, g) for g in sorted(V.GROUPS[kind].values())]
        elif word == "hop":
            t += [(word, g) for g in (0, 2, 4, 6, 8, 10, 12, 16)]
        elif word == "wait":
            t += [(word, g) for g in (1, 2, 4, 8, 12, 16, 20, 40)]
        elif word == "signal":
            t += [(word, g) for g in range(1, 9)]
        elif kind == "opt":
            t.append((word, 0))
    assert len(t) <= 128, len(t)
    return t


ACT1_TABLE = _act1_table()
ACT1 = {(V.ACTIONS[w], g): i for i, (w, g) in enumerate(ACT1_TABLE)}


def use_table(module):
    """Point the encoder at another instruction table (labvm's extended one)."""
    global V, ACT1_TABLE, ACT1
    V = module
    ACT1_TABLE = _act1_table()
    ACT1 = {(V.ACTIONS[w], g): i for i, (w, g) in enumerate(ACT1_TABLE)}


# ---- the peephole: format-1 instructions -> format-2 tokens ----------------------------

class Tok:
    __slots__ = ("kind", "a", "b", "target", "size")

    def __init__(self, kind, a=None, b=None, target=None):
        self.kind, self.a, self.b, self.target = kind, a, b, target

    def __repr__(self):
        return "%s(%s,%s%s)" % (self.kind, self.a, self.b,
                                "" if self.target is None else " ->%d" % self.target)


ALL = frozenset(["addm", "jcc", "pushs", "short", "act1", "dropend"])


def lower(entries, ins, fuse=True, feats=None):
    """Format-1 instructions -> (entries with token indexes, tokens, ends). Jump
    targets become token indexes; a handler's final END is dropped."""
    F = (ALL if fuse else frozenset(["dropend"])) if feats is None else frozenset(feats)
    starts = sorted(set(e[2] for e in entries))
    bounds = set(starts[1:] + [len(ins)])
    toks, at = [], {}
    k = 0
    while k < len(ins):
        at[k] = len(toks)
        i = ins[k]
        nxt = ins[k + 1] if k + 1 < len(ins) else None
        # ADDM: LOAD k; PUSH c; ADD|SUB; STORE k (no jump lands inside)
        if "addm" in F and k + 3 < len(ins) and i.op == "LOAD" and isa.push_value(ins[k + 1]) is not None \
                and ins[k + 2].op in ("ADD", "SUB") and ins[k + 3].op == "STORE" \
                and ins[k + 3].args[0] == i.args[0]:
            c = isa.push_value(ins[k + 1]) * (1 if ins[k + 2].op == "ADD" else -1)
            if -128 <= c <= 127:
                toks.append(Tok("ADDM", i.args[0], c))
                for j in (1, 2, 3):
                    at[k + j] = None                       # nothing may jump here
                k += 4
                continue
        if "jcc" in F and nxt is not None and nxt.op == "JZ" and i.op in CMPS + ["BIT", "NOT"]:
            toks.append(Tok("JN" + i.op if i.op != "NOT" else "JNZ", target=nxt.target))
            at[k + 1] = None
            k += 2
            continue
        if "dropend" in F and i.op == "END" and k + 1 in bounds:
            at[k] = len(toks)                               # a jump here is a jump to the end
            k += 1
            continue
        v = isa.push_value(i)
        if v is not None:
            toks.append(Tok("PUSHS", v) if "pushs" in F and -4 <= v <= 27 else Tok(i.op, v))
        elif "short" in F and i.op in ("LOAD", "STORE", "SENSE"):
            toks.append(Tok(i.op + "K", i.args[0]))
        elif i.op == "ACTK" and "act1" in F and (i.args[0], i.args[1]) in ACT1:
            toks.append(Tok("ACT1", ACT1[(i.args[0], i.args[1])]))
        elif i.op in ("JMP", "JZ"):
            toks.append(Tok(i.op, target=i.target))
        else:
            toks.append(Tok(i.op, *i.args))
        k += 1
    at[len(ins)] = len(toks)
    for t in toks:
        if t.target is not None:
            assert at.get(t.target) is not None, "a jump into a fused group"
            t.target = at[t.target]
    ents = [(ev, f, at[ix]) for ev, f, ix in entries]
    return ents, toks


_SIMPLE = {"END": 0, "RAND": 6, "ADD": 7, "SUB": 8, "MUL": 9, "DIV": 10, "MOD": 11, "NEG": 12,
           "EQ": 13, "NE": 14, "LT": 15, "LE": 16, "GT": 17, "GE": 18, "NOT": 19, "AND": 20,
           "OR": 21, "BIT": 26}


def tok_bytes(t, rel=0):
    k = t.kind
    if k in _SIMPLE:
        return bytes([_SIMPLE[k]])
    if k == "PUSH8":
        return bytes([1, t.a & 0xFF])
    if k == "PUSH16":
        return bytes([2, t.a & 0xFF, (t.a >> 8) & 0xFF])
    if k in ("LOAD", "STORE", "SENSE"):
        return bytes([V.OP[k], t.a])
    if k in ("ACT", "ACTK"):
        return bytes([V.OP[k], t.a] + ([t.b] if k == "ACTK" else []))
    if k == "PUSHS":
        return bytes([0x24 + t.a])
    if k == "LOADK":
        return bytes([0x40 + t.a])
    if k == "STOREK":
        return bytes([0x48 + t.a])
    if k == "SENSEK":
        return bytes([0x50 + t.a])
    if k == "ACT1":
        return bytes([0x80 + t.a])
    if k == "ADDM":
        return bytes([0x78 + t.a, t.b & 0xFF])
    if k in ("JMP", "JZ"):
        return bytes([V.OP[k], rel])
    if k.startswith("JN"):
        code = {"JNBIT": 0x76, "JNZ": 0x77}.get(k)
        if code is None:
            code = 0x70 + CMPS.index(k[2:])
        return bytes([code, rel])
    raise ValueError(k)


def layout(ents, toks):
    """Bytes for the whole script."""
    hdr = bytearray([FORMAT2, len(ents)])
    offs_at = []
    for ev, f, _ix in ents:
        if 0 <= f <= 6:
            hdr.append(ev | (f << 5))
        else:
            hdr += bytes([ev | (7 << 5), f])
        offs_at.append(len(hdr))
        hdr.append(0)
    base = len(hdr)
    pos, p = [], base
    for t in toks:                                          # every token's size is fixed
        pos.append(p)
        p += len(tok_bytes(t))
    pos.append(p)
    out = bytearray(hdr)
    for t in toks:
        here = len(out)
        if t.target is not None:
            rel = pos[t.target] - (here + 2)
            assert 0 <= rel <= 255, rel
            out += tok_bytes(t, rel)
        else:
            out += tok_bytes(t)
    for (ev, f, ix), o in zip(ents, offs_at):
        out[o] = pos[ix]
    return bytes(out)


def encode(code, fuse=True, feats=None):
    e, ins = isa.decode(code)
    ents, toks = lower(e, ins, fuse, feats)
    return layout(ents, toks)


def code_cost(code, feats):
    """Code bytes (no header) with only `feats` switched on."""
    e, ins = isa.decode(code)
    _ents, toks = lower(e, ins, feats=feats)
    return sum(len(tok_bytes(t)) for t in toks)


def header_cost(code, compact=True):
    """Header bytes for the script's entries: format 1 (3 each) or format 2."""
    n = code[1]
    if not compact:
        return 2 + 3 * n
    return 2 + sum(2 + (0 if 0 <= code[3 + 3 * e] <= 6 else 1) for e in range(n))


# ---- the machine for format 2, to prove the encoding means the same --------------------

def _decode_one(s, pc):
    """(name, args, length) of the instruction at pc."""
    b = s[pc]
    if b <= 0x1A:
        o = V.OPS[b]
        return o.name, list(s[pc + 1: pc + 1 + o.nops]), 1 + o.nops
    if 0x20 <= b <= 0x3F:
        return "PUSHS", [b - 0x24], 1
    if 0x40 <= b <= 0x47:
        return "LOAD", [b - 0x40], 1
    if 0x48 <= b <= 0x4F:
        return "STORE", [b - 0x48], 1
    if 0x50 <= b <= 0x6F:
        return "SENSE", [b - 0x50], 1
    if 0x70 <= b <= 0x75:
        return "JN" + CMPS[b - 0x70], [s[pc + 1]], 2
    if b == 0x76:
        return "JNBIT", [s[pc + 1]], 2
    if b == 0x77:
        return "JNZ", [s[pc + 1]], 2
    if 0x78 <= b <= 0x7F:
        return "ADDM", [b - 0x78, isa.s8(s[pc + 1])], 2
    if b >= 0x80:
        return "ACT1", [b - 0x80], 1
    raise ValueError("unknown op %#x" % b)


def entries2(s):
    n, p, out = s[1], 2, []
    for _ in range(n):
        ev, f = s[p] & 31, (s[p] >> 5) & 7
        p += 1
        if f == 7:
            f = s[p]
            p += 1
        out.append((ev, f, s[p]))
        p += 1
    return out, p


def run2(s, ev, arg, mem, io, steps_max=64):
    s = bytes(s)
    ents, c0 = entries2(s)
    pc = None
    for e, f, off in ents:
        if e != ev:
            continue
        if ev == V.TICK:
            if f > 1 and V._cmod(arg, f):
                continue
        elif f and f != arg:
            continue
        pc = off
        break
    if pc is None:
        return V.NONE, 0
    end = min([o for _e, _f, o in ents if o > pc] + [len(s)])
    st = []
    cmp = {"EQ": lambda a, b: a == b, "NE": lambda a, b: a != b, "LT": lambda a, b: a < b,
           "LE": lambda a, b: a <= b, "GT": lambda a, b: a > b, "GE": lambda a, b: a >= b}
    for i in range(steps_max):
        if pc == end:
            return V.DONE, i
        name, a, ln = _decode_one(s, pc)
        pc += ln
        if name == "END":
            return V.DONE, i + 1
        if name in ("PUSH8",):
            st.append(isa.s8(a[0]))
        elif name == "PUSH16":
            st.append(V.w16(a[0] | a[1] << 8))
        elif name == "PUSHS":
            st.append(a[0])
        elif name == "LOAD":
            st.append(mem[a[0]])
        elif name == "STORE":
            mem[a[0]] = st.pop()
        elif name == "SENSE":
            st.append(max(-32768, min(32767, int(io.sense(a[0])))))
        elif name == "RAND":
            x = st.pop()
            st.append((io.rnd() & 0xFFFFFFFF) % x if x > 0 else 0)
        elif name in ("ADD", "SUB", "MUL", "DIV", "MOD", "EQ", "NE", "LT", "LE", "GT", "GE",
                      "AND", "OR", "BIT"):
            y, x = st.pop(), st.pop()
            r = {"ADD": lambda: x + y, "SUB": lambda: x - y, "MUL": lambda: x * y,
                 "DIV": lambda: V._cdiv(x, y) if y else 0, "MOD": lambda: V._cmod(x, y) if y else 0,
                 "AND": lambda: int(bool(x) and bool(y)), "OR": lambda: int(bool(x) or bool(y)),
                 "BIT": lambda: (x >> y) & 1 if 0 <= y <= 15 else 0}.get(name)
            st.append(V.w16(r() if r else int(cmp[name](x, y))))
        elif name == "NEG":
            st.append(V.w16(-st.pop()))
        elif name == "NOT":
            st.append(int(not st.pop()))
        elif name == "JMP":
            pc += a[0]
        elif name == "JZ":
            if not st.pop():
                pc += a[0]
        elif name.startswith("JN") and name[2:] in cmp:
            y, x = st.pop(), st.pop()
            if not cmp[name[2:]](x, y):
                pc += a[0]
        elif name == "JNBIT":
            y, x = st.pop(), st.pop()
            if not ((x >> y) & 1 if 0 <= y <= 15 else 0):
                pc += a[0]
        elif name == "JNZ":
            if st.pop():
                pc += a[0]
        elif name == "ADDM":
            mem[a[0]] = V.w16(mem[a[0]] + a[1])
        elif name == "ACT":
            io.act(a[0], st.pop())
        elif name == "ACTK":
            io.act(a[0], a[1])
        elif name == "ACT1":
            w, g = ACT1_TABLE[a[0]]
            io.act(V.ACTIONS[w], g)
        else:
            raise ValueError(name)
        if len(st) > 8:
            return V.FAULT, i + 1
    return V.LIMIT, steps_max
