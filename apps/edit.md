# Edit

_Moved from CLAUDE.md on 2026-10-27._

**Edit's preview is set in Atkinson Hyperlegible and word-wraps** (2026-09-25),
using `ui13`/`ui13b`, with code still in 6x8. It used to cut a long line at the
screen edge and never draw the rest. Rows break at the last space that fits, a
word too long for a row breaks inside, and list items and quotes hang under
their text. `test/test_edit_preview.c` checks it against a font of uneven
widths.
