"""Text the way the device's apps read it: one line, tab-separated fields.

A field must not hold a tab or a newline -- the device splits on them --
so anything a person or a service wrote goes through flat() first.
server/tests/test_wire.py pins it.
"""
import re
import unicodedata


def flat(s, n=None, ascii=False):
    """One line: every run of whitespace (tabs, newlines, any Unicode space)
    one space, the ends trimmed, cut at n characters. ascii=True drops what
    the device's font cannot draw (nothing past 0x7E) before that."""
    s = str(s or "")
    if ascii:
        s = s.encode("ascii", "ignore").decode()
    s = " ".join(s.split())
    return s if n is None else s[:n]


# ---- text the device can draw ---------------------------------------------------

_REPLACE = {
    "–": "-", "—": " - ", "−": "-", "‐": "-", "‑": "-",
    "‘": "'", "’": "'", "‚": "'", "“": '"', "”": '"',
    "„": '"', "«": '"', "»": '"', "…": "...", "⁄": "/",
    "°": " deg", "×": "x", "·": "-", "•": "-", " ": " ",
    "′": "'", "″": '"', "ß": "ss", "æ": "ae", "Æ": "AE",
    "œ": "oe", "Œ": "OE", "ø": "o", "Ø": "O", "ł": "l",
    "Ł": "L", "ð": "d", "þ": "th", "±": "+/-", "≈": "~",
    "£": "GBP ", "€": "EUR ", "¥": "JPY ",
}


_EMPTY_PARENS = re.compile(r"\s*\(\s*[,;:]?\s*\)")
_OPEN_PUNCT = re.compile(r"\(\s*[,;:]?\s*")


def ascii_text(s):
    """One line of what the 6x8 font and ui13 can draw: accents taken off,
    dashes and quotes made plain, everything else dropped, spaces collapsed."""
    s = "".join(_REPLACE.get(c, c) for c in str(s or ""))
    s = unicodedata.normalize("NFKD", s)
    s = "".join(_REPLACE.get(c, c) for c in s)        # NFKD makes some of them (1/2)
    s = flat(s, ascii=True)
    # What was only Chinese or Greek in brackets leaves the brackets behind.
    s = _EMPTY_PARENS.sub("", s)
    s = s.replace(" ,", ",").replace(" ;", ";")
    return flat(_OPEN_PUNCT.sub("(", s))
