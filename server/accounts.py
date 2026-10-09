"""Who is asking: people, their devices, and where their files are.

See docs/superpowers/specs/2026-10-06-accounts-design.md.

    CARDOS_STATE/accounts.json
      {"users":   {"alex": {"pw": "pbkdf2$...", "admin": true}, ...},
       "devices": [{"id": "d1a2", "user": "alex", "label": "ADV",
                    "hash": sha256(token), "token": token}, ...]}

The token is kept as well as its hash (2026-10-06: the owner asked for one
that can be looked up again rather than shown once). accounts.json is
owner-only on disk, like google.json beside it. A device from before that
has only its hash, until it is given a new token.

A request is somebody's once its token or cookie has been checked
(app.Handler.authorised, notes._allowed, dash.logged_in): current() is
that user for the rest of it. user_dir() is where that user's files are.

Without an accounts.json the server is one person, as it always was:
current() is None and user_dir() is CARDOS_STATE itself.
"""
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import threading

from . import dash

ITERATIONS = 200000
PER_USER = ("google.json", "toggl.json", "ids.json", "daily.json", "notes", "photos", "music")
_NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,23}$")

_local = threading.local()
_lock = threading.Lock()


# ---- the request's user ---------------------------------------------------------

def current():
    return getattr(_local, "user", None)


def set_current(user):
    _local.user = user


def user_dir():
    """Where the current user's files live: made on first use."""
    u = current()
    if not u or not enabled():
        return dash.state_dir()
    d = os.path.join(dash.state_dir(), "users", u)
    os.makedirs(d, mode=0o700, exist_ok=True)
    return d


# ---- the file -------------------------------------------------------------------

def _path():
    return os.path.join(dash.state_dir(), "accounts.json")


def enabled():
    return os.path.exists(_path())


def load():
    try:
        with open(_path(), encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {}
    d.setdefault("users", {})
    d.setdefault("devices", [])
    return d


def _save(d):
    from . import store
    store.write_json(_path(), d, indent=1)


# ---- passwords ----------------------------------------------------------------------

def hash_password(pw, salt=None):
    salt = salt or secrets.token_hex(16)
    h = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt.encode(), ITERATIONS).hex()
    return "pbkdf2$%d$%s$%s" % (ITERATIONS, salt, h)


def check_password(stored, pw):
    try:
        _, n, salt, h = stored.split("$")
        got = hashlib.pbkdf2_hmac("sha256", pw.encode(), salt.encode(), int(n)).hex()
    except (ValueError, AttributeError):
        return False
    return hmac.compare_digest(got, h)


def _token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


# ---- people -------------------------------------------------------------------------

def users():
    return load()["users"]


def is_admin(user=None):
    """An admin, or the one person a server without accounts has."""
    if not enabled():
        return True
    u = users().get(user if user is not None else current())
    return bool(u and u.get("admin"))


def can_build(user=None):
    """May use Build (and its conversation): an admin, or someone given
    "build": true -- whose turns are fenced to their own apps (fence.py)."""
    if not enabled():
        return True
    name = user if user is not None else current()
    u = users().get(name)
    return bool(u and (u.get("admin") or u.get("build")))


def fenced(user=None):
    """Is this person's Build fenced to their own apps? Everyone but an admin."""
    return enabled() and not is_admin(user)


def set_build(name, on):
    """Give or take Build from someone. An admin has it anyway."""
    with _lock:
        d = load()
        if name not in d["users"]:
            raise ValueError("no such user")
        d["users"][name]["build"] = bool(on)
        _save(d)


def login(name, pw):
    """The name, if the password is theirs."""
    name = (name or "").strip().lower()
    u = users().get(name)
    if u and check_password(u.get("pw", ""), pw):
        return name
    return None


def password_hash(name):
    u = users().get(name)
    return u.get("pw") if u else None


def add_user(name, pw, admin=False):
    name = (name or "").strip().lower()
    if not _NAME.match(name):
        raise ValueError("a name is letters and digits, up to 24")
    if len(pw or "") < 6:
        raise ValueError("a password is six characters at least")
    with _lock:
        d = load()
        if name in d["users"]:
            raise ValueError("there is already a %s" % name)
        d["users"][name] = {"pw": hash_password(pw), "admin": bool(admin)}
        _save(d)
    return name


def set_password(name, pw):
    if len(pw or "") < 6:
        raise ValueError("a password is six characters at least")
    with _lock:
        d = load()
        if name not in d["users"]:
            raise ValueError("no such person")
        d["users"][name]["pw"] = hash_password(pw)
        _save(d)


def remove_user(name):
    """Them and their devices. Their files stay, in case."""
    with _lock:
        d = load()
        if name not in d["users"]:
            raise ValueError("no such person")
        if d["users"][name].get("admin") and sum(1 for u in d["users"].values() if u.get("admin")) == 1:
            raise ValueError("the last admin stays")
        del d["users"][name]
        d["devices"] = [x for x in d["devices"] if x["user"] != name]
        _save(d)


# ---- devices ------------------------------------------------------------------------

def user_for_token(token):
    """Whose device this token is, or None."""
    if not token:
        return None
    h = _token_hash(token)
    for dev in load()["devices"]:
        if hmac.compare_digest(dev["hash"], h):
            return dev["user"]
    return None


def devices(user):
    """Their devices, with each token where it is known."""
    return [{"id": x["id"], "label": x["label"], "token": x.get("token")}
            for x in load()["devices"] if x["user"] == user]


def add_device(user, label, token=None):
    """A new device for `user`: (its id, its token). The token is only ever
    shown now -- what is kept is its hash."""
    token = token or secrets.token_urlsafe(18)
    label = " ".join((label or "device").split())[:24] or "device"
    with _lock:
        d = load()
        if user not in d["users"]:
            raise ValueError("no such person")
        dev = {"id": secrets.token_hex(3), "user": user, "label": label, "hash": _token_hash(token),
               "token": token}
        d["devices"].append(dev)
        _save(d)
    return dev["id"], token


def new_token(user, dev_id):
    """A device's token replaced: the old one stops working. The new one."""
    token = secrets.token_urlsafe(18)
    with _lock:
        d = load()
        for x in d["devices"]:
            if x["id"] == dev_id and (x["user"] == user or is_admin(user)):
                x["hash"], x["token"] = _token_hash(token), token
                _save(d)
                return token
    raise ValueError("no such device")


def know_token(token):
    """Fill in the token of a device that has only its hash -- the server's
    own --token, given again at every start."""
    if not token or not enabled():
        return
    h = _token_hash(token)
    with _lock:
        d = load()
        for x in d["devices"]:
            if x["hash"] == h and not x.get("token"):
                x["token"] = token
                _save(d)


def remove_device(user, dev_id):
    with _lock:
        d = load()
        keep = [x for x in d["devices"] if not (x["id"] == dev_id and (x["user"] == user or is_admin(user)))]
        if len(keep) == len(d["devices"]):
            raise ValueError("no such device")
        d["devices"] = keep
        _save(d)


# ---- the first start ----------------------------------------------------------------

def migrate(owner, password, token):
    """From one person to accounts, once: the owner with the dashboard's
    password and the server's token as their first device, and their files
    moved under users/<owner>/. Nothing to do if accounts.json exists or
    there is no password to give the owner."""
    if enabled() or not password:
        return False
    owner = (owner or "alex").strip().lower()
    base = dash.state_dir()
    d = {"users": {owner: {"pw": hash_password(password), "admin": True}}, "devices": []}
    if token:
        d["devices"].append({"id": secrets.token_hex(3), "user": owner, "label": "first device",
                             "hash": _token_hash(token), "token": token})
    dest = os.path.join(base, "users", owner)
    os.makedirs(dest, mode=0o700, exist_ok=True)
    for name in PER_USER:
        src = os.path.join(base, name)
        if os.path.exists(src) and not os.path.exists(os.path.join(dest, name)):
            shutil.move(src, os.path.join(dest, name))
    _save(d)
    return True
