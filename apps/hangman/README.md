# Hangman

_Moved from CLAUDE.md on 2026-10-27._

**Hangman** (2026-10-16, `apps/hangman/hangman.c`, Games), two players on
one device. The setter types a word or phrase (letters and spaces, at most
28, two lines of 14 without splitting a word) shown as dots -- Tab shows it
-- passes the device over, and the guesser types letters. Six misses finish
the figure (head, body, arms, legs) and the setter scores; every letter
found cuts him down to stand on the grass, arms up, and the guesser scores.
Then the other one sets the word. Esc in a round asks before giving up; `e`
on the score screen names the players. The score is the session's: nothing
is saved. Rules in `apps/hangman/hangman.h` (`apps/hangman/test/test_hangman.c`), the
screen in `apps/hangman/test/test_hangman_paint.c` (`HANGMAN_DUMP=dir`). Neither game
marks damage: every repaint is whole and composed off the panel.
Since 2026-10-21 the left panel is an old sepia photograph: the empty
gallows on the Fort Bravo film set (CC0, Wikimedia Commons; the crop is
`apps/hangman/tools/hangman/gallows.jpg`), toned, grained, vignetted and dithered to 15
tones, with a man in hat and coat drawn by `apps/hangman/tools/make_hangman.py` in
pieces (hanging, head fallen, standing with arms up) over it -- no real
person is in it. The script writes `apps/hangman/hangman_art.h` (7 KB of
constants); `--preview F.png` draws the poses. `paint_scene` composes a row
at a time through `api->pixels` (which `test/fakescreen.h` now has), and
the app's colours went sepia to match.
