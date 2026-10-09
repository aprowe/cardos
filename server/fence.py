"""What a Build turn may change, for someone who is not the owner.

The owner's turns change anything, as always. Anyone else given Build
(accounts: "build": true) may make new apps and change the apps they made,
and nothing else: not the kernel, not the server, not anybody else's app.

An app's files are
    apps/NAME.c             the app
    apps/NAME_*.h, NAME.h   its own headers
    apps/icons/NAME.txt     its colour icon (tools/make_color_icons.py reads them)
    its line in apps/folders.txt
and a person owns the ones they created. Ownership is recorded the moment a
new file is first written -- by the hook, before the write -- in
CARDOS_STATE/app_owners.json: {"files": {"apps/chess.c": "britney", ...}}.
A file that already exists and is not recorded as theirs is never theirs,
which is every app the owner wrote.

Three layers, any one of which would do, so that no one of them has to be
perfect:
  1. the turn's prompt says what is allowed (fence_prompt);
  2. a Claude Code PreToolUse hook (server/fence_hook.py) refuses a write
     outside the fence before it happens, and any read outside the repo;
  3. after the turn, before anything is built, every changed path outside
     the fence is put back as git has it (enforce) -- folders.txt line by
     line.
"""
import os
import re
import subprocess
import threading

from . import store

_STEM = re.compile(r"^[a-z][a-z0-9_]{0,15}$")
_lock = threading.Lock()

FOLDERS = "apps/folders.txt"


def owners_path(state_dir):
    return os.path.join(state_dir, "app_owners.json")


def load(state_dir):
    d = store.read_json(owners_path(state_dir))
    return d if isinstance(d, dict) and isinstance(d.get("files"), dict) else {"files": {}}


def _save(state_dir, d):
    store.write_json(owners_path(state_dir), d, indent=1, sort_keys=True)


def rel(root, path):
    """`path` relative to the repository, with forward slashes, or None if
    it is outside it."""
    full = os.path.realpath(os.path.join(root, path))
    base = os.path.realpath(root)
    try:
        r = os.path.relpath(full, base)
    except ValueError:                  # another drive, on Windows
        return None
    r = r.replace(os.sep, "/")
    if r == ".." or r.startswith("../") or os.path.isabs(r):
        return None
    return r


def stem_of(r):
    """NAME, for a path shaped like an app's file: apps/NAME.c, apps/NAME.h,
    apps/icons/NAME.txt."""
    m = (re.match(r"^apps/([a-z][a-z0-9_]*)\.(c|h)$", r) or
         re.match(r"^apps/icons/([a-z][a-z0-9_]*)\.txt$", r))
    return m.group(1) if m else None


def app_of(r, files=None):
    """The app an app-shaped file is part of. A header may be the app's own
    (apps/chess.h) or one of several (apps/chess_board.h): the longest
    prefix, cut at an underscore, that is an app in `files`."""
    s = stem_of(r)
    if not s or not r.endswith(".h") or files is None:
        return s
    parts = s.split("_")
    for k in range(len(parts), 0, -1):
        cand = "_".join(parts[:k])
        if "apps/%s.c" % cand in files:
            return cand
    return s


def owned(state_dir, user):
    """{path} recorded as `user`'s."""
    return {p for p, u in load(state_dir)["files"].items() if u == user}


def apps_of(state_dir, user):
    return sorted({app_of(p) for p in owned(state_dir, user) if p.endswith(".c")} - {None})


def may_write(state_dir, root, user, path, claim=False):
    """(ok, why). `claim`: a new file that is allowed is recorded as theirs."""
    r = rel(root, path)
    if r is None:
        return False, "outside the repository"
    if r == FOLDERS:
        return True, ""                  # line by line, after the turn (enforce)
    with _lock:
        files_now = load(state_dir)["files"]
    s = app_of(r, files_now)
    if not s or not _STEM.match(s):
        return False, ("%s is not one of your app's files -- you can change "
                       "apps/NAME.c, apps/NAME_*.h and apps/icons/NAME.txt for "
                       "apps you made" % r)
    with _lock:
        d = load(state_dir)
        files = d["files"]
        if files.get(r) == user:
            return True, ""
        if r in files:
            return False, "%s belongs to %s" % (r, files[r])
        # A new file, for an app that is theirs or nobody's.
        main = "apps/%s.c" % s
        app_owner = files.get(main)
        if os.path.exists(os.path.join(root, r)):
            return False, "%s is part of the system, not yours to change" % r
        if r != main:
            if app_owner != user:
                return False, "make apps/%s.c first: %s is part of that app" % (s, r)
        elif app_owner not in (None, user) or os.path.exists(os.path.join(root, main)):
            return False, "there is already an app called %s" % s
        if claim:
            files[r] = user
            _save(state_dir, d)
        return True, ""


def fence_prompt(state_dir, user):
    mine = apps_of(state_dir, user)
    return (
        "You are working for %s, who may make new apps and change only the apps "
        "they made: %s. For an app NAME that means apps/NAME.c, its own headers "
        "apps/NAME_*.h, its line 'NAME Games' (or another folder) in "
        "apps/folders.txt, and its colour icon in apps/icons/NAME.txt -- the "
        "app's name from capp_info on the first line, then 16 rows of 16 "
        "characters in the INK letters of tools/make_color_icons.py. Nothing "
        "else can be changed: not the kernel, the server, tools/, or any other "
        "app; writes there are refused. Read anything in the repository you "
        "like -- the other apps are the best examples. If asked for something "
        "outside this, say so plainly and suggest asking the owner.\n\n"
        % (user, ", ".join(mine) if mine else "none yet"))


# ---- after the turn ---------------------------------------------------------

def _git(root, *args, check=False):
    return subprocess.run(["git"] + list(args), cwd=root, capture_output=True, check=check)


def _head(root, r):
    out = _git(root, "show", "HEAD:" + r)
    return out.stdout if out.returncode == 0 else None


def _folder_lines(text):
    out = []
    for line in text.splitlines():
        parts = line.split()
        stem = parts[0] if parts and not line.lstrip().startswith("#") else None
        out.append((stem, line))
    return out


def merge_folders(old, new, mine):
    """folders.txt as it was, with only the lines for `mine` taken from new."""
    new_by = {s: l for s, l in _folder_lines(new) if s}
    seen, out = set(), []
    for s, line in _folder_lines(old):
        if s in mine:
            seen.add(s)
            if s in new_by:
                out.append(new_by[s])
            continue                    # theirs, and deleted: gone
        out.append(line)
    for s, line in _folder_lines(new):
        if s in mine and s not in seen:
            out.append(line)
            seen.add(s)
    return "\n".join(out) + "\n"


def enforce(state_dir, root, user, paths):
    """Put back every path in `paths` that `user` may not have changed.
    Returns the list put back."""
    back = []
    mine_apps = set(apps_of(state_dir, user))
    own = owned(state_dir, user)
    for r in sorted(paths):
        if r == FOLDERS:
            old = _head(root, r)
            full = os.path.join(root, r)
            try:
                with open(full, encoding="utf-8") as f:
                    new = f.read()
            except OSError:
                new = ""
            if old is None:
                continue
            merged = merge_folders(old.decode("utf-8", "replace"), new, mine_apps)
            if merged != new:
                with open(full, "w", encoding="utf-8", newline="\n") as f:
                    f.write(merged)
                back.append(r + " (other apps' lines)")
            continue
        if r in own:
            continue
        full = os.path.join(root, r)
        if _head(root, r) is not None:
            _git(root, "checkout", "HEAD", "--", r)
        elif os.path.isfile(full):
            os.remove(full)
        back.append(r)
    return back
