"""What the device can pull from this PC: the firmware and the app binaries.

Served by webproxy.py under /update. The manifest is one line per thing,
whitespace separated, because the device has no JSON parser:

    firmware <sha256 of the ELF, hex> <size>
    app <name> <fnv1a32 of the file, hex> <size>

Identity is a hash, never a version number. The firmware hash is the one
every ESP-IDF image already carries in its esp_app_desc_t -- the device reads
its own with esp_app_get_description() and compares -- so nothing has to be
bumped, and a rebuild that changed nothing is correctly not an update. The
app hash is FNV-1a 32, the function kernel/ui/icons.c already uses for the
blob stamp, computed over the .capp file exactly as it sits on the card.

Anything served here lands on the device and runs, so /update is behind the
same token as /chat.
"""
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, ".pio", "build", "cardputer", "firmware.bin")
APPS_DIR = os.path.join(ROOT, "build", "apps")

# esp_app_desc_t sits after the 24-byte image header and the first 8-byte
# segment header. Within it: magic u32, secure_version u32, reserv1 u32[2],
# version[32], project_name[32], time[16], date[16], idf_ver[32], then the
# ELF SHA-256. Checked against a real image in the commit that added this.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_SHA_OFFSET = 144


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def firmware_sha(data):
    """The ELF SHA-256 from the image's app descriptor, or None if this is
    not an image that carries one."""
    if len(data) < APP_DESC_OFFSET + APP_DESC_SHA_OFFSET + 32:
        return None
    (magic,) = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return None
    off = APP_DESC_OFFSET + APP_DESC_SHA_OFFSET
    return data[off:off + 32].hex()


def app_files(apps_dir=None):
    apps_dir = apps_dir or APPS_DIR
    if not os.path.isdir(apps_dir):
        return []
    return sorted(f for f in os.listdir(apps_dir) if f.endswith(".capp"))


def manifest(firmware=None, apps_dir=None):
    """The locations default at call time, not definition time, so a test
    can point the module somewhere else."""
    firmware = firmware or FIRMWARE
    apps_dir = apps_dir or APPS_DIR
    lines = []
    if os.path.isfile(firmware):
        with open(firmware, "rb") as f:
            data = f.read()
        sha = firmware_sha(data)
        if sha:
            lines.append("firmware %s %d" % (sha, len(data)))
    for name in app_files(apps_dir):
        with open(os.path.join(apps_dir, name), "rb") as f:
            data = f.read()
        lines.append("app %s %08x %d" % (name[:-len(".capp")], fnv1a32(data), len(data)))
    return "".join(l + "\n" for l in lines)


def app_path(name, apps_dir=None):
    """The file for `name`, or None. A name is letters, digits and the odd
    dash -- never a path, whatever the request says."""
    if not name or not all(c.isalnum() or c in "-_" for c in name):
        return None
    p = os.path.join(apps_dir or APPS_DIR, name + ".capp")
    return p if os.path.isfile(p) else None
