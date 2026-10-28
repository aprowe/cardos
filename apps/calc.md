# Calc

_Moved from CLAUDE.md on 2026-10-27._

**Calc is a graphing calculator** (`apps/calc.c`, 2026-09-24). A roll of
sums (`ans`, `a=5`, implicit multiplication, sin..round, `n!`), and any
line that is `y=...` or mentions x becomes one of four graphs: Tab shows
them, arrows pan, + - zoom, f fits, t traces. fn-p prints the view on
screen, the graph as a `%%` picture. Single-precision float, shown to seven
digits, because that is the hardware -- and an app links no libgcc, so
Calc carries its own `__divsf3` (`a / b` compiles to a call) and its own
libm. Struct assignment compiles to `memcpy`, which is not there either;
use `api->mem_cpy`. `test/test_calc.c` checks the maths against the
host's libm.
