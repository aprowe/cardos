"""The three calls the device makes, against the real handler.

The agent itself is stubbed: this is checking the contract apps/claude.c
depends on -- post returns an id immediately, polling says pending until it
does not, an answer is delivered once and then forgotten -- not whether Claude
can write C. Starting a real agent from a test would edit this repository.
"""
import os, sys, threading, time, urllib.request, urllib.error
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))             # the repository root

from server import app
from server import chat as chatmod
from http.server import ThreadingHTTPServer

FAKE_DELAY = 1.5


class StubService(chatmod.ChatService):
    def __init__(self, **kw):
        super().__init__(claude="stub", **kw)

    def _claude(self, text):
        time.sleep(FAKE_DELAY)                 # a turn takes a while
        if "boom" in text:
            raise RuntimeError("the agent fell over")
        self.session_id = "session-1"
        return "you said: " + text


def get(url, token=None):
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    return urllib.request.urlopen(req, timeout=10).read().decode()


def post(url, body, token=None):
    req = urllib.request.Request(url, data=body.encode(), method="POST")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    return urllib.request.urlopen(req, timeout=10).read().decode()


def check(label, got, want):
    ok = "ok  " if got == want else "FAIL"
    print("  %s %-46s %r" % (ok, label, got if ok == "FAIL" else want))
    return ok == "ok  "


def main():
    app.Handler.chat = StubService()
    srv = ThreadingHTTPServer(("127.0.0.1", 8137), app.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:8137"
    fails = 0

    print("the device's three calls:")
    r = post(base + "/chat", "hello there")
    fails += not check("post returns an id straight away", r.split()[0], "id")
    jid = r.split()[1]

    t0 = time.time()
    fails += not check("and it returned before the turn finished",
                       time.time() - t0 < FAKE_DELAY, True)

    r = get(base + "/chat?id=" + jid)
    fails += not check("polling while it works says pending", r.strip(), "pending")

    deadline = time.time() + 10
    while time.time() < deadline:
        r = get(base + "/chat?id=" + jid)
        if not r.startswith("pending"):
            break
        time.sleep(0.3)
    head, _, body = r.partition("\n")
    fails += not check("the outcome is the first line", head, "done")
    fails += not check("and the answer follows it", body, "you said: hello there")

    r = get(base + "/chat?id=" + jid)
    fails += not check("an answer is delivered once", r.split("\n")[0], "error")

    # a failing turn is reported, not swallowed
    jid = post(base + "/chat", "boom").split()[1]
    deadline = time.time() + 10
    while time.time() < deadline:
        r = get(base + "/chat?id=" + jid)
        if not r.startswith("pending"):
            break
        time.sleep(0.3)
    fails += not check("a crash comes back as an error", r.split("\n")[0], "error")
    fails += not check("with the reason", "the agent fell over" in r, True)

    fails += not check("new conversation is acknowledged",
                       get(base + "/chat/new").strip(), "ok")
    fails += not check("and forgets the session id",
                       app.Handler.chat.session_id, None)

    print("the shared secret:")
    app.Handler.chat.token = "swordfish"
    try:
        post(base + "/chat", "hello")
        fails += not check("a request without one is refused", "allowed", "refused")
    except urllib.error.HTTPError as e:
        fails += not check("a request without one is refused", e.code, 403)
    r = post(base + "/chat", "hello", token="swordfish")
    fails += not check("and with one it goes through", r.split()[0], "id")

    srv.shutdown()
    print("\n%d failure(s)" % fails)
    return 1 if fails else 0


sys.exit(main())
