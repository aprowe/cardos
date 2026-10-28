"""What the build, the server and the tools know about each app, read from
the app itself.

An app is apps/NAME.c, or apps/DIR/NAME.c when it has files of its own (a
folder may hold more than one: apps/midi has MIDI and Circuit). Everything
about it lives beside its source -- nothing central lists apps:

    NAME.c            its code; `@app` lines in a comment near the top say
                      what the build needs to know (below)
    NAME.icon.txt     its colour icon: the app's name (as in capp_info) on
                      the first line, sixteen rows of art in the INK letters
                      of tools/make_color_icons.py under it
    test/test_*.c     host tests, run with test/ (a folder app's)
    tools/            its generators and import scripts (a folder app's)
    server/           a server plugin (server/plugins.py; a folder app's)
    README.md         what is worth knowing about it (a folder app's)

The `@app` lines, one key and its value each, are comment lines starting
`@app` (in practice at the end of the top comment):

    @app folder Games        the launcher folder: Plan, Make, Net, Games,
                             System, or - for the top level (CLI apps stay
                             on the default PATH). Required.
    @app release core        core: in a CardOS release; extra (the default):
                             an app that will live in a repo of its own
    @app embed yes           carried inside the firmware, so a blank card
                             can be linked, repaired and updated
    @app budget WHY          allowed over the data budget, and why
    @app multi {JSON}        built from many sources and linked against the
                             toolchain's libc (Python): "dirs" to compile
                             every .c in, "include" dirs, "defines"; dirs are
                             relative to the app's own folder
    @app routes /msg /share  a server plugin's URL prefixes besides /NAME

`python tools/appmeta.py` lists every app and what it says.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(ROOT, "apps")
FOLDER_NAMES = ("Plan", "Make", "Net", "Games", "System")
KEYS = ("folder", "release", "embed", "budget", "multi", "routes")
_LINE = re.compile(r"^\s*(?:/\*|\*|//)\s*@app\s+([a-z]+)\s*(.*?)\s*(?:\*/)?\s*$")


class App:
    def __init__(self, stem, src):
        self.stem = stem
        self.src = src
        self.dir = os.path.dirname(src)
        self.own_dir = self.dir if os.path.normcase(self.dir) != os.path.normcase(APPS) else None
        self.meta = {}

    @property
    def folder(self):
        f = self.meta.get("folder")
        return None if f in (None, "-") else f

    @property
    def placed(self):
        return "folder" in self.meta

    @property
    def release(self):
        return self.meta.get("release", "extra")

    @property
    def embed(self):
        return self.meta.get("embed", "no") == "yes"

    @property
    def budget(self):
        return self.meta.get("budget")

    @property
    def multi(self):
        raw = self.meta.get("multi")
        if not raw:
            return None
        spec = json.loads(raw)
        rel = lambda d: os.path.normpath(os.path.join(self.dir, d))
        return {"dirs": [rel(d) for d in spec.get("dirs", [])],
                "include": [rel(d) for d in spec.get("include", [])],
                "defines": spec.get("defines", []),
                "config": rel(spec["config"]) if spec.get("config") else None}

    @property
    def routes(self):
        return self.meta.get("routes", "").split()

    @property
    def icon(self):
        p = os.path.join(self.dir, self.stem + ".icon.txt")
        return p if os.path.isfile(p) else None

    def __repr__(self):
        return "App(%s, %s)" % (self.stem, os.path.relpath(self.src, ROOT))


def read_meta(src):
    """{key: value} from a source's @app lines."""
    out = {}
    with open(src, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "@app" not in line:
                continue
            m = _LINE.search(line)
            if not m:
                continue
            key, value = m.group(1), m.group(2)
            if key not in KEYS:
                raise SystemExit("%s: @app %s is not one of %s"
                                 % (os.path.relpath(src, ROOT), key, ", ".join(KEYS)))
            out[key] = value
    return out


def app_dirs():
    """Where apps are: this repository's apps/, then each folder in
    CARDOS_APP_DIRS (separated by os.pathsep) -- another repository's apps,
    laid out the same way. The build, the server's plugins and /update all
    look here, so an app kept in a repo of its own is found like any other."""
    out = [APPS]
    for d in (os.environ.get("CARDOS_APP_DIRS") or "").split(os.pathsep):
        d = d.strip()
        if d and os.path.isdir(d) and os.path.normcase(os.path.abspath(d)) not in                 [os.path.normcase(x) for x in out]:
            out.append(os.path.abspath(d))
    return out


def sources(apps_dir=APPS):
    """Every app's source: every .c directly in apps_dir, and every .c
    directly in a folder of it. One level only."""
    srcs = []
    # apps/claude.md is apps/CLAUDE.md on a case-blind disk, which Claude
    # Code loads as instructions for everything under apps/ (2026-10-27):
    # an app with notes and that name is a folder, apps/claude/README.md.
    if any(f.lower() == "claude.md" for f in os.listdir(apps_dir)):
        raise SystemExit("%s/claude.md would be read as CLAUDE.md: make the app a folder"
                         % os.path.relpath(apps_dir, ROOT))
    for f in sorted(os.listdir(apps_dir)):
        p = os.path.join(apps_dir, f)
        if f.endswith(".c") and os.path.isfile(p):
            srcs.append(p)
        elif os.path.isdir(p):
            srcs.extend(os.path.join(p, g) for g in sorted(os.listdir(p))
                        if g.endswith(".c") and os.path.isfile(os.path.join(p, g)))
    return srcs


def apps(*dirs):
    """{stem: App} for every app under each of dirs (app_dirs() by
    default): another repo's apps folder is just one more. Two sources with one
    stem are refused -- the .capp is named by the stem."""
    out = {}
    for d in dirs or app_dirs():
        for src in sources(d):
            stem = os.path.splitext(os.path.basename(src))[0]
            if stem in out:
                raise SystemExit("two apps called %s: %s and %s" % (
                    stem, os.path.relpath(out[stem].src, ROOT), os.path.relpath(src, ROOT)))
            a = App(stem, src)
            a.meta = read_meta(src)
            out[stem] = a
    return out


def folders(*dirs):
    """{stem: folder or None} for every app that says where it goes."""
    return {s: a.folder for s, a in apps(*dirs).items() if a.placed}


def icon_files(*dirs):
    """Every NAME.icon.txt under the app folders, sorted."""
    out = []
    for d in dirs or app_dirs():
        for top, subs, files in os.walk(d):
            subs[:] = [s for s in subs if s not in ("mp", "test", "tools", "server")]
            out += [os.path.join(top, f) for f in files if f.endswith(".icon.txt")]
    return sorted(out)


def plugin_dirs(*dirs):
    """{app folder name: path} for every apps/NAME/server/__init__.py."""
    out = {}
    for d in dirs or app_dirs():
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f, "server")
            if os.path.isfile(os.path.join(p, "__init__.py")):
                out[f] = p
    return out


def main():
    for stem, a in sorted(apps(*sys.argv[1:]).items()):
        print("%-10s %-7s %-6s %-5s %s" % (stem, a.meta.get("folder", "?"), a.release,
                                         "embed" if a.embed else "",
                                         os.path.relpath(a.src, ROOT)))


if __name__ == "__main__":
    main()
