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
import re
import sys
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

# A turn is stopped when it goes quiet, not when it gets long. The 300-second
# wall clock this replaced killed a new-app request that had read the right
# files and spent 2 min 20 s thinking before writing: indistinguishable, from
# outside, from a hang. Silence past IDLE_TIMEOUT is a hang; STEP_TIMEOUT is
# the backstop for one step that is busy and getting nowhere. Thinking emits
# nothing, so IDLE_TIMEOUT has to cover the longest think (140 s measured).
IDLE_TIMEOUT = 240
STEP_TIMEOUT = 900
PLAN_TIMEOUT = 180
TURN_TIMEOUT = STEP_TIMEOUT        # old name; the test stubs still read it

# A request becomes at most this many steps, each its own turn.
MAX_STEPS = 6

PLAN_PROMPT = """\
Split the request below into steps for a coding agent working in this
repository (CardOS; CLAUDE.md describes it). Each step must be small enough to
finish in a few minutes: one file written, one feature added, one fix. A small
request, or a question, is one step -- do not pad it. At most %d steps.

Plan exactly what was asked, the simplest version of it: no features the
request did not mention. No step to build, compile, test or install -- that
happens by itself after the last step, and the agent cannot run commands.

Reply with the numbered steps only, one line each, and nothing else.

Request: %s"""

STEP_PROMPT = """\
%s

That request has been split into steps:
%s

Do step %d now, and only step %d. Later steps come as their own messages.
End with one short sentence saying what you did."""


class TurnTimeout(Exception):
    """A turn stopped for taking too long; the message says which way."""


def describe(event):
    """What a stream-json event says the agent is doing, in a few words for a
    40-column status bar, or None if it is not worth saying."""
    if event.get("type") != "assistant":
        return None
    for block in (event.get("message") or {}).get("content") or []:
        if block.get("type") != "tool_use":
            continue
        name = block.get("name", "")
        inp = block.get("input") or {}
        path = os.path.basename(str(inp.get("file_path") or inp.get("path") or ""))
        if name == "Read":
            return "reading " + path if path else "reading"
        if name == "Edit" or name == "MultiEdit":
            return "editing " + path if path else "editing"
        if name == "Write":
            return "writing " + path if path else "writing"
        if name in ("Grep", "Glob"):
            return "searching"
        return name.lower()
    return None


# One log line fits two rows of the device's 40-column window.
LOG_LINE_MAX = 76

# Log bytes per poll: Build's reply buffer is 4000, less the status lines.
POLL_LOG_BYTES = 3000


def _one_line(s, n=LOG_LINE_MAX):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[:n - 3] + "..."


def log_lines(event, root=None):
    """The lines a stream-json event adds to the log Build shows while it
    waits: one per tool call, with what it touched, and the first line of
    anything the agent says. Paths are made relative to `root`, because the
    repository's absolute path is 20 of the 40 columns there are."""
    if event.get("type") != "assistant":
        return []

    def rel(p):
        p = str(p or "")
        if root and p.startswith(root):
            p = p[len(root):].lstrip("/\\")
        return p.replace("\\", "/")

    out = []
    for block in (event.get("message") or {}).get("content") or []:
        kind = block.get("type")
        if kind == "text":
            first = (block.get("text") or "").strip().split("\n")[0]
            if first:
                out.append(_one_line("> " + first))
        elif kind == "tool_use":
            name = (block.get("name") or "").lower()
            inp = block.get("input") or {}
            path = rel(inp.get("file_path") or inp.get("path") or "")
            if name in ("grep", "glob"):
                what = "%s '%s'" % (name, " ".join(str(inp.get("pattern", "")).split()))
                out.append(_one_line(what + (" in " + path if path else "")))
            else:
                out.append(_one_line(name + (" " + path if path else "")))
    return out


def run_stream(cmd, env, cwd, on_status, idle, cap, on_log=None):
    """Run `claude ... --output-format stream-json` and return (result text,
    session id). Reads events as they come, reports what the agent is doing,
    and kills it on `idle` seconds of silence or `cap` seconds in all -- the
    reason this streams at all: a single JSON blob at the end says nothing
    until it is too late to say anything."""
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            encoding="utf-8", errors="replace")
    lines = queue.Queue()
    err = []

    def pump_out():
        for line in proc.stdout:
            lines.put(line)
        lines.put(None)                                 # end of stream

    def pump_err():                  # drained so a chatty stderr cannot block
        for line in proc.stderr:
            err.append(line)

    threading.Thread(target=pump_out, daemon=True).start()
    threading.Thread(target=pump_err, daemon=True).start()

    started = last = time.time()
    text, sid, final = None, None, None
    try:
        while True:
            now = time.time()
            why = ("took longer than %d s" % cap if now - started > cap else
                   "went quiet for %d s" % idle if now - last > idle else None)
            if why:
                e = TurnTimeout(why)
                e.session_id = sid                      # see ChatService._claude
                raise e
            try:
                line = lines.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                break                                   # the agent finished
            last = time.time()
            try:
                event = json.loads(line)
            except ValueError:
                continue
            sid = event.get("session_id") or sid
            if event.get("type") == "result":
                final = event
                continue
            what = describe(event)
            if what and on_status:
                on_status(what)
            if on_log:
                for line in log_lines(event, cwd):
                    on_log(line)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

    if final is not None:
        text = final.get("result")
        if not (isinstance(text, str) and text.strip()) and final.get("is_error"):
            text = "error: %s" % json.dumps(final)[:MAX_REPLY]
    if not (isinstance(text, str) and text.strip()):
        tail = "".join(err).strip()
        text = tail[-MAX_REPLY:] if tail else "(no output)"
    return text.strip(), sid


def parse_plan(text, request):
    """The numbered lines of a planner's reply, at most MAX_STEPS of them. A
    reply with no list is one step: the request itself."""
    steps = []
    for line in (text or "").splitlines():
        m = re.match(r"\s*(\d+)[.)]\s+(.+)", line)
        if m:
            steps.append(m.group(2).strip())
    return steps[:MAX_STEPS] or [request]


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
    # server/build.py.
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
            self.jobs[jid] = {"state": "pending", "reply": "", "status": "",
                              "log": [], "at": time.time()}
        t = threading.Thread(target=self._run, args=(jid, text), daemon=True)
        t.start()
        return jid

    def poll(self, jid):
        with self.lock:
            job = self.jobs.get(jid)
            if not job:
                return "error", "no such request"
            if job["state"] == "pending":
                return "pending", job.get("status", "")
            # Answers are read once and dropped: the device has them now, and
            # holding every reply of every conversation is a leak with a very
            # slow fuse.
            self.jobs.pop(jid, None)
            return job["state"], job["reply"]

    def progress(self, jid, since=0):
        """(status, log lines from `since` on) for a job still pending."""
        with self.lock:
            job = self.jobs.get(jid) or {}
            return job.get("status", ""), list(job.get("log", [])[max(0, since):])

    def reset(self):
        with self.lock:
            self.session_id = None
            self.jobs.clear()
            # A turn already running belongs to the conversation just
            # forgotten. It used to finish a minute later and put its session
            # id back, and the next "new" conversation was resumed into it.
            self.generation += 1

    # ---- running the agent ------------------------------------------------

    def run_turn(self, text, report=None, log=None):
        """A request, here and now: (state, reply). What a job thread runs,
        and what BuildingChat wraps in a build.

        Planned first, then one turn per step in the same session, so each
        step has its own timeout and the device can be told which one is
        running. `report(line)` hears the status as it changes."""
        report = report or (lambda line: None)
        log = log or (lambda line: None)
        generation = self.generation
        report("planning")
        try:
            steps = self._plan(text)
        except Exception as e:                       # noqa: BLE001 - reported
            sys.stderr.write("chat: planning failed (%s); one step\n" % e)
            steps = [text]
        n = len(steps)
        if n > 1:
            # The whole plan, up front: seeing it only in the final answer
            # was seeing it after it no longer mattered.
            log("plan: %d steps" % n)
            for i, s in enumerate(steps, 1):
                log(_one_line("%d. %s" % (i, s)))

        state, results, failed = "done", [], None
        for k, step in enumerate(steps, 1):
            if self.generation != generation:
                break                    # /chat/new: this job is not wanted now
            prefix = "step %d/%d" % (k, n) if n > 1 else ""
            report(prefix or "working")
            if n > 1:
                log(_one_line("-- step %d/%d: %s" % (k, n, step)))
            prompt = text if n == 1 else STEP_PROMPT % (
                text, "\n".join("%d. %s" % (i, s) for i, s in enumerate(steps, 1)), k, k)

            def on_status(what, prefix=prefix):
                report("%s: %s" % (prefix, what) if prefix else what)
            try:
                results.append(self._claude(prompt, on_status=on_status, on_log=log))
            except Exception as e:                   # noqa: BLE001 - reported
                state, failed = "error", (k, str(e) if isinstance(e, TurnTimeout)
                                          else "%s: %s" % (type(e).__name__, e))
                break

        if n == 1:
            reply = results[0] if results else "stopped: %s" % failed[1]
        else:
            ticks = "\n".join("[%s] %d. %s" % ("x" if i <= len(results) else " ", i, s)
                              for i, s in enumerate(steps, 1))
            last = results[-1] if results else ""
            reply = ticks + ("\n\n" + last if last else "")
            if failed:
                reply += "\n\nstep %d stopped: %s" % failed
        if len(reply) > MAX_REPLY:
            reply = reply[:MAX_REPLY] + "\n\n[cut: reply was %d characters]" % len(reply)
        return state, reply

    def _run(self, jid, text):
        def report(line):
            with self.lock:
                if jid in self.jobs and self.jobs[jid]["state"] == "pending":
                    self.jobs[jid]["status"] = line[:60]
        def log(line):
            with self.lock:
                if jid in self.jobs and self.jobs[jid]["state"] == "pending":
                    self.jobs[jid]["log"].append(line)
        state, reply = self.run_turn(text, report=report, log=log)
        with self.lock:
            if jid in self.jobs:
                self.jobs[jid] = {"state": state, "reply": reply, "status": "",
                                  "at": time.time()}

    def _plan(self, text):
        """The request as numbered steps: one planning call, no tools, no
        session -- it must not become part of the conversation it plans."""
        if not self.claude:
            return [text]
        cmd = [self.claude, "-p", PLAN_PROMPT % (MAX_STEPS, text),
               "--output-format", "stream-json", "--verbose",
               "--max-turns", "1", "--disallowed-tools", ALLOWED_TOOLS]
        if self.model:
            cmd += ["--model", self.model]
        reply, _ = run_stream(cmd, self._child_env(), self.cwd, None,
                              idle=PLAN_TIMEOUT, cap=PLAN_TIMEOUT)
        steps = parse_plan(reply, text)
        sys.stderr.write("chat: plan of %d: %s\n" % (len(steps), "; ".join(steps)[:200]))
        return steps

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

    def _claude(self, text, on_status=None, on_log=None):
        """One turn of the agent in the conversation's session: its answer.
        Raises TurnTimeout if it goes quiet or runs past the step cap."""
        if not self.claude:
            return ("no claude CLI on PATH. Install Claude Code, or start the "
                    "server with --claude <path>.")

        cmd = [self.claude, "-p", text,
               # Streamed, so the device can be told what is happening and a
               # silent agent can be told from a busy one.
               "--output-format", "stream-json", "--verbose",
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

        def keep(sid):
            # A turn already running belongs to the conversation it started
            # in; after /chat/new its session id must not come back.
            if sid and generation == self.generation:
                self.session_id = sid

        # Serialised: one agent in one working tree at a time.
        with self.run_lock:
            try:
                text, sid = run_stream(cmd, self._child_env(), self.cwd, on_status,
                                       idle=IDLE_TIMEOUT, cap=STEP_TIMEOUT,
                                       on_log=on_log)
            except TurnTimeout as e:
                # Kept even so: "finish it" should resume the session that
                # got part of the way, not start from nothing.
                keep(getattr(e, "session_id", None))
                raise
        keep(sid)
        return text


# ---- routes: the device's three calls --------------------------------------
#
# Three short requests rather than one long one, because the device's shell is
# a single cooperative loop and a two-minute request is a frozen machine.

def post_chat(h, path, args):
    """ask Claude; returns an id"""
    text = h.body(64 << 10).decode("utf-8", "replace").strip()
    if not text:
        h.text("empty\n", 400)
        return
    jid = h.chat.start(text)
    sys.stderr.write("chat #%d: %s\n" % (jid, text[:70]))
    h.text("id %d\n" % jid)


def get_chat(h, path, args):
    """the answer to ?id=N, once it is ready"""
    jid = h.int_arg(args, "id", 0)
    state, reply = h.chat.poll(jid)
    if state == "pending":
        # The status on the second line: "step 2/3: writing timer.c". A Build
        # that predates it tests only the first two letters, and is unaffected.
        # With ?from=K, the log lines from K on follow, one per line: the
        # device counts what it got and asks from there next time. Capped so a
        # poll fits Build's 4 KB reply; the rest comes on the next one.
        out = "pending\n" + reply
        if "from" in args:
            _, lines = h.chat.progress(jid, h.int_arg(args, "from", 0))
            budget = POLL_LOG_BYTES
            for line in lines:
                if len(line) + 1 > budget:
                    break
                out += "\n" + line
                budget -= len(line) + 1
        h.text(out)
        return
    # The state on its own line, so the device can tell an answer from a
    # failure without parsing anything.
    h.text(state + "\n" + reply)
    sys.stderr.write("chat #%d: %s, %d chars\n" % (jid, state, len(reply)))


def get_chat_new(h, path, args):
    """forget the conversation"""
    h.chat.reset()
    sys.stderr.write("chat: new conversation\n")
    h.text("ok\n")


ROUTES = [
    ("POST", "/chat", post_chat),
    ("GET", "/chat", get_chat),
    ("GET", "/chat/new", get_chat_new),
]
