#!/usr/bin/env python3
"""Where the firmware's flash and RAM actually go, from the linker map.

`size -A` gives the totals; this gives the attribution, which is the part that
tells you what to cut. Contributions are deduplicated by load address -- each
one has a unique address, and that is what stops the two line formats the map
uses from being counted twice.

    python tools/mapsize.py [--json]
"""

import collections
import json
import os
import re
import sys

MAP = os.path.join(".pio", "build", "cardputer", "cardos.map")

ONE = re.compile(r"^\s(\.[\w.$-]+)\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S.*)$")
HEAD = re.compile(r"^\s(\.[\w.$-]+)$")
TAIL = re.compile(r"^\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S.*)$")

# ESP32-S3 address windows.
FLASH = ((0x3C000000, 0x3E000000),   # rodata, mapped
         (0x42000000, 0x44000000))   # text, mapped
RAM = ((0x3FC80000, 0x3FD00000),     # data bus
       (0x40370000, 0x403E0000))     # instruction bus (IRAM)


def in_range(addr, ranges):
    return any(lo <= addr < hi for lo, hi in ranges)


def parse(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    seen, out = set(), []
    i = 0
    while i < len(lines):
        m = ONE.match(lines[i])
        if m:
            sec, addr, size, obj = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
            i += 1
        else:
            h = HEAD.match(lines[i])
            t = TAIL.match(lines[i + 1]) if h and i + 1 < len(lines) else None
            if not t:
                i += 1
                continue
            sec, addr, size, obj = h.group(1), t.group(1), int(t.group(2), 16), t.group(3)
            i += 2
        if size == 0 or addr in seen:
            continue
        seen.add(addr)
        out.append((sec, int(addr, 16), size, obj.strip()))
    return out


def component(obj):
    """A human-sized name for whatever produced this. None for map noise."""
    if obj.startswith("0x"):
        return None
    norm = obj.replace("\\", "/")
    if "(" in obj:
        lib = os.path.basename(obj.split("(")[0])
        if lib.startswith("lib") and lib.endswith(".a"):
            return lib[3:-2]
        return lib
    if not norm.endswith(".o"):
        return None
    if "/kernel/" in norm or "/src/" in norm:
        return "CardOS"
    return os.path.basename(norm)


def main():
    if not os.path.exists(MAP):
        raise SystemExit("no map at " + MAP + " -- build first")

    flash, ram = collections.Counter(), collections.Counter()
    for sec, addr, size, obj in parse(MAP):
        name = component(obj)
        if not name:
            continue
        if in_range(addr, FLASH):
            flash[name] += size
        elif in_range(addr, RAM):
            ram[name] += size

    if "--json" in sys.argv:
        json.dump({"flash": dict(flash), "ram": dict(ram)}, sys.stdout, indent=1)
        return

    for title, c in (("flash: code + rodata", flash),
                     ("internal RAM: data + bss + iram", ram)):
        total = sum(c.values())
        print("== %s: %d bytes attributed" % (title, total))
        top = c.most_common(20)
        for k, v in top:
            print("   %-32s %9d  %5.1f%%" % (k, v, 100.0 * v / total))
        print("   %-32s %9d" % ("(everything else)", total - sum(v for _, v in top)))
        print()


if __name__ == "__main__":
    main()
