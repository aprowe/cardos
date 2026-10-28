#!/usr/bin/env python3
"""Today's NYT crossword, from wordhint.net's answers page, into the
droplet's crossword library -- run on the laptop, once a day.

    python tools/wordhint_nyt.py              fetch, convert, send; a week kept
    python tools/wordhint_nyt.py --dry-run    fetch and convert, print, send nothing
    python tools/wordhint_nyt.py --file PAGE.html [--dry-run]

Why the laptop: the page is behind Cloudflare, which serves a plain request
from a home connection and challenges the droplet's. So this fetches here
and hands the converted puzzle to `python -m server.crossword put` on the
droplet over ssh (the root login deploy_droplet.sh uses; HOST overrides).
Kept there as DATE-nyt, with any more than a week older deleted. The page
is read by server/crossword.py (`from_wordhint`), which checks the grid
against the answer tables before anything is sent.

Exit status: 0 sent (or printed), 1 the page could not be read or sent.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from server import crossword  # noqa: E402

URL = "https://www.wordhint.net/crossword-clues-answers/nyt/today/"
HOST = os.environ.get("HOST", "root@157.245.252.47")
REMOTE = ("cd /home/cardos/cardos && sudo -u cardos env CARDOS_STATE=/var/lib/cardos "
          "/opt/cardos/.venv/bin/python -m server.crossword put %s --keep 7")
KEEP = 7


def fetch():
    import requests
    r = requests.get(URL, timeout=60)
    if r.status_code != 200:
        raise RuntimeError("%s answered %d%s" % (URL, r.status_code,
                           " (a Cloudflare challenge)" if "Just a moment" in r.text else ""))
    return r.text


def main(argv):
    dry = "--dry-run" in argv
    try:
        if "--file" in argv:
            with open(argv[argv.index("--file") + 1], encoding="utf-8") as f:
                page = f.read()
        else:
            page = fetch()
        pz = crossword.from_wordhint(page)
    except Exception as e:
        print("wordhint_nyt: %s" % e, file=sys.stderr)
        return 1
    text = crossword.to_text(pz)
    pid = pz["date"] + crossword.NYT_SUFFIX
    if dry:
        print("%s, %dx%d, %d clues\n" % (pid, pz["w"], pz["h"], len(pz["across"]) + len(pz["down"])))
        print(text, end="")
        return 0
    p = subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, REMOTE % pid],
                       input=text.encode("ascii"), capture_output=True)
    sys.stdout.write(p.stdout.decode("utf-8", "replace"))
    if p.returncode:
        sys.stderr.write(p.stderr.decode("utf-8", "replace"))
        print("wordhint_nyt: sending to %s failed (%d)" % (HOST, p.returncode), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
