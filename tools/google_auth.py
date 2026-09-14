#!/usr/bin/env python3
"""Sign CardOS in to Google, once, from this PC.

The Cardputer has no browser and no way to show a consent screen, so the
interactive half of OAuth happens here. This runs the authorization-code flow
against a loopback redirect, ends up with a refresh token, and pushes that plus
the client credentials to the device over serial. From then on the device
refreshes its own access tokens and never needs a browser again.

Google's device-code flow would avoid this step, but it is offered only to the
"TVs and limited input devices" client type and does not cover the Tasks scope.

FIRST, once, in the Google Cloud console (I cannot do this part for you):

  1. console.cloud.google.com -> create or pick a project
  2. APIs & Services -> Library -> enable "Google Tasks API"
  3. APIs & Services -> OAuth consent screen -> External, add yourself under
     "Test users". It can stay unpublished; a test user's refresh token lasts
     seven days, so publishing it ("In production") is worth doing if you want
     it to keep working.
  4. Credentials -> Create credentials -> OAuth client ID -> **Desktop app**
  5. Download the JSON, or copy the client ID and secret

THEN:

    python tools/google_auth.py --client-json ~/Downloads/client_secret_*.json
    python tools/google_auth.py --id XXXX.apps.googleusercontent.com --secret GOCSPX-...

A browser opens; approve; the tool writes the credentials to COM3.

WHERE THE SECRETS END UP: in the device's NVS, in the clear -- flash encryption
is not enabled on this board. The refresh token is enough to read and write
that account's tasks until revoked, so if the board is lost, revoke it at
myaccount.google.com/permissions. Nothing is written to this repository.
"""

import argparse
import glob
import http.server
import json
import os
import secrets
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
# Both, space separated, because one refresh token serves every app on the
# device. Adding Calendar means re-running this and consenting again -- a
# token minted for Tasks alone is refused by the Calendar API, with a 403 that
# says "insufficient authentication scopes" and not which scope is missing.
SCOPE = " ".join([
    "https://www.googleapis.com/auth/tasks",
    "https://www.googleapis.com/auth/calendar.events",
])

_result = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.urlparse(self.path)
        if q.path != "/":
            self.send_response(404)
            self.end_headers()
            return
        args = urllib.parse.parse_qs(q.query)
        _result.update({k: v[0] for k, v in args.items()})

        ok = "code" in _result
        body = (
            "<html><body style='font-family:system-ui;padding:3rem'>"
            "<h2>%s</h2><p>%s</p></body></html>"
            % (
                "CardOS is signed in." if ok else "Sign-in failed.",
                "You can close this tab and go back to the terminal."
                if ok
                else "Google said: " + _result.get("error", "no code returned"),
            )
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass                      # the default logger writes to stderr


def read_client_json(path):
    matches = glob.glob(os.path.expanduser(path))
    if not matches:
        raise SystemExit("no file matching " + path)
    with open(matches[0], encoding="utf-8") as f:
        data = json.load(f)
    node = data.get("installed") or data.get("web")
    if not node:
        raise SystemExit("that JSON has no 'installed' or 'web' section -- "
                         "is it an OAuth client file?")
    return node["client_id"], node["client_secret"]


def authorize(client_id, client_secret, port):
    """Run the loopback flow and return a refresh token."""
    redirect = "http://127.0.0.1:%d" % port
    state = secrets.token_urlsafe(16)

    params = {
        "client_id": client_id,
        "redirect_uri": redirect,
        "response_type": "code",
        "scope": SCOPE,
        "state": state,
        # Both are needed to be handed a refresh token: offline asks for one at
        # all, and consent forces the prompt even if this account has approved
        # before -- without it a second run gets an access token and no refresh
        # token, which looks like the flow silently half-worked.
        "access_type": "offline",
        "prompt": "consent",
    }
    url = AUTH_URL + "?" + urllib.parse.urlencode(params)

    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    print("opening a browser to approve access...")
    print("  if nothing opens, paste this in yourself:\n  " + url + "\n")
    webbrowser.open(url)
    thread.join(timeout=300)
    server.server_close()

    if "code" not in _result:
        raise SystemExit("no authorization code came back: " +
                         _result.get("error", "timed out"))
    if _result.get("state") != state:
        raise SystemExit("state mismatch -- the reply did not come from the "
                         "request this tool made")

    body = urllib.parse.urlencode({
        "code": _result["code"],
        "client_id": client_id,
        "client_secret": client_secret,
        "redirect_uri": redirect,
        "grant_type": "authorization_code",
    }).encode()

    req = urllib.request.Request(TOKEN_URL, data=body)
    with urllib.request.urlopen(req, timeout=30) as r:
        token = json.load(r)

    if "refresh_token" not in token:
        raise SystemExit(
            "Google returned an access token but no refresh token. That "
            "happens when this account has already approved this client; "
            "remove it at myaccount.google.com/permissions and run again.")
    return token["refresh_token"]


def push(port, client_id, client_secret, refresh_token):
    """Send the credentials to the device over serial."""
    try:
        import serial
    except ImportError:
        raise SystemExit("pyserial is not installed: python -m pip install pyserial")

    import time
    s = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.5)
    s.reset_input_buffer()

    # Escape out of whatever shell is up, so the commands reach the console.
    s.write(b"\x1b")
    time.sleep(1.2)
    s.write(b"\x1b")
    time.sleep(1.2)
    s.read(65536)

    out = ""
    for cmd in ("google id %s" % client_id,
                "google secret %s" % client_secret,
                "google token %s" % refresh_token,
                "google"):
        s.write(cmd.encode() + b"\r")
        time.sleep(1.5)
        out += s.read(65536).decode("utf-8", "replace")
    s.close()

    # Echo only what the device said about itself, never the values back.
    for line in out.splitlines():
        if line.startswith(("google", "not configured", "configured",
                            "stored", "signed in")):
            print("  device: " + line)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--client-json", help="the OAuth client JSON from Google")
    ap.add_argument("--id", help="client ID, if not using --client-json")
    ap.add_argument("--secret", help="client secret")
    ap.add_argument("--port", default="COM3", help="serial port (default COM3)")
    ap.add_argument("--redirect-port", type=int, default=8765,
                    help="local port for the OAuth redirect")
    ap.add_argument("--no-push", action="store_true",
                    help="print the refresh token instead of sending it")
    args = ap.parse_args()

    if args.client_json:
        client_id, client_secret = read_client_json(args.client_json)
    elif args.id and args.secret:
        client_id, client_secret = args.id, args.secret
    else:
        ap.error("need --client-json, or both --id and --secret")

    refresh = authorize(client_id, client_secret, args.redirect_port)
    print("got a refresh token.")

    if args.no_push:
        print("\nrefresh token (keep it secret):\n" + refresh)
        return

    print("sending it to %s..." % args.port)
    push(args.port, client_id, client_secret, refresh)
    print("\ndone. Try `run todo` on the device.")
    print("Revoke at myaccount.google.com/permissions if the board is lost --")
    print("the credentials sit in unencrypted flash.")


if __name__ == "__main__":
    main()
