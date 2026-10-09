"""Text the way the device's apps read it: one line, tab-separated fields.

A field must not hold a tab or a newline -- the device splits on them --
so anything a person or a service wrote goes through flat() first.
server/tests/test_wire.py pins it.
"""


def flat(s, n=None, ascii=False):
    """One line: every run of whitespace (tabs, newlines, any Unicode space)
    one space, the ends trimmed, cut at n characters. ascii=True drops what
    the device's font cannot draw (nothing past 0x7E) before that."""
    s = str(s or "")
    if ascii:
        s = s.encode("ascii", "ignore").decode()
    s = " ".join(s.split())
    return s if n is None else s[:n]
