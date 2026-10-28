# Toggl

_Moved from CLAUDE.md on 2026-10-27._

**Toggl targets and the day's focus** (2026-10-01). A target is hours a
project should get, weekly from Monday or in total since a date
(`server/toggl.py`, set on the dashboard's Toggl card, in Toggl's `g` view
-- e hours, n new, d remove -- or `do toggl target NAME 10 week`); `toggl targets` answers `%bar NN label` lines, which Today
draws and the printer prints as filled bars (`printdoc.c`).

**Toggl** (2026-10-01). `apps/toggl.c` (Net) shows the running timer in
`num30`, starts one from a list of recent entries or a typed description,
and stops it; commands `status`, `start TEXT`, `stop`, `today` (a Today
section: `Toggl | toggl today`). `server/toggl.py` holds the API token --
pasted into the dashboard's Toggl card, kept in toggl.json -- and answers
in lines. Toggl has an hourly request quota, so the server caches projects
and recent entries and the app counts the timer itself, asking only on
open and after an action. Both of an app's HTTP calls now carry the
device's token to its own server (`own_bearer` in cardapi.c); before, only
`http_start` did, and a command using the blocking `http` was refused.
