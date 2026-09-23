"""What the device can pull from this PC: the firmware and the app binaries.

Served under /update (the routes are at the bottom). The manifest is one line per thing,
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
import sys
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIRMWARE = os.path.join(ROOT, ".pio", "build", "cardputer", "firmware.bin")
APPS_DIR = os.path.join(ROOT, "build", "apps")
# Which folder each app belongs in. Shared with tools/build_apps.py, which
# seeds the firmware from it; the manifest carries it so a first install
# lands in the folder too, not at the top level.
FOLDERS_FILE = os.path.join(ROOT, "apps", "folders.txt")

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
    return sorted(f for f in os.listdir(apps_dir)
                  if f.endswith(".capp") and os.path.isfile(os.path.join(apps_dir, f)))


def load_folders(path=None):
    """{app: folder} from apps/folders.txt; top-level apps ("-") are left out."""
    out = {}
    try:
        with open(path or FOLDERS_FILE, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].split()
                if len(line) == 2 and line[1] != "-" and valid_name(line[1]):
                    out[line[0]] = line[1]
    except OSError:
        pass
    return out


def manifest(firmware=None, apps_dir=None, folders=None):
    """The locations default at call time, not definition time, so a test
    can point the module somewhere else."""
    firmware = firmware or FIRMWARE
    apps_dir = apps_dir or APPS_DIR
    folders = load_folders() if folders is None else folders
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
        stem = name[:-len(".capp")]
        line = "app %s %08x %d" % (stem, fnv1a32(data), len(data))
        # Last, so firmware that predates it reads the line and ignores it.
        if folders.get(stem):
            line += " " + folders[stem]
        lines.append(line)
    return "".join(l + "\n" for l in lines)


def valid_name(name):
    """A name is letters, digits and the odd dash -- never a path, whatever
    the request says."""
    return bool(name) and len(name) <= 32 and \
        all(c.isalnum() or c in "-_" for c in name)


def app_path(name, apps_dir=None):
    """The file for `name`, or None."""
    if not valid_name(name):
        return None
    p = os.path.join(apps_dir or APPS_DIR, name + ".capp")
    return p if os.path.isfile(p) else None


# ---- the store: what a server with no build tree serves -------------------
#
# With --store, server/build.py publishes finished builds here and /update
# reads from here exactly as it would read a build tree -- same manifest,
# same hashes -- but never a half-written one.

def store_paths(store):
    """(firmware, apps_dir) inside a store directory."""
    return os.path.join(store, "firmware.bin"), os.path.join(store, "apps")


def check_artifact(kind, data):
    """None if `data` could plausibly be a `kind` ("firmware" or "app"), or a
    sentence saying why not. Not a verification -- the device does that --
    only a refusal of what is obviously not the thing, such as an HTML error
    page uploaded by a script that did not check its status."""
    if kind == "firmware":
        return None if firmware_sha(data) else "not an ESP-IDF image"
    if kind == "app":
        return None if data[:4] == b"\x7fELF" else "not an ELF file"
    return "unknown kind"


def put_artifact(path, data):
    """Write `data` to `path` so that a reader sees the old file or the new
    one and never a half: a temporary file beside it, then a rename over it."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ---- routes ---------------------------------------------------------------
#
# Anything served here lands on the device and runs, so all of it is behind
# the token.

def get_manifest(h, path, args):
    """what can be installed: firmware sha, app hashes"""
    firmware, apps_dir = h.update_files()
    h.text(manifest(firmware=firmware, apps_dir=apps_dir))


def get_firmware(h, path, args):
    """the firmware image"""
    firmware, _ = h.update_files()
    if not os.path.isfile(firmware):
        h.text("no firmware built\n", 404)
        return
    h.file(firmware)
    sys.stderr.write("update: sent firmware\n")


def get_app(h, path, args):
    """one .capp"""
    _, apps_dir = h.update_files()
    p = app_path(path[len("/update/app/"):], apps_dir)
    if not p:
        h.text("no such app\n", 404)
        return
    h.file(p)
    sys.stderr.write("update: sent %s\n" % os.path.basename(p))


ROUTES = [
    ("GET", "/update", get_manifest),
    ("GET", "/update/firmware", get_firmware),
    ("GET", "/update/app/*", get_app),
]
