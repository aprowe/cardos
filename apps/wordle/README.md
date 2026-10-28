# Wordle

_Moved from CLAUDE.md on 2026-10-27._

**Wordle** (2026-10-16, `apps/wordle/wordle.c`, Games). Six guesses at a
five-letter word, the standard duplicate-letter rules (greens first, then a
letter is yellow only while the answer has one unmatched -- pinned case by
case in `apps/wordle/test/test_wordle.c`), a keyboard strip coloured by each letter's
best. The daily puzzle is the same word for everyone on a date: day 0 is
2026-01-01 (puzzle #1), the answer `(day * WD_DAILY_MUL + WD_DAILY_ADD) %
WD_NANSWERS`, every answer once before any repeats; with no clock it is
practice only. Practice (`n` when a game is over, or from Tab's stats) is a
random answer counted nowhere. Stats -- played, won, streak, best, the
guess distribution -- and today's guesses so far are /var/wordle/stats.txt
through safefile.h, so leaving the app keeps the board. The words are
`apps/wordle/tools/make_wordle.py` over `apps/wordle/tools/wordle/`: ENABLE2K's five-letter words
(public domain) less `blocked.txt` are the guesses (8586), and
`answers.txt`, 1029 common words written by hand, the answers. One sorted
list, two bytes a word (the first two letters pick a bucket, the last three
are 15 bits, the top bit marks an answer): 19 KB of constants, which the
icon scan reads into RAM -- do not grow it lightly. The rules are
`apps/wordle/wordle.h`; `apps/wordle/test/test_wordle_paint.c` plays it into a
framebuffer with the real fonts (`test/fakescreen.h`; `WORDLE_DUMP=dir`).

**Friends** (2026-10-27, `apps/wordle/wdfriends.h`). A finished daily is
published as your `pub/wordle` value of `day` -- the day, the result, each
guess as its colours (g y .), the streak; never the letters -- which only
friends can read (server/friends.py). Then Wordle reads everyone's
(`/kv/each`) and shows the friends who finished the same day under your
result ("Sam 4/6"); `f` shows each one's grid, `r` reads again, `s` sends
"Wordle #N 3/6" to Chat as you (`/share`, your Chat name from
/config/chat.txt). Nothing else is sent, and nothing is sent without a
network: Wordle does not ask for one. Tests: apps/wordle/test/
test_wordle_friends.c, and the flow end to end in test_wordle_paint.c.

