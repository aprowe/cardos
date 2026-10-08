"""Claude Code's PreToolUse hook for a fenced Build turn (server/fence.py).

Run by Claude Code before every tool call, with the call as JSON on stdin;
exit 2 refuses it and what is on stderr goes back to the model as the
reason. The fence is in the environment the server started the turn with:
FENCE_USER, FENCE_STATE (CARDOS_STATE), FENCE_ROOT (the repository).

Writes go through fence.may_write, which also records a new file as the
user's. Reads -- and searches -- must stay inside the repository: the
server's state beside it holds every device's token.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from server import fence  # noqa: E402

WRITES = {"Write", "Edit", "MultiEdit", "NotebookEdit"}
READS = {"Read", "Glob", "Grep", "LS", "TodoWrite"}


def refuse(why):
    sys.stderr.write(why + "\n")
    sys.exit(2)


def main():
    user = os.environ.get("FENCE_USER")
    state = os.environ.get("FENCE_STATE")
    root = os.environ.get("FENCE_ROOT")
    if not (user and state and root):
        refuse("the fence is not set up; nothing can be changed")
    try:
        call = json.load(sys.stdin)
    except ValueError:
        refuse("could not read the tool call")
    tool = call.get("tool_name", "")
    args = call.get("tool_input") or {}
    path = args.get("file_path") or args.get("notebook_path") or args.get("path")

    if tool in WRITES:
        if not path:
            refuse("%s with no path" % tool)
        ok, why = fence.may_write(state, root, user, path, claim=True)
        if not ok:
            refuse("refused: " + why)
        return
    if tool in READS:
        if path and fence.rel(root, path) is None:
            refuse("refused: only files in the repository can be read")
        return
    refuse("refused: %s is not available here" % tool)


if __name__ == "__main__":
    main()
