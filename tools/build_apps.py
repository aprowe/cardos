#!/usr/bin/env python3
"""Build the CardOS app binaries (.capp) and the blob header that embeds them.

Each app in apps/*.c is compiled on its own and linked complete at address 0
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


def code_data(path):
    """(.code, .data) as the loader allocates them: .code is linked at 0 and
    goes to executable RAM, .data (with .bss) at 0x10000000 to the heap --
    each one block, which is what has to be found free."""
    import struct
    data = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)
    code = dat = 0
    for i in range(e_shnum):
        s = struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
        if not s[2] & 2:                                  # SHF_ALLOC
            continue
        if s[3] >= 0x10000000:
            dat += s[5]
        else:
            code += s[5]
    return code, dat


# The size budget, since app code runs from flash
# (docs/superpowers/specs/2026-10-09-xip-app-code-design.md). An app's data
# goes in the 28 KB arena when it is the one on screen -- ARENA_SIZE in
# kernel/app/arena.h -- and its code into the flash cache, so neither needs
# a piece of a heap that breaks up over hours. An app that keeps its code in
# RAM (CAPP_CODE_IN_RAM, for an inner loop) still needs one block of
# executable RAM, and after a day the largest was 24 KB.
DATA_BUDGET = 28 * 1024          # ARENA_SIZE
RAM_CODE_BUDGET = 16 * 1024      # CAPP_CODE_IN_RAM apps
MAX_CODE = 96 * 1024             # CAPP_MAX_CODE in kernel/app/elfload.c
CAPP_CODE_IN_RAM = 0x0040

# Apps allowed over the budget for now, each with why. Take one off as soon
# as it is back under -- the build says when.
OVER_BUDGET = {
}


def check_budget(built):
    """Print code and data per app, write build/apps/sizes.txt, and refuse an
    app over budget that is not in OVER_BUDGET."""
    rows, bad = [], []
    for name, elf in built:
        code, dat = code_data(elf)
        flags = read_info(elf)[0]
        in_ram = bool(flags & CAPP_CODE_IN_RAM)
        over = []
        if dat > DATA_BUDGET:
            over.append("data %d > %d (the arena)" % (dat, DATA_BUDGET))
        if in_ram and code > RAM_CODE_BUDGET:
            over.append("code %d > %d for CAPP_CODE_IN_RAM" % (code, RAM_CODE_BUDGET))
        if code > MAX_CODE:
            over.append("code %d > %d" % (code, MAX_CODE))
        rows.append((name, code, dat, in_ram, over))
    with open(os.path.join(OUT, "sizes.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# app        code    data  where  (bytes; data <= %d, RAM code <= %d)\n"
                % (DATA_BUDGET, RAM_CODE_BUDGET))
        for name, code, dat, in_ram, over in rows:
            f.write("%-10s %7d %7d  %s%s\n" % (name, code, dat, "ram  " if in_ram else "flash",
                                              "  OVER" if over else ""))
    print("  %s" % os.path.relpath(os.path.join(OUT, "sizes.txt"), ROOT))
    for name, code, dat, in_ram, over in rows:
        if over and name in OVER_BUDGET:
            print("  warning: %s is over budget (%s), allowed: %s"
                  % (name, "; ".join(over), OVER_BUDGET[name]))
        elif over:
            bad.append("%s: %s" % (name, "; ".join(over)))
        elif name in OVER_BUDGET:
            print("  note: %s is under budget now -- take it out of OVER_BUDGET" % name)
    if bad:
        raise SystemExit(
            "over the app size budget (data must fit the 28 KB arena; "
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
# are link-time addresses: apps/capp.ld links .data at DATA_BASE, so an
# address minus DATA_BASE is an offset into the .data section's bytes.
DATA_BASE = 0x10000000
INFO_COMMANDS, INFO_NCOMMANDS = 56, 60
ACTION_SIZE, PARAM_SIZE = 28, 12
ARG_TYPES = {1: "text", 2: "int", 3: "bool", 4: "choice"}
CMD_YES, CMD_NET = 0x01, 0x02


def _data_section(elf):
    import struct
    with open(elf, "rb") as f:
        data = f.read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sec = lambda i: struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
    names = sec(shstrndx)[4]
    for i in range(shnum):
        sh = sec(i)
        end = data.index(bytes([0]), names + sh[0])
        if data[names + sh[0]:end] == b".data":
            return data[sh[4]:sh[4] + sh[5]]
    raise SystemExit("%s: no .data section" % elf)


def read_commands(elf):
    """The app's commands: [{"id", "about", "net", "params": [{"name",
    "type", "about"}]}], only entries marked CAPP_CMD_YES."""
    import struct
    d = _data_section(elf)

    def ptr(off):
        v, = struct.unpack_from("<I", d, off)
        return None if v == 0 else v - DATA_BASE

    def cstr(off):
        if off is None:
            return None
        return d[off:d.index(bytes([0]), off)].decode("utf-8", "replace")

    table = ptr(INFO_COMMANDS)
    n = d[INFO_NCOMMANDS]
    out = []
    for i in range(n if table is not None else 0):
        a = table + i * ACTION_SIZE
        cmd = d[a + 25]
        if not cmd & CMD_YES:
            continue
        params, np = ptr(a + 20), d[a + 24]
        plist = []
        for j in range(np if params is not None else 0):
            p = params + j * PARAM_SIZE
            plist.append({"name": cstr(ptr(p)),
                          "type": ARG_TYPES.get(d[p + 4], "?"),
                          "about": cstr(ptr(p + 8))})
        out.append({"id": cstr(ptr(a)), "about": cstr(ptr(a + 16)),
                    "net": bool(cmd & CMD_NET), "params": plist})
    return out


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


def build(src):
    name = os.path.splitext(os.path.basename(src))[0]
    obj = os.path.join(OUT, name + ".o")
    elf = os.path.join(OUT, name + ".capp")

    run([GCC] + CFLAGS + ["-isystem", gcc_include_dir(), "-c", src, "-o", obj])
    run([LD, "-q", "-T", os.path.join(APPS, "capp.ld"),
         "--gc-sections", "-o", elf, obj])
    check_image(elf, name)
    check_placement(name, elf)
    before = loaded_bytes(elf)
    strip_relocs(elf)
    check_image(elf, name)
    if loaded_bytes(elf) != before:
        raise SystemExit("%s: stripping relocations changed the loaded image" % name)

    size = os.path.getsize(elf)
    code, dat = code_data(elf)
    print("  %-14s %6d bytes  code %6d  data %6d" % (name + ".capp", size, code, dat))
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


def main():
    if not os.path.exists(GCC):
        raise SystemExit("no Xtensa toolchain at " + TOOLCHAIN)
    os.makedirs(OUT, exist_ok=True)

    srcs = sorted(os.path.join(APPS, f) for f in os.listdir(APPS)
                  if f.endswith(".c"))
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
