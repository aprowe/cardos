"""Prove a re-encoding means the same: run jarvm.simulate_day on the format-1
bytes with the real machine, and on the re-encoded bytes with the
candidate's machine, and compare every handler run (event, result, actions,
position) and the memory at the end. Step counts are allowed to differ --
fused instructions take fewer steps; that is the point.

    python apps/jar/tools/bytecode_lab/verify.py
"""
import corpus
import dense
from isa import V


def day(script, runner, seed):
    tr = []
    real_run, real_check = V.run, V.check
    if runner is not None:
        V.run = runner
        V.check = lambda s: 0
    try:
        rep = V.simulate_day(script, seed=seed, trace=lambda d: tr.append(
            (d["hour"], d["tick"], d["event"], d["arg"], d["result"], tuple(d["actions"]), d["x"])))
    finally:
        V.run, V.check = real_run, real_check
    return tr, rep.mem


def same(code, enc, runner, seeds=(1, 2, 3)):
    for seed in seeds:
        a = day(code, None, seed)
        b = day(enc, runner, seed)
        if a != b:
            for x, y in zip(a[0], b[0]):
                if x != y:
                    return "seed %d: %r vs %r" % (seed, x, y)
            return "seed %d: memory %r vs %r" % (seed, a[1], b[1])
    return None


def main():
    c = corpus.load()
    bad = 0
    for s in c:
        e = dense.encode(s["code"])
        why = same(s["code"], e, dense.run2)
        if why:
            bad += 1
            print("DIFFERS %s: %s" % (s["name"], why))
    print("format 2 (dense): %d of %d scripts behave identically over 3 simulated days"
          % (len(c) - bad, len(c)))


if __name__ == "__main__":
    main()
