"""Part 1: what today's bytecode spends its bytes on.

    python apps/jar/tools/bytecode_lab/measure.py [--json out.json]

Sizes per source, header vs code, opcode frequency (count and bytes), the
operands (pushed constants, ACTK action/argument pairs, slots, senses, jump
distances) and the n-grams worth fusing.
"""
import collections
import json
import sys

import corpus
import isa


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def size_table(c):
    rows = []
    for o, ss in list(corpus.by_origin(c).items()) + [("all", c)]:
        n = sorted(len(s["code"]) for s in ss)
        hdr = sum(2 + 3 * s["code"][1] for s in ss)
        tot = sum(n)
        rows.append({"origin": o, "scripts": len(ss), "bytes": tot, "mean": tot / len(ss),
                     "median": n[len(n) // 2], "p90": n[int(len(n) * 0.9)], "max": n[-1],
                     "header_pct": pct(hdr, tot), "mean_fill_pct": pct(tot / len(ss), 256),
                     "handlers_mean": sum(s["code"][1] for s in ss) / len(ss),
                     "over_128": sum(1 for v in n if v > 128), "over_200": sum(1 for v in n if v > 200)})
    return rows


def op_stats(c):
    cnt, byts = collections.Counter(), collections.Counter()
    pushes, actk, act0, slots, senses, jumps, cmpjz = (collections.Counter() for _ in range(7))
    bigram, trigram = collections.Counter(), collections.Counter()
    total = 0
    for s in c:
        _e, ins = isa.decode(s["code"])
        for k, i in enumerate(ins):
            sz = 1 + len(i.args)
            cnt[i.op] += 1
            byts[i.op] += sz
            total += sz
            v = isa.push_value(i)
            if v is not None:
                pushes[v] += 1
            if i.op == "ACTK":
                actk[(isa.V.ACTION_NAME[i.args[0]], i.args[1])] += 1
            if i.op == "ACT":
                act0[isa.V.ACTION_NAME[i.args[0]]] += 1
            if i.op in ("LOAD", "STORE"):
                slots[(i.op, i.args[0])] += 1
            if i.op == "SENSE":
                senses[isa.V._SENSES[i.args[0]][1]] += 1
            if i.target is not None:
                jumps[ins[i.target].pc - i.pc if i.target < len(ins) else len(s["code"]) - i.pc] += 1
            def cls(j):
                x = ins[j]
                if x.op in ("PUSH8", "PUSH16"):
                    return "PUSH"
                if x.op == "ACTK":
                    return "ACTK"
                return x.op
            if k + 1 < len(ins):
                bigram[(cls(k), cls(k + 1))] += 1
            if k + 2 < len(ins):
                trigram[(cls(k), cls(k + 1), cls(k + 2))] += 1
    return {"total_code_bytes": total, "count": cnt, "bytes": byts, "pushes": pushes, "actk": actk,
            "act": act0, "slots": slots, "senses": senses, "jumps": jumps,
            "bigram": bigram, "trigram": trigram}


def main():
    c = corpus.load()
    rows = size_table(c)
    print("SIZES")
    for r in rows:
        print("  %-13s n=%3d bytes=%5d mean=%5.1f med=%3d p90=%3d max=%3d hdr=%4.1f%% fill=%4.1f%% "
              "handlers=%.1f >128:%d >200:%d" % (
                  r["origin"], r["scripts"], r["bytes"], r["mean"], r["median"], r["p90"], r["max"],
                  r["header_pct"], r["mean_fill_pct"], r["handlers_mean"], r["over_128"], r["over_200"]))
    st = op_stats(c)
    T = st["total_code_bytes"]
    print("\nOPCODES (code bytes %d, header excluded)" % T)
    for op, b in st["bytes"].most_common():
        print("  %-7s n=%4d bytes=%5d  %4.1f%%" % (op, st["count"][op], b, pct(b, T)))
    for key, k in (("pushes", 20), ("actk", 25), ("act", 10), ("slots", 16), ("senses", 19),
                   ("jumps", 20), ("bigram", 25), ("trigram", 25)):
        print("\n%s (top %d of %d kinds, %d uses)" % (key.upper(), k, len(st[key]), sum(st[key].values())))
        for v, n in st[key].most_common(k):
            print("  %5d  %s" % (n, v))
    small = sum(n for v, n in st["pushes"].items() if -8 <= v <= 23)
    print("\npushes in -8..23: %d of %d" % (small, sum(st["pushes"].values())))
    if len(sys.argv) > 2 and sys.argv[1] == "--json":
        out = {"sizes": rows, "per_script": [{"name": s["name"], "origin": s["origin"], "bytes": len(s["code"]),
                                              "handlers": s["code"][1]} for s in c],
               "opcodes": [{"op": op, "n": st["count"][op], "bytes": b} for op, b in st["bytes"].most_common()],
               "total_code_bytes": T}
        for key in ("pushes", "actk", "act", "slots", "senses", "jumps", "bigram", "trigram"):
            out[key] = [[str(v), n] for v, n in st[key].most_common(40)]
        with open(sys.argv[2], "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
