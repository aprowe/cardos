"""A focus for the day and a fun fact, for the Today page.

    GET /daily?date=YYYY-MM-DD&kind=focus|fact   -> one line

Both are written once a day by a short, stateless Claude call -- no
repository, no tools -- and kept in daily.json in CARDOS_STATE, so the
page shows the same two all day however often it is gathered. The device
sends its own date, so "the day" is its day, not the server's. Without
Claude (or if the call fails) a few written here stand in, chosen by date.
"""
import os
import sys
import time
import zlib

from . import accounts, dash, store


PROMPT = (
    "Write two short lines for someone's printed morning page. "
    "Line 1 starts 'FOCUS: ' and is one gentle, specific thing to be mindful of today "
    "(at most 12 words, no quotes, not preachy). "
    "Line 2 starts 'FACT: ' and is one surprising, true, verifiable fun fact "
    "(at most 25 words). Vary the subjects. Output only the two lines. "
    "Today is %s.")

FOCUS = [
    "Finish one thing completely before starting the next.",
    "Notice when you're rushing, and slow down for one breath.",
    "Say thank you to someone for something small.",
    "Leave your phone in another room for the first hour.",
    "Drink a glass of water before every coffee.",
    "Ask one question you'd usually be too proud to ask.",
    "Take the stairs, and look out a window on the way.",
]
FACT = [
    "Octopuses have three hearts, and two of them stop beating when they swim.",
    "Honey found in Egyptian tombs, over 3,000 years old, was still edible.",
    "A day on Venus is longer than its year.",
    "Bananas are berries, but strawberries are not.",
    "Wombat droppings are cube-shaped.",
    "The Eiffel Tower is about 15 cm taller in summer, as the iron expands.",
    "There are more trees on Earth than stars in the Milky Way, by most estimates.",
]


def path():
    return os.path.join(accounts.user_dir(), "daily.json")


def _load():
    return store.read_json(path(), {})


def fallback(date):
    """Lines written here, chosen by date. Marked, so they are not kept:
    a Claude that is back in an hour should still get to write the day."""
    n = zlib.crc32(date.encode())
    return {"focus": FOCUS[n % len(FOCUS)], "fact": FACT[(n // 7) % len(FACT)],
            "fallback": True}


def generate(date, chat):
    """Ask Claude for the day's two lines; the fallback if it cannot."""
    from .chat import ClaudeError, ask_once
    if not (chat and chat.claude):
        return fallback(date)
    try:
        out, _ = ask_once(chat, PROMPT % date, 90)
    except ClaudeError as e:
        sys.stderr.write("daily: %s\n" % e)
        return fallback(date)
    got = {}
    for line in out.splitlines():
        line = line.strip().strip("*").strip()
        for key in ("focus", "fact"):
            if line.lower().startswith(key + ":"):
                got[key] = line[len(key) + 1:].strip().strip('"')
    if not got.get("focus") or not got.get("fact"):
        return fallback(date)
    return got


def for_date(date, chat):
    d = _load()
    if date in d:
        return d[date]
    day = generate(date, chat)
    if day.get("fallback"):
        return day

    def keep(d):
        d.setdefault(date, day)                   # someone else's, if it got there first
        return dict(sorted(d.items())[-14:])      # two weeks is plenty
    return store.update_json(path(), keep, {})[date]


def get_daily(h, path, args):
    """the day's focus or fun fact, one line"""
    date = (args.get("date") or [time.strftime("%Y-%m-%d")])[0]
    kind = (args.get("kind") or ["focus"])[0]
    try:
        time.strptime(date, "%Y-%m-%d")
    except ValueError:
        h.text("error date= is YYYY-MM-DD\n", 400)
        return
    if kind not in ("focus", "fact"):
        h.text("error kind= is focus or fact\n", 400)
        return
    h.text(for_date(date, h.chat)[kind] + "\n")


ROUTES = [("GET", "/daily", get_daily)]
