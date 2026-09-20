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

TOOLCHAIN = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "toolchain-xtensa-esp-elf", "bin")
GCC = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-gcc.exe")
LD = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-ld.exe")
READELF = os.path.join(TOOLCHAIN, "xtensa-esp32s3-elf-readelf.exe")

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
ALLOWED_RELOCS = {
    "R_XTENSA_NONE", "R_XTENSA_32", "R_XTENSA_SLOT0_OP", "R_XTENSA_ASM_EXPAND",
    "R_XTENSA_DIFF8", "R_XTENSA_DIFF16", "R_XTENSA_DIFF32",
}


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


# Which folder of /apps each app is seeded into.
#
# The launcher reads one level of subdirectory, and `update apps` finds an app
# wherever it already sits (see capp_path in kernel/net/update.c), so grouping
# is purely a question of where the blob is first written.
#
# CLI-only apps are deliberately absent: they never appear as an icon, and
# leaving them at the top level keeps them on the default PATH, so `grep TODO
# /apps` still resolves from the console with nothing to configure.
FOLDERS = {
    "mines":    "Games",
    "pinball":  "Games",

    "web":      "Net",
    "claude":   "Net",
    "build":    "Net",
    "stocks":   "Net",
    "screen":   "Net",
    "share":    "Net",

    "calendar": "Tools",
    "files":    "Tools",
    "ide":      "Tools",
    "explorer": "Tools",
    "edit":     "Tools",
    "photo":    "Tools",
    "todo":     "Tools",
    "memo":     "Tools",
}


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

    size = os.path.getsize(elf)
    print("  %-8s %6d bytes" % (name + ".capp", size))
    return name, elf


def emit_header(built):
    lines = [
        "/* Generated by tools/build_apps.py -- do not edit.",
        " *",
        " * The app binaries, embedded so first boot can write them to the",
        " * filesystem. Without this the apps would only reach the device via a",
        " * card reader, which is not in the loop.",
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
    lines.append("#endif /* CARDOS_CAPP_BLOBS_H */")

    with open(HEADER, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    total = sum(os.path.getsize(p) for _, p in built)
    print("  %s (%d bytes of flash)" % (os.path.relpath(HEADER, ROOT), total))


def main():
    if not os.path.exists(GCC):
        raise SystemExit("no Xtensa toolchain at " + TOOLCHAIN)
    os.makedirs(OUT, exist_ok=True)

    srcs = sorted(os.path.join(APPS, f) for f in os.listdir(APPS)
                  if f.endswith(".c"))
    if not srcs:
        raise SystemExit("no apps in " + APPS)

    print("building %d app(s):" % len(srcs))
    built = [build(s) for s in srcs]
    emit_header(built)


if __name__ == "__main__":
    main()
