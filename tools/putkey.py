"""Put an API key on the Cardputer's card, as /claude.key, over serial.

    python tools/putkey.py --claude-code            # your Claude Code login's token
    python tools/putkey.py ANTHROPIC_API_KEY        # from that environment variable
    python tools/putkey.py --file key.txt           # from a file

The Claude Code token is the OAuth access token in ~/.claude/.credentials.json
-- the one Claude Code itself uses. It expires after some hours; run this
again when the device says the API refused it.

The key is typed into the device console as `echo KEY > /claude.key`, one
character at a time, the way tools/shots.py types anything. It is never
printed here, and on the device it goes to the card and from there only to
api.anthropic.com (see kernel/sys/agent.c). The console line that carried it
is scrolled away by a `clear` afterwards.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import shots


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("var", nargs="?", help="environment variable holding the key")
    ap.add_argument("--file", help="a file holding the key instead")
    ap.add_argument("--claude-code", action="store_true",
                    help="use the Claude Code login's OAuth token")
    ap.add_argument("--port", default="COM3")
    a = ap.parse_args(argv)

    if a.claude_code:
        import json
        path = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")
        try:
            creds = json.load(open(path))
        except OSError:
            print("no Claude Code login found at %s" % path, file=sys.stderr)
            return 1
        key = (creds.get("claudeAiOauth") or {}).get("accessToken", "").strip()
    elif a.file:
        key = open(a.file).read().strip()
    elif a.var:
        key = os.environ.get(a.var, "").strip()
    else:
        ap.error("name a variable or a --file")
    if not key:
        print("no key found", file=sys.stderr)
        return 1
    if any(c in key for c in " \"'\\\n\r|<>"):
        print("that does not look like a key I can type safely", file=sys.stderr)
        return 1

    dev = shots.Device(a.port, out_dir=os.environ.get("TEMP", "."))
    shots.home(dev)
    dev.line("echo %s > /claude.key" % key, 1.0)
    dev.line("clear", 0.3)
    dev.s.reset_input_buffer()
    dev.line("ls /", 1.5)
    listing = dev.drain()
    ok = "claude.key" in listing
    print("claude.key is on the card" if ok else "did not see claude.key in ls / -- check the console")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
