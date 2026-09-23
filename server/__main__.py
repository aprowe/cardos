"""The CardOS server: everything the device talks to that is not the open
internet. Build's conversation with Claude Code, voice, updates, screenshots,
the desktop stream and the web renderer, on one port.

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
    ap.add_argument("--chrome", default=None)
    ap.add_argument("--test", help="render this URL to page.cpx and exit")
    ap.add_argument("--width", type=int, default=420,
                    help="CSS viewport width, --no-pixel only; wider is scaled to 240")
    ap.add_argument("--no-pixel", action="store_true",
                    help="photographic mode: real fonts, rendered wide and scaled down")
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

    try:
        from .render import render
        Handler.chrome = args.chrome or render.find_chrome()
    except (ImportError, SystemExit) as e:
        # The droplet has no Chrome and no need of one; say so and serve the
        # rest. --test is the one thing that cannot go on without it.
        if args.test:
            raise
        render = None
        Handler.chrome = args.chrome
        sys.stderr.write("render: no Chrome (%s); /render will fail\n" % e)

    if args.test:
        if args.no_pixel:
            im, _ = render.shoot(Handler.chrome, args.test, 2000, 6000, args.width)
        else:
            im, _ = render.shoot_pixelfont(Handler.chrome, args.test, 2000, 6000)
        if im is None:
            raise SystemExit("render failed")
        im = render.trim(im)
        data = render.to_cpx(im)
        with open("page.cpx", "wb") as f:
            f.write(data)
        print("page.cpx: %dx%d, %d bytes (raw %d)"
              % (im.size[0], im.size[1], len(data), im.size[0] * im.size[1] * 2))
        return

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

    # Threading, because a chat turn takes a minute, a render takes ten
    # seconds, and the device polls for its answer throughout. On the
    # single-threaded server every poll queued behind the work it was polling.
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("cardos server on port %d" % args.port)
    print("  chrome: %s" % (Handler.chrome or "none -- /render will fail"))
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
