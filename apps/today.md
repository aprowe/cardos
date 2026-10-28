# Today

_Moved from CLAUDE.md on 2026-10-27._

Today's `daily
focus` and `daily fact` come from `/daily` (`server/daily.py`): one
stateless Claude call a day, cached; a fallback line when Claude fails, not
cached, so the day is still written once Claude is back.

`apps/today.c` knows nothing about calendars: `/config/today.txt` lists
sections, `Heading | app command`, and each is answered by
`api->run_command` (API 33) -- `calendar today`, `todo show Chores`,
`habits today`, `stocks portfolio` to start. `t` turns the page to tomorrow: a
section asking `today` is asked `tomorrow` (Calendar and Habits have one),
and `do today tomorrow` prints it. A new section is a line; a new
kind of section is a command on some app. Answers written `[ ] task` print
as boxes (Todo's `show` and Habits' `today` answer that way). fn-p prints in
the print fonts, `l` picks the todo list, `e` edits the sections, and `do
today print` is "Carlos, print my day". Gathering is one command a tick, so
the screen says what it is waiting on.
