"""Make a corpus of Jar scripts the way the server's behaviour stage does:
the real LANGUAGE_GUIDE_V2, the real "at most 12 lines" rule, a batch of
varied items -- asked of Claude through the claude CLI, one call per batch.

    python apps/jar/tools/bytecode_lab/gen_corpus.py OUT.json [--n 40] [--rich]

--rich drops the 12-line rule and asks for the most elaborate behaviour the
language allows (to see what fills 256 bytes). The scripts are compiled by
apps/jar/server/jarvm.py; only the ones that compile are kept. Nothing on the server
or the droplet is touched.
"""
import argparse
import json
import os
import re
import subprocess
import sys

os.environ.setdefault("CARDOS_JAR_V2", "1")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, ROOT)
from apps.jar.server import jarvm  # noqa: E402

KINDS = ["critter (walks about)", "decor on the floor", "a hanging thing", "a floating thing",
         "furniture", "a plant"]


def prompt(n, rich, seed):
    rule = ("Each script: at most 12 lines, short and charming, as the game asks." if not rich else
            "Each script: as elaborate and lifelike as the language allows -- several handlers, "
            "memory used as state (modes, counters, timers), reactions to traits and signals -- "
            "while staying under 256 bytes compiled (about 25-35 lines).")
    return "\n".join([
        "You write behaviour scripts for things in a cosy game set inside a glass jar.",
        "Invent %d different things (vary them: %s) -- creatures, gadgets, plants, ornaments, "
        "furniture -- each with 1-4 speech bubbles, 1-4 frames, a few traits. Batch seed %d." % (
            n, ", ".join(KINDS), seed),
        rule,
        "React to traits, never to names. Bubble and frame numbers count from 1.",
        "",
        "Answer ONLY with a JSON array, no prose, no code fence: "
        '[{"name": ..., "kind": ..., "bubbles": N, "frames": N, "script": "..."}]',
        "",
        "The script language:",
        jarvm.LANGUAGE_GUIDE_V2,
    ])


def ask(text, model):
    cmd = ["claude", "-p", "--output-format", "text"]
    if model:
        cmd += ["--model", model]
    r = subprocess.run(cmd, input=text, capture_output=True, text=True, encoding="utf-8",
                       timeout=900, shell=(os.name == "nt"))
    out = r.stdout.strip()
    m = re.search(r"\[.*\]", out, re.S)
    if not m:
        raise SystemExit("no JSON in the answer:\n" + out[:2000] + r.stderr[:2000])
    return json.loads(m.group(0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--n", type=int, default=40)
    ap.add_argument("--rich", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--model", default=None)
    a = ap.parse_args()
    items = ask(prompt(a.n, a.rich, a.seed), a.model)
    kept, bad = [], 0
    for it in items:
        try:
            code = jarvm.compile_script(it["script"], nbub=it.get("bubbles") or 4,
                                        nframes=it.get("frames") or 4, v2=True)
        except jarvm.ScriptError as e:
            bad += 1
            print("refused %s: %s" % (it.get("name"), e))
            continue
        it["bytes"] = len(code)
        it["origin"] = "claude-rich" if a.rich else "claude-short"
        kept.append(it)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(kept, f, indent=1)
    print("kept %d, refused %d -> %s" % (len(kept), bad, a.out))


if __name__ == "__main__":
    main()
