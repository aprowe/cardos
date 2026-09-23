"""The update routes, against a fixture build directory.

The real firmware.bin is used when there is one, because the one thing worth
checking against reality is that the ELF SHA is read from the right offset.
The apps are made up, so this does not depend on build_apps.py having run.
"""
import os, shutil, sys, tempfile, threading, urllib.request, urllib.error
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
    with open(os.path.join(apps, "pinball.capp"), "wb") as f:
        f.write(b"a")                       # fnv1a32("a") = e40c292c
    with open(os.path.join(apps, "notes.txt"), "wb") as f:
        f.write(b"not an app")

    print("the manifest:")
    m = updates.manifest(firmware=os.path.join(tmp, "none.bin"), apps_dir=apps)
    fails += not check("lists each .capp with its hash and size",
                       m, "app pinball e40c292c 1\n")
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
    updates.FIRMWARE = fw
    updates.APPS_DIR = apps
    app.Handler.chat = chatmod.ChatService(claude="stub")
    srv = ThreadingHTTPServer(("127.0.0.1", 8138), app.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:8138"

    r = get(base + "/update").decode()
    fails += not check("/update is the manifest", r.split("\n")[1],
                       "app pinball e40c292c 1")
    fails += not check("/update/app/NAME is the file",
                       get(base + "/update/app/pinball"), b"a")
    fails += not check("/update/firmware is the image",
                       get(base + "/update/firmware"), bytes(fake))
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
                       get(base + "/update", token="swordfish").decode()[:3], "fir")

    srv.shutdown()
    shutil.rmtree(tmp)
    print("\n%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
