"""The Traitors (apps/traitors): the round table is a conversation, and
Claude speaks for the computer players.

The host device decides what a computer player does -- its coin, its vote --
in code, from a suspicion of everyone that the game moves. The talk moves it
too. When the table opens (mode open) each computer says whom it suspects
and why. Then whenever a person says something (mode reply) one to three of
them answer -- anyone named first -- and Claude says whose mind changed:
`lean SEAT TARGET`, which the device turns into suspicion. So a person who
argues well can turn the vote. Offline, or when this fails, the device has
plain lines of its own and leans on the names a person says.

    POST /traitors/talk   body: the situation, a line each (below)
                          -> "ID\\n"; 400 a body that says nothing, 429 the day is used
    GET  /traitors/talk?id=ID
                          -> "pending\\n" | "ok\\n" then "SEAT\\tTEXT\\n" a speaker
                             | "error WHY\\n"

The body, plain lines, unknown ones ignored:

    mode open|reply                   open: everyone's opening words; reply:
                                      answer the last thing said
    round N
    seat S NAME ROLE VOTE SUSPECT     a computer at the table: ROLE faithful or
                                      traitor, SUSPECT the seat it suspects most
                                      (VOTE is the old field, ignored now)
    person S NAME                     a person at the table
    gone S NAME HOW                   HOW: banished-faithful, banished-traitor, murdered
    bio S TEXT                        who they are: "29, paramedic"
    log TEXT                          what happened, in order
    say S TEXT                        the table's talk so far, in order

The reply: "ok", then "SEAT\tTEXT" a speaker, then "lean\tSEAT\tTARGET" for
each computer whose suspicion the talk moved toward TARGET.

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

from server import ask, jobs, kv

TALK_DAILY = 240         # openings and replies a day, each person (a game is ~15)
BODY_MAX = 4000
LINE_MAX = 110

_jobs = jobs.Table(1800)

SYSTEM = ("You write what contestants say at the round table of a reality TV show like "
          "The Traitors: ordinary people -- a paramedic, a builder, a teacher -- at a castle, "
          "playing for a cash prize. They talk like real people on that show: first names, "
          "modern and natural, emotional, sometimes blunt or tearful, never theatrical, never "
          "fantasy or old-fashioned speech. Plain ASCII only.")


def parse(body):
    """The situation, from the device's lines."""
    s = {"round": 1, "speakers": [], "people": [], "gone": [], "log": [], "talk": [], "mode": "open",
         "bio": {}}
    for raw in body.splitlines():
        parts = raw.strip().split()
        if not parts:
            continue
        kind = parts[0]
        try:
            if kind == "round" and len(parts) >= 2:
                s["round"] = int(parts[1])
            elif kind == "mode" and len(parts) >= 2:
                s["mode"] = "reply" if parts[1] == "reply" else "open"
            elif kind == "say" and len(parts) >= 3:
                s["talk"].append({"seat": int(parts[1]), "text": raw.strip().split(None, 2)[2][:200]})
            elif kind == "seat" and len(parts) >= 5:
                s["speakers"].append({"seat": int(parts[1]), "name": parts[2][:16],
                                      "role": "traitor" if parts[3] == "traitor" else "faithful",
                                      "vote": int(parts[4]), "suspect": int(parts[-1])})
            elif kind == "person" and len(parts) >= 3:
                s["people"].append({"seat": int(parts[1]), "name": parts[2][:16]})
            elif kind == "gone" and len(parts) >= 4:
                s["gone"].append({"seat": int(parts[1]), "name": parts[2][:16], "how": parts[3]})
            elif kind == "bio" and len(parts) >= 3:
                s["bio"][int(parts[1])] = raw.strip().split(None, 2)[2][:40]
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


def _roles(s, who):
    out = []
    for p in s["speakers"]:
        if p["role"] == "traitor":
            out.append("- %s (seat %d) is secretly a Traitor: protects themself and the other "
                       "Traitor, deflects, may agree with a case against a Faithful, and never hints "
                       "who the Traitors are." % (p["name"], p["seat"]))
        else:
            out.append("- %s (seat %d) is Faithful and does not know who the Traitors are; leans "
                       "on what the table saw. Suspects %s most so far." %
                       (p["name"], p["seat"], who.get(p["suspect"], "nobody yet")))
    return out


def prompt_for(s):
    who = names(s)
    alive = [who[p["seat"]] for p in s["speakers"]] + [who[p["seat"]] for p in s["people"]]
    def who_is(p):
        bio = s["bio"].get(p["seat"])
        return "%s (%s)" % (p["name"], bio) if bio else p["name"]
    alive = [who_is(p) for p in s["speakers"]] + [who_is(p) for p in s["people"]]
    out = ["It is round %d. At the round table now: %s." % (s["round"], ", ".join(alive))]
    if s["people"]:
        out.append("People (not computers): %s." % ", ".join(p["name"] for p in s["people"]))
    if s["gone"]:
        out.append("Gone: " + "; ".join(
            "%s (%s)" % (g["name"], {"banished-faithful": "banished, was Faithful",
                                      "banished-traitor": "banished, was a Traitor",
                                      "murdered": "murdered in the night"}.get(g["how"], g["how"]))
            for g in s["gone"]))
    if s["log"]:
        out.append("What has happened so far:\n" + "\n".join("- " + l for l in s["log"][-14:]))
    if s["talk"]:
        out.append("The talk at this table so far:\n" + "\n".join(
            "%s: %s" % (who.get(t["seat"], "?"), t["text"]) for t in s["talk"][-16:]))
    out.append("")
    out.append("The computer players:")
    out.extend(_roles(s, who))
    out.append("")
    if s["mode"] == "open":
        out.append("The round table opens. Write one opening line for each computer player: whom "
                   "they suspect and why, from what the table saw.")
    else:
        last = s["talk"][-1] if s["talk"] else None
        out.append("Answer the last thing said%s. Choose one to three computer players who would "
                   "answer -- anyone it names or accuses first, defending or pushing back; agree "
                   "only when it is convincing. One line each." %
                   (" (by %s)" % who.get(last["seat"], "?") if last else ""))
    out.append("Then, under \"leans\", list each computer whose suspicion the talk has moved, and "
               "toward whom (a seat number). A Traitor's lean is what it pretends to believe; a "
               "Faithful's is what it now believes. Leave out anyone unmoved.")
    out.append("")
    out.append("Each line is spoken aloud, at most %d characters, and does not repeat another's "
               "wording. Nobody may state anyone's secret role as fact." % (LINE_MAX - 10))
    return "\n".join(out)


def schema_for(s):
    seats = [p["seat"] for p in s["speakers"]]
    table = seats + [p["seat"] for p in s["people"]]
    lo, hi = (len(seats), len(seats)) if s["mode"] == "open" else (1, min(3, len(seats)))
    return {"type": "object", "required": ["lines"], "additionalProperties": False,
            "properties": {
                "lines": {"type": "array", "minItems": lo, "maxItems": hi,
                          "items": {"type": "object", "required": ["seat", "text"],
                                    "additionalProperties": False,
                                    "properties": {"seat": {"enum": seats},
                                                   "text": {"type": "string", "minLength": 1,
                                                            "maxLength": LINE_MAX * 2}}}},
                "leans": {"type": "array", "maxItems": len(seats),
                          "items": {"type": "object", "required": ["seat", "target"],
                                    "additionalProperties": False,
                                    "properties": {"seat": {"enum": seats},
                                                   "target": {"enum": table}}}}}}


def clean(text):
    """One line the device's font can draw."""
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = text.replace("—", "--").replace("–", "-").replace("…", "...")
    text = re.sub(r"[^\x20-\x7e]", "", " ".join(text.split()))
    return text[:LINE_MAX].rstrip()


def answer(value, s):
    """The lines -- an opening in the speakers' order, a reply in the order
    Claude gave -- a seat and a tab each, then the leans."""
    lines, seen = [], set()
    items = value.get("lines", [])
    if s["mode"] == "open":
        order = {p["seat"]: i for i, p in enumerate(s["speakers"])}
        items = sorted(items, key=lambda it: order.get(it["seat"], 99))
    for item in items:
        text = clean(item["text"])
        if item["seat"] in seen or not text:
            continue
        seen.add(item["seat"])
        lines.append("%d\t%s\n" % (item["seat"], text))
    for lean in value.get("leans", []) or []:
        if lean["seat"] != lean["target"]:
            lines.append("lean\t%d\t%d\n" % (lean["seat"], lean["target"]))
    return "".join(lines)


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
