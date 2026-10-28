"""The CardOS server: everything the device talks to that is not the open
internet. Build's conversation with Claude Code, voice, updates, screenshots,
and the desktop stream, on one port.

    python -m server --port 8080                     on the laptop
    python -m server --port 8081 --token ... \\
        --build --store /var/lib/cardos/store        on the droplet

server/app.py is the HTTP layer; each service module declares its own routes.
"""
import argparse
import os
import sys
from http.server import ThreadingHTTPServer

from .app import Handler, ROOT_DIR
from .chat import ChatService
from .voice import Voice


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m server", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--chrome", default=None, help=argparse.SUPPRESS)   # the web renderer's; gone
    ap.add_argument("--claude-cli", default=None,
                    help="path to the claude executable, if it is not on PATH")
    ap.add_argument("--model", default=None,
                    help="model for the chat session, e.g. claude-opus-5")
    ap.add_argument("--whisper", default=None,
                    help="directory holding whisper-cli and a ggml model "
                         "(default: the sibling cardlet project's)")
    ap.add_argument("--token", default=None,
                    help="require this shared secret on every route (bearer or X-Token)")
    ap.add_argument("--api-key", action="store_true",
                    help="let the agent use ANTHROPIC_API_KEY from the environment "
                         "instead of the login this machine already has")
    ap.add_argument("--store", default=None,
                    help="serve /update from this directory rather than the "
                         "build tree")
    ap.add_argument("--build", action="store_true",
                    help="after a chat turn that changed the code, build it, "
                         "publish to --store and commit; see server/build.py")
    args = ap.parse_args(argv)
    if args.build and not args.store:
        raise SystemExit("--build publishes into --store; give it one")

    Handler.chrome = None             # --chrome is accepted and ignored: the web renderer is gone

    kw = dict(claude=args.claude_cli, token=args.token,
              model=args.model, use_api_key=args.api_key)
    if args.build:
        from .build import BuildingChat
        Handler.chat = BuildingChat(store=args.store, **kw)
    else:
        Handler.chat = ChatService(**kw)
    if args.store:
        os.makedirs(args.store, exist_ok=True)
        Handler.store = args.store
    Handler.voice = Voice(whisper_dir=args.whisper)

    # More than one person (server/accounts.py): the first start with both a
    # token and a dashboard password makes the owner's account from them and
    # moves their files under users/. Every device keeps working.
    if args.token and os.environ.get("DASH_PASSWORD"):
        from . import accounts
        if accounts.migrate(os.environ.get("CARDOS_OWNER", "alex"), os.environ["DASH_PASSWORD"],
                            args.token):
            print("  accounts: made, with %s as the owner" % os.environ.get("CARDOS_OWNER", "alex"))
        accounts.know_token(args.token)

    # The apps' own background work (server/plugins.py): Crossword's NYT
    # Midi, today's and a week back, looked for hourly.
    from . import plugins
    plugins.start_all()

    # Threading, because a chat turn takes a minute, a render takes ten
    # seconds, and the device polls for its answer throughout. On the
    # single-threaded server every poll queued behind the work it was polling.
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("cardos server on port %d" % args.port)
    print("  claude: %s" % (Handler.chat.claude or "NOT FOUND -- /chat will fail"))
    print("  voice:  %s" % ("whisper ready" if Handler.voice.ready()
                            else "NOT FOUND -- /voice will fail"))
    print("  builds: %s" % ("yes, publishing to " + args.store if args.build else "no"))
    if not args.token:
        # Said plainly, once, where it can still be acted on.
        print("")
        print("  This server runs Claude Code in %s with permission" % ROOT_DIR)
        print("  to edit it, and asks nothing of whoever connects. Anything that")
        print("  can reach this port can change that folder. --token SECRET")
        print("  requires a shared string, which the device reads from")
        print("  /config/claude.token on its card.")
    srv.serve_forever()


if __name__ == "__main__":
    main()
