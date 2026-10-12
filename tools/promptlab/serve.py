"""The prompt lab: the staged item pipeline and the Jarscript 3 sandbox, on
one local page.

    python tools/promptlab/serve.py            http://127.0.0.1:8765/
    python tools/promptlab/serve.py --dry      every stage answered by a stub
    python tools/promptlab/serve.py --port 9000 --model claude-sonnet-5 --v2

Local only: it listens on 127.0.0.1, answers only requests that name that
host (a page elsewhere cannot reach it through a DNS trick), and takes POSTs
only as application/json (a page elsewhere cannot send that without asking
first, and it is never answered). It spends Claude calls on the local
`claude` CLI, exactly as the server makes them (server/chat.py ask_once, the
stages' schemas checked as server/ask.py checks them, limit=None).

Importing lab points CARDOS_STATE at a fresh temporary directory before any
server module is loaded (see lab.py): the real state, the pool and the
signing key are never touched.

The API (JSON in, JSON out; long calls are jobs: {"job": id}, then poll
GET /api/job?id=; any POST with "wait": true answers when it is done):
    GET  /api/defaults                  stage templates + schemas, dice, config
    POST /api/prompts  {inputs, v2}     each stage's default template for these inputs
    POST /api/roll     {seed, k, bribe, kind, v2}           -> one find's dice
    POST /api/run_stage {stage, inputs, template, schema, effort, model}
    POST /api/run_find {inputs, overrides, variant, ask_tibbs, model, efforts}
    POST /api/batch    {n, inputs, overrides, variant, ask_tibbs, parallel, seed}
    POST /api/tibbs    {inputs, template, resume}
    POST /api/check    {inputs, variant}                     -> combine + check + record
    POST /api/js3check {script, frames}
    GET  /api/job?id=
    GET  /api/history  [?id=]
    GET  /api/experiments ; GET /api/experiment?name= ; POST /api/experiment ;
    POST /api/experiment/delete {name}
The page is / ; the sandbox is /jarsandbox/ (tools/jarsandbox).
"""
import argparse
import json
import os
import sys
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lab  # noqa: E402 - sets CARDOS_STATE to a scratch directory before the server modules

BODY_MAX = 2 * 1024 * 1024
STATIC = {
    "/": (os.path.join(HERE, "index.html"), "text/html; charset=utf-8"),
    "/index.html": (os.path.join(HERE, "index.html"), "text/html; charset=utf-8"),
    "/jarsandbox/": (os.path.join(lab.SANDBOX, "index.html"), "text/html; charset=utf-8"),
    "/jarsandbox/index.html": (os.path.join(lab.SANDBOX, "index.html"), "text/html; charset=utf-8"),
    "/jarsandbox/jarscript3.js": (os.path.join(lab.SANDBOX, "jarscript3.js"),
                                  "text/javascript; charset=utf-8"),
    "/jarsandbox/jarscript3.md": (os.path.join(lab.SANDBOX, "jarscript3.md"),
                                  "text/markdown; charset=utf-8"),
}


def _job_view(job, full=True):
    if job is None:
        return None
    out = {k: job.get(k) for k in ("id", "kind", "state", "progress", "error", "secs", "label")}
    out["partial"] = list(job.get("partial") or [])
    if full:
        out["result"] = job.get("result")
        if job.get("trace"):
            out["trace"] = job["trace"]
    return out


JOB_KINDS = {"run_stage": ("stage", lab.run_stage), "run_find": ("find", lab.run_find),
             "batch": ("batch", lab.run_batch), "tibbs": ("tibbs", lab.tibbs)}


class Handler(BaseHTTPRequestHandler):
    server_version = "promptlab/1"
    allowed_hosts = ()

    def log_message(self, fmt, *args):                  # quiet: the jobs say what matters
        if os.environ.get("PROMPTLAB_VERBOSE"):
            sys.stderr.write("%s\n" % (fmt % args))

    # ---- replies
    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj, code=200):
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json; charset=utf-8")

    def fail(self, code, why):
        self.json({"error": why}, code)

    def _host_ok(self):
        host = (self.headers.get("Host") or "").lower()
        return host in self.allowed_hosts

    def _body(self):
        if "application/json" not in (self.headers.get("Content-Type") or ""):
            raise ValueError("send application/json")
        n = int(self.headers.get("Content-Length") or 0)
        if n > BODY_MAX:
            raise ValueError("body too large")
        raw = self.rfile.read(n) if n else b"{}"
        obj = json.loads(raw.decode("utf-8") or "{}")
        if not isinstance(obj, dict):
            raise ValueError("send a JSON object")
        return obj

    # ---- GET
    def do_GET(self):
        if not self._host_ok():
            return self.fail(403, "this lab answers only http://127.0.0.1 (or localhost)")
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path in STATIC:
                path, ctype = STATIC[u.path]
                with open(path, "rb") as f:
                    return self._send(200, f.read(), ctype)
            if u.path == "/api/defaults":
                return self.json(lab.defaults(None, _bool(q.get("v2"))))
            if u.path == "/api/job":
                job = lab.JOBS.get(q.get("id", ""))
                return self.json(_job_view(job)) if job else self.fail(404, "no such job")
            if u.path == "/api/history":
                if q.get("id"):
                    job = lab.JOBS.get(q["id"])
                    return self.json(_job_view(job)) if job else self.fail(404, "no such run")
                return self.json({"history": list(reversed(lab.JOBS.history))})
            if u.path == "/api/experiments":
                return self.json({"experiments": lab.list_experiments(), "dir": lab.EXP_DIR})
            if u.path == "/api/experiment":
                return self.json(lab.load_experiment(q.get("name", "")))
            if u.path == "/favicon.ico":
                return self._send(204, b"", "image/x-icon")
            return self.fail(404, "nothing at %s" % u.path)
        except FileNotFoundError:
            return self.fail(404, "not found")
        except Exception as e:                          # noqa: BLE001
            return self.fail(500, "%s: %s" % (type(e).__name__, e))

    # ---- POST
    def do_POST(self):
        if not self._host_ok():
            return self.fail(403, "this lab answers only http://127.0.0.1 (or localhost)")
        u = urlparse(self.path)
        try:
            body = self._body()
        except ValueError as e:
            return self.fail(400, str(e))
        name = u.path[len("/api/"):] if u.path.startswith("/api/") else ""
        try:
            if name in JOB_KINDS:
                kind, fn = JOB_KINDS[name]
                if body.get("wait"):
                    return self.json(_job_view(lab.JOBS.run_now(kind, fn, body)))
                return self.json({"job": lab.JOBS.start(kind, fn, body)})
            if name == "prompts":
                return self.json(lab.defaults(body.get("inputs"), _bool(body.get("v2"))))
            if name == "roll":
                return self.json(lab.roll(body.get("seed"), body.get("k", 0), body.get("bribe", 0),
                                          _bool(body.get("v2")), body.get("kind") or None))
            if name == "check":
                inp = body.get("inputs") or {}
                variant = body.get("variant") or "jarvm"
                chk = lab.check(inp, variant, _bool(body.get("v2")))
                out = {"check": {k: chk[k] for k in ("parts", "why", "js3", "ok")}, "raw": chk["raw"]}
                if chk["ok"]:
                    out.update(lab.finish(chk["raw"], inp, variant, inp.get("behaviour") or {},
                                          _bool(body.get("v2"))))
                else:
                    r = lab._norm_inputs(inp)["rolls"]
                    out["sandbox"] = lab.sandbox_def(chk["raw"], r, variant, inp.get("behaviour") or {})
                return self.json(out)
            if name == "js3check":
                return self.json(lab.js3_check(body.get("script", ""), body.get("frames") or 1))
            if name == "experiment":
                return self.json(lab.save_experiment(body))
            if name == "experiment/delete":
                return self.json({"deleted": lab.delete_experiment(body.get("name"))})
            return self.fail(404, "no API %s" % u.path)
        except (ValueError, KeyError, TypeError) as e:
            return self.fail(400, "%s: %s" % (type(e).__name__, e))
        except Exception as e:                          # noqa: BLE001
            traceback.print_exc()
            return self.fail(500, "%s: %s" % (type(e).__name__, e))


def _bool(v):
    if v is None or v == "":
        return None
    if isinstance(v, str):
        return v.lower() in ("1", "true", "yes", "on")
    return bool(v)


def make_server(port=8765, host="127.0.0.1"):
    """A server bound to 127.0.0.1 only (port 0: any free one)."""
    if host not in ("127.0.0.1", "localhost"):
        raise ValueError("the prompt lab listens on 127.0.0.1 only")
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    real = srv.server_address[1]

    class H(Handler):
        allowed_hosts = ("127.0.0.1:%d" % real, "localhost:%d" % real)
    srv.RequestHandlerClass = H
    srv.daemon_threads = True
    return srv


def main(argv=None):
    ap = argparse.ArgumentParser(description="The prompt lab (local only).")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--dry", action="store_true", help="answer every stage with a stub, no Claude")
    ap.add_argument("--dry-delay", type=float, default=0.4, help="seconds a stub takes (--dry)")
    ap.add_argument("--claude", default=None, help="the claude CLI (default: found on PATH)")
    ap.add_argument("--model", default=None, help="model for every stage (default: %s)"
                    % lab.shopkeep.MODEL)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--v2", dest="v2", action="store_true", default=None,
                   help="item language version 2 (forms, traits, voices)")
    g.add_argument("--v1", dest="v2", action="store_false")
    args = ap.parse_args(argv)
    cfg = lab.configure(dry=args.dry, dry_delay=args.dry_delay if args.dry else 0,
                        claude=args.claude, model=args.model, v2=args.v2)
    if not args.dry and not cfg.chat.claude:
        print("no claude CLI found: run with --dry, or pass --claude PATH", file=sys.stderr)
        return 2
    srv = make_server(args.port)
    print("prompt lab: http://127.0.0.1:%d/  (%s, model %s, item language v%d)" % (
        srv.server_address[1], "DRY: stubs, no Claude" if cfg.dry else "live: %s" % cfg.chat.claude,
        cfg.model, 2 if cfg.v2 else 1))
    print("  scratch state: %s (the real one is never opened)" % lab.STATE)
    print("  node for Jarscript 3 checks: %s" % (cfg.node or "not found (browser only)"))
    print("  experiments: %s" % lab.EXP_DIR)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
