"""Which part of format 2 saves what: each feature alone on top of format 1,
and format 2 with each one taken away.

    python tools/bytecode_lab/ablation.py
"""
import corpus
import dense

NAMES = {"hdr": "compact entries (event+filter in 1 byte)", "dropend": "implicit END at handler end",
         "act1": "ACT1: action+argument in one byte (128-entry table)",
         "pushs": "PUSHS: small constant in the opcode", "short": "LOAD/STORE/SENSE k in the opcode",
         "jcc": "compare-and-branch (cmp/BIT/NOT + JZ fused)", "addm": "ADDM: mem[k] += c"}


def total(c, feats, compact):
    return sum(dense.header_cost(s["code"], compact) + dense.code_cost(s["code"], feats) for s in c)


def main():
    c = corpus.load()
    base = sum(len(s["code"]) for s in c)
    assert total(c, [], False) == base, (total(c, [], False), base)
    full = total(c, dense.ALL, True)
    print("format 1: %d   format 2: %d (%.1f%%)\n" % (base, full, 100.0 * full / base))
    print("%-52s %10s %14s" % ("feature", "alone", "removed from 2"))
    for f in ["hdr"] + sorted(dense.ALL):
        alone = total(c, [f] if f != "hdr" else [], f == "hdr")
        without = total(c, dense.ALL - {f}, f != "hdr")
        print("%-52s %7d (-%4.1f%%) %7d (+%4.1f%%)" % (NAMES[f], alone, 100.0 * (base - alone) / base,
                                                    without, 100.0 * (without - full) / base))


if __name__ == "__main__":
    main()
