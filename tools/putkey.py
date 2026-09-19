"""Put an API key on the Cardputer's card, as /config/claude.key, over serial.

    python tools/putkey.py ANTHROPIC_API_KEY        # from that environment variable
    python tools/putkey.py --file key.txt           # from a file

An API key from console.anthropic.com. Not a Claude Code login token: this
tool used to offer that, and the API refused it (rate_limit_error, "Error")
and then revoked the login -- those tokens are for Claude Code alone.

The key is typed into the device console as `echo KEY > /config/claude.key`, one
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
    ap.add_argument("--port", default="COM3")
    a = ap.parse_args(argv)

    if a.file:
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
    dev.line("echo %s > /config/claude.key" % key, 1.0)
    dev.line("clear", 0.3)
    dev.s.reset_input_buffer()
    dev.line("ls /config", 1.5)
    listing = dev.drain()
    ok = "claude.key" in listing
    print("claude.key is on the card" if ok else "did not see claude.key in ls / -- check the console")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
