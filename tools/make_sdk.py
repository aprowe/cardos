#!/usr/bin/env python3
"""Write the CardOS app SDK: what a repository of apps of its own builds
and tests against, without the kernel's source.

    python tools/make_sdk.py OUT

OUT then holds, at the paths the apps already include them by:

    kernel/app/capp.h      the API table, and what an app hands the kernel
    kernel/app/capprun.h   (the app table's size, which the build checks)
    apps/capp.ld           the link script: the one hardware fact that shapes
                           every app (apps/capp.ld says which)
    apps/*.h               the headers apps share: str.h, footer.h,
                           confirm.h, safefile.h, termlog.h, social.h, ...
    test/                  tinytest.h, fakeapi.h/.c, fakescreen.h and the
                           6x8 and .cfnt fonts it draws with, for host tests
    tools/build_apps.py    the build, with appmeta.py and make_color_icons.py
    VERSION                the API version these headers are, and the oldest
                           a firmware that has them will load (CAPP_API_MIN)

An app repository is laid out like this one's apps/ (tools/appmeta.py): its
apps are NAME.c or NAME/NAME.c, each with its `@app` lines, its
NAME.icon.txt, its test/ and, if it has one, its server/. Build them with

    python OUT/tools/build_apps.py --apps PATH/TO/REPO/apps

which writes OUT/build/apps/*.capp (and their colour icons in
OUT/build/icons) for the server's store. An app built against this SDK
loads on any firmware from CAPP_API_MIN to its CAPP_API_VERSION and later
(kernel/app/capp.h says the rules): it checks api->version before calling
anything newer than CAPP_API_MIN.
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = [
    "kernel/app/capp.h",
    "kernel/app/capprun.h",
    "kernel/console/font6x8.h",
    "kernel/ui/cfont.h",
    "kernel/ui/cfont.c",
    "apps/capp.ld",
    "test/tinytest.h",
    "test/fakeapi.h",
    "test/fakeapi.c",
    "test/fakescreen.h",
    "tools/build_apps.py",
    "tools/appmeta.py",
    "tools/make_color_icons.py",
]
# The headers more than one app uses: every .h directly in apps/. An app's
# own headers live in its folder and are not the SDK's.
SHARED_DIR = "apps"


def api_versions():
    with open(os.path.join(ROOT, "kernel", "app", "capp.h"), encoding="utf-8") as f:
        s = f.read()
    v = int(re.search(r"#define\s+CAPP_API_VERSION\s+(\d+)", s).group(1))
    m = re.search(r"#define\s+CAPP_API_MIN\s+(\d+)", s)
    return v, int(m.group(1)) if m else v


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1 or argv[0].startswith("-"):
        raise SystemExit(__doc__.split("\n\n")[1])
    out = os.path.abspath(argv[0])
    if os.path.normcase(out).startswith(os.path.normcase(ROOT + os.sep)):
        raise SystemExit("write the SDK outside this repository")
    files = list(FILES) + sorted(
        "%s/%s" % (SHARED_DIR, f) for f in os.listdir(os.path.join(ROOT, SHARED_DIR))
        if f.endswith(".h") and os.path.isfile(os.path.join(ROOT, SHARED_DIR, f)))
    for rel in files:
        src = os.path.join(ROOT, rel)
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    v, lo = api_versions()
    with open(os.path.join(out, "VERSION"), "w", encoding="utf-8", newline="\n") as f:
        f.write("api %d\nmin %d\n" % (v, lo))
    print("%s: %d files, API %d (loads on firmware from API %d)" % (out, len(files) + 1, v, lo))


if __name__ == "__main__":
    main()
