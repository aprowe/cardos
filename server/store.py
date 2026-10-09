"""The server's JSON state files, read and written one way.

Every module kept its own: a `.tmp` beside the file, json.dump, os.replace.
Some made the file owner-only and some left it to the umask; some locked a
read-modify-write and some did not; all of them used the same fixed `.tmp`
name, so two writers at once wrote into one temporary file. This is that
once:

    read_json(path, default)   the parsed file, or `default` if it is
                               missing or unreadable
    write_json(path, obj)      a new temporary file beside it (mkstemp),
                               fsynced, mode 0600, then os.replace: a reader
                               sees the old file or the new one, never half
    update_json(path, fn, default)
                               read, fn(obj) changes it (or returns a new
                               one), write -- under a lock for that path, so
                               two requests changing one file do not undo
                               each other. fn may raise to change nothing.

Everything in CARDOS_STATE is somebody's: tokens, logins, notes. Files are
0600 and directories this makes are 0700.
"""
import json
import os
import tempfile
import threading

_locks = {}
_locks_lock = threading.Lock()


def lock_for(path):
    """The lock for one file. Re-entrant, so a fn given to update_json may
    call something that updates the same file."""
    key = os.path.normcase(os.path.abspath(path))
    with _locks_lock:
        lk = _locks.get(key)
        if lk is None:
            lk = _locks[key] = threading.RLock()
        return lk


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, obj, indent=None, sort_keys=False):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=os.path.basename(path) + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=indent, sort_keys=sort_keys)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o600)              # mkstemp's already; said, not assumed
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def update_json(path, fn, default=None, **dump):
    """The file changed by fn, written back; what was written."""
    with lock_for(path):
        obj = read_json(path, default)
        got = fn(obj)
        if got is not None:
            obj = got
        write_json(path, obj, **dump)
        return obj
