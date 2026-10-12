"""A trial of the staged item pipeline against the real Claude, in a scratch
state: python tools/jar_stages_trial.py OUT.json [--stocks 5] [--claude-cli PATH]

Runs Tibbs's staged stock question and the four stages for each stock's
new finds, exactly as the server would with CARDOS_JAR_STAGES=1, but with
CARDOS_STATE in a temporary directory (a Tibbs with no past, no pool, no
ledger) and a dummy signature -- it never touches the live state, the pool
or the signing key. Writes every find, with what each stage said, to OUT.
"""
import argparse
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--stocks", type=int, default=5)
    ap.add_argument("--finds", type=int, default=4)
    ap.add_argument("--claude-cli", default=None)
    args = ap.parse_args()
    state = tempfile.mkdtemp(prefix="jartrial-")
    os.environ["CARDOS_STATE"] = state                  # before the server modules read it
    from server import jar, kv
    from server.jar import jarstages, shopkeep
    from server.chat import ChatService
    chat = ChatService(claude=args.claude_cli) if args.claude_cli else ChatService()
    report = {"state": state, "v2": jar.jarvm.V2, "stocks": []}
    logs = []

    def log(s):
        logs.append(s)
        print(s, flush=True)

    for b in range(args.stocks):
        t0 = time.time()
        line, _, search = shopkeep.stock_turn(chat, "trial", [], 0, args.finds, staged=True,
                                              log=log)
        log("stock %d: %r" % (b + 1, search))
        stock = {"line": line, "search": search, "finds": [], "secs": 0}
        try:
            recs = jarstages.generate(chat, None, {"owned": []}, None,
                                      signer=lambda d: bytes(64), want=args.finds,
                                      kind_at=b * args.finds, search=search, log=log)
        except Exception as e:                          # noqa: BLE001 - reported
            log("stock %d: %s" % (b + 1, e))
            recs = []
        st = kv.store()
        for rec in recs:
            it = jar.decode(rec)
            c = jarstages.concept_of(it["id"], st) or {}
            stock["finds"].append({
                "id": it["id"], "name": it["name"], "line": it["line"],
                "kind": [k for k, v in jar.enums()["kinds"].items() if v == it["kind"]][0],
                "price": it["price"], "activity": c.get("activity"),
                "travel": jar.TRAVELS[it["travel"]], "idle": jar.IDLES[it["idle"]],
                "lively": jar.LIVELY[it["lively"]], "role": it["role"], "scale": it["scale"],
                "part": it["part"], "voice": it["voice"], "tags": it["tags"],
                "bubbles": it["bubbles"], "habits": [list(h) for h in it["habits"]],
                "script_bytes": len(it["script"]),
                "pal": ["#%02x%02x%02x" % (((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                                            (p & 31) * 255 // 31) for p in it["pal"]],
                "frames": [jar.unpack_frame(f) for f in it["frames"]],
                "concept": c.get("concept"), "look": c.get("look"), "intent": c.get("intent"),
                "note": c.get("note")})
        stock["secs"] = round(time.time() - t0)
        report["stocks"].append(stock)
        with open(args.out, "w", encoding="utf-8") as f:   # written as it goes
            json.dump(dict(report, log=logs), f, indent=1)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
