"""apps/jar/server/jarvm.py, loaded a second time over apps/jar/jarvm.def + lab_ext.def:
the real compiler for a language with motion primitives, without touching
either file. `X` is that module."""
import os
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def load():
    with open(os.path.join(ROOT, "apps", "jar", "jarvm.def"), encoding="utf-8") as f:
        base = f.read()
    with open(os.path.join(HERE, "lab_ext.def"), encoding="utf-8") as f:
        ext = f.read()
    fd, path = tempfile.mkstemp(suffix=".def")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(base + "\n" + ext)
    with open(os.path.join(ROOT, "server", "jar", "jarvm.py"), encoding="utf-8") as f:
        src = f.read()
    src = src.replace("_load_def()\n", "_load_def(%r)\n" % path, 1)
    src = src.replace("}[kind]\n        acts.append", "}.get(kind, word + ' X')\n        acts.append")
    m = types.ModuleType("jarvm_lab")
    m.__file__ = os.path.join(ROOT, "server", "jar", "jarvm.py")
    exec(compile(src, "jarvm_lab", "exec"), m.__dict__)
    os.unlink(path)
    return m


X = load()
