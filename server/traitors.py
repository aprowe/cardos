"""The Traitors (apps/traitors): what the computer players say at the round
table, written by Claude.

The host device decides everything a computer player does -- its vote, its
coin, whom it suspects -- in code, so a turn never waits on this. What it
cannot do is talk. So before the round table it posts the situation, and
each speaker's line comes back to be shown as they "speak", accusing the one
it is going to vote for anyway. Offline, or when this fails, the device has
plain lines of its own.

    POST /traitors/talk   body: the situation, a line each (below)
                          -> "ID\\n"; 400 a body that says nothing, 429 the day is used
    GET  /traitors/talk?id=ID
                          -> "pending\\n" | "ok\\n" then "SEAT\\tTEXT\\n" a speaker
                             | "error WHY\\n"

The body, plain lines, unknown ones ignored:

    round N
    seat S NAME ROLE VOTE SUSPECT     a computer to speak: ROLE faithful or
                                      traitor, VOTE and SUSPECT seat numbers
    person S NAME                     a person at the table (never speaks here)
    gone S NAME HOW                   HOW: banished-faithful, banished-traitor, murdered
    log TEXT                          what happened, in order

A speaker's role is a secret: it is given to Claude so a Traitor can lie
well, and nothing that comes back may say who is a Traitor unless the
table already knows. The device never sees another's role this way: the
lines are the only thing returned.
"""
import json
import re
import secrets
import sys
import threading

from . import ask, jobs, kv

TALK_DAILY = 60          # round tables a day, each person
BODY_MAX = 4000
LINE_MAX = 110

_jobs = jobs.Table(1800)

SYSTEM = ("You write dialogue for computer players in a gothic hidden-role party game "
          "set in a Scottish castle. Short, in character, dramatic but never cruel. "
          "Plain ASCII only.")


def parse(body):
    """The situation, from the device's lines."""
    s = {"round": 1, "speakers": [], "people": [], "gone": [], "log": []}
    for raw in body.splitlines():
        parts = raw.strip().split()
        if not parts:
            continue
        kind = parts[0]
        try:
            if kind == "round" and len(parts) >= 2:
                s["round"] = int(parts[1])
            elif kind == "seat" and len(parts) >= 6:
                s["speakers"].append({"seat": int(parts[1]), "name": parts[2][:16],
                                      "role": "traitor" if parts[3] == "traitor" else "faithful",
                                      "vote": int(parts[4]), "suspect": int(parts[5])})
            elif kind == "person" and len(parts) >= 3:
                s["people"].append({"seat": int(parts[1]), "name": parts[2][:16]})
            elif kind == "gone" and len(parts) >= 4:
                s["gone"].append({"seat": int(parts[1]), "name": parts[2][:16], "how": parts[3]})
            elif kind == "log":
                s["log"].append(raw.strip()[4:][:160])
        except ValueError:
            continue
    return s


def names(s):
    n = {}
    for p in s["speakers"] + s["people"] + s["gone"]:
        n[p["seat"]] = p["name"]
    return n


def prompt_for(s):
    who = names(s)
    alive = [who[p["seat"]] for p in s["speakers"]] + [who[p["seat"]] for p in s["people"]]
    out = ["It is round %d. At the round table now: %s." % (s["round"], ", ".join(alive))]
    if s["gone"]:
        out.append("Gone: " + "; ".join(
            "%s (%s)" % (g["name"], {"banished-faithful": "banished, was Faithful",
                                      "banished-traitor": "banished, was a Traitor",
                                      "murdered": "murdered in the night"}.get(g["how"], g["how"]))
            for g in s["gone"]))
    if s["log"]:
        out.append("What has happened so far:\n" + "\n".join("- " + l for l in s["log"][-14:]))
    out.append("")
    out.append("Write one line each for these players, who now speak in turn before the vote:")
    for p in s["speakers"]:
        target = who.get(p["vote"], "someone")
        if p["role"] == "traitor":
            out.append("- %s (seat %d) is secretly a Traitor and must not let it show. They will vote "
                       "to banish %s: give a plausible, Faithful-sounding reason, and never hint "
                       "who the Traitors are." % (p["name"], p["seat"], target))
        else:
            out.append("- %s (seat %d) is Faithful and does not know who the Traitors are. They will "
                       "vote to banish %s: give the reason they believe, from what has happened." %
                       (p["name"], p["seat"], target))
    out.append("")
    out.append("Each line is spoken aloud, at most %d characters, names %s's target, and does not "
               "repeat another's wording. Nobody may state anyone's secret role as fact; refer only "
               "to what the table saw." % (LINE_MAX - 10, "the speaker"))
    return "\n".join(out)


def schema_for(s):
    seats = [p["seat"] for p in s["speakers"]]
    return {"type": "object", "required": ["lines"], "additionalProperties": False,
            "properties": {"lines": {"type": "array", "minItems": len(seats), "maxItems": len(seats),
                                     "items": {"type": "object", "required": ["seat", "text"],
                                               "additionalProperties": False,
                                               "properties": {"seat": {"enum": seats},
                                                              "text": {"type": "string", "minLength": 1,
                                                                       "maxLength": LINE_MAX * 2}}}}}}


def clean(text):
    """One line the device's font can draw."""
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = text.replace("—", "--").replace("–", "-").replace("…", "...")
    text = re.sub(r"[^\x20-\x7e]", "", " ".join(text.split()))
    return text[:LINE_MAX].rstrip()


def answer(value, s):
    """The lines, in the speakers' order, a seat and a tab each."""
    by_seat = {}
    for item in value.get("lines", []):
        by_seat.setdefault(item["seat"], clean(item["text"]))
    return "".join("%d\t%s\n" % (p["seat"], by_seat[p["seat"]])
                   for p in s["speakers"] if by_seat.get(p["seat"]))


def _run(jid, chat, s, user):
    try:
        value = ask.ask_shape(chat, prompt_for(s), schema_for(s), user=user, limit=None,
                              system=SYSTEM)
        result = ("ok", answer(value, s))
    except Exception as e:                      # Claude, the check: the device has its own lines
        result = ("error", str(e) or type(e).__name__)
    _jobs.replace(jid, result)
    sys.stderr.write("traitors: %s %s\n" % (jid, result[0]))


def post_talk(h, path, args):
    """the computer players' words at the round table"""
    try:
        body = h.body(BODY_MAX).decode("utf-8", "replace")
    except ValueError as e:
        h.text("error %s\n" % e, 400)
        return
    s = parse(body)
    if not s["speakers"]:
        h.text("error nobody to speak\n", 400)
        return
    if not (h.chat and h.chat.claude):
        h.text("error this server runs without Claude\n", 503)
        return
    user = kv.me()
    try:
        ask.take_turn(user, TALK_DAILY)
    except ask.RateLimited as e:
        h.text("error %s\n" % e, 429)
        return
    jid = secrets.token_hex(5)
    _jobs.put(jid, ("pending", None))
    threading.Thread(target=_run, args=(jid, h.chat, s, user), daemon=True).start()
    h.text(jid + "\n")


def get_talk(h, path, args):
    """what they said, once it is written"""
    jid = (args.get("id") or [""])[0]
    job = _jobs.get(jid)
    if not job:
        h.text("error no such request\n", 404)
        return
    state, value = job
    if state == "pending":
        h.text("pending\n")
    elif state == "ok":
        h.text("ok\n" + value)
    else:
        h.text("error %s\n" % " ".join(str(value).split()))


ROUTES = [
    ("POST", "/traitors/talk", post_talk, "device_or_dash"),
    ("GET", "/traitors/talk", get_talk, "device_or_dash"),
]
