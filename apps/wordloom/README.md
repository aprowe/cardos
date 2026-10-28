# Wordloom

**Wordloom** (2026-10-27, `apps/wordloom/wordloom.c`, Games). A word game
for two friends, played a move at a time -- at once or hours apart --
through the server's rooms (`server/rooms.py`, `apps/social.h`). No server
code of its own.

**The game** is Wordloom's own, not a copy of a commercial one: an 11x11
loom (fits the screen at 11 px a cell), racks of six, eighty tiles with
their own counts and values (`WL_COUNT`, `WL_VALUE`), and premiums laid out
in `WL_LAYOUT`: knot (teal, letter x2; the centre is one), gold (amber,
letter x3), warp (rose, word x2), loom (violet, word x3). Two bonuses: the
*weave*, +5 for a play that makes three or more words at once, and the
*bolt*, +20 for laying all six tiles. A turn is a weave (tiles in one row or
column, no empty cell between, the first through the centre and at least
two tiles, every later one touching the loom), a swap (needs as many tiles
in the bag), a pass, or a resignation (allowed on either turn). Four
scoreless turns in a row end it, each player losing their rack's value;
laying your last tile with the bag empty *ties off*: +10, and the other
loses what they hold. Every rule is `apps/wordloom/wordloom.h`, host-tested
in `apps/wordloom/test/test_wordloom.c`.

**Words are on honour.** There is no dictionary on the device: a word list
is megabytes and flash is the constraint. `wl_check` already takes a
`WlDict` callback asked about every word a play makes (the app passes 0),
so a dictionary later -- a compressed list, or a server check -- is a
function, not a change to the rules.

**What travels.** A room's state is `wordloom 1\nseed N\n`; the players are
the room's members, creator first (the server never reorders them), and
the creator moves first. Each move is one room move, sent with `say=` so
only the opponent is told:

    play R C a|d LETTERS   first tile laid at row R, column C; a across, d down;
                           the letters laid, stepping over tiles already there
    swap LETTERS
    pass
    resign

Every device replays the moves from the seed (`wl_replay`: xorshift32,
Fisher-Yates, tiles drawn from the end of the bag, a swap draws then
reshuffles), so the state is never rewritten. A move out of turn, from a
stranger or refused by the rules is skipped by every device alike.

**On the card**: `/var/wordloom/ID.txt` per room (the seed and players, then
`SEQ<TAB>FROM<TAB>MOVE` lines) lets the games list score every game without
a request each; a room whose last move is newer is read after the list
comes. While a game is open its new moves are asked for every 3 s, only when
no request is in flight. `/var/wordloom/me.txt` is the device's account
name, which the server never says: it is the room member who is not among
your friends (or, for a game you just started, not the friend you chose).

**Keys**: the games -- Enter open, n new (the friend picker), r read again.
A game -- letters lay a tile at the cursor (it moves on, over tiles already
there), Space across/down, Backspace takes one back, Esc takes all back (or
goes to the games), Enter weaves, Tab swap / pass / resign (pass and resign
are confirmed), r reads now when it is not your turn. Arrows move the cursor
plain (`CAPP_TEXT_ARROWS`), since only letters are typed.

**Not social media**: nothing is posted; a move notifies only the opponent.
Finished games stay in the list until the server ages the room out (90
idle days); leaving a room is not offered yet.

Tests: `apps/wordloom/test/test_wordloom.c` (rules, scoring by hand, the
wire, replay), `apps/wordloom/test/test_wordloom_paint.c` (the real app
against a fake friends-and-rooms server: alex and sam on blank cards play
a weave each, a swap, a pass and a resignation; every screen fully painted;
`WORDLOOM_DUMP=dir` writes them).
