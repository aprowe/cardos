"""The update routes, against a fixture build directory.

The real firmware.bin is used when there is one, because the one thing worth
checking against reality is that the ELF SHA is read from the right offset.
The apps are made up, so this does not depend on build_apps.py having run.
"""
import os, shutil, sys, tempfile, threading, unittest, urllib.request, urllib.error
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import app, updates
from server import chat as chatmod
from http.server import ThreadingHTTPServer


def get(url, token=None):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    return urllib.request.urlopen(req, timeout=10).read()


def check(label, got, want):
    ok = "ok  " if got == want else "FAIL"
    shown = repr(got if ok == "FAIL" else want)
    print("  %s %-50s %s" % (ok, label, shown[:80]))
    return ok == "ok  "


def main():
    fails = 0
    tmp = tempfile.mkdtemp()
    apps = os.path.join(tmp, "apps")
    os.mkdir(apps)
    with open(os.path.join(apps, "mines.capp"), "wb") as f:
        f.write(b"a")                       # fnv1a32("a") = e40c292c
    with open(os.path.join(apps, "notes.txt"), "wb") as f:
        f.write(b"not an app")
    icons = os.path.join(tmp, "icons")              # beside apps, as build/icons is
    os.mkdir(icons)
    cic = b"CIC1" + bytes(516)
    with open(os.path.join(icons, "Dashboard Link.cic"), "wb") as f:
        f.write(cic)
    with open(os.path.join(icons, "bad.name.cic"), "wb") as f:
        f.write(cic)

    print("the manifest:")
    m = updates.manifest(firmware=os.path.join(tmp, "none.bin"), apps_dir=apps,
                         folders={})
    fails += not check("lists each .capp with its hash and size",
                       m, "app mines e40c292c 1\nversion server %s\n" % updates.server_version())
    m = updates.manifest(firmware=os.path.join(tmp, "none.bin"), apps_dir=apps,
                         folders={"mines": "Games"})
    fails += not check("and the folder it belongs in, last",
                       m, "app mines e40c292c 1 Games\nversion server %s\n"
                       % updates.server_version())
    m = updates.manifest(firmware=os.path.join(tmp, "none.bin"), apps_dir=apps,
                         folders={}, icons=True)
    fails += not check("asked, the icons follow the apps, named to the line's end",
                       m, "app mines e40c292c 1\nicon %08x 520 Dashboard Link\n"
                       "version server %s\n" % (updates.fnv1a32(cic), updates.server_version()))
    fails += not check("an icon name is never a path",
                       [updates.valid_icon_name(n) for n in
                        ("Dashboard Link", "folder-Games", "../x", "a/b", "a.b", " x", "")],
                       [True, True, False, False, False, False, False])
    fails += not check("each app says its own folder (@app folder)",
                       updates.load_folders().get("timer"), "Plan")
    fails += not check("where '-' means the top level",
                       "grep" in updates.load_folders(), False)
    fails += not check("fnv1a of the empty string is the reference value",
                       updates.fnv1a32(b""), 0x811c9dc5)

    fake = bytearray(400)
    fake[32:36] = (0xABCD5432).to_bytes(4, "little")
    fake[32 + 144:32 + 176] = bytes(range(32))
    fw = os.path.join(tmp, "firmware.bin")
    with open(fw, "wb") as f:
        f.write(fake)
    m = updates.manifest(firmware=fw, apps_dir=apps)
    fails += not check("the firmware line carries the descriptor's SHA",
                       m.split("\n")[0],
                       "firmware " + bytes(range(32)).hex() + " 400")
    fails += not check("a file without a descriptor is not offered",
                       updates.firmware_sha(b"\xe9" * 400), None)

    if os.path.isfile(updates.FIRMWARE):
        with open(updates.FIRMWARE, "rb") as f:
            data = f.read()
        sha = updates.firmware_sha(data)
        fails += not check("the real build has a readable SHA",
                           bool(sha) and sha != "00" * 32, True)

    print("the routes:")
    # Two images that differ, so which one was served is visible.
    fake_rel = bytearray(fake)
    fake_rel[32 + 144:32 + 176] = bytes(range(100, 132))
    fw_rel = os.path.join(tmp, "firmware-release.bin")
    with open(fw_rel, "wb") as f:
        f.write(fake_rel)
    updates.FIRMWARE = fw
    updates.FIRMWARE_RELEASE = fw_rel
    updates.APPS_DIR = apps
    app.Handler.chat = chatmod.ChatService(claude="stub")
    srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_address[1]

    r = get(base + "/update").decode()
    fails += not check("/update is the manifest", r.split("\n")[1],
                       "app mines e40c292c 1 Games")
    fails += not check("/update/app/NAME is the file",
                       get(base + "/update/app/mines"), b"a")
    fails += not check("no icons unless asked: older firmware reads 4 KB",
                       "icon " in r, False)
    fails += not check("?icons=1 lists them",
                       [l for l in get(base + "/update?icons=1").decode().split("\n")
                        if l.startswith("icon ")],
                       ["icon %08x 520 Dashboard Link" % updates.fnv1a32(cic)])
    fails += not check("/update/icon/NAME is the file, the space escaped",
                       get(base + "/update/icon/Dashboard%20Link"), cic)
    for bad in ("..%2Ficons%2FDashboard%20Link", "nothing", "bad.name"):
        try:
            get(base + "/update/icon/" + bad)
            fails += not check("icon %s is 404" % bad, "served", 404)
        except urllib.error.HTTPError as e:
            fails += not check("icon %s is 404" % bad, e.code, 404)
    fails += not check("no flavor is firmware older than the split: release",
                       r.split("\n")[0],
                       "firmware " + bytes(range(100, 132)).hex() + " 400")
    fails += not check("?flavor=debug is the debug build's manifest",
                       get(base + "/update?flavor=debug").decode().split("\n")[0],
                       "firmware " + bytes(range(32)).hex() + " 400")
    fails += not check("and the apps are the same for both",
                       get(base + "/update?flavor=debug").decode().split("\n")[1:],
                       r.split("\n")[1:])
    fails += not check("/update/firmware is the release image",
                       get(base + "/update/firmware"), bytes(fake_rel))
    fails += not check("/update/firmware?flavor=release too",
                       get(base + "/update/firmware?flavor=release"), bytes(fake_rel))
    fails += not check("/update/firmware?flavor=debug is the debug image",
                       get(base + "/update/firmware?flavor=debug"), bytes(fake))
    try:
        get(base + "/update/firmware?flavor=nightly")
        fails += not check("an unknown flavor is refused", "served", "refused")
    except urllib.error.HTTPError as e:
        fails += not check("an unknown flavor is refused", e.code, 500)
    os.remove(fw_rel)
    try:
        get(base + "/update/firmware")
        fails += not check("a flavor not built is 404", "served", 404)
    except urllib.error.HTTPError as e:
        fails += not check("a flavor not built is 404", e.code, 404)
    fails += not check("and its manifest offers no firmware",
                       get(base + "/update").decode().split("\n")[0][:4], "app ")
    try:
        get(base + "/update/app/../../etc/passwd")
        fails += not check("a path is not a name", "served", "refused")
    except urllib.error.HTTPError as e:
        fails += not check("a path is not a name", e.code, 404)
    try:
        get(base + "/update/app/nothing")
        fails += not check("an unknown app is 404", "served", 404)
    except urllib.error.HTTPError as e:
        fails += not check("an unknown app is 404", e.code, 404)

    print("the shared secret:")
    app.Handler.chat.token = "swordfish"
    try:
        get(base + "/update")
        fails += not check("no token, no manifest", "allowed", "refused")
    except urllib.error.HTTPError as e:
        fails += not check("no token, no manifest", e.code, 403)
    fails += not check("with the token it is served",
                       get(base + "/update?flavor=debug", token="swordfish").decode()[:3],
                       "fir")

    srv.shutdown()
    srv.server_close()
    shutil.rmtree(tmp)
    print("\n%d failure(s)" % fails)
    return 1 if fails else 0


class Script(unittest.TestCase):
    """So `unittest discover` runs this too -- the token check on /update,
    the route that ships firmware, was outside the suite."""

    def test_the_whole_script(self):
        # main() points updates at its fixtures; put them back after.
        for name in ("FIRMWARE", "FIRMWARE_RELEASE", "APPS_DIR"):
            self.addCleanup(setattr, updates, name, getattr(updates, name))
        self.addCleanup(setattr, app.Handler, "chat", app.Handler.chat)
        self.assertEqual(main(), 0)


if __name__ == "__main__":
    sys.exit(main())
