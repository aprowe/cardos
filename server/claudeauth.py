"""Claude's login on the server, from the dashboard.

Everything here that runs Claude -- chat, Build, voice commands, the day's
focus -- needs a login, and on the droplet that is a long-lived token
(`claude setup-token`) in CLAUDE_CODE_OAUTH_TOKEN. When it was revoked the
only fix was a terminal on the PC and an ssh to edit /etc/cardos/env.

Now the dashboard does it. "Sign in" runs `claude setup-token` here, in a
pseudo-terminal, because it is an interactive program: it prints a link,
the person signs in on claude.com, is shown a code, and pastes the code
back -- into the dashboard, which types it into the waiting program. What
the program prints then is the token. It is kept in CARDOS_STATE/claude.json
(mode 600) and wins over the one in the environment, from the next call on:
no restart. A token made elsewhere can be pasted instead.

    status()          {"source": "saved"|"env"|"none", "ok": bool|None, "why": str}
    SignIn.start()    the sign-in link
    SignIn.finish(c)  the token, saved
"""
import json
import os
import re
import select
import sys
import threading
import time

from . import dash

TOKEN_RE = re.compile(rb"sk-ant-oat[0-9]{2}-[A-Za-z0-9_\-]{20,}")
URL_RE = re.compile(rb"https://[^\s\x07\x1b\"'<>]*oauth/authorize\?[^\s\x07\x1b\"'<>]+")
ANSI_RE = re.compile(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b\[[0-9;?]*[ -/]*[@-~]")

_lock = threading.Lock()
_status_cache = {"at": 0, "value": None}


def path():
    return os.path.join(dash.state_dir(), "claude.json")


def saved_token():
    try:
        with open(path(), encoding="utf-8") as f:
            return (json.load(f).get("token") or "").strip()
    except (OSError, ValueError):
        return ""


def save_token(token, how):
    token = token.strip()
    if not TOKEN_RE.fullmatch(token.encode()):
        raise ValueError("that does not look like a Claude token (sk-ant-oat...)")
    from . import store
    store.write_json(path(), {"token": token, "since": int(time.time()), "how": how})
    _status_cache["at"] = 0


def forget():
    try:
        os.remove(path())
    except OSError:
        pass
    _status_cache["at"] = 0


def apply(env):
    """The saved token, over whatever the environment had."""
    t = saved_token()
    if t:
        env["CLAUDE_CODE_OAUTH_TOKEN"] = t
    return env


def since():
    try:
        with open(path(), encoding="utf-8") as f:
            return json.load(f).get("since")
    except (OSError, ValueError):
        return None


def clean(raw):
    """Terminal output as text: escapes gone, whitespace collapsed."""
    return " ".join(ANSI_RE.sub(b"", raw).decode("utf-8", "replace").split())


def find_url(raw):
    """The sign-in link. Read from the raw bytes, where the hyperlink escape
    carries it whole -- the drawn copy is wrapped at the terminal's width."""
    m = URL_RE.search(raw)
    return m.group(0).decode() if m else None


def find_token(raw):
    # Escapes out, line ends kept: joined up, the word after the token would
    # run on into it. The terminal is wide enough that it is not wrapped.
    m = TOKEN_RE.search(ANSI_RE.sub(b"", raw))
    return m.group(0).decode() if m else None


def status(chat, fresh=False):
    """Whether Claude answers, asked at most every ten minutes: a call costs
    a few seconds and a little of the account."""
    source = "saved" if saved_token() else (
        "env" if os.environ.get("CLAUDE_CODE_OAUTH_TOKEN") else "none")
    now = time.time()
    with _lock:
        if not fresh and _status_cache["value"] and now - _status_cache["at"] < 600:
            v = dict(_status_cache["value"])
            v["source"] = source
            return v
    from .chat import ClaudeError, ask_once
    ok, why = None, ""
    if not (chat and chat.claude):
        why = "this server runs without Claude"
    else:
        try:
            ask_once(chat, "Reply with the word OK.", 90)
            ok = True
        except ClaudeError as e:
            ok, why = False, e.why[:200]
    v = {"source": source, "ok": ok, "why": why, "since": since(), "checked": int(now)}
    with _lock:
        _status_cache.update(at=now, value=v)
    return v


class SignIn:
    """One `claude setup-token`, from link to token. One at a time; a new
    start ends the old one."""

    _current = None

    def __init__(self, cli, env):
        self.cli, self.env = cli, env
        self.pid = self.fd = None
        self.buf = b""
        self.started = time.time()

    @classmethod
    def begin(cls, cli, env):
        with _lock:
            if cls._current:
                cls._current.close()
            cls._current = s = cls(cli, env)
        s.start()
        return s

    @classmethod
    def current(cls):
        s = cls._current
        if s and time.time() - s.started > 900:     # a sign-in left open
            s.close()
            return None
        return s

    def start(self, wait=25):
        import pty
        import fcntl
        import struct
        import termios
        env = dict(self.env)
        env.pop("CLAUDE_CODE_OAUTH_TOKEN", None)    # not the old, revoked one
        env["TERM"] = "xterm-256color"
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.execvpe(self.cli, [self.cli, "setup-token"], env)
            finally:
                os._exit(127)
        # Wide, so nothing it draws is wrapped.
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 1000, 0, 0))
        url = self.read_until(find_url, wait)
        if not url:
            text = clean(self.buf)[-300:]
            self.close()
            raise RuntimeError("claude setup-token showed no link: " + (text or "nothing"))
        return url

    def read_until(self, find, wait):
        end = time.time() + wait
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.3)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    break
                if not d:
                    break
                self.buf += d
                got = find(self.buf)
                if got:
                    return got
        return find(self.buf)

    @property
    def url(self):
        return find_url(self.buf)

    def finish(self, code, wait=60):
        """Type the code; the token it prints, saved."""
        code = code.strip()
        if not code or len(code) > 400 or any(c in code for c in "\r\n"):
            raise ValueError("paste the code claude.com showed, on its own")
        mark = len(self.buf)
        os.write(self.fd, code.encode())
        time.sleep(0.3)
        os.write(self.fd, b"\r")
        token = self.read_until(lambda b: find_token(b[mark:]), wait)
        if not token:
            text = clean(self.buf[mark:])[-300:]
            raise RuntimeError(text or "no token came back")
        save_token(token, "signed in on the dashboard")
        self.close()
        return token

    def close(self):
        if self.pid:
            try:
                os.kill(self.pid, 9)
                os.waitpid(self.pid, 0)
            except OSError:
                pass
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.pid = self.fd = None
        if SignIn._current is self:
            SignIn._current = None
        sys.stderr.write("claude sign-in: closed\n")
