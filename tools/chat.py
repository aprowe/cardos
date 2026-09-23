"""Claude, reachable from the Cardputer.

The device has a keyboard, a screen and 100 KB of heap. It cannot run a model,
and it cannot hold a conversation's worth of context either. So the machine
that already renders web pages for it runs the agent as well, and the device
talks to it over three plain-HTTP calls.

The important part is that this is not a chat with a model -- it is Claude Code
running *in this repository*, with permission to edit it. Asking the device to
"make the flippers stronger" edits apps/pinball.c on this machine. That is the
whole point of building it, and it is also the reason for the warning the
server prints on startup: anything on the network that can reach the port can
drive an agent with write access to this folder. Pass --token to require a
shared secret, which the device reads from /config/claude.token on its card.

Three calls rather than one because a reply takes anywhere from five seconds to
two minutes, and the device's shell is a single cooperative loop: a blocking
request that long is a frozen machine. So:

    POST /chat        the message; returns an id straight away
    GET  /chat?id=N   "pending", or the reply
    GET  /chat/new    forget the conversation and start again

The device polls once a second from its tick handler and stays responsive
throughout.

Conversation state is one Claude Code session, resumed by id, so the twentieth
message still knows what the first one was about -- and, because the session
runs in this directory, it reads CLAUDE.md like any other session here.
"""

import json
import os
import queue
import shutil
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What the agent is allowed to do without anyone to ask. Editing and running
# things is the point; the list is here so it is a decision rather than an
# accident, and so it can be narrowed in one place.
ALLOWED_TOOLS = "Read,Edit,Write,Glob,Grep,Bash,TodoWrite,WebFetch,WebSearch"

# A reply has to fit on a 240x135 screen with a 6x8 font -- 40 columns -- and
# in a buffer on a device with 100 KB of heap. Past this it is cut, and said
# to have been cut, rather than silently truncated somewhere less obvious.
MAX_REPLY = 3500

# One turn should not run for ten minutes with a device polling at it.
TURN_TIMEOUT = 300


def _find_claude():
    for name in ("claude", "claude.cmd", "claude.exe"):
        p = shutil.which(name)
        if p:
            return p
    return None


class ChatService:
    """Jobs, and the one session they all belong to.

    Every request runs on its own thread and the device polls for the answer,
    so the HTTP server never blocks -- but the jobs are serialised through a
    lock, because two Claude sessions editing the same repository at once is a
    merge conflict with no one to resolve it."""

    # What the agent may use. A subclass on a public host narrows this; see
    # tools/buildstep.py.
    allowed_tools = ALLOWED_TOOLS
    disallowed_tools = None

    def __init__(self, claude=None, cwd=ROOT, token=None, model=None,
                 use_api_key=False):
        self.claude = claude or _find_claude()
        self.cwd = cwd
        self.token = token
        self.model = model
        self.use_api_key = use_api_key
        self.session_id = None
        self.generation = 0        # bumped by reset(); see _claude
        self.jobs = {}
        self.next_id = 1
        self.lock = threading.Lock()
        self.run_lock = threading.Lock()

    # ---- the queue the device sees --------------------------------------

    def start(self, text):
        with self.lock:
            jid = self.next_id
            self.next_id += 1
            self.jobs[jid] = {"state": "pending", "reply": "", "at": time.time()}
        t = threading.Thread(target=self._run, args=(jid, text), daemon=True)
        t.start()
        return jid

    def poll(self, jid):
        with self.lock:
            job = self.jobs.get(jid)
            if not job:
                return "error", "no such request"
            if job["state"] == "pending":
                return "pending", ""
            # Answers are read once and dropped: the device has them now, and
            # holding every reply of every conversation is a leak with a very
            # slow fuse.
            self.jobs.pop(jid, None)
            return job["state"], job["reply"]

    def reset(self):
        with self.lock:
            self.session_id = None
            self.jobs.clear()
            # A turn already running belongs to the conversation just
            # forgotten. It used to finish a minute later and put its session
            # id back, and the next "new" conversation was resumed into it.
            self.generation += 1

    # ---- running the agent ------------------------------------------------

    def run_turn(self, text):
        """One turn, here and now: (state, reply). What a job thread runs, and
        what tools/buildagent.py calls directly, having its own queue."""
        state, reply = "done", ""
        try:
            reply = self._claude(text)
        except subprocess.TimeoutExpired:
            state, reply = "error", "timed out after %d seconds" % TURN_TIMEOUT
        except Exception as e:                       # noqa: BLE001 - reported
            state, reply = "error", "%s: %s" % (type(e).__name__, e)

        if len(reply) > MAX_REPLY:
            reply = reply[:MAX_REPLY] + "\n\n[cut: reply was %d characters]" % len(reply)
        return state, reply

    def _run(self, jid, text):
        state, reply = self.run_turn(text)
        with self.lock:
            if jid in self.jobs:
                self.jobs[jid] = {"state": state, "reply": reply, "at": time.time()}

    def _child_env(self):
        """The environment the agent runs in, minus two kinds of inheritance.

        ANTHROPIC_API_KEY, because Claude Code prefers it over the login the
        machine already has, and a stale or empty-balance key turns every reply
        into "Credit balance is too low" -- which looks like a broken server
        rather than the wrong credentials. Dropping it falls back to whoever is
        signed in, which is what someone starting this on their own laptop
        means. Pass --api-key to keep it.

        And the CLAUDE_CODE_* variables, because a session started from inside
        another one inherits its ids and sockets and can end up talking to its
        parent's machinery instead of standing on its own."""
        env = dict(os.environ)
        if not self.use_api_key:
            env.pop("ANTHROPIC_API_KEY", None)
            env.pop("ANTHROPIC_AUTH_TOKEN", None)
        # ... except the OAuth token, which on a server with no interactive login
        # is the login. It is a credential, not a session id.
        for k in list(env):
            if k == "CLAUDE_CODE_OAUTH_TOKEN":
                continue
            if k.startswith("CLAUDE_CODE_") or k in ("CLAUDECODE", "CLAUDE_PID",
                                                     "CLAUDE_PROJECT_DIR"):
                env.pop(k, None)
        return env

    def _claude(self, text):
        if not self.claude:
            return ("no claude CLI on PATH. Install Claude Code, or start the "
                    "server with --claude <path>.")

        cmd = [self.claude, "-p", text,
               "--output-format", "json",
               # Edits without asking: there is nobody at this end to ask, and
               # a request that silently does nothing is worse than one that
               # does what it was told.
               "--permission-mode", "acceptEdits",
               "--allowed-tools", self.allowed_tools,
               "--add-dir", self.cwd]
        if self.disallowed_tools:
            cmd += ["--disallowed-tools", self.disallowed_tools]
        if self.model:
            cmd += ["--model", self.model]
        if self.session_id:
            cmd += ["--resume", self.session_id]
        generation = self.generation

        # Serialised: one agent in one working tree at a time.
        with self.run_lock:
            out = subprocess.run(cmd, cwd=self.cwd, capture_output=True,
                                 text=True, encoding="utf-8", errors="replace",
                                 timeout=TURN_TIMEOUT, env=self._child_env())

        body = (out.stdout or "").strip()
        if not body:
            err = (out.stderr or "").strip()
            return err[-MAX_REPLY:] if err else "(no output)"

        # --output-format json gives one object with the answer and the id to
        # resume next time. Parsed defensively: a future version that changes
        # the shape should degrade to showing the text, not to an exception.
        try:
            obj = json.loads(body)
        except ValueError:
            return body

        if isinstance(obj, dict):
            sid = obj.get("session_id") or obj.get("sessionId")
            if sid and generation == self.generation:
                self.session_id = sid
            for key in ("result", "text", "response", "content"):
                v = obj.get(key)
                if isinstance(v, str) and v.strip():
                    return v.strip()
            if obj.get("is_error"):
                return "error: %s" % json.dumps(obj)[:MAX_REPLY]
        return body
