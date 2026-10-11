"""Today's bytecode (format 1) taken apart: the header's entries and the code
as a list of instructions with jump targets turned into instruction indexes,
so an encoder can lay them out again in another format."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
os.environ.setdefault("CARDOS_JAR_V2", "1")
from server import jarvm as V  # noqa: E402

OPS = V.OPS
OPNAME = {o.code: o.name for o in OPS}


class Ins:
    __slots__ = ("op", "args", "pc", "target")

    def __init__(self, op, args, pc):
        self.op, self.args, self.pc, self.target = op, args, pc, None   # target: index

    def __repr__(self):
        return "%s%s%s" % (self.op, "".join(" %d" % a for a in self.args),
                           " ->%d" % self.target if self.target is not None else "")


def decode(code):
    """(entries [(event, filter, ins index)], [Ins])."""
    code = bytes(code)
    n = code[1]
    c0 = 2 + 3 * n
    ins, at = [], {}
    pc = c0
    while pc < len(code):
        o = OPS[code[pc]]
        at[pc] = len(ins)
        ins.append(Ins(o.name, list(code[pc + 1: pc + 1 + o.nops]), pc))
        pc += 1 + o.nops
    at[len(code)] = len(ins)
    for i in ins:
        if i.op in ("JMP", "JZ"):
            i.target = at[i.args[0]]
    entries = []
    for e in range(n):
        ev, f, off = code[2 + 3 * e: 5 + 3 * e]
        entries.append((ev, f, at[off]))
    return entries, ins


def s8(v):
    return v - 256 if v > 127 else v


def push_value(i):
    if i.op == "PUSH8":
        return s8(i.args[0])
    if i.op == "PUSH16":
        return V.w16(i.args[0] | (i.args[1] << 8))
    return None
