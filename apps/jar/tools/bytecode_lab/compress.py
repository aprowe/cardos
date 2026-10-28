"""Part 2 and 3: the candidate encodings and compressors, measured on the
corpus. Every trained model (Huffman tables, LZ dictionaries) is trained
leave-one-out -- on every script except the one being coded -- so the
numbers are what a table fixed in the firmware would do on a script it has
never seen, not what it does on its own training data.

    python apps/jar/tools/bytecode_lab/compress.py [--json out.json]
"""
import collections
import heapq
import json
import sys
import zlib

import corpus
import dense
import isa


# ---- Huffman ---------------------------------------------------------------------------

def huff_lengths(freq, maxlen=12):
    """Code lengths for a frequency table (a dict symbol -> count), length-limited
    by flattening -- good enough for estimates; a real table would be canonical."""
    items = [(f, i, s) for i, (s, f) in enumerate(freq.items()) if f > 0]
    if len(items) == 1:
        return {items[0][2]: 1}
    heap = [(f, i, [s]) for f, i, s in items]
    heapq.heapify(heap)
    depth = collections.Counter()
    k = len(heap)
    while len(heap) > 1:
        f1, _, a = heapq.heappop(heap)
        f2, _, b = heapq.heappop(heap)
        for s in a + b:
            depth[s] += 1
        k += 1
        heapq.heappush(heap, (f1 + f2, k, a + b))
    while max(depth.values()) > maxlen:          # crude limiting
        for s in depth:
            depth[s] = min(depth[s], maxlen)
        break
    return dict(depth)


def huff_cost(symbols, freq, escape_bits):
    """Bits to code `symbols` with a table built from `freq` (+1 smoothing for an
    escape); a symbol the table never saw costs the escape code + escape_bits."""
    f = dict(freq)
    f["<esc>"] = max(1, sum(freq.values()) // 200)
    L = huff_lengths(f)
    bits = 0
    for s in symbols:
        bits += L[s] if s in L else L["<esc>"] + escape_bits
    return bits


# ---- token streams ---------------------------------------------------------------------

def f2_tokens(code):
    """Format-2 code as (opcode symbol, operand fields [(kind, value)])."""
    e, ins = isa.decode(code)
    ents, toks = dense.lower(e, ins)
    out = []
    for t in toks:
        b = dense.tok_bytes(t, 0)
        fields = []
        if t.target is not None:
            fields.append(("jump", None))
        elif len(b) > 1:
            fields += [("byte", x) for x in b[1:]]
        out.append((b[0], fields))
    return ents, out


def bitpacked_bits(code, op_freq, hdr_freq):
    """Bit-packed format 2: its tokens with Huffman-coded opcodes (a table fixed in
    flash), operands raw, jump targets as 11-bit bit offsets (executed in place,
    a jump has to name a bit), entries Huffman-coded as (event, filter) with an
    11-bit code offset each."""
    ents, toks = f2_tokens(code)
    bits = 8 + 5                                       # format, entry count
    bits += huff_cost([(ev, f) for ev, f, _ in ents], hdr_freq, 12) + 11 * len(ents)
    bits += huff_cost([op for op, _ in toks], op_freq, 8)
    for _op, fields in toks:
        for kind, _v in fields:
            bits += 11 if kind == "jump" else 8
    return bits


# ---- LZ with a shared dictionary (flash-resident) -------------------------------------

def build_dict(blobs, size, minlen=3, maxlen=16):
    """A static dictionary: the substrings that would save the most, greedily,
    until `size` bytes. Overlaps are allowed (a later pick may contain an
    earlier one); fine for an estimate."""
    gain = collections.Counter()
    for b in blobs:
        seen = set()
        for L in range(minlen, maxlen + 1):
            for i in range(len(b) - L + 1):
                s = b[i:i + L]
                if s not in seen:
                    seen.add(s)
                    gain[s] += 1
    scored = sorted(((n * (len(s) - 2), s) for s, n in gain.items() if n >= 3), reverse=True)
    d = bytearray()
    for _g, s in scored:
        if len(d) + len(s) > size:
            continue
        if s in d:
            continue
        d += s
        if len(d) >= size - minlen:
            break
    return bytes(d)


def lz_bits(data, dictionary, off_bits=None, len_bits=4, minlen=3):
    """Greedy LZ: a flag bit, then a literal byte, or an offset into
    (dictionary + what is decoded so far) and a length minlen..minlen+15."""
    window = bytearray(dictionary)
    if off_bits is None:
        off_bits = max(1, (len(dictionary) + 256 - 1).bit_length())
    maxl = minlen + (1 << len_bits) - 1
    i, bits = 0, 0
    while i < len(data):
        best = 0
        hay = bytes(window)
        for L in range(min(maxl, len(data) - i), minlen - 1, -1):
            if hay.find(data[i:i + L]) >= 0:
                best = L
                break
        if best:
            bits += 1 + off_bits + len_bits
            window += data[i:i + best]
            i += best
        else:
            bits += 9
            window.append(data[i])
            i += 1
    return bits


def deflate_len(data, zdict=None):
    if zdict:
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY, zdict[-32768:])
    else:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return len(c.compress(data) + c.flush())


# ---- the table -------------------------------------------------------------------------

def run(c, dict_sizes=(256, 512, 1024)):
    f1 = [s["code"] for s in c]
    f2 = [dense.encode(x) for x in f1]
    f2nofuse = []
    for x in f1:
        e, ins = isa.decode(x)
        ents, toks = dense.lower(e, ins, fuse=False)
        f2nofuse.append(dense.layout(ents, toks))
    # global frequency tables, then leave-one-out by subtraction
    byte_f1 = collections.Counter(b for x in f1 for b in x)
    byte_f2 = collections.Counter(b for x in f2 for b in x)
    op_f, hdr_f = collections.Counter(), collections.Counter()
    toks_of = []
    for x in f1:
        ents, toks = f2_tokens(x)
        toks_of.append((ents, toks))
        op_f.update(op for op, _ in toks)
        hdr_f.update((ev, f) for ev, f, _ in ents)
    res = collections.OrderedDict((k, 0) for k in (
        "f1", "f1_hdr_end", "f2", "huff_f1", "huff_f2", "f3_bitpacked", "deflate_f2",
        "deflate_f2_zdict") + tuple("lz_f2_dict%d" % d for d in dict_sizes) + ("lz_f2_nodict",))
    per = []
    for k, s in enumerate(c):
        a, b = f1[k], f2[k]
        ents, toks = toks_of[k]
        lo_b1 = byte_f1 - collections.Counter(a)
        lo_b2 = byte_f2 - collections.Counter(b)
        lo_op = op_f - collections.Counter(op for op, _ in toks)
        lo_hd = hdr_f - collections.Counter((ev, f) for ev, f, _ in ents)
        others = b"".join(f2[j] for j in range(len(c)) if j != k)
        row = {
            "f1": len(a), "f1_hdr_end": len(f2nofuse[k]), "f2": len(b),
            "huff_f1": (huff_cost(list(a), lo_b1, 8) + 7) // 8,
            "huff_f2": (huff_cost(list(b), lo_b2, 8) + 7) // 8,
            "f3_bitpacked": (bitpacked_bits(a, lo_op, lo_hd) + 7) // 8,
            "deflate_f2": deflate_len(b), "deflate_f2_zdict": deflate_len(b, others),
            "lz_f2_nodict": (lz_bits(b, b"") + 7) // 8,
        }
        per.append(row)
    # LZ with dictionaries: leave-one-out is slow to rebuild per script, so
    # 5-fold: each fifth coded with a dictionary built from the other four.
    folds = 5
    for d in dict_sizes:
        for fo in range(folds):
            train = [f2[j] for j in range(len(c)) if j % folds != fo]
            dic = build_dict(train, d)
            for j in range(fo, len(c), folds):
                per[j]["lz_f2_dict%d" % d] = (lz_bits(f2[j], dic) + 7) // 8
    for row in per:
        for key in res:
            res[key] += row[key]
    return res, per


def main():
    c = corpus.load()
    res, per = run(c)
    base = res["f1"]
    print("%d scripts, %d bytes as today (format 1)\n" % (len(c), base))
    rows = []
    for k, v in res.items():
        rows.append({"format": k, "bytes": v, "pct_of_f1": 100.0 * v / base})
        print("  %-20s %6d bytes  %5.1f%%" % (k, v, 100.0 * v / base))
    by = corpus.by_origin(c)
    print("\nby source (mean bytes)")
    split = {}
    for o, ss in by.items():
        idx = [k for k, s in enumerate(c) if s["origin"] == o]
        split[o] = {key: sum(per[k][key] for k in idx) / len(idx) for key in res}
        print("  %-13s " % o + "  ".join("%s=%.1f" % (key, split[o][key]) for key in
                                          ("f1", "f2", "f3_bitpacked", "lz_f2_dict512", "deflate_f2_zdict")))
    if len(sys.argv) > 2 and sys.argv[1] == "--json":
        with open(sys.argv[2], "w") as f:
            json.dump({"totals": rows, "by_origin": split, "n": len(c)}, f, indent=1)


if __name__ == "__main__":
    main()
