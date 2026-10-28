# Kart

_Moved from CLAUDE.md on 2026-10-27._

**Kart is SNES Mario Kart on the Cardputer** (2026-10-06, `apps/kart/kart.c`,
Games). Mode 7 floor -- each row below the horizon a line across a
128x128-cell map (4 bits a cell, painted at start from 256 waypoints in
`apps/kart/kart_data.h`, which `apps/kart/tools/make_kart.py` makes from a spline and
refuses if the road runs into itself) -- and billboard sprites for karts
(four sides and mirrors), item boxes, bananas, coins and trees. Six karts,
three laps, mushrooms and bananas, coins for top speed, CPU karts on the
racing line with a rubber band. Steered by tilt (roll from the pose at the
start; lean back to brake; `x` swaps the axis, `i` flips it, in
/config/kart.txt with the best time), arrows on the original. The game is
`apps/kart/kart.h`, host-tested in `apps/kart/test/test_kart.c`; `KART_DUMP=dir` writes
frames. Drawn in 15-row strips in paint, physics in 16 ms steps in tick.
