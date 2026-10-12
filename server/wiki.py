"""Wikipedia for the device, in lines it can read.

The Wikipedia app (apps/wiki.c) has no JSON parser, no TLS worth spending on
this, and a font of 95 characters. The server does the HTTPS and the JSON,
throws away the markup, and answers in plain ASCII lines:

    GET /wiki/search?q=TEXT      one result a line: title <tab> short description
                                 (empty when nothing is found)
    GET /wiki/page?title=T[&page=N]
                                 page <tab> N <tab> TOTAL <tab> title, then the
                                 text: a paragraph a line, `## Heading` and
                                 `### Subheading` lines. N counts from 1; each
                                 page is at most PAGE bytes, broken between
                                 paragraphs, so it fits the app's buffer
    GET /wiki/random             the first page of a random article, as above
    GET /wiki/summary?q=TOPIC    title <tab> description, then the article's
                                 first paragraph on one line (for commands:
                                 Today, voice). The topic is searched for, so
                                 "the moon" finds Moon
    GET /wiki/featured           today's featured article, as summary does

Every route takes `lang=xx` (default en). Errors are "error why" with a
status: 404 for no such article, 502 when Wikipedia itself fails.

Wikimedia asks for a User-Agent that says who is calling (UA below). What
does not change for minutes is kept for ten: searches, articles split into
pages, summaries. Random is never kept.
"""
import re
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import json
from datetime import datetime, timezone

from . import wire

UA = "CardOS/1.0 (Wikipedia reader for a handheld; https://cardos.arowe.net) python-urllib"
PAGE = 5000              # bytes of text a page; the app's buffer is 6 KB
RESULTS = 10
KEEP_FOR = 600
KEEP_MAX = 64

# Sections that are lists of links and citations: nothing to read on a
# screen 240 pixels wide, and much of an article's length.
SKIP = {"see also", "references", "notes", "external links", "further reading",
        "bibliography", "sources", "citations", "footnotes", "notes and references",
        "references and notes", "works cited", "general and cited sources",
        "cited sources", "general references", "explanatory notes"}

_lock = threading.Lock()
_cache = {}              # key -> (until, value)


class WikiError(Exception):
    def __init__(self, status, why):
        Exception.__init__(self, why)
        self.status = status
        self.why = why


# ---- the network, in one place, so the tests can stand in for Wikipedia -----------

def http_json(url):
    """(status, parsed JSON or None). A 4xx/5xx is returned, not raised."""
    req = urllib.request.Request(url, headers={"User-Agent": UA,
                                               "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw, status = r.read(), r.status
    except urllib.error.HTTPError as e:
        raw, status = e.read(), e.code
    try:
        return status, json.loads(raw) if raw.strip() else None
    except ValueError:
        return status, None


def get(url, missing_ok=False):
    try:
        status, j = http_json(url)
    except (urllib.error.URLError, OSError) as e:
        raise WikiError(502, "cannot reach Wikipedia: %s" % getattr(e, "reason", e))
    if status == 404 and missing_ok:
        return None
    if status >= 400 or j is None:
        raise WikiError(502, "Wikipedia said %d" % status)
    return j


def cached(key, fn):
    now = time.time()
    with _lock:
        hit = _cache.get(key)
        if hit and hit[0] > now:
            return hit[1]
    value = fn()
    with _lock:
        if len(_cache) >= KEEP_MAX:
            for k in sorted(_cache, key=lambda k: _cache[k][0])[:len(_cache) - KEEP_MAX + 1]:
                del _cache[k]
        _cache[key] = (now + KEEP_FOR, value)
    return value


def base(lang):
    return "https://%s.wikipedia.org" % lang


def quote(title):
    return urllib.parse.quote(title.replace(" ", "_"), safe="")


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
    s = wire.flat(s, ascii=True)
    # What was only Chinese or Greek in brackets leaves the brackets behind.
    s = _EMPTY_PARENS.sub("", s)
    s = s.replace(" ,", ",").replace(" ;", ";")
    return wire.flat(_OPEN_PUNCT.sub("(", s))


def drop_math(s):
    """{\\displaystyle ...} is what a formula becomes in plain text: TeX, with
    braces inside it. Take it out whole."""
    out, i = [], 0
    while True:
        j = s.find("{\\displaystyle", i)
        if j < 0:
            out.append(s[i:])
            return "".join(out)
        out.append(s[i:j])
        depth, k = 0, j
        while k < len(s):
            if s[k] == "{":
                depth += 1
            elif s[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        i = k + 1


_HEAD = re.compile(r"^(={2,6})\s*(.*?)\s*=+\s*$")


def blocks(extract):
    """An article's plain-text extract (exsectionformat=wiki) as the lines the
    device shows: paragraphs, `## ` and `### ` headings; the sections that
    are only links and citations gone, and a heading with nothing under it."""
    out = []                      # (level, text); level 0 for a paragraph
    skipping = 0                  # the level of the section being skipped
    for raw in drop_math(extract or "").split("\n"):
        m = _HEAD.match(raw.strip())
        if m:
            level, title = len(m.group(1)), ascii_text(m.group(2))
            if skipping and level > skipping:
                continue
            skipping = 0
            if title.lower() in SKIP:
                skipping = level
                continue
            out.append((level, title))
            continue
        if skipping:
            continue
        text = ascii_text(raw)
        if text:
            out.append((0, text))
    kept = []
    for i, (level, text) in enumerate(out):
        if level:
            nxt = out[i + 1] if i + 1 < len(out) else None
            if nxt is None or (nxt[0] and nxt[0] <= level):
                continue
            kept.append(("## " if level == 2 else "### ") + text)
        else:
            kept.append(text)
    return kept


def _cut(text, size):
    """A paragraph longer than a page, in pieces broken between words."""
    pieces = []
    while len(text) > size:
        k = text.rfind(" ", 0, size + 1)
        if k <= 0:
            k = size
        pieces.append(text[:k].rstrip())
        text = text[k:].lstrip()
    if text:
        pieces.append(text)
    return pieces


def paginate(paras, size=PAGE):
    """Paragraphs into pages of at most `size` bytes, joined by newlines,
    broken only between paragraphs (or between words, inside one too long
    for a page). A heading never ends a page: it goes over with its text."""
    lines = []
    for p in paras:
        lines.extend(_cut(p.strip(), size))
    pages, cur, n = [], [], 0
    for i, line in enumerate(lines):
        need = len(line) + (1 if cur else 0)
        if line.startswith("#") and i + 1 < len(lines):
            need += 1 + min(len(lines[i + 1]), 200)       # room for some of its text
        if cur and n + need > size:
            pages.append("\n".join(cur))
            cur, n = [], 0
        n += len(line) + (1 if cur else 0)
        cur.append(line)
    if cur:
        pages.append("\n".join(cur))
    return pages


# ---- Wikipedia ----------------------------------------------------------------------

def _strip_tags(s):
    return re.sub(r"<[^>]*>", "", s or "")


def search(q, lang="en"):
    def fetch():
        j = get("%s/w/rest.php/v1/search/page?%s" % (
            base(lang), urllib.parse.urlencode({"q": q, "limit": RESULTS})))
        out = []
        for p in j.get("pages") or []:
            title = ascii_text(p.get("title"))
            if not title:
                continue
            desc = ascii_text(p.get("description") or _strip_tags(p.get("excerpt")))
            out.append((title, desc[:100]))
        return out
    return cached(("search", lang, q.lower()), fetch)


def article(title, lang="en"):
    """(title, [pages]) of an article, or None if there is none."""
    def fetch():
        j = get("%s/w/api.php?%s" % (base(lang), urllib.parse.urlencode({
            "action": "query", "prop": "extracts", "explaintext": 1,
            "exsectionformat": "wiki", "redirects": 1, "format": "json",
            "formatversion": 2, "titles": title})))
        pages = ((j.get("query") or {}).get("pages")) or []
        if not pages or pages[0].get("missing") or pages[0].get("invalid"):
            return None
        p = pages[0]
        return ascii_text(p.get("title") or title), paginate(blocks(p.get("extract")))
    a = cached(("article", lang, title), fetch)
    if a and a[0] != title:
        # The device asks for the next page by the title it was given (a
        # redirect's target, a random one's), which is then already here.
        cached(("article", lang, a[0]), lambda: a)
    return a


def page_text(title, n, lang="en"):
    a = article(title, lang)
    if a is None:
        raise WikiError(404, "no article called %s" % ascii_text(title))
    name, pages = a
    if not pages:
        pages = ["(this article has no text)"]
    if n < 1 or n > len(pages):
        raise WikiError(404, "%s has %d pages" % (name, len(pages)))
    return "page\t%d\t%d\t%s\n%s\n" % (n, len(pages), name, pages[n - 1])


def random_title(lang="en"):
    j = get("%s/api/rest_v1/page/random/summary" % base(lang))
    if not j.get("title"):
        raise WikiError(502, "Wikipedia gave no random article")
    return j["title"]


def _summary_lines(title, desc, extract):
    first = ""
    for line in (extract or "").split("\n"):
        first = (first + " " + line.strip()).strip()
    return "%s\t%s\n%s\n" % (ascii_text(title), ascii_text(desc), ascii_text(first))


def summary(q, lang="en"):
    def fetch():
        found = search(q, lang)
        if not found:
            return None
        j = get("%s/api/rest_v1/page/summary/%s" % (base(lang), quote(found[0][0])),
                missing_ok=True)
        if not j:
            return None
        return _summary_lines(j.get("title") or found[0][0], j.get("description"),
                              j.get("extract"))
    s = cached(("summary", lang, q.lower()), fetch)
    if s is None:
        raise WikiError(404, "nothing on Wikipedia about %s" % ascii_text(q))
    return s


def featured(lang="en"):
    day = datetime.now(timezone.utc).strftime("%Y/%m/%d")

    def fetch():
        j = get("%s/api/rest_v1/feed/featured/%s" % (base(lang), day))
        tfa = j.get("tfa") or {}
        title = (tfa.get("titles") or {}).get("normalized") or tfa.get("title")
        if not title:
            return None
        return _summary_lines(title, tfa.get("description"), tfa.get("extract"))
    s = cached(("featured", lang, day), fetch)
    if s is None:
        raise WikiError(404, "no featured article today")
    return s


# ---- routes ----------------------------------------------------------------------------

def _arg(args, name):
    return (args.get(name) or [""])[0].strip()


def _lang(args):
    lang = _arg(args, "lang") or "en"
    if not re.match(r"^[a-z]{2,3}(-[a-z]+)?$", lang):
        raise ValueError("lang= is a Wikipedia language code, like en")
    return lang


def _route(fn):
    def wrapped(h, path, args):
        try:
            h.text(fn(args))
        except WikiError as e:
            sys.stderr.write("wiki: %s\n" % e.why)
            h.text("error %s\n" % e.why, e.status)
        except ValueError as e:
            h.text("error %s\n" % e, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


@_route
def get_search(args):
    """Wikipedia articles matching q, a line each"""
    q = _arg(args, "q")
    if not q:
        raise ValueError("q= is what to search for")
    return "".join("%s\t%s\n" % r for r in search(q[:200], _lang(args)))


@_route
def get_page(args):
    """a page of a Wikipedia article as plain lines"""
    title = _arg(args, "title")
    if not title:
        raise ValueError("title= is the article")
    try:
        n = int(_arg(args, "page") or "1")
    except ValueError:
        raise ValueError("page= is a number")
    return page_text(title[:300], n, _lang(args))


@_route
def get_random(args):
    """the first page of a random Wikipedia article"""
    lang = _lang(args)
    return page_text(random_title(lang), 1, lang)


@_route
def get_summary(args):
    """a topic's first paragraph on Wikipedia"""
    q = _arg(args, "q")
    if not q:
        raise ValueError("q= is the topic")
    return summary(q[:200], _lang(args))


@_route
def get_featured(args):
    """today's featured Wikipedia article"""
    return featured(_lang(args))


ROUTES = [
    ("GET", "/wiki/search", get_search),
    ("GET", "/wiki/page", get_page),
    ("GET", "/wiki/random", get_random),
    ("GET", "/wiki/summary", get_summary),
    ("GET", "/wiki/featured", get_featured),
]
