#!/usr/bin/env python3
"""Build the CardOS app binaries (.capp) and the blob header that embeds them.

Each app in apps/*.c (or apps/DIR/*.c, see app_sources) is compiled on its own and linked complete at address 0
with the relocations retained (-q), which is what lets the on-device loader be
a hundred lines instead of a thousand: the only fixups left in the output are
absolute R_XTENSA_32 entries, and applying one is adding the load address.

Apps link against no libc and no CRT. Everything they are allowed to do arrives
through the CardApi table, so an undefined symbol here is a mistake in the app,
not something to satisfy with -lc -- the link is checked for exactly that.

The compiled .capp files are also emitted as a C header so the firmware can
write them to the filesystem on first boot. The device has no card reader in
the loop, and an app the user cannot get onto the card is an app that does not
exist.

    python tools/build_apps.py
"""

import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(ROOT, "apps")
OUT = os.path.join(ROOT, "build", "apps")
HEADER = os.path.join(ROOT, "kernel", "app", "capp_blobs.h")

# The apps the firmware carries, for a card that has none: enough to link
# the device to the dashboard and repair the card from there or by hand.
# Everything else comes from `update apps`. All of them used to be
# embedded -- 948 KB of the image by 2026-10-01, which pushed the firmware
# past the 2304 KB update slots, so `update os` could no longer install it.
# Update joins them for the same reason Memory, About and Settings need no
# `update apps` first: the one app that fetches updates must not itself be
# missing, or stale, on a card that has never synced.
EMBED = ("dashlink", "files", "edit", "update")

TOOLCHAIN = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "toolchain-xtensa-esp-elf", "bin")
# The droplet builds too (see server/build.py), and Linux has no .exe.
EXE = ".exe" if os.name == "nt" else ""
GCC = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-gcc" + EXE)
LD = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-ld" + EXE)
READELF = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-readelf" + EXE)

CFLAGS = [
    "-std=gnu99",
    "-Os",
    "-g0",
    "-mlongcalls",          # the image can land anywhere; no call range bets
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-builtin",         # no sneaking in memcpy behind the API table
    "-ffreestanding",       # no libc: stdint/stddef come from GCC itself
    "-nostdinc",            # ... and no libc headers to sneak it from
    "-fno-jump-tables",     # jump tables are absolute; relocatable but noisy
    "-Wall",
    "-Wextra",
    "-Werror",
    # The contract grows by adding fields at the end of CappInfo, CappAction
    # and CappUi, and an app that has no use for them leaves them out: that is
    # how Mines stays untouched when commands arrive (API 30). -Wextra would
    # call every one of those initializers an error.
    "-Wno-missing-field-initializers",
    "-I", ROOT,
]

# stdint.h and stddef.h come from the compiler, not the C library, so the one
# include path an app gets is GCC's own.
def gcc_include_dir():
    out = subprocess.run([GCC, "-print-file-name=include"],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n")
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(1)
    return r.stdout


# Relocation types the loader understands. R_XTENSA_32 is absolute and gets the
# load address added; the rest are PC-relative or empty and survive a uniform
# shift untouched. Anything else means the loader would have to grow, so the
# build fails here rather than the device failing later.
#
# R_XTENSA_ASM_SIMPLIFY (first seen in Jar Factory, 2026-10-09) is the
# linker's note that it relaxed a -mlongcalls call (L32R + CALLX8) into a
# direct CALL8. CALL8 is PC-relative -- (PC & ~3) + offset -- so, like the
# SLOT0_OP branches, it survives the shift as long as the code block is
# word-aligned, which heap_caps_malloc always returns.
ALLOWED_RELOCS = {
    "R_XTENSA_NONE", "R_XTENSA_32", "R_XTENSA_SLOT0_OP", "R_XTENSA_ASM_EXPAND",
    "R_XTENSA_ASM_SIMPLIFY", "R_XTENSA_DIFF8", "R_XTENSA_DIFF16", "R_XTENSA_DIFF32",
}


SHT_RELA = 4
R_XTENSA_32 = 1


def strip_relocs(path):
    """Keep only the relocations the loader applies, R_XTENSA_32.

    The link keeps every relocation (ld -q), and the loader skips all but the
    absolute ones: SLOT0_OP marks a PC-relative operand, which is right
    wherever the image lands, and NONE marks nothing. They were about 40% of
    every .capp -- Todo's 66 KB file held 23 KB of them -- read past at every
    load and carried in every download (2026-10-01).

    The file is rewritten from the first non-loaded section on: the header,
    the program headers and the code and data stay byte for byte where they
    were; the relocation, symbol and string sections are packed behind them
    and the section table is written last. Returns bytes saved."""
    import struct
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit("%s: not a 32-bit little-endian ELF" % path)
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    shdrs = [list(struct.unpack_from("<10I", data, e_shoff + i * e_shentsize))
             for i in range(e_shnum)]
    # name type flags addr offset size link info addralign entsize
    SHF_ALLOC = 2
    tail = [i for i, s in enumerate(shdrs) if s[1] != 0 and not (s[2] & SHF_ALLOC)]
    if not tail:
        return 0
    start = min(shdrs[i][4] for i in tail)
    out = bytearray(data[:start])
    for i in sorted(tail, key=lambda i: shdrs[i][4]):
        s = shdrs[i]
        body = bytes(data[s[4]:s[4] + s[5]])
        if s[1] == SHT_RELA:
            ent = s[9] or 12
            body = b"".join(body[k:k + ent] for k in range(0, len(body), ent)
                            if struct.unpack_from("<I", body, k + 4)[0] & 0xFF == R_XTENSA_32)
        align = max(s[8], 1)
        while len(out) % align:
            out.append(0)
        s[4], s[5] = len(out), len(body)
        out += body
    while len(out) % 4:
        out.append(0)
    struct.pack_into("<I", out, 0x20, len(out))
    for s in shdrs:
        out += struct.pack("<10I", *s)
    saved = len(data) - len(out)
    open(path, "wb").write(out)
    return saved


def loaded_bytes(path):
    """What the loader copies: every SHF_ALLOC section's bytes, in order."""
    import struct
    data = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    out = []
    for i in range(e_shnum):
        s = struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
        if s[2] & 2 and s[1] == 1:                       # SHF_ALLOC, PROGBITS
            out.append((s[3], data[s[4]:s[4] + s[5]]))
    return out


# Where apps/capp.ld links each part, and what the loader makes of it
# (kernel/app/elfload.c): code at 0; the data block at DATA_BASE -- .data,
# then the overlay region all of an app's overlays share; the constants at
# RODATA_BASE, in flash beside the code when the app runs from the flash
# cache and in RAM after the data block otherwise. See
# docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md.
SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_WRITE, SHF_ALLOC = 1, 2
DATA_BASE = 0x10000000            # CAPP_DATA_ORIGIN in kernel/app/capprel.h
RODATA_BASE = 0x20000000          # CAPP_RODATA_ORIGIN
OVL_MAX = 4                       # CAPP_OVL_MAX in kernel/app/capp.h
KNOWN_SECTIONS = {".code", ".data", ".rodata"} | {".ovl%d" % n for n in range(OVL_MAX)}


def align16(n):
    return (n + 15) & ~15


def sections(elf):
    """Every section header of a built .capp, named, and the file's bytes."""
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    names = raw[shstrndx][4]
    out = []
    for s in raw:
        end = data.index(bytes([0]), names + s[0])
        out.append({"name": data[names + s[0]:end].decode("ascii", "replace"),
                    "type": s[1], "flags": s[2], "addr": s[3], "offset": s[4],
                    "size": s[5], "align": s[8]})
    return out, data


def layout(elf):
    """What the loader allocates. block is the data block: .data and then
    the largest overlay, alignment included -- the arena's share when the
    app runs from the flash cache, since its constants are in flash."""
    secs, _ = sections(elf)
    lay = {"code": 0, "data": 0, "rodata": 0, "block": 0, "ovl": {}, "ovl_addr": set()}
    for s in secs:
        if not s["flags"] & SHF_ALLOC or s["size"] == 0:
            continue
        if s["addr"] >= RODATA_BASE:
            lay["rodata"] += s["size"]
        elif s["addr"] >= DATA_BASE:
            lay["block"] = max(lay["block"], s["addr"] - DATA_BASE + s["size"])
            m = re.match(r"\.ovl([0-9])$", s["name"])
            if m:
                lay["ovl"][int(m.group(1))] = s["size"]
                lay["ovl_addr"].add(s["addr"])
            elif s["name"] == ".data":
                lay["data"] = s["size"]
        else:
            lay["code"] += s["size"]
    return lay


# The size budget, since app code runs from flash
# (docs/superpowers/specs/2026-10-09-xip-app-code-design.md) and its
# constants with it (docs/superpowers/specs/2026-10-10-app-constants-in-flash-design.md).
# What has to fit the 28 KB arena -- ARENA_SIZE in kernel/app/arena.h -- is
# the data block: .data and the largest overlay. An app that keeps its code
# in RAM (CAPP_CODE_IN_RAM, for an inner loop) keeps its constants in RAM
# too, after the block, and still needs one block of executable RAM; after a
# day the largest was 24 KB.
DATA_BUDGET = 28 * 1024          # ARENA_SIZE
RAM_CODE_BUDGET = 16 * 1024      # CAPP_CODE_IN_RAM apps
MAX_CODE = 96 * 1024             # CAPP_MAX_CODE in kernel/app/elfload.c
CAPP_CODE_IN_RAM = 0x0040

# Apps allowed over the budget for now, each with why. Take one off as soon
# as it is back under -- the build says when.
OVER_BUDGET = {
}


def check_budget(built):
    """Print the layout per app, write build/apps/sizes.txt, and refuse an
    app over budget that is not in OVER_BUDGET."""
    rows, bad = [], []
    for name, elf in built:
        lay = layout(elf)
        flags = read_info(elf)[0]
        in_ram = bool(flags & CAPP_CODE_IN_RAM)
        need = align16(lay["block"]) + lay["rodata"] if in_ram else lay["block"]
        over = []
        if need > DATA_BUDGET:
            over.append("data block %d > %d (the arena)" % (need, DATA_BUDGET))
        if in_ram and lay["code"] > RAM_CODE_BUDGET:
            over.append("code %d > %d for CAPP_CODE_IN_RAM" % (lay["code"], RAM_CODE_BUDGET))
        if lay["code"] > MAX_CODE:
            over.append("code %d > %d" % (lay["code"], MAX_CODE))
        rows.append((name, lay, in_ram, over))
    with open(os.path.join(OUT, "sizes.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# app        code    data   ovl0   ovl1   ovl2   ovl3  rodata   block  where"
                "  (bytes; block = data + largest overlay <= %d; RAM code <= %d)\n"
                % (DATA_BUDGET, RAM_CODE_BUDGET))
        for name, lay, in_ram, over in rows:
            ovl = ["%6d" % lay["ovl"][n] if n in lay["ovl"] else "     -" for n in range(OVL_MAX)]
            f.write("%-10s %7d %7d %s %7d %7d  %s%s\n"
                    % (name, lay["code"], lay["data"], " ".join(ovl), lay["rodata"],
                       lay["block"], "ram  " if in_ram else "flash", "  OVER" if over else ""))
    print("  %s" % os.path.relpath(os.path.join(OUT, "sizes.txt"), ROOT))
    for name, lay, in_ram, over in rows:
        if over and name in OVER_BUDGET:
            print("  warning: %s is over budget (%s), allowed: %s"
                  % (name, "; ".join(over), OVER_BUDGET[name]))
        elif over:
            bad.append("%s: %s" % (name, "; ".join(over)))
        elif name in OVER_BUDGET:
            print("  note: %s is under budget now -- take it out of OVER_BUDGET" % name)
    if bad:
        raise SystemExit(
            "over the app size budget (the data block must fit the 28 KB arena; "
            "see OVER_BUDGET in tools/build_apps.py):\n  " + "\n  ".join(bad))


def check_image(elf, name):
    """Refuse anything the on-device loader could not load."""
    text = run([READELF, "-r", "-s", "--wide", elf])

    bad = set()
    for line in text.splitlines():
        m = re.search(r"\bR_XTENSA_[A-Z0-9_]+", line)
        if m and m.group(0) not in ALLOWED_RELOCS:
            bad.add(m.group(0))
    if bad:
        raise SystemExit("%s: relocations the loader cannot apply: %s"
                         % (name, ", ".join(sorted(bad))))

    # Both exported symbols must survive the link. capp_info in particular is
    # read by the loader and referenced by nothing, so --gc-sections discards it
    # unless the linker script KEEPs it -- a mistake that otherwise shows up as
    # an app that loads and then has no name.
    for sym in ("capp_main", "capp_info"):
        if not re.search(r"\b%s\b" % sym, text):
            raise SystemExit("%s: %s did not survive the link" % (name, sym))

    # An undefined symbol here means the app reached for something outside the
    # API table. Nothing will resolve it at load time.
    undef = [l.split()[-1] for l in text.splitlines()
             if re.search(r"\bUND\b", l) and len(l.split()) > 7]
    undef = [u for u in undef if u and u != "Name"]
    if undef:
        raise SystemExit("%s: unresolved symbols (apps link against nothing): %s"
                         % (name, ", ".join(sorted(set(undef)))))


# Which folder of /apps each app goes in: apps/folders.txt, shared with the
# server, which puts it in the /update manifest so a first install lands in
# the folder too. Every app must have a line -- "-" for the top level, where
# the CLI apps stay on the default PATH -- because an app nobody placed used to
# land at the top level with no folder, which is where Build's apps all went.
FOLDERS_FILE = os.path.join(APPS, "folders.txt")


def load_folders():
    """{app: folder or None} for every app apps/folders.txt names."""
    out = {}
    with open(FOLDERS_FILE, encoding="utf-8") as f:
        for line in f:
            words = line.split("#", 1)[0].split()
            if len(words) == 2:
                out[words[0]] = None if words[1] == "-" else words[1]
    return out


FOLDERS = load_folders()

# The descriptor's layout (CappInfo in kernel/app/capp.h): capp_info is the
# first thing in .data (apps/capp.ld keeps it there), so it can be read out of
# the ELF without running or relocating anything.
CAPP_CLI = 0x0001
ICON_OFFSET, ICON_BYTES = 20, 32


def read_info(elf):
    """(flags, name, icon bytes) from a built .capp's capp_info."""
    import struct
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    def section(i):
        return struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
    names_off = section(shstrndx)[4]
    for i in range(shnum):
        sh = section(i)
        end = data.index(bytes([0]), names_off + sh[0])
        if data[names_off + sh[0]:end] == b".data":
            off = sh[4]
            _ver, flags = struct.unpack_from("<HH", data, off)
            name = data[off + 4:off + 20].split(bytes([0]))[0].decode("ascii", "replace")
            return flags, name, data[off + ICON_OFFSET:off + ICON_OFFSET + ICON_BYTES]
    raise SystemExit("%s: no .data section, so no capp_info" % elf)


# The commands, followed out of capp_info the same way (CappInfo.commands and
# CappAction/CappParam in kernel/app/capp.h, 32-bit Xtensa layout). Pointers
# are link-time addresses, into .data or -- for anything const, which is
# nearly everything capp_info points to -- into .rodata.
INFO_HELP, INFO_COMMANDS, INFO_NCOMMANDS = 52, 56, 60
ACTION_SIZE, PARAM_SIZE = 28, 12
ARG_TYPES = {1: "text", 2: "int", 3: "bool", 4: "choice"}
CMD_YES, CMD_NET = 0x01, 0x02


class Image:
    """A built .capp's .data and .rodata bytes, read by link address -- as the
    kernel reads capp_info and its tables in the icon scan, with the
    constants in RAM beside the data."""

    def __init__(self, elf):
        secs, data = sections(elf)
        self.elf = elf
        self.parts = [(s["addr"], data[s["offset"]:s["offset"] + s["size"]])
                      for s in secs
                      if s["name"] in (".data", ".rodata") and s["type"] == SHT_PROGBITS]
        if not any(a == DATA_BASE for a, _ in self.parts):
            raise SystemExit("%s: no .data section, so no capp_info" % elf)

    def where(self, addr):
        """(bytes, offset) holding addr, or None outside .data and .rodata."""
        for base, b in self.parts:
            if base <= addr < base + len(b):
                return b, addr - base
        return None

    def _at(self, addr, n):
        w = self.where(addr)
        if w is None or w[1] + n > len(w[0]):
            raise SystemExit("%s: 0x%08x is not in .data or .rodata" % (self.elf, addr))
        return w

    def u8(self, addr):
        b, o = self._at(addr, 1)
        return b[o]

    def u32(self, addr):
        b, o = self._at(addr, 4)
        return struct.unpack_from("<I", b, o)[0]

    def cstr(self, addr):
        if not addr:
            return None
        b, o = self._at(addr, 1)
        return b[o:b.index(bytes([0]), o)].decode("utf-8", "replace")


def read_commands(elf):
    """The app's commands: [{"id", "about", "net", "params": [{"name",
    "type", "about"}]}], only entries marked CAPP_CMD_YES."""
    img = Image(elf)
    table = img.u32(DATA_BASE + INFO_COMMANDS)
    n = img.u8(DATA_BASE + INFO_NCOMMANDS)
    out = []
    for i in range(n if table else 0):
        a = table + i * ACTION_SIZE
        cmd = img.u8(a + 25)
        if not cmd & CMD_YES:
            continue
        params, np = img.u32(a + 20), img.u8(a + 24)
        plist = []
        for j in range(np if params else 0):
            p = params + j * PARAM_SIZE
            plist.append({"name": img.cstr(img.u32(p)),
                          "type": ARG_TYPES.get(img.u8(p + 4), "?"),
                          "about": img.cstr(img.u32(p + 8))})
        out.append({"id": img.cstr(img.u32(a)), "about": img.cstr(img.u32(a + 16)),
                    "net": bool(cmd & CMD_NET), "params": plist})
    return out


def check_info(elf, name):
    """Everything capp_info points to is in .data's bytes or in .rodata: the
    icon scan reads it without running the app, with the constants loaded
    into RAM and nothing else -- not the code, not an overlay."""
    img = Image(elf)

    def ok(what, v):
        if v and img.where(v) is None:
            raise SystemExit(
                "%s: capp_info's %s points at 0x%08x, outside .data and .rodata (in an "
                "overlay, or the code). The icon scan reads capp_info and what it points "
                "to without running the app, so all of it must be data or constants."
                % (name, what, v))
        return v

    ok("help", img.u32(DATA_BASE + INFO_HELP))
    table = ok("commands", img.u32(DATA_BASE + INFO_COMMANDS))
    for i in range(img.u8(DATA_BASE + INFO_NCOMMANDS) if table else 0):
        a = ok("command %d" % i, table + i * ACTION_SIZE)
        for what, off in (("id", 0), ("label", 4), ("menu", 8), ("about", 16)):
            ok("command %d %s" % (i, what), img.u32(a + off))
        params = ok("command %d params" % i, img.u32(a + 20))
        for j in range(img.u8(a + 24) if params else 0):
            p = ok("command %d param %d" % (i, j), params + j * PARAM_SIZE)
            ok("command %d param %d name" % (i, j), img.u32(p))
            ok("command %d param %d about" % (i, j), img.u32(p + 8))


def check_overlays(secs, name):
    """The overlay sections as apps/capp.ld's OVERLAY lays them out: zeroed
    state only (NOBITS -- the compiler refuses an initialiser, this is the
    backstop), all at one 16-aligned address after .data, which is what lets
    them share the region."""
    ovl = [s for s in secs if re.match(r"\.ovl[0-9]$", s["name"]) and s["size"]]
    if not ovl:
        return
    for s in ovl:
        if s["type"] != SHT_NOBITS:
            raise SystemExit(
                "%s: %s holds initialised bytes; an overlay may hold only zeroed state "
                "(a CAPP_OVERLAY variable takes no initialiser)" % (name, s["name"]))
    data = [s for s in secs if s["name"] == ".data"]
    end = data[0]["addr"] + data[0]["size"] if data else DATA_BASE
    addrs = {s["addr"] for s in ovl}
    if len(addrs) != 1 or min(addrs) < end or min(addrs) % 16:
        raise SystemExit(
            "%s: overlays must all start at one 16-aligned address after .data; found %s"
            % (name, ", ".join("%s at 0x%08x" % (s["name"], s["addr"]) for s in ovl)))


def check_layout(elf, name):
    """Refuse a layout the loader does not expect: a section it would not
    place, constants that are writable, misplaced or over-aligned, capp_info
    anywhere but the start of .data, or capp_info reaching outside .data and
    .rodata."""
    secs, _ = sections(elf)
    alloc = [s for s in secs if s["flags"] & SHF_ALLOC and s["size"]]
    for s in alloc:
        if s["name"] not in KNOWN_SECTIONS:
            raise SystemExit(
                "%s: section %s is not one the loader places (.code, .data, .rodata, "
                ".ovl0-.ovl%d); overlays are numbered 0-%d"
                % (name, s["name"], OVL_MAX - 1, OVL_MAX - 1))
        if s["name"] == ".rodata":
            if s["addr"] != RODATA_BASE:
                raise SystemExit("%s: .rodata at 0x%08x, not 0x%08x" % (name, s["addr"], RODATA_BASE))
            if s["flags"] & SHF_WRITE:
                raise SystemExit(
                    "%s: .rodata is writable -- something not const was put in a .rodata "
                    "section. Constants run from flash, which an app cannot write." % name)
            if s["align"] > 16:
                raise SystemExit(
                    "%s: .rodata wants %d-byte alignment; the flash cache places constants "
                    "on a 16-byte boundary" % (name, s["align"]))
    check_overlays(secs, name)
    syms = run([READELF, "-s", "--wide", elf])
    m = re.search(r"^\s*\d+:\s+([0-9a-f]{8})\s+\d+\s+OBJECT\s+GLOBAL\s+\S+\s+\S+\s+capp_info\s*$",
                  syms, re.M)
    if not m or int(m.group(1), 16) != DATA_BASE:
        raise SystemExit(
            "%s: capp_info is not the first thing in .data; apps/capp.ld KEEPs it there, "
            "and the loader and read_info read it from there" % name)
    check_info(elf, name)


def catalog_line(app, c):
    """The same text kernel/app/cmdline.c's cmdline_catalog_line makes."""
    words = [app, c["id"]]
    for p in c["params"]:
        words.append("%s:%s" % (p["name"], p["about"] if p["type"] == "choice"
                                else p["type"]))
    if c["net"]:
        words.append("net")
    line = " ".join(words)
    return line + (" # " + c["about"] if c["about"] else "")


def emit_catalog(built):
    """build/apps/commands.json: every app's commands, for the server's voice
    prompt and anything else off the device that needs to know."""
    import json
    cat = {}
    for name, elf in built:
        cmds = read_commands(elf)
        if cmds:
            cat[name] = cmds
    path = os.path.join(OUT, "commands.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cat, f, indent=1, sort_keys=True)
    total = sum(len(v) for v in cat.values())
    print("  %s (%d command%s in %d app%s)" % (os.path.relpath(path, ROOT), total,
          "" if total == 1 else "s", len(cat), "" if len(cat) == 1 else "s"))


def check_placement(stem, elf):
    """Refuse an app that nobody placed or that has no face. A Build turn that
    makes an app reads this when it fails, so the message says what to do."""
    if stem not in FOLDERS:
        raise SystemExit(
            "%s: apps/folders.txt has no line for it. Add one: '%s Tools' (or "
            "Games, Net -- the folders there are), or '%s -' for a CLI app at "
            "the top level." % (stem, stem, stem))
    flags, name, icon = read_info(elf)
    if not flags & CAPP_CLI and not any(icon):
        raise SystemExit(
            "%s: its 16x16 icon in capp_info is blank. Draw one: 32 bytes, "
            "1bpp, two bytes a row, bit 7 leftmost (apps/timer.c has one)." % stem)
    # And the colour one the launcher actually shows. Without it the app
    # wears generic.cic, a blank page -- which is how Habits, Memo, Share and
    # Timer looked until 2026-09-23, each with a perfectly good mono icon.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_color_icons
    if not flags & CAPP_CLI and name not in make_color_icons.ICONS:
        raise SystemExit(
            "%s: no colour icon for '%s' in tools/make_color_icons.py -- the "
            "launcher would show a blank page. Add a 16x16 picture under that "
            "name in ICONS (its INK letters are the palette), then run "
            "python tools/make_color_icons.py." % (stem, name))


def seed_name(stem):
    """Where this app's .capp is written, relative to /apps."""
    folder = FOLDERS.get(stem)
    return "%s/%s.capp" % (folder, stem) if folder else "%s.capp" % stem


def compile_link(src, out_dir=OUT):
    """Compile and link one app; the .capp's path. Linked -n (no page
    alignment): the constants are a third segment, and page-aligned
    segments added up to 4 KB of padding to every file and every download,
    which the loader -- reading by section offset -- never needed."""
    name = os.path.splitext(os.path.basename(src))[0]
    obj = os.path.join(out_dir, name + ".o")
    elf = os.path.join(out_dir, name + ".capp")
    run([GCC] + CFLAGS + ["-isystem", gcc_include_dir(), "-c", src, "-o", obj])
    run([LD, "-q", "-n", "-T", os.path.join(APPS, "capp.ld"),
         "--gc-sections", "-o", elf, obj])
    return elf


def build(src):
    name = os.path.splitext(os.path.basename(src))[0]
    elf = compile_link(src)
    check_image(elf, name)
    check_placement(name, elf)
    before = loaded_bytes(elf)
    strip_relocs(elf)
    check_image(elf, name)
    if loaded_bytes(elf) != before:
        raise SystemExit("%s: stripping relocations changed the loaded image" % name)
    check_layout(elf, name)

    size = os.path.getsize(elf)
    lay = layout(elf)
    print("  %-14s %6d bytes  code %6d  data %6d  rodata %6d"
          % (name + ".capp", size, lay["code"], lay["block"], lay["rodata"]))
    return name, elf


def emit_header(built):
    lines = [
        "/* Generated by tools/build_apps.py -- do not edit.",
        " *",
        " * The few apps the firmware carries (EMBED in tools/build_apps.py), so",
        " * a card with none can still be linked to the dashboard and repaired.",
        " * The rest arrive with `update apps`.",
        " */",
        "#ifndef CARDOS_CAPP_BLOBS_H",
        "#define CARDOS_CAPP_BLOBS_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
    ]
    for name, path in built:
        data = open(path, "rb").read()
        lines.append("static const uint8_t capp_blob_%s[] = {" % name)
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            lines.append("  " + " ".join("0x%02X," % b for b in chunk))
        lines.append("};")
        lines.append("")

    lines.append("typedef struct {")
    lines.append("  const char    *name;      /* path under /apps, folder included */")
    lines.append("  const uint8_t *data;")
    lines.append("  size_t         size;")
    lines.append("} CappBlob;")
    lines.append("")
    lines.append("static const CappBlob CAPP_BLOBS[] = {")
    for name, path in built:
        lines.append('  { "%s", capp_blob_%s, sizeof capp_blob_%s },'
                     % (seed_name(name), name, name))
    lines.append("};")
    lines.append("")
    lines.append("#define CAPP_BLOB_COUNT (sizeof CAPP_BLOBS / sizeof CAPP_BLOBS[0])")
    lines.append("")
    # FNV-1a over every blob in table order: which set of apps this firmware
    # carries. Worked out here rather than hashed over 200 KB at every boot.
    stamp = 2166136261
    for name, path in built:
        for b in open(path, "rb").read():
            stamp = ((stamp ^ b) * 16777619) & 0xFFFFFFFF
    lines.append("#define CAPP_BLOB_STAMP 0x%08Xu" % stamp)
    lines.append("")
    lines.append("#endif /* CARDOS_CAPP_BLOBS_H */")

    with open(HEADER, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    total = sum(os.path.getsize(p) for _, p in built)
    print("  %s (%d bytes of flash)" % (os.path.relpath(HEADER, ROOT), total))


# Every app on the card holds an entry in the kernel's app table for as long
# as the device is up (CAPPRUN_APPS, kernel/app/capprun.h). One too many and
# the last app the scan finds is simply missing -- Share, the day Counter
# arrived. Four spare, for programs run by path, which take an entry for the
# length of their run.
SLOT_SPARE = 4


def check_slots(napps):
    with open(os.path.join(ROOT, "kernel", "app", "capprun.h"), encoding="utf-8") as f:
        m = re.search(r"#define\s+CAPPRUN_APPS\s+(\d+)", f.read())
    limit = int(m.group(1))
    if napps + SLOT_SPARE > limit:
        raise SystemExit(
            "%d apps, and CAPPRUN_APPS in kernel/app/capprun.h is %d: raise it to "
            "at least %d, or the launcher silently drops an app"
            % (napps, limit, napps + SLOT_SPARE))


def app_sources(apps_dir=APPS):
    """Every app's source. An app is apps/NAME.c, or -- when it has files of
    its own beside its shared headers -- apps/DIR/NAME.c: every .c directly
    in a folder of apps/ is an app (apps/midi holds MIDI and Circuit). One
    level only. The .capp is NAME.capp either way, so moving an app into a
    folder changes nothing on the card; two sources with one NAME are refused."""
    srcs = []
    for f in sorted(os.listdir(apps_dir)):
        p = os.path.join(apps_dir, f)
        if f.endswith(".c") and os.path.isfile(p):
            srcs.append(p)
        elif os.path.isdir(p):
            srcs.extend(os.path.join(p, g) for g in sorted(os.listdir(p))
                        if g.endswith(".c") and os.path.isfile(os.path.join(p, g)))
    seen = {}
    for s in srcs:
        name = os.path.splitext(os.path.basename(s))[0]
        if name in seen:
            raise SystemExit("two apps called %s: %s and %s" % (
                name, os.path.relpath(seen[name], ROOT), os.path.relpath(s, ROOT)))
        seen[name] = s
    return sorted(srcs, key=lambda s: os.path.basename(s))


def main():
    if not os.path.exists(GCC):
        raise SystemExit("no Xtensa toolchain at " + TOOLCHAIN)
    os.makedirs(OUT, exist_ok=True)

    srcs = app_sources()
    if not srcs:
        raise SystemExit("no apps in " + APPS)

    check_slots(len(srcs))
    print("building %d app(s):" % len(srcs))
    built = [build(s) for s in srcs]
    # A .capp whose source is gone (Jar Shop and Jar Post became screens of
    # Jar Factory) must not stay here: the server publishes what is here.
    names = {n for n, _ in built}
    for f in os.listdir(OUT):
        if f.endswith(".capp") and f[:-len(".capp")] not in names:
            os.remove(os.path.join(OUT, f))
            print("  removed %s: no source for it any more" % f)
    missing = [n for n in EMBED if n not in dict(built)]
    if missing:
        raise SystemExit("EMBED names apps that were not built: %s" % ", ".join(missing))
    check_budget(built)
    emit_header([b for b in built if b[0] in EMBED])
    emit_catalog(built)


if __name__ == "__main__":
    main()
