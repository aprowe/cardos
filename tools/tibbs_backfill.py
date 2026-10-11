"""Put Tibbs's past into his history (server/shopkeep.py, "his memory").

Before 2026-10-11 the only record of what he said was his Claude Code
session's own transcript. Run once on the server, as the account that runs
it, with CARDOS_STATE set:

    python tools/tibbs_backfill.py [SESSION.jsonl]

With no file it reads the transcript of the session in the store
(tibbs/session). Each user turn and the assistant's text that answered it
become one history line, timed by the transcript, the player taken from
"(account NAME)" in the prompt. Lines already there are not written twice.
Then it brings his notebook up to date from them.
"""
import datetime
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from server import kv, shopkeep  # noqa: E402


def text_of(content):
    if isinstance(content, str):
        return content
    out = []
    for c in content or []:
        if isinstance(c, dict) and c.get("type") == "text":
            out.append(c.get("text", ""))
    return "\n".join(out)


def when(row):
    try:
        return datetime.datetime.fromisoformat(row["timestamp"].replace("Z", "+00:00")).timestamp()
    except (KeyError, ValueError):
        return None


def exchanges(path):
    """[(t, said, reply)] in order."""
    out, said, t0, reply = [], None, None, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            try:
                row = json.loads(ln)
            except ValueError:
                continue
            msg = row.get("message") or {}
            if row.get("type") == "user" and msg.get("role") == "user":
                txt = text_of(msg.get("content"))
                if not txt.strip():
                    continue                              # a tool result, not a turn
                if said is not None and reply:
                    out.append((t0, said, "\n".join(reply).strip()))
                said, t0, reply = txt, when(row), []
            elif row.get("type") == "assistant" and said is not None:
                txt = text_of(msg.get("content"))
                if txt.strip():
                    reply.append(txt)
    if said is not None and reply:
        out.append((t0, said, "\n".join(reply).strip()))
    return out


def main():
    st = kv.store()
    if len(sys.argv) > 1:
        path = sys.argv[1]
    else:
        sid = shopkeep._get(st, "tibbs/session", None)
        if not sid:
            sys.exit("no session in the store")
        proj = "-" + shopkeep.home().strip("/").replace("/", "-")
        path = os.path.join(os.path.expanduser("~/.claude/projects"), proj, sid + ".jsonl")
    rows = exchanges(path)
    have = {(r.get("t"), r.get("said")) for r in shopkeep.history(10 ** 6)}
    n = 0
    for t, said, reply in rows:
        t = int(t or 0)
        if (t, said) in have:
            continue
        m = re.search(r"\(account ([A-Za-z0-9_.-]+)\)", said)
        kind = "stock" if "needs filling" in said else "talk"
        shopkeep.history_add(kind, m.group(1) if m else "", said, reply, now=t)
        n += 1
    print("%d exchanges from %s, %d new" % (len(rows), path, n))
    if n and "--no-notebook" not in sys.argv:
        from server import chat as _chat
        svc = _chat.ChatService(claude=os.environ.get("CARDOS_CLAUDE", "claude"))
        while shopkeep.update_notebook(svc, st):
            print("notebook: up to", shopkeep._get(st, "tibbs/noted_t", 0))


if __name__ == "__main__":
    main()
