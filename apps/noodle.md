# Noodle

_Moved from CLAUDE.md on 2026-10-27._

Noodle
(`apps/noodle.c`, Games) is the first user. It is the car-lot tube man: blow
on the mic and he inflates and flails, stop and he folds over. He is drawn in
15-row strips into a buffer and blitted, redrawing only where he moved.
`NOODLE_DUMP=dir build\cardos_tests.exe` writes frames to look at, which is
how the physics was tuned. Apps are not told when they are closed, so it
records in 8 s pieces restarted from tick: leaving the app leaves the mic on
for at most that long.
