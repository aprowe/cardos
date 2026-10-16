"""What the device can pull from this PC: the firmware and the app binaries.

Served under /update (the routes are at the bottom). The manifest is one line per thing,
whitespace separated, because the device has no JSON parser:

    firmware <sha256 of the ELF, hex> <size>
    app <name> <fnv1a32 of the file, hex> <size> [folder]
    icon <fnv1a32 of the file, hex> <size> <name, to the end of the line>

The icon lines are the launcher's colour icons, /apps/icons/NAME.cic on the
card, so that a new app's picture arrives with `update apps` rather than only
inside a firmware. They are sent only when the device asks (?icons=1), and
after the apps. Firmware from before them would skip the lines -- manifest.c
has always passed over a first word it does not know -- but it reads the
manifest into 4 KB, and fifty icons would push the last apps past the end.
An icon is named as the launcher looks it up, by the app's own name, which
may have a space in it ("Dashboard Link"); hence last on the line.

There are two firmware builds, and ?flavor= picks which one the manifest and
/update/firmware are about: "debug" (the `cardputer` env, -Og) or "release"
(`-e release`, -O2). A device asks for the flavor it is running unless told
otherwise (`update all debug`). No flavor at all is firmware older than the
split, and it is given release. The apps are the same for both.

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
FIRMWARE = os.path.join(ROOT, ".pio", "build", "cardputer", "firmware.bin")   # debug
FIRMWARE_RELEASE = os.path.join(ROOT, ".pio", "build", "release", "firmware.bin")

FLAVORS = ("release", "debug")
DEFAULT_FLAVOR = "release"


def flavor_arg(args):
    """The ?flavor= of a request, or the default. Anything else is refused
    rather than quietly served the default: a device that asked for a
    flavor must get that one or be told."""
    raw = (args.get("flavor") or [""])[0]
    if not raw:
        return DEFAULT_FLAVOR
    if raw not in FLAVORS:
        raise ValueError("flavor=%r is not one of %s" % (raw, ", ".join(FLAVORS)))
    return raw


def firmware_path(flavor):
    """The build tree's image for a flavor. Read from the module at call
    time, so a test can point FIRMWARE somewhere else."""
    return FIRMWARE if flavor == "debug" else FIRMWARE_RELEASE
APPS_DIR = os.path.join(ROOT, "build", "apps")
# The .cic files, beside the apps: tools/make_color_icons.py writes them (and
# tools/build_apps.py has it do so, so the droplet's build has them too).
ICONS_DIR = os.path.join(ROOT, "build", "icons")
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


def firmware_version(data):
    """The version the image carries (`git describe` when it was built:
    "v0.15.1" or "v0.15.1-2-gabc1234"), or None."""
    off = APP_DESC_OFFSET + 16                          # magic, secure_version, reserved
    if len(data) < off + 32 or struct.unpack_from("<I", data, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        return None
    v = data[off:off + 32].split(b"\0", 1)[0].decode("ascii", "replace").strip()
    return v or None


_server_version = None


def server_version():
    """This server's version: `git describe` of the tree it runs from, read once."""
    global _server_version
    if _server_version is None:
        import subprocess
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        try:
            _server_version = subprocess.run(
                ["git", "describe", "--tags", "--always", "--dirty"], cwd=root,
                capture_output=True, text=True, timeout=10).stdout.strip() or "unknown"
        except Exception:                               # noqa: BLE001 - only a label
            _server_version = "unknown"
    return _server_version


def app_files(apps_dir=None):
    apps_dir = apps_dir or APPS_DIR
    if not os.path.isdir(apps_dir):
        return []
    return sorted(f for f in os.listdir(apps_dir)
                  if f.endswith(".capp") and os.path.isfile(os.path.join(apps_dir, f)))


def icons_dir_for(apps_dir):
    """Where the icons are for a given apps directory: its sibling `icons`
    -- build/icons beside build/apps, STORE/icons beside STORE/apps."""
    return os.path.join(os.path.dirname(os.path.normpath(apps_dir)), "icons")


def icon_files(icons_dir):
    if not icons_dir or not os.path.isdir(icons_dir):
        return []
    return sorted(f for f in os.listdir(icons_dir)
                  if f.endswith(".cic") and valid_icon_name(f[:-len(".cic")])
                  and os.path.isfile(os.path.join(icons_dir, f)))


def valid_icon_name(name):
    """An icon is named by its app's name: letters, digits, '-', '_' and
    inner spaces. Never a path. 23 characters at most, which is what the
    device keeps (MANIFEST_ICON_NAME_MAX)."""
    return bool(name) and len(name) <= 23 and name == name.strip() and \
        all(c.isalnum() and c.isascii() or c in "-_ " for c in name)


def icon_path(name, icons_dir):
    """The file for icon `name`, or None."""
    if not valid_icon_name(name):
        return None
    p = os.path.join(icons_dir, name + ".cic")
    return p if os.path.isfile(p) else None


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


def manifest(firmware=None, apps_dir=None, folders=None, icons_dir=None, icons=False):
    """The locations default at call time, not definition time, so a test
    can point the module somewhere else. `icons` adds the icon lines, from
    `icons_dir` (by default the one beside `apps_dir`)."""
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
            # Firmware that does not know these lines skips them (manifest.c).
            v = firmware_version(data)
            if v:
                lines.append("version firmware %s" % v)
    for name in app_files(apps_dir):
        with open(os.path.join(apps_dir, name), "rb") as f:
            data = f.read()
        stem = name[:-len(".capp")]
        line = "app %s %08x %d" % (stem, fnv1a32(data), len(data))
        # Last, so firmware that predates it reads the line and ignores it.
        if folders.get(stem):
            line += " " + folders[stem]
        lines.append(line)
    if icons:
        for fn in icon_files(icons_dir or icons_dir_for(apps_dir)):
            with open(os.path.join(icons_dir or icons_dir_for(apps_dir), fn), "rb") as f:
                data = f.read()
            lines.append("icon %08x %d %s" % (fnv1a32(data), len(data), fn[:-len(".cic")]))
    if lines:
        lines.append("version server %s" % server_version())
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

def store_paths(store, flavor=DEFAULT_FLAVOR):
    """(firmware, apps_dir) inside a store directory. The debug image keeps
    the name it had before there were two."""
    name = "firmware.bin" if flavor == "debug" else "firmware-release.bin"
    return os.path.join(store, name), os.path.join(store, "apps")


def check_artifact(kind, data):
    """None if `data` could plausibly be a `kind` ("firmware" or "app"), or a
    sentence saying why not. Not a verification -- the device does that --
    only a refusal of what is obviously not the thing, such as an HTML error
    page uploaded by a script that did not check its status."""
    if kind == "firmware":
        return None if firmware_sha(data) else "not an ESP-IDF image"
    if kind == "app":
        return None if data[:4] == b"\x7fELF" else "not an ELF file"
    if kind == "icon":
        return None if data[:4] == b"CIC1" else "not a .cic icon"
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
    """what can be installed: firmware sha, app hashes (?flavor=debug|release),
    and with ?icons=1 the colour icons' hashes"""
    firmware, apps_dir = h.update_files(flavor_arg(args))
    icons = (args.get("icons") or [""])[0] == "1"
    h.text(manifest(firmware=firmware, apps_dir=apps_dir, icons=icons))


def get_firmware(h, path, args):
    """the firmware image (?flavor=debug|release)"""
    flavor = flavor_arg(args)
    firmware, _ = h.update_files(flavor)
    if not os.path.isfile(firmware):
        h.text("no %s firmware built\n" % flavor, 404)
        return
    h.file(firmware)
    sys.stderr.write("update: sent %s firmware\n" % flavor)


def get_app(h, path, args):
    """one .capp"""
    _, apps_dir = h.update_files()
    p = app_path(path[len("/update/app/"):], apps_dir)
    if not p:
        h.text("no such app\n", 404)
        return
    h.file(p)
    sys.stderr.write("update: sent %s\n" % os.path.basename(p))


def get_icon(h, path, args):
    """one colour icon (.cic), by the app's name: /update/icon/Dashboard%20Link"""
    import urllib.parse
    _, apps_dir = h.update_files()
    p = icon_path(urllib.parse.unquote(path[len("/update/icon/"):]), icons_dir_for(apps_dir))
    if not p:
        h.text("no such icon\n", 404)
        return
    h.file(p)


ROUTES = [
    ("GET", "/update", get_manifest),
    ("GET", "/update/firmware", get_firmware),
    ("GET", "/update/app/*", get_app),
    ("GET", "/update/icon/*", get_icon),
]
