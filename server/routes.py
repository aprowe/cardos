"""What route functions share, beyond app.Handler's reply helpers.

Who may call a route is not here: it is the fourth field of the route's
tuple, and app._dispatch enforces it before the route runs --

    "token"            the device's token (the default)
    "admin"            the token, and the server's owner
    "device_or_dash"   the device's token or the dashboard's cookie
    "dash"             the dashboard's cookie; a JSON 403 the page reads
    "open"             anyone; the route checks for itself if it must
"""


def arg(args, name, default=""):
    """A query parameter, or the default."""
    return (args.get(name) or [default])[0]


def text_route(fn):
    """fn(h, args) as a route; a ValueError is the caller's mistake, a 400
    and one line saying what."""
    def wrapped(h, path, args):
        try:
            fn(h, args)
        except ValueError as e:
            h.text("error %s\n" % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped
