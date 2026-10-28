# Clock

_Moved from CLAUDE.md on 2026-10-27._

The Clock app (`apps/clock.c`) is the face and the editor; it re-reads the
file before every change and finds its alarm by content, never position,
so it cannot undo what the kernel changed. Commands: `do clock add 7:30
wake up`, `list`, `next`, `off 7:30`.
