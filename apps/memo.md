# Memo

_Moved from CLAUDE.md on 2026-10-27._

`apps/memo.c` records into `/home/memos` (named by
the clock, `0920-1142.wav`, or by count when there is no clock), lists with
lengths, plays back with a bar, deletes after asking. The strip with the
meter is drawn in place and repainted only when it would look different, at
most fifteen times a second -- clearing and redrawing it every tick was a
visible flicker.
