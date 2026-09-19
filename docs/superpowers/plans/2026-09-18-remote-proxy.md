# Remote Proxy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `tools/webproxy.py` into a modular proxy whose `server` profile — conversational Claude, whisper, and firmware/app updates with upload — is safe to run on a public host, and take the Anthropic API key off the device.

**Architecture:** A small core (routing, auth, config, rate limiting) plus feature modules selected by a profile. A module outside the profile is never imported, so the deployed server has no code path to a route it does not serve. The repo-editing Claude Code subprocess stays in a `dev`-only module.

**Tech Stack:** Python 3.13, stdlib `http.server`, the `anthropic` SDK, `pytest` for the new suite.

**Spec:** `docs/specs/2026-09-18-remote-proxy-design.md`

## Global Constraints

- Secrets come from the environment only, never command-line flags: `ANTHROPIC_API_KEY`, `CARDOS_DEVICE_TOKEN`, `CARDOS_ADMIN_TOKEN`, `CARDOS_PROFILE`.
- Model is `claude-opus-5`. Adaptive thinking (`thinking={"type": "adaptive"}`). Streaming for the chat call.
- Token comparison is `hmac.compare_digest`. Auth failure is `401` with `WWW-Authenticate: Bearer`.
- No secret may appear in a log line, an error body, or `/status`.
- New tests are pytest under `tools/tests/`. The existing `tools/test_chat.py`, `tools/test_update.py`, `tools/test_shots.py` stay as plain scripts; do not convert them.
- Run the new suite with `python -m pytest tools/tests -q`.
- Before writing the chat module, read `python/claude-api/README.md` from the `claude-api` skill. Do not guess SDK call shapes.
- The C host suite (`.\build.bat`) must stay green; these tasks do not touch C until Task 9.

---

### Task 1: Config from the environment, with startup refusals

**Files:**
- Create: `proxy/__init__.py` (empty)
- Create: `proxy/config.py`
- Test: `tools/tests/test_config.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `Settings` dataclass with fields `profile: str`, `device_token: str`, `admin_token: str | None`, `api_key: str | None`, `modules: tuple[str, ...]`; `load_settings(env: dict) -> Settings`; `ConfigError(Exception)`.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_config.py
import pytest
from proxy.config import load_settings, ConfigError

GOOD = "t" * 32

def test_server_profile_requires_a_device_token():
    with pytest.raises(ConfigError) as e:
        load_settings({"CARDOS_PROFILE": "server", "ANTHROPIC_API_KEY": "k"})
    assert "CARDOS_DEVICE_TOKEN" in str(e.value)

def test_a_short_device_token_is_refused():
    with pytest.raises(ConfigError):
        load_settings({"CARDOS_PROFILE": "server",
                       "CARDOS_DEVICE_TOKEN": "short",
                       "ANTHROPIC_API_KEY": "k"})

def test_server_profile_requires_an_api_key_because_chat_is_in_it():
    with pytest.raises(ConfigError) as e:
        load_settings({"CARDOS_PROFILE": "server", "CARDOS_DEVICE_TOKEN": GOOD})
    assert "ANTHROPIC_API_KEY" in str(e.value)

def test_server_profile_loads_chat_voice_and_update_only():
    s = load_settings({"CARDOS_PROFILE": "server",
                       "CARDOS_DEVICE_TOKEN": GOOD,
                       "ANTHROPIC_API_KEY": "k"})
    assert s.modules == ("chat", "voice", "update")

def test_dev_profile_loads_everything_and_needs_no_token():
    s = load_settings({"CARDOS_PROFILE": "dev"})
    assert "repo" in s.modules and "shot" in s.modules

def test_an_unknown_profile_is_refused():
    with pytest.raises(ConfigError):
        load_settings({"CARDOS_PROFILE": "banana"})
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_config.py -q`
Expected: FAIL, `ModuleNotFoundError: No module named 'proxy'`

- [ ] **Step 3: Write the implementation**

```python
# proxy/config.py
"""Settings, and the refusal to start without them.

Secrets come from the environment and never from a flag: a flag is visible in
`ps` to every user on the box and in shell history. A missing secret is a
refusal to start rather than a warning, because the failure mode of a warning
is an unauthenticated proxy on a public address that nobody noticed.
"""
from dataclasses import dataclass

TOKEN_MIN = 32

PROFILES = {
    "server": ("chat", "voice", "update"),
    "dev": ("chat", "voice", "update", "shot", "screen", "repo"),
}


class ConfigError(Exception):
    """Startup refused. The message says which variable, never its value."""


@dataclass(frozen=True)
class Settings:
    profile: str
    device_token: str
    admin_token: str | None
    api_key: str | None
    modules: tuple[str, ...]


def load_settings(env):
    profile = env.get("CARDOS_PROFILE", "dev")
    if profile not in PROFILES:
        raise ConfigError("CARDOS_PROFILE must be one of: %s"
                          % ", ".join(sorted(PROFILES)))

    modules = PROFILES[profile]
    device_token = env.get("CARDOS_DEVICE_TOKEN", "")
    admin_token = env.get("CARDOS_ADMIN_TOKEN") or None
    api_key = env.get("ANTHROPIC_API_KEY") or None

    if profile == "server":
        if len(device_token) < TOKEN_MIN:
            raise ConfigError(
                "CARDOS_DEVICE_TOKEN must be set and at least %d characters "
                "in the server profile" % TOKEN_MIN)
        if "chat" in modules and not api_key:
            raise ConfigError("ANTHROPIC_API_KEY must be set when chat is enabled")

    return Settings(profile, device_token, admin_token, api_key, modules)
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests/test_config.py -q`
Expected: 6 passed

- [ ] **Step 5: Commit**

```bash
git add proxy/__init__.py proxy/config.py tools/tests/test_config.py
git commit -m "proxy: settings from the environment, and refusing to start without them"
```

---

### Task 2: The core — module registry, routing, auth

**Files:**
- Create: `proxy/core.py`
- Test: `tools/tests/test_core.py`

**Interfaces:**
- Consumes: `proxy.config.Settings`, `load_settings`.
- Produces: `build_app(settings, registry=None) -> App`; `App.routes: dict[(method, path)] -> handler`; `App.authorise(headers, need_admin=False) -> bool`; `RemoteUnsafe(Exception)`. A module is a module object exposing `ROUTES: list[(method, path, handler)]` and `REMOTE_SAFE: bool`.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_core.py
import types, pytest
from proxy.config import load_settings
from proxy.core import build_app, RemoteUnsafe

GOOD = "d" * 32
ADMIN = "a" * 32

def mod(name, safe, routes=()):
    m = types.ModuleType(name)
    m.REMOTE_SAFE = safe
    m.ROUTES = list(routes)
    return m

def server_settings(**over):
    env = {"CARDOS_PROFILE": "server", "CARDOS_DEVICE_TOKEN": GOOD,
           "ANTHROPIC_API_KEY": "k"}
    env.update(over)
    return load_settings(env)

def test_a_remote_unsafe_module_cannot_load_into_the_server_profile():
    reg = {"chat": mod("chat", True), "voice": mod("voice", True),
           "update": mod("update", True), "repo": mod("repo", False)}
    s = server_settings()
    object.__setattr__(s, "modules", ("chat", "repo"))
    with pytest.raises(RemoteUnsafe):
        build_app(s, reg)

def test_only_the_profiles_routes_exist():
    reg = {"chat": mod("chat", True, [("POST", "/chat", lambda r: None)]),
           "voice": mod("voice", True, [("POST", "/voice", lambda r: None)]),
           "update": mod("update", True, [("GET", "/update", lambda r: None)]),
           "repo": mod("repo", False, [("POST", "/render", lambda r: None)])}
    app = build_app(server_settings(), reg)
    assert ("POST", "/chat") in app.routes
    assert ("POST", "/render") not in app.routes

def test_the_right_token_is_accepted_and_the_wrong_one_is_not():
    app = build_app(server_settings(), {})
    assert app.authorise({"Authorization": "Bearer " + GOOD})
    assert not app.authorise({"Authorization": "Bearer " + "x" * 32})
    assert not app.authorise({})

def test_the_device_token_is_not_enough_to_upload():
    app = build_app(server_settings(CARDOS_ADMIN_TOKEN=ADMIN), {})
    assert not app.authorise({"Authorization": "Bearer " + GOOD}, need_admin=True)
    assert app.authorise({"Authorization": "Bearer " + ADMIN}, need_admin=True)

def test_with_no_admin_token_nothing_can_upload():
    app = build_app(server_settings(), {})
    assert not app.authorise({"Authorization": "Bearer " + GOOD}, need_admin=True)
    assert not app.authorise({"Authorization": "Bearer " + ADMIN}, need_admin=True)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_core.py -q`
Expected: FAIL, `ImportError: cannot import name 'build_app'`

- [ ] **Step 3: Write the implementation**

```python
# proxy/core.py
"""Routing, auth, and the rule that keeps the server profile honest.

The enforcement that matters is negative: a module outside the profile is
never imported, so the deployed server has no code path to a route it does not
serve. A runtime flag would be a thing to forget to set.
"""
import hmac


class RemoteUnsafe(Exception):
    """A module that must not be exposed was named in the server profile."""


class App:
    def __init__(self, settings):
        self.settings = settings
        self.routes = {}

    def authorise(self, headers, need_admin=False):
        """Constant-time, and `need_admin` is a different secret rather than a
        flag on the same one: whoever can upload can run code on every device,
        which is not the privilege a device needs to read its own mail."""
        want = self.settings.admin_token if need_admin else self.settings.device_token
        if not want:
            return False
        given = headers.get("Authorization", "")
        if given.startswith("Bearer "):
            given = given[7:]
        else:
            given = headers.get("X-Token", "")
        if not given:
            return False
        return hmac.compare_digest(want, given)


def build_app(settings, registry=None):
    if registry is None:
        registry = _import_modules(settings.modules)

    app = App(settings)
    for name in settings.modules:
        module = registry.get(name)
        if module is None:
            continue
        if settings.profile == "server" and not getattr(module, "REMOTE_SAFE", False):
            raise RemoteUnsafe(
                "%s is not safe to expose and cannot load in the server profile"
                % name)
        for method, path, handler in getattr(module, "ROUTES", ()):
            app.routes[(method, path)] = handler
    return app


def _import_modules(names):
    import importlib
    out = {}
    for name in names:
        out[name] = importlib.import_module("proxy.modules." + name)
    return out
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests/test_core.py -q`
Expected: 5 passed

- [ ] **Step 5: Commit**

```bash
git add proxy/core.py tools/tests/test_core.py
git commit -m "proxy: a core that refuses to expose what must not be exposed"
```

---

### Task 3: The HTTP server, and 401 on every route

**Files:**
- Create: `proxy/server.py`
- Modify: `proxy/core.py` (add `Request` dataclass)
- Test: `tools/tests/test_http.py`

**Interfaces:**
- Consumes: `build_app`, `App`.
- Produces: `serve(app, host="127.0.0.1", port=8080)`; `make_handler(app) -> class`; `Request` with fields `method`, `path`, `query: dict`, `headers`, `body: bytes`; a handler returns `(status: int, content_type: str, body: bytes)`.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_http.py
import threading, urllib.request, urllib.error, types
from http.server import ThreadingHTTPServer
from proxy.config import load_settings
from proxy.core import build_app
from proxy.server import make_handler

GOOD = "d" * 32

def mod(name, routes):
    m = types.ModuleType(name)
    m.REMOTE_SAFE = True
    m.ROUTES = routes
    return m

def start():
    reg = {"chat": mod("chat", [("GET", "/ping", lambda r: (200, "text/plain", b"pong"))]),
           "voice": mod("voice", []), "update": mod("update", [])}
    s = load_settings({"CARDOS_PROFILE": "server", "CARDOS_DEVICE_TOKEN": GOOD,
                       "ANTHROPIC_API_KEY": "k"})
    app = build_app(s, reg)
    srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(app))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d" % srv.server_address[1]

def fetch(url, token=None):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()

def test_no_token_is_401_not_403():
    srv, base = start()
    try:
        code, _ = fetch(base + "/ping")
        assert code == 401
    finally:
        srv.shutdown()

def test_the_right_token_gets_through():
    srv, base = start()
    try:
        assert fetch(base + "/ping", GOOD) == (200, b"pong")
    finally:
        srv.shutdown()

def test_an_unknown_route_is_404_even_with_a_good_token():
    srv, base = start()
    try:
        code, _ = fetch(base + "/nope", GOOD)
        assert code == 404
    finally:
        srv.shutdown()

def test_no_response_body_ever_contains_the_token():
    srv, base = start()
    try:
        for url in ("/ping", "/nope"):
            for tok in (None, GOOD, "wrong" * 8):
                _, body = fetch(base + url, tok)
                assert GOOD.encode() not in body
    finally:
        srv.shutdown()
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_http.py -q`
Expected: FAIL, `ModuleNotFoundError: No module named 'proxy.server'`

- [ ] **Step 3: Write the implementation**

```python
# proxy/server.py
"""The HTTP surface. Every route is behind the token, including /status:
the old /status named the repo path and whether a key was present, which is
free reconnaissance for anyone who finds the port."""
import urllib.parse
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler

MAX_BODY = 64 << 10


@dataclass
class Request:
    method: str
    path: str
    query: dict
    headers: object
    body: bytes


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            """Method and path only. The default logs the request line, which
            would put a token in the log for anyone who sent one as a query
            parameter."""
            pass

        def _run(self, method):
            q = urllib.parse.urlparse(self.path)
            handler = app.routes.get((method, q.path))
            if handler is None:
                self._send(404, "text/plain", b"no such route\n")
                return
            need_admin = getattr(handler, "need_admin", False)
            if not app.authorise(self.headers, need_admin=need_admin):
                self._send(401, "text/plain", b"unauthorised\n",
                           extra={"WWW-Authenticate": "Bearer"})
                return
            try:
                n = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                self._send(400, "text/plain", b"bad Content-Length\n")
                return
            limit = getattr(handler, "max_body", MAX_BODY)
            if n < 0 or n > limit:
                self._send(413, "text/plain", b"body too large\n")
                return
            body = self.rfile.read(n) if n else b""
            req = Request(method, q.path, urllib.parse.parse_qs(q.query),
                          self.headers, body)
            status, ctype, out = handler(req)
            self._send(status, ctype, out)

        def _send(self, status, ctype, body, extra=None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self._run("GET")

        def do_POST(self):
            self._run("POST")

        def do_PUT(self):
            self._run("PUT")

    return Handler


def serve(app, host="127.0.0.1", port=8080):
    """Loopback by default. TLS belongs to a reverse proxy in front of this,
    not to certificate renewal code living in here."""
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer((host, port), make_handler(app))
    srv.serve_forever()
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests/test_http.py -q`
Expected: 4 passed

- [ ] **Step 5: Commit**

```bash
git add proxy/server.py tools/tests/test_http.py
git commit -m "proxy: the HTTP surface, every route behind the token"
```

---

### Task 4: The voice module, moved

**Files:**
- Create: `proxy/modules/__init__.py` (empty)
- Create: `proxy/modules/voice.py`
- Modify: `tools/voice.py` (import target only; leave the transcription code where it is)
- Test: `tools/tests/test_voice_module.py`

**Interfaces:**
- Consumes: `Request` from `proxy.server`.
- Produces: module-level `ROUTES = [("POST", "/voice", handle_voice)]`, `REMOTE_SAFE = True`; `handle_voice(req) -> (status, ctype, body)`; `set_transcriber(fn)` where `fn(wav_bytes) -> str`, for tests.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_voice_module.py
from proxy.server import Request
from proxy.modules import voice

def req(body):
    return Request("POST", "/voice", {}, {}, body)

def test_a_wav_becomes_text():
    voice.set_transcriber(lambda b: "hello there")
    status, ctype, body = voice.handle_voice(req(b"RIFF....WAVE"))
    assert status == 200
    assert body == b"hello there"

def test_an_empty_body_is_refused_without_calling_whisper():
    called = []
    voice.set_transcriber(lambda b: called.append(b) or "x")
    status, _, _ = voice.handle_voice(req(b""))
    assert status == 400
    assert not called

def test_the_module_is_remote_safe_and_owns_one_route():
    assert voice.REMOTE_SAFE is True
    assert voice.ROUTES == [("POST", "/voice", voice.handle_voice)]
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_voice_module.py -q`
Expected: FAIL, `ModuleNotFoundError: No module named 'proxy.modules'`

- [ ] **Step 3: Write the implementation**

```python
# proxy/modules/voice.py
"""Audio in, text out. Remote-safe: it touches a temporary file and whisper,
and nothing else on the host."""
REMOTE_SAFE = True

_transcribe = None


def set_transcriber(fn):
    """For tests, and for wiring the real whisper in at startup without this
    module importing it at import time -- the server profile should not pay
    for a model load to answer /status."""
    global _transcribe
    _transcribe = fn


def _default_transcriber(wav_bytes):
    import sys, os
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
    import voice as voicetool
    return voicetool.transcribe_bytes(wav_bytes)


def handle_voice(req):
    if not req.body:
        return 400, "text/plain", b"no audio\n"
    fn = _transcribe or _default_transcriber
    text = fn(req.body)
    return 200, "text/plain", text.encode("utf-8")


handle_voice.max_body = 4 << 20        # a few seconds of 16 kHz mono

ROUTES = [("POST", "/voice", handle_voice)]
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests/test_voice_module.py -q`
Expected: 3 passed

If `tools/voice.py` has no `transcribe_bytes`, add one that wraps its existing file-based entry point by writing to a temporary file — do not restructure the transcription code.

- [ ] **Step 5: Commit**

```bash
git add proxy/modules/__init__.py proxy/modules/voice.py tools/voice.py tools/tests/test_voice_module.py
git commit -m "proxy: voice as a module"
```

---

### Task 5: The chat module — conversational Claude

**Files:**
- Create: `proxy/modules/chat.py`
- Test: `tools/tests/test_chat_module.py`

**Interfaces:**
- Consumes: `Request`.
- Produces: `ROUTES` for `POST /chat`, `GET /chat`, `GET /chat/new`; `REMOTE_SAFE = True`; `Conversation` with `turns: list[dict]`, `add(role, text)`, `trimmed() -> list[dict]`; `set_client(fn)` where `fn(messages) -> str`.

**Read first:** `python/claude-api/README.md` from the `claude-api` skill, for the exact `client.messages.stream(...)` shape. Do not guess it.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_chat_module.py
import time
from proxy.server import Request
from proxy.modules import chat

def post(text):
    return Request("POST", "/chat", {}, {}, text.encode())

def get(cid):
    return Request("GET", "/chat", {"id": [str(cid)]}, {}, b"")

def settle(cid, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        status, _, body = chat.handle_poll(get(cid))
        if body != b"pending\n":
            return status, body
        time.sleep(0.05)
    raise AssertionError("never answered")

def test_post_returns_an_id_and_the_answer_arrives_on_a_later_poll():
    chat.set_client(lambda messages: "you said: " + messages[-1]["content"])
    chat.reset_all()
    status, _, body = chat.handle_post(post("hello"))
    assert status == 200 and body.startswith(b"id ")
    cid = int(body.split()[1])
    status, answer = settle(cid)
    assert answer == b"you said: hello"

def test_an_answer_is_delivered_once_and_then_forgotten():
    chat.set_client(lambda m: "once")
    chat.reset_all()
    cid = int(chat.handle_post(post("x"))[2].split()[1])
    settle(cid)
    status, _, body = chat.handle_poll(get(cid))
    assert body != b"once"

def test_the_conversation_keeps_context_across_turns():
    seen = []
    chat.set_client(lambda m: seen.append(len(m)) or "ok")
    chat.reset_all()
    cid = int(chat.handle_post(post("first"))[2].split()[1])
    settle(cid)
    chat.handle_post(post("second"))
    time.sleep(0.5)
    assert seen[-1] > seen[0]        # history grew

def test_a_conversation_is_capped_and_drops_the_oldest_turns():
    c = chat.Conversation(max_turns=4)
    for i in range(10):
        c.add("user", "m%d" % i)
    assert len(c.trimmed()) <= 4
    assert c.trimmed()[-1]["content"] == "m9"
    assert all(t["content"] != "m0" for t in c.trimmed())

def test_chat_new_clears_it():
    chat.set_client(lambda m: "ok")
    chat.reset_all()
    cid = int(chat.handle_post(post("x"))[2].split()[1])
    settle(cid)
    status, _, _ = chat.handle_new(Request("GET", "/chat/new", {}, {}, b""))
    assert status == 200
    assert chat.conversation_count() == 0

def test_the_module_is_remote_safe():
    assert chat.REMOTE_SAFE is True
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_chat_module.py -q`
Expected: FAIL, `ModuleNotFoundError: No module named 'proxy.modules.chat'`

- [ ] **Step 3: Write the implementation**

```python
# proxy/modules/chat.py
"""Conversational Claude. No tools, no filesystem, no repo.

Three calls, not one, because the device's shell is a single cooperative loop
and a two-minute request is a frozen machine: POST returns an id immediately,
GET says pending until it does not, GET /chat/new starts over. That reasoning
belongs to the device and is unchanged by the model living on a server now.
"""
import threading

REMOTE_SAFE = True

MODEL = "claude-opus-5"
MAX_TURNS = 40
SYSTEM = ("You are talking to someone through a handheld device with a "
          "240x135 pixel screen and a tiny keyboard. Keep answers short and "
          "plain. No markdown, no lists unless asked.")

_client = None
_lock = threading.Lock()
_convs = {}
_next_id = [0]


class Conversation:
    def __init__(self, max_turns=MAX_TURNS):
        self.turns = []
        self.max_turns = max_turns
        self.answer = None
        self.pending = False

    def add(self, role, text):
        self.turns.append({"role": role, "content": text})

    def trimmed(self):
        """Oldest first out. A cap in turns rather than tokens because the
        device's own conversation is already bounded and this only has to stop
        a long session growing without limit."""
        return self.turns[-self.max_turns:]


def set_client(fn):
    """`fn(messages) -> str`. Tests pass a stub; startup passes the SDK call."""
    global _client
    _client = fn


def reset_all():
    with _lock:
        _convs.clear()


def conversation_count():
    with _lock:
        return len(_convs)


def _ask(cid, text):
    conv = _convs[cid]
    conv.add("user", text)
    try:
        reply = _client(conv.trimmed())
    except Exception as e:
        reply = "could not reach Claude: %s" % type(e).__name__
    with _lock:
        conv.add("assistant", reply)
        conv.answer = reply
        conv.pending = False


def handle_post(req):
    text = req.body.decode("utf-8", "replace").strip()
    if not text:
        return 400, "text/plain", b"empty\n"
    with _lock:
        cid = _next_id[0] = _next_id[0] + 1
        conv = _convs.setdefault(cid, Conversation())
        conv.pending = True
    threading.Thread(target=_ask, args=(cid, text), daemon=True).start()
    return 200, "text/plain", ("id %d\n" % cid).encode()


def handle_poll(req):
    try:
        cid = int(req.query.get("id", ["0"])[0])
    except ValueError:
        return 400, "text/plain", b"bad id\n"
    with _lock:
        conv = _convs.get(cid)
        if conv is None:
            return 404, "text/plain", b"no such conversation\n"
        if conv.pending:
            return 200, "text/plain", b"pending\n"
        answer, conv.answer = conv.answer, None
    if answer is None:
        return 200, "text/plain", b"pending\n"
    return 200, "text/plain", answer.encode("utf-8")


def handle_new(req):
    reset_all()
    return 200, "text/plain", b"ok\n"


def make_sdk_client(api_key):
    """The real one. Streaming because a long answer on a non-streaming call
    can outlive the HTTP timeout; get_final_message collapses it back to text."""
    import anthropic
    client = anthropic.Anthropic(api_key=api_key)

    def ask(messages):
        with client.messages.stream(
            model=MODEL,
            max_tokens=2048,
            system=SYSTEM,
            thinking={"type": "adaptive"},
            messages=messages,
        ) as stream:
            msg = stream.get_final_message()
        if getattr(msg, "stop_reason", None) == "refusal":
            return "I can't answer that one."
        return "".join(b.text for b in msg.content if b.type == "text")

    return ask


ROUTES = [("POST", "/chat", handle_post),
          ("GET", "/chat", handle_poll),
          ("GET", "/chat/new", handle_new)]
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests/test_chat_module.py -q`
Expected: 6 passed

- [ ] **Step 5: Commit**

```bash
git add proxy/modules/chat.py tools/tests/test_chat_module.py
git commit -m "proxy: conversational Claude, no tools and no repo"
```

---

### Task 6: The update module — manifest, download, upload

**Files:**
- Create: `proxy/modules/update.py`
- Test: `tools/tests/test_update_module.py`

**Interfaces:**
- Consumes: `Request`.
- Produces: `ROUTES` for `GET /update`, `GET /update/firmware`, `GET /update/app/<name>`, `PUT /update/firmware`, `PUT /update/app/<name>`; `REMOTE_SAFE = True`; `set_store(path)`; handlers for the PUT routes carry `need_admin = True`.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_update_module.py
import json, hashlib
from proxy.server import Request
from proxy.modules import update

def put(path, body):
    return Request("PUT", path, {}, {}, body)

def get(path):
    return Request("GET", path, {}, {}, b"")

def test_uploading_then_downloading_round_trips(tmp_path):
    update.set_store(str(tmp_path))
    status, _, _ = update.handle_put_app(put("/update/app/todo.capp", b"BINARY"))
    assert status == 200
    status, _, body = update.handle_get_app(get("/update/app/todo.capp"))
    assert status == 200 and body == b"BINARY"

def test_the_manifest_lists_what_was_uploaded_with_its_hash(tmp_path):
    update.set_store(str(tmp_path))
    update.handle_put_app(put("/update/app/todo.capp", b"BINARY"))
    status, _, body = update.handle_manifest(get("/update"))
    m = json.loads(body)
    entry = [a for a in m["apps"] if a["name"] == "todo.capp"][0]
    assert entry["sha256"] == hashlib.sha256(b"BINARY").hexdigest()

def test_uploads_need_the_admin_token():
    assert update.handle_put_app.need_admin is True
    assert update.handle_put_firmware.need_admin is True
    assert getattr(update.handle_get_app, "need_admin", False) is False

def test_a_name_that_is_a_path_is_refused(tmp_path):
    update.set_store(str(tmp_path))
    status, _, _ = update.handle_put_app(put("/update/app/../../etc/passwd", b"x"))
    assert status == 400

def test_a_failed_upload_leaves_the_previous_artifact_intact(tmp_path):
    update.set_store(str(tmp_path))
    update.handle_put_app(put("/update/app/todo.capp", b"GOOD"))
    update.handle_put_app(put("/update/app/todo.capp", b""))     # empty: refused
    _, _, body = update.handle_get_app(get("/update/app/todo.capp"))
    assert body == b"GOOD"

def test_a_missing_artifact_is_404(tmp_path):
    update.set_store(str(tmp_path))
    status, _, _ = update.handle_get_app(get("/update/app/nope.capp"))
    assert status == 404
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_update_module.py -q`
Expected: FAIL, `ModuleNotFoundError`

- [ ] **Step 3: Write the implementation**

```python
# proxy/modules/update.py
"""Artifacts the device downloads, and the push that puts them there.

An upload is written to a temporary file and moved into place only once it is
complete, so a device fetching mid-upload cannot get half an image. The device
checks what it downloaded against the manifest hash before it replaces
anything (kernel/net/update.c), so a truncated download cannot reach a boot
slot -- this end only has to avoid publishing one.
"""
import hashlib
import json
import os

REMOTE_SAFE = True

_store = None


def set_store(path):
    global _store
    _store = path
    os.makedirs(os.path.join(path, "apps"), exist_ok=True)


def _safe_name(path, prefix):
    name = path[len(prefix):]
    if not name or "/" in name or "\\" in name or name.startswith("."):
        return None
    return name


def _write_atomically(dest, body):
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(body)
    os.replace(tmp, dest)


def handle_put_app(req):
    name = _safe_name(req.path, "/update/app/")
    if not name or not req.body:
        return 400, "text/plain", b"bad upload\n"
    _write_atomically(os.path.join(_store, "apps", name), req.body)
    return 200, "text/plain", b"stored\n"


handle_put_app.need_admin = True
handle_put_app.max_body = 8 << 20


def handle_put_firmware(req):
    if not req.body:
        return 400, "text/plain", b"bad upload\n"
    _write_atomically(os.path.join(_store, "firmware.bin"), req.body)
    return 200, "text/plain", b"stored\n"


handle_put_firmware.need_admin = True
handle_put_firmware.max_body = 8 << 20


def handle_get_app(req):
    name = _safe_name(req.path, "/update/app/")
    if not name:
        return 400, "text/plain", b"bad name\n"
    path = os.path.join(_store, "apps", name)
    if not os.path.exists(path):
        return 404, "text/plain", b"no such app\n"
    with open(path, "rb") as f:
        return 200, "application/octet-stream", f.read()


def handle_get_firmware(req):
    path = os.path.join(_store, "firmware.bin")
    if not os.path.exists(path):
        return 404, "text/plain", b"no firmware\n"
    with open(path, "rb") as f:
        return 200, "application/octet-stream", f.read()


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def handle_manifest(req):
    apps = []
    appdir = os.path.join(_store, "apps")
    for name in sorted(os.listdir(appdir)):
        if name.endswith(".part"):
            continue
        apps.append({"name": name, "sha256": _sha(os.path.join(appdir, name))})
    fw = os.path.join(_store, "firmware.bin")
    out = {"apps": apps,
           "firmware": _sha(fw) if os.path.exists(fw) else None}
    return 200, "application/json", json.dumps(out).encode()


ROUTES = [("GET", "/update", handle_manifest),
          ("GET", "/update/firmware", handle_get_firmware),
          ("PUT", "/update/firmware", handle_put_firmware)]
```

Route matching for `/update/app/<name>` is a prefix, which the exact-match table in Task 3 cannot express. Add prefix support to `proxy/server.py`: after the exact lookup misses, walk `app.routes` for a key whose path ends in `/` and which the request path starts with. Register the app routes as `("GET", "/update/app/")` and `("PUT", "/update/app/")`, and add a test in `tools/tests/test_http.py` that a prefix route matches `/update/app/todo.capp` and that `/update/appfoo` does not match it.

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests -q`
Expected: all pass

- [ ] **Step 5: Commit**

```bash
git add proxy/modules/update.py proxy/server.py tools/tests/test_update_module.py tools/tests/test_http.py
git commit -m "proxy: an update store with an admin-only push"
```

---

### Task 7: Rate limiting

**Files:**
- Modify: `proxy/core.py`
- Modify: `proxy/server.py`
- Test: `tools/tests/test_rate_limit.py`

**Interfaces:**
- Consumes: `App`.
- Produces: `App.allow(now: float) -> bool`; `App.rate_limit` defaults to 60 requests per 60 seconds.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_rate_limit.py
from proxy.config import load_settings
from proxy.core import build_app

GOOD = "d" * 32

def app():
    return build_app(load_settings({"CARDOS_PROFILE": "server",
                                    "CARDOS_DEVICE_TOKEN": GOOD,
                                    "ANTHROPIC_API_KEY": "k"}), {})

def test_a_burst_is_allowed_then_refused():
    a = app()
    a.rate_limit = (3, 60.0)
    assert all(a.allow(1000.0) for _ in range(3))
    assert not a.allow(1000.0)

def test_the_window_rolls_forward():
    a = app()
    a.rate_limit = (3, 60.0)
    for _ in range(3):
        a.allow(1000.0)
    assert not a.allow(1000.0)
    assert a.allow(1061.0)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_rate_limit.py -q`
Expected: FAIL, `AttributeError: 'App' object has no attribute 'allow'`

- [ ] **Step 3: Write the implementation**

In `proxy/core.py`, add to `App.__init__`: `self.rate_limit = (60, 60.0)` and `self._hits = []`. Then:

```python
    def allow(self, now):
        """A device that polls is a device that can loop, and the API key is
        the asset behind this. A count per rolling window, in the core rather
        than per module, so a new route cannot forget it."""
        limit, window = self.rate_limit
        self._hits = [t for t in self._hits if now - t < window]
        if len(self._hits) >= limit:
            return False
        self._hits.append(now)
        return True
```

In `proxy/server.py`, inside `_run` after the auth check passes:

```python
            import time
            if not app.allow(time.time()):
                self._send(429, "text/plain", b"slow down\n",
                           extra={"Retry-After": "10"})
                return
```

- [ ] **Step 4: Run it and watch it pass**

Run: `python -m pytest tools/tests -q`
Expected: all pass

- [ ] **Step 5: Commit**

```bash
git add proxy/core.py proxy/server.py tools/tests/test_rate_limit.py
git commit -m "proxy: a rate limit in the core, where a new route cannot forget it"
```

---

### Task 8: The entry point, the dev modules, and deleting Chrome

**Files:**
- Create: `proxy/__main__.py`
- Create: `proxy/modules/repo.py`, `proxy/modules/shot.py`, `proxy/modules/screen.py`
- Delete: the Chrome rendering code in `tools/webproxy.py` (`find_chrome`, `shoot*`, `downscale`, `to_cpx`, `encode_row`, `rgb565_swapped`, `trim`, `_gamma`, and the `/render` route) and the Build half
- Modify: `tools/webproxy.py` becomes a thin shim that runs `proxy` in the `dev` profile, so existing habits keep working
- Test: `tools/tests/test_profiles.py`

**Interfaces:**
- Consumes: everything above.
- Produces: `python -m proxy` reads the environment and serves.

- [ ] **Step 1: Write the failing test**

```python
# tools/tests/test_profiles.py
import pytest
from proxy.config import load_settings
from proxy.core import build_app, RemoteUnsafe

GOOD = "d" * 32

def test_the_server_profile_has_no_repo_shot_or_screen_routes():
    app = build_app(load_settings({"CARDOS_PROFILE": "server",
                                   "CARDOS_DEVICE_TOKEN": GOOD,
                                   "ANTHROPIC_API_KEY": "k"}))
    paths = {p for _, p in app.routes}
    assert not any(p.startswith("/render") for p in paths)
    assert "/shot" not in paths and "/screen" not in paths

def test_the_repo_module_declares_itself_unsafe():
    from proxy.modules import repo
    assert repo.REMOTE_SAFE is False

def test_forcing_repo_into_the_server_profile_is_refused():
    s = load_settings({"CARDOS_PROFILE": "server", "CARDOS_DEVICE_TOKEN": GOOD,
                       "ANTHROPIC_API_KEY": "k"})
    object.__setattr__(s, "modules", ("chat", "repo"))
    with pytest.raises(RemoteUnsafe):
        build_app(s)

def test_there_is_no_chrome_left_in_the_tree():
    import pathlib
    src = pathlib.Path("tools/webproxy.py").read_text(encoding="utf-8")
    assert "chrome" not in src.lower()
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python -m pytest tools/tests/test_profiles.py -q`
Expected: FAIL on the missing modules and on Chrome still being present

- [ ] **Step 3: Write the implementation**

`proxy/modules/repo.py` moves `tools/chat.py`'s `ChatService` behind `ROUTES` for `POST /repo/chat` and `GET /repo/chat`, with `REMOTE_SAFE = False` and a module docstring saying why: it runs Claude Code with `--permission-mode acceptEdits` in this repo, so anything that reaches it can edit the source.

`proxy/modules/shot.py` and `proxy/modules/screen.py` move the `/shot` and `/screen` handlers from `tools/webproxy.py` unchanged, both `REMOTE_SAFE = False`.

```python
# proxy/__main__.py
"""python -m proxy. Everything it needs is in the environment; there are no
flags, because a flag carrying a secret is a secret in `ps`."""
import os
import sys

from proxy.config import load_settings, ConfigError
from proxy.core import build_app, RemoteUnsafe
from proxy.server import serve


def main():
    try:
        settings = load_settings(os.environ)
    except ConfigError as e:
        sys.stderr.write("refusing to start: %s\n" % e)
        return 2

    if "chat" in settings.modules:
        from proxy.modules import chat
        chat.set_client(chat.make_sdk_client(settings.api_key))
    if "update" in settings.modules:
        from proxy.modules import update
        update.set_store(os.environ.get("CARDOS_STORE", "store"))

    try:
        app = build_app(settings)
    except RemoteUnsafe as e:
        sys.stderr.write("refusing to start: %s\n" % e)
        return 2

    host = "127.0.0.1" if settings.profile == "server" else "0.0.0.0"
    port = int(os.environ.get("CARDOS_PORT", "8080"))
    sys.stderr.write("profile %s on %s:%d, modules: %s\n"
                     % (settings.profile, host, port, ", ".join(settings.modules)))
    serve(app, host, port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run everything**

Run: `python -m pytest tools/tests -q` then `python tools/test_chat.py`
Expected: pytest all pass; the old script still passes against the dev profile

- [ ] **Step 5: Commit**

```bash
git add proxy tools/webproxy.py tools/tests/test_profiles.py
git commit -m "proxy: profiles, an entry point, and the end of the Chrome renderer"
```

---

### Task 9: Take the API key off the device

**Files:**
- Modify: `kernel/sys/agent.c:28` (the URL) and its `read_key` / `s_auth` construction
- Modify: `kernel/app/capp.h` (`CAPP_PROXY_DEFAULT` comment only if the port changes)
- Delete: `tools/putkey.py`
- Modify: `CLAUDE.md` (the agent paragraph, and the putkey line)

**Interfaces:**
- Consumes: the `chat` module's `POST /chat`.
- Produces: nothing other code depends on.

**This task needs the device on USB.** Do not start it without hardware.

- [ ] **Step 1: Point the agent at the proxy**

In `kernel/sys/agent.c`, replace the hardcoded `https://api.anthropic.com/v1/messages` with the proxy base from `env_get("PROXY")` (falling back to `CAPP_PROXY_DEFAULT`) plus `/v1/messages`, and replace the `x-api-key` / OAuth header construction in `read_key()` with the device token read from `/claude.token` as a bearer. Keep the existing "no key" error path, changing its message to name the token file.

- [ ] **Step 2: Build and flash**

```bash
python tools/build_apps.py
python -m platformio run -t upload --upload-port COM3
```

- [ ] **Step 3: Verify on hardware**

Run the proxy in the dev profile, open the Claude app on the device, send a message, and confirm a reply. Then:

```bash
python tools/shots.py keys "log 20\r" --wait 2
```

Expected: the log shows the request going to the proxy, and no line contains a key.

- [ ] **Step 4: Remove the key from the card**

On the device console: `rm /claude.key`. Confirm the agent still answers.

- [ ] **Step 5: Commit**

```bash
git rm tools/putkey.py
git add kernel/sys/agent.c CLAUDE.md
git commit -m "The device holds a proxy token, not an Anthropic key"
```

---

### Task 10: Deploy notes

**Files:**
- Create: `docs/deploy-proxy.md`

- [ ] **Step 1: Write the notes**

Cover: the environment file and its four variables; generating a token with
`python -c "import secrets; print(secrets.token_urlsafe(32))"`; a Caddy or nginx
block terminating TLS and proxying to `127.0.0.1:8080`; a systemd unit with
`EnvironmentFile=`; the release push (`curl -X PUT -H "Authorization: Bearer $CARDOS_ADMIN_TOKEN" --data-binary @firmware.bin https://host/update/firmware`); and how to point a device at it (`set PROXY=https://host`, and writing the device token to `/claude.token`).

- [ ] **Step 2: Commit**

```bash
git add docs/deploy-proxy.md
git commit -m "docs: deploying the proxy"
```

---

## Self-Review Notes

Spec coverage checked section by section. Every spec section maps to a task:
module layout (2, 8), secrets and identity (1, 2), the device losing its API key
(9), chat on the server (5), update with upload (6), transport and abuse (3, 7,
10), what gets tested (every task), order of work (task order follows the spec's
seven steps, with deploy notes split out as 10).

Two deviations from the spec, both deliberate: `/repo/chat` rather than reusing
`/chat` for the repo-editing agent, so the two can never be confused by a
misconfigured profile; and prefix routing added in Task 6 rather than Task 3,
because it is not needed until the app artifact routes exist.
