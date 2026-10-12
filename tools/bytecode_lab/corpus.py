"""The corpus: every Jar script this lab can find, compiled by the real
compiler (server/jar/jarvm.py, language v2).

Sources:
  local   string literals in the repo's tests, fixture maker, Jar Lab and the
          guide's example that compile as scripts (found by walking the
          Python AST, so nothing is retyped)
  claude-short / claude-rich
          corpus/*.json from gen_corpus.py: scripts written by Claude from
          the real LANGUAGE_GUIDE_V2, with and without the 12-line rule

    python tools/bytecode_lab/corpus.py      # a one-line summary per source
"""
import ast
import glob
import json
import os
import sys

os.environ.setdefault("CARDOS_JAR_V2", "1")
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, ROOT)
from server.jar import jarvm as V  # noqa: E402

LOCAL_FILES = ["server/tests/test_jarvm.py", "server/tests/test_jar.py",
               "server/tests/test_jarlab.py", "server/tests/test_jarstages.py",
               "server/jar/jarlab.py", "tools/make_jarvm_fixtures.py"]


def _strings(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            yield node.value
        elif isinstance(node, ast.BinOp):            # "a" "b" is one Constant; "a" + "b" is not
            try:
                v = ast.literal_eval(node)
                if isinstance(v, str):
                    yield v
            except Exception:                        # noqa: BLE001
                pass


def _try(src):
    if "on " not in src or "end" not in src:
        return None
    try:
        return V.compile_script(src, nbub=4, nframes=4, v2=True)
    except Exception:                                # noqa: BLE001 - not a script, or a bad one on purpose
        return None


def guide_example():
    g = V.LANGUAGE_GUIDE_V2
    return "\n".join(l[2:] for l in g.split("EXAMPLE\n", 1)[1].splitlines())


def load(include_local=True, include_claude=True, dedupe=True):
    out, seen = [], set()

    def add(name, origin, src, code):
        if dedupe and code in seen:
            return
        seen.add(code)
        out.append({"name": name, "origin": origin, "src": src, "code": code})

    if include_local:
        for p in LOCAL_FILES:
            for k, s in enumerate(_strings(p)):
                c = _try(s)
                if c and V.check(c) == 0:
                    add("%s#%d" % (os.path.basename(p), k), "local", s, c)
        s = guide_example()
        add("guide example", "local", s, V.compile_script(s, v2=True))
    if include_claude:
        for p in sorted(glob.glob(os.path.join(HERE, "corpus", "*.json"))):
            with open(p, encoding="utf-8") as f:
                for it in json.load(f):
                    c = _try(it["script"])
                    if c:
                        add(it["name"], it.get("origin", "claude"), it["script"], c)
    return out


def by_origin(corpus):
    d = {}
    for s in corpus:
        d.setdefault(s["origin"], []).append(s)
    return d


if __name__ == "__main__":
    c = load()
    for o, ss in by_origin(c).items():
        n = [len(s["code"]) for s in ss]
        print("%-13s %3d scripts, %5d bytes, mean %.1f, max %d" % (o, len(ss), sum(n), sum(n) / len(n), max(n)))
