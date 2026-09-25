"""Build after the turn: what makes Build work with no PC in the loop.

python -m server --build --store DIR uses BuildingChat in place of ChatService. A
Build message is still a turn of Claude Code in this repository -- on the
droplet, the repository is a git clone -- and then, if the turn changed
anything that runs on the device:

  1. the apps are rebuilt (tools/build_apps.py), and the firmware too if
     anything outside apps/ changed;
  2. whatever now differs from the store is published into it, atomically --
     the store is what /update serves, never the build tree, which is
     half-written while a build runs;
  3. the turn is committed on whatever branch the clone has checked out
     ("remote"), so the laptop can `git pull droplet remote` and see it.

A turn that does not build is committed too, marked as such, and nothing is
published: the work is not lost, and the device is not handed it.

The agent here gets no Bash. The server runs the build itself, so the agent
needs only to read and edit files, and a leaked device token is then a way to
propose code rather than a shell on a public host.
See docs/superpowers/specs/2026-09-22-remote-build-design.md.
"""
import hashlib
import os
import subprocess
import sys
import threading

from . import updates
from .chat import ChatService, ROOT

BUILD_TIMEOUT = 1800          # a first firmware build on one core is slow
ERROR_TAIL = 1200

# Paths whose change needs no build: prose, the PC-side tools, the host
# test suite and this server, none of which the device ever sees.
NO_BUILD = ("docs/", "tools/", "test/", "host/", "server/")

_CLEAN = object()


def changed(before, after):
    """Paths whose state differs between two snapshots. A path absent from a
    snapshot was clean, which is a state of its own: a dirty file reverted to
    clean changed, and so did a clean file deleted (hashed as None)."""
    return {p for p in set(before) | set(after)
            if before.get(p, _CLEAN) != after.get(p, _CLEAN)}


def build_plan(paths):
    """(build apps, build firmware) for a set of changed paths.

    The firmware is not rebuilt for an app-only change. It embeds every app as
    a seed blob, so a rebuild would give it a new hash, and every app edit
    would show up on the device as an OS update as well."""
    code = {p for p in paths
            if not p.startswith(NO_BUILD) and not p.lower().endswith(".md")}
    return bool(code), any(not p.startswith("apps/") for p in code)


def needs_fonts(paths):
    """Did the turn ask for a font? A line in fonts/fonts.txt is how an app
    requests one (CLAUDE.md), and the agent has no shell to run
    make_cfnt.py itself -- so the build does, only then."""
    return any(p.startswith("fonts/") for p in paths)


def git_snapshot(root=ROOT):
    """{path: sha1 of its content, or None if deleted} for every path git
    calls changed or untracked. Content, not status letters: a turn that edits
    an already-modified file leaves the letters exactly as they were."""
    out = subprocess.run(["git", "status", "--porcelain=v1", "-z", "-uall"],
                         cwd=root, capture_output=True, check=True).stdout
    entries = out.split(b"\0")
    snap, i = {}, 0
    while i < len(entries):
        e = entries[i]
        i += 1
        if len(e) < 4:
            continue
        status, path = e[:2], e[3:].decode("utf-8", "replace")
        if status[:1] in (b"R", b"C"):
            i += 1                      # a rename's old name follows
        full = os.path.join(root, path)
        if os.path.isfile(full):
            with open(full, "rb") as f:
                snap[path] = hashlib.sha1(f.read()).hexdigest()
        else:
            snap[path] = None
    return snap


def errors_tail(log):
    """The compiler's complaints if it made any, else the end of the log. The
    reply is read on a 240-pixel screen."""
    lines = log.splitlines()
    errs = [l for l in lines if "error" in l.lower()]
    text = "\n".join(errs[-10:]) if errs else "\n".join(lines[-15:])
    return text[-ERROR_TAIL:]


def publish(store, firmwares, apps_dir, firmware):
    """Copy into the store whatever differs from it. Returns the names
    published ("firmware" for debug, "firmware-release", or an app's name).

    `firmwares` is {flavor: image}; a bare path is the debug one alone, which
    is what there was before the release build. The firmware only when this
    turn built it: see build_plan. A flavor whose image is missing is skipped
    rather than failed, so a tree that has only built one still publishes it."""
    if isinstance(firmwares, str):
        firmwares = {"debug": firmwares}
    sent = []
    # Every artifact is checked before any is written: a bad one must not
    # leave the store half updated.
    todo = []
    for flavor in updates.FLAVORS:
        src = firmwares.get(flavor)
        if not firmware or not src or not os.path.isfile(src):
            continue
        store_fw, _ = updates.store_paths(store, flavor)
        have = updates.manifest(firmware=store_fw, apps_dir=os.devnull)
        want = updates.manifest(firmware=src, apps_dir=os.devnull)
        if want and want != have:
            todo.append(("firmware" if flavor == "debug" else "firmware-" + flavor,
                         src, store_fw, "firmware"))
    _, store_apps = updates.store_paths(store)
    have = set(updates.manifest(firmware=os.devnull, apps_dir=store_apps).splitlines())
    for line in updates.manifest(firmware=os.devnull, apps_dir=apps_dir).splitlines():
        if line in have:
            continue
        name = line.split()[1]
        todo.append((name, os.path.join(apps_dir, name + ".capp"),
                     os.path.join(store_apps, name + ".capp"), "app"))
    blobs = []
    for name, src, dst, kind in todo:
        with open(src, "rb") as fh:
            data = fh.read()
        why = updates.check_artifact(kind, data)
        if why:
            raise ValueError("%s: %s" % (name, why))
        blobs.append((name, dst, data))
    for name, dst, data in blobs:
        updates.put_artifact(dst, data)
        sent.append(name)
    return sent


class BuildingChat(ChatService):
    """A ChatService whose turns end in a build, a publish and a commit."""

    allowed_tools = "Read,Edit,Write,Glob,Grep,TodoWrite"
    disallowed_tools = "Bash,WebFetch,WebSearch"

    def __init__(self, store, firmware=None, apps_dir=None, **kw):
        super().__init__(**kw)
        self.store = store
        # {flavor: image}. Both are built for a firmware change; see build().
        self.firmware = firmware or {"debug": updates.FIRMWARE,
                                     "release": updates.FIRMWARE_RELEASE}
        self.apps_dir = apps_dir or updates.APPS_DIR
        # The whole turn, build included, is one unit: a second message must
        # not start editing while the first one's build is reading the tree.
        self.turn_lock = threading.Lock()

    def snapshot(self):
        return git_snapshot(self.cwd)

    def build(self, apps, firmware, fonts=False):
        """(ok, log). Fonts, then the apps: the firmware embeds both."""
        steps = []
        if fonts:
            steps.append([sys.executable, os.path.join("tools", "make_cfnt.py"), "--all"])
        if apps:
            steps.append([sys.executable, os.path.join("tools", "build_apps.py")])
        if firmware:
            # Both flavors: the device updates to whichever it runs, so a
            # change that reached only one would leave the other behind.
            steps.append([sys.executable, "-m", "platformio", "run",
                          "-e", "cardputer", "-e", "release"])
        log = ""
        for cmd in steps:
            try:
                r = subprocess.run(cmd, cwd=self.cwd, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace",
                                   timeout=BUILD_TIMEOUT)
            except subprocess.TimeoutExpired:
                return False, "%s took more than %d s" % (cmd[-1], BUILD_TIMEOUT)
            log += (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0:
                return False, log
        return True, log

    def commit(self, message):
        """Commit everything the turn left, on the checked-out branch. The
        short hash, or None if there was nothing to commit."""
        git = ["git", "-c", "user.name=CardOS droplet",
               "-c", "user.email=cardos@droplet.invalid"]
        subprocess.run(git + ["add", "-A"], cwd=self.cwd, check=True,
                       capture_output=True)
        r = subprocess.run(git + ["commit", "-q", "-m", message], cwd=self.cwd,
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=self.cwd,
                              capture_output=True, text=True).stdout.strip()

    def run_turn(self, text, report=None, log=None):
        report = report or (lambda line: None)
        log = log or (lambda line: None)
        with self.turn_lock:
            before = self.snapshot()
            # A step that stopped still leaves its edits, and they are built:
            # whatever it managed is on disk, and the reply says where it
            # stopped.
            state, reply = super().run_turn(text, report=report, log=log)
            paths = changed(before, self.snapshot())
            if not paths:
                return state, reply
            title = "Build: " + " ".join(text.split())[:60]
            apps, firmware = build_plan(paths)
            if not (apps or firmware):
                self.commit(title + "\n\n" + text)
                return state, reply
            report("building firmware" if firmware else "building apps")
            log("-- building %s (%d files changed)"
                % ("apps and firmware" if firmware else "apps", len(paths)))
            ok, out = self.build(apps, firmware, needs_fonts(paths))
            if not ok:
                self.commit(title + " (does not build)\n\n" + text)
                return state, (reply + "\n\nbuild failed, nothing published:\n"
                               + errors_tail(out))
            report("publishing")
            log("-- built; publishing")
            try:
                sent = publish(self.store, self.firmware, self.apps_dir, firmware)
            except (OSError, ValueError) as e:
                self.commit(title + " (not published)\n\n" + text)
                return state, reply + "\n\npublish failed: %s" % e
            self.commit(title + "\n\n" + text)
            if sent:
                return state, (reply + "\n\npublished: " + ", ".join(sent)
                               + " -- /update installs")
            return state, reply + "\n\nbuilt; nothing new to install"


# ---- deploying: tools/deploy_droplet.sh --------------------------------------
#
# The deploy used to build and publish both firmwares every time. The firmware
# embeds every app as a seed blob, so an app-only change gave it a new hash and
# every device was offered an OS update that changed nothing but the apps it
# carries -- which `update apps` delivers anyway. The same rule as build_plan,
# applied to what changed since the store's firmware was last published.

FIRMWARE_REV = "firmware.rev"     # in the store: the commit its firmware is


def firmware_due(store, root=ROOT):
    """(due, why): does a deploy at HEAD need to build and publish firmware?"""
    for flavor in updates.FLAVORS:
        fw, _ = updates.store_paths(store, flavor)
        if not os.path.isfile(fw):
            return True, "the store has no %s firmware" % flavor
    try:
        with open(os.path.join(store, FIRMWARE_REV), encoding="utf-8") as f:
            rev = f.read().strip()
    except OSError:
        return True, "no record of which commit the store's firmware was built from"
    r = subprocess.run(["git", "diff", "--name-only", rev, "HEAD"], cwd=root,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return True, "cannot diff from %s" % rev[:12]
    paths = {p for p in r.stdout.splitlines() if p}
    if build_plan(paths)[1]:
        outside = sorted(p for p in paths if not p.startswith("apps/"))
        return True, "changed outside apps/: " + ", ".join(outside[:4]) + \
            (" ..." if len(outside) > 4 else "")
    return False, "nothing outside apps/ changed since %s" % rev[:12]


def record_firmware(store, root=ROOT):
    """Remember HEAD as the commit the store's firmware now is."""
    rev = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True,
                         text=True, check=True).stdout.strip()
    updates.put_artifact(os.path.join(store, FIRMWARE_REV), (rev + "\n").encode())


def deploy(store, always=False, root=ROOT, run=subprocess.run):
    """Build the apps, the firmware if due, and publish. 0 or 1, for a shell."""
    due, why = (True, "asked for") if always else firmware_due(store, root)
    print("   firmware: %s (%s)" % ("building" if due else "skipped", why))
    steps = [[sys.executable, os.path.join("tools", "build_apps.py")]]
    if due:
        steps.append([sys.executable, "-m", "platformio", "run",
                      "-e", "cardputer", "-e", "release"])
    for cmd in steps:
        r = run(cmd, cwd=root, capture_output=True, text=True,
                encoding="utf-8", errors="replace")
        out = (r.stdout or "") + (r.stderr or "")
        keep = [l for l in out.splitlines()
                if any(w in l for w in ("Flash:", "SUCCESS", "FAILED", "rror", "commands in"))]
        for line in keep[-6:]:
            print("  " + line)
        if r.returncode != 0:
            print("   build failed; nothing published")
            return 1
    fws = {"debug": updates.FIRMWARE, "release": updates.FIRMWARE_RELEASE}
    sent = publish(store, fws, updates.APPS_DIR, due)
    if due:
        record_firmware(store, root)
    print("   published:", ", ".join(sent) or "nothing")
    return 0


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="build and publish into a store")
    ap.add_argument("command", choices=["deploy"])
    ap.add_argument("--store", required=True)
    ap.add_argument("--firmware", choices=["auto", "always"], default="auto",
                    help="auto: only when something outside apps/ changed")
    a = ap.parse_args()
    sys.exit(deploy(a.store, always=a.firmware == "always"))
