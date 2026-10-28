# Todo

_Moved from CLAUDE.md on 2026-10-27._

**A task is claimed for a day by its due date** (2026-10-12). The list is
a running list; the agenda is what was claimed. `c` in Todo claims the
highlighted task for today (for tomorrow after 6 pm) or lets it go, `+`/`-`
move its date a day, `w` takes one typed (today, tom, fri, +3, 10/15). The
agenda is every open task due today *or earlier*, from every list, so a
claim not finished is still there the next day: the top of `o`, and
`todo today LIST` -- Today's To do section, which falls back to LIST's open
tasks when nothing is claimed (and on tomorrow's page is asked `tomorrow`).
Dates are Google's, so one set on a phone is a claim too. The server sends
them only when asked (`/todo/tasks?...&due=1`), because an older Todo
splits a line into three fields; a push sends the date only when it changed
here (`due_dirty`), so a tick does not undo a date set elsewhere. Today
rewrites an old `todo show X` section to `todo today X` once.

**Todo syncs once, at open** (2026-09-18). One sweep: the lists, then the
list on screen (pending edits pushed first, then pulled), then every other
list straight into its own cache file. Then it stops -- `s` asks for another.
It used to re-pull the current list every ten minutes, which rearranged the
screen while you were reading it and still never fetched the lists you were
not looking at. A list off screen is absorbed into its file rather than into
the item array, because the array is what is on display. `o` is the overview:
every list's open tasks under its own heading, read from those files. `d`
toggles a pending delete rather than only setting it.

**Todo has lists** (2026-09-17). All of the account's task lists, one at a
time: only the current list's tasks are in memory, each list caches to
`/todo/<id>.cache`, and `/todo/lists` remembers the names and the choice so
Left/Right work offline. `l` is the picker. Switching mid-pull drops that
reply; switching mid-push waits, because a dropped push reply means the item
is sent twice. A new list can be made from the list picker (`n`).
