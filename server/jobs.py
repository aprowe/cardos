"""What the server keeps in memory between one request and the next.

Table: things a device polls for or comes back to, by id -- Build's jobs,
MIDI songs being written, conversations about a document. Each entry has
an owner, the person whose request made it (accounts.current(); None on a
server without accounts), and a time, when it was put or last touched.
Every put sweeps out what is older than the table's ttl, and the oldest
past its cap: a job whose device never came back for it is not kept for
the life of the process. A get or pop by someone who is not the owner
finds nothing, as if it were not there.

LRU: a cache of answers that can be asked again (the firmware catalog,
time zones by address), capped at so many entries, least recently used
out first -- an entry for every search anyone typed is a leak.
"""
import threading
import time
from collections import OrderedDict

MINE = object()        # owner=MINE: whoever's request this is (accounts.current())
ANY = object()         # owner=ANY: the server's own threads, which belong to no one


def _me():
    from . import accounts
    return accounts.current()


class Table:
    def __init__(self, ttl, cap=None):
        self.ttl = ttl
        self.cap = cap
        self.lock = threading.RLock()
        self._d = OrderedDict()           # key -> [at, owner, value], oldest first

    def _sweep(self, now):
        while self._d:
            k, (at, _, _) = next(iter(self._d.items()))
            if now - at <= self.ttl and (self.cap is None or len(self._d) <= self.cap):
                break
            del self._d[k]

    def _find(self, key, owner, now):
        e = self._d.get(key)
        if e is None or now - e[0] > self.ttl:
            return None
        if owner is not ANY and e[1] != (_me() if owner is MINE else owner):
            return None
        return e

    def put(self, key, value, owner=MINE):
        now = time.time()
        with self.lock:
            self._d.pop(key, None)
            self._d[key] = [now, _me() if owner is MINE else owner, value]
            self._sweep(now)

    def get(self, key, owner=MINE, touch=False):
        now = time.time()
        with self.lock:
            e = self._find(key, owner, now)
            if e is None:
                return None
            if touch:
                e[0] = now
                self._d.move_to_end(key)
            return e[2]

    def pop(self, key, owner=MINE):
        with self.lock:
            if self._find(key, owner, time.time()) is None:
                return None
            return self._d.pop(key)[2]

    def replace(self, key, value):
        """A new value for an entry still there, its owner kept and its
        clock restarted -- a job's thread, finishing. False if it is gone."""
        now = time.time()
        with self.lock:
            e = self._d.get(key)
            if e is None:
                return False
            e[0], e[2] = now, value
            self._d.move_to_end(key)
            return True

    def remove_if(self, fn):
        """Drop every entry for which fn(owner, value) is true."""
        with self.lock:
            for k in [k for k, e in self._d.items() if fn(e[1], e[2])]:
                del self._d[k]

    def __len__(self):
        with self.lock:
            return len(self._d)

    def __contains__(self, key):
        with self.lock:
            return key in self._d


class LRU:
    def __init__(self, cap):
        self.cap = cap
        self._d = OrderedDict()
        self._lock = threading.Lock()

    def get(self, key, default=None):
        with self._lock:
            if key not in self._d:
                return default
            self._d.move_to_end(key)
            return self._d[key]

    def __contains__(self, key):
        with self._lock:
            return key in self._d

    def __getitem__(self, key):
        with self._lock:
            self._d.move_to_end(key)
            return self._d[key]

    def __setitem__(self, key, value):
        with self._lock:
            self._d[key] = value
            self._d.move_to_end(key)
            while len(self._d) > self.cap:
                self._d.popitem(last=False)

    def __len__(self):
        with self._lock:
            return len(self._d)

    def clear(self):
        with self._lock:
            self._d.clear()
