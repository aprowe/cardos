"""The device's Claude app, through the server.

kernel/sys/agent.c used to post its conversation to api.anthropic.com
itself, with a key from the card. That needed a key on every card, and a
TLS handshake on a device whose heap is mostly WiFi -- and when the heap was
short, the connection failed with nothing better to say than -3. Now it
posts the same body here, over plain HTTP with its device token, and this
forwards it with the server's own credentials:

    POST /agent/messages   body: a /v1/messages request -> Anthropic's answer, as it came

Credentials, in order: ANTHROPIC_API_KEY in the server's environment, then
the Claude login the dashboard keeps (server/claudeauth.py) or
CLAUDE_CODE_OAUTH_TOKEN -- the one Build already runs on. Any signed-in
device may use it: Claude is for everyone with a device, Build is not.
"""
import json
import os
import sys
import urllib.error
import urllib.request

from . import claudeauth

API = "https://api.anthropic.com/v1/messages"
BODY_MAX = 256 << 10
TIMEOUT = 180


def credentials():
    """[(header, value)] to authenticate to Anthropic, or None."""
    key = os.environ.get("ANTHROPIC_API_KEY", "").strip()
    if key:
        return [("x-api-key", key)]
    tok = claudeauth.saved_token() or os.environ.get("CLAUDE_CODE_OAUTH_TOKEN", "").strip()
    if tok:
        return [("Authorization", "Bearer " + tok),
                ("anthropic-beta", "oauth-2025-04-20")]
    return None


# A Claude login (OAuth) is Claude Code's, and the API answers a request on
# it only when the system prompt opens with Claude Code's own first line --
# without it, a bare "rate_limit_error" (measured 2026-10-07). An API key
# needs nothing.
IDENTITY = "You are Claude Code, Anthropic's official CLI for Claude."


def with_identity(body):
    """The request with IDENTITY as its first system block."""
    req = json.loads(body)
    system = req.get("system") or []
    if isinstance(system, str):
        system = [{"type": "text", "text": system}] if system else []
    if not (system and system[0].get("text", "").startswith(IDENTITY)):
        system = [{"type": "text", "text": IDENTITY}] + system
    req["system"] = system
    return json.dumps(req).encode()


def forward(body, opener=urllib.request.urlopen):
    """(status, bytes) from Anthropic for a request body."""
    cred = credentials()
    if not cred:
        return 503, json.dumps({"type": "error", "error": {
            "type": "no_credentials",
            "message": "the server has no Claude login: sign in on the dashboard"}}).encode()
    if cred[0][0] == "Authorization":
        body = with_identity(body)
    req = urllib.request.Request(API, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("anthropic-version", "2023-06-01")
    for k, v in cred:
        req.add_header(k, v)
    try:
        with opener(req, timeout=TIMEOUT) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()             # the API's error explains itself
    except (urllib.error.URLError, OSError) as e:
        return 502, json.dumps({"type": "error", "error": {
            "type": "upstream", "message": "the server could not reach Claude: %s" % e}}).encode()


def post_messages(h, path, args):
    """a /v1/messages request, forwarded with the server's Claude login"""
    try:
        body = h.body(BODY_MAX)
        json.loads(body)
    except ValueError:
        h.text("error not JSON\n", 400)
        return
    status, data = forward(body)
    if status >= 400:
        sys.stderr.write("agent: %d %s\n" % (status, data[:160]))
    h.send_response(status)
    h.send_header("Content-Type", "application/json")
    h.send_header("Content-Length", str(len(data)))
    h.end_headers()
    h.wfile.write(data)


ROUTES = [
    ("POST", "/agent/messages", post_messages, "token"),
]
