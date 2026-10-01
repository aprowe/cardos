#!/usr/bin/env python3
"""Run console lines on the device, through the droplet, while Dashboard
Link is open on it.

    python tools/devsh.py                 # is the device linked?
    python tools/devsh.py mem "log 20"    # each argument is one console line
    python tools/devsh.py "update all"    # waits; an update takes minutes

It is the dashboard's Terminal without the browser: ssh to the droplet,
sign a dashboard session there (from the server's token and DASH_PASSWORD
in /etc/cardos/env -- nothing secret leaves the droplet), and POST each
line to /dash/term, printing what the device printed. Lines that would
take over the device's screen (launch, desk, run) are refused by the
device. `update os` restarts it, which ends the link until Dashboard Link
is opened again.
"""
import json
import shlex
import subprocess
import sys

HOST = "root@157.245.252.47"

# Runs on the droplet: a session cookie for this one call, then the request.
# The env file has Windows line ends; systemd strips them, a shell does not.
REMOTE = r'''
set -a; . /etc/cardos/env; set +a
T=$(systemctl cat cardos-proxy | grep -o -- "--token [^ \"]*" | cut -d" " -f2)
T=$(eval echo $T | tr -d '\r')
P=$(printf %s "$DASH_PASSWORD" | tr -d '\r')
cd /home/cardos/cardos
C=$(sudo -u cardos /opt/cardos/.venv/bin/python -c "from server import dash; print(dash.make_cookie('$T' + chr(0) + '$P'))")
if [ -z "$BODY" ]; then
  curl -s -m 10 -H "Cookie: cardos_dash=$C" http://127.0.0.1:8081/dash/files/status
else
  curl -s -m 330 -H "Cookie: cardos_dash=$C" -H "Content-Type: application/json" \
       --data-binary "$BODY" http://127.0.0.1:8081/dash/term
fi
'''


def call(line=None):
    body = json.dumps({"line": line}) if line else ""
    # Bytes, not text: on Windows a text pipe turns every \n into \r\n.
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", HOST,
                        "BODY=%s bash -s" % shlex.quote(body)],
                       input=REMOTE.encode(), capture_output=True, timeout=360)
    out = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0 and not out:
        sys.exit("ssh failed: %s" % r.stderr.decode("utf-8", "replace").strip())
    try:
        return json.loads(out)
    except ValueError:
        sys.exit("the server said: %s" % out.strip()[:300])


def main(lines):
    st = call()
    if not st.get("connected"):
        sys.exit("not linked: open Dashboard Link on the device (launcher > Net)")
    if not lines:
        print("linked")
        return
    for line in lines:
        print("/> " + line)
        j = call(line)
        if j.get("error"):
            print("error: " + j["error"])
            if j.get("gone"):
                sys.exit(1)
            continue
        out = j.get("output", "")
        print(("(refused) " if j.get("refused") else "") + out.rstrip("\n"))


if __name__ == "__main__":
    main(sys.argv[1:])
