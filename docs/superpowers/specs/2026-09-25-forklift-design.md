# Forklift -- a warehouse you program (design)

2026-09-25. A demake of *The Farmer Was Replaced*, as a warehouse. Asked for:
a coding sandbox to play on the device for fun; the keyboard types, slowly;
a language fit for that; functional, because that is what the player likes.
Decided in conversation: program a robot (not machines); text editing with
help (not a structure editor); one endless warehouse with unlocks (not
levels). "Just build it" -- this note records the design rather than gating
it.

## The game

A grid warehouse: supply bays down the left (one per item kind, endless),
a shipping dock on the right, racks in between once the warehouse grows.
Orders arrive one at a time ("red, red, blue"). A forklift bot runs the
player's program every step; shipping what the order wants earns credits,
shipping what it does not costs a little. Credits buy upgrades: forks that
carry more, a faster motor, a second and third bot running the same
program, a bigger warehouse with more kinds, bigger orders and better pay.
It saves to the card and never ends.

## The language

Tiny, pure, expression-only, ML-flavoured; a program is a few definitions,
and `bot` is the one the robot calls each step. It returns an action.

    # the bot calls this every step
    want = first order
    bot = empty holding
      ? (at (src want) ? take want : go (src want))
      : (at ship ? drop : go ship)

- A definition is `name params = expr`; lines that start with a space
  continue it, so a definition can be laid out on a 40-column screen.
- `c ? a : b`, `x -> e`, application by juxtaposition, `(x, y)` positions,
  `[a, b]` lists, the usual arithmetic and comparison, `&& || !`, `#`
  comments. No loops, no mutation; recursion with a fuel limit.
- World values: `holding order cap me pos ship`; `src k`, `at p`,
  `dist p`, `col p`, `row p`. Kinds: `red blue green yellow white`.
- Lists: `first rest len empty has count map filter fold`; `abs min max`.
- Actions: `go p` (one step on the shortest path), `take k`, `drop`,
  `wait`.
- Every step is a fresh evaluation: no state survives between steps
  except the world, which is what makes it functional and short.

Interpreted, not compiled: a tree walker over a fixed node pool, a cell
arena reset each step, a fuel limit (steps of evaluation) and a depth
limit (the C stack). Portable C in `apps/forklang.h`, host-tested.

## The editor

A small text editor inside the game, because the language is the game.
Tab completes names (built-ins, your definitions, kinds); brackets close
themselves; Enter keeps the indent and adds two after `? : -> (`; the
program is parsed as you type and the first error is in the status line
with its line. Escape runs the new program if it parses and keeps the old
one if not.

## Views

Warehouse (default): map, credits, the order, errors. `e` code, `s` shop,
`r` reference (every built-in with a line), space pause, `f` fast.
Code, Shop, Reference: Escape back.

## Files

`apps/forklang.h` (language), `apps/forklift.c` (game, editor, views),
`test/test_forklang.c`, `test/test_forklift.c`. Saved under
`/home/forklift`: `state.txt` (credits, levels, stats) and `bot.fl` (the
program), through `apps/safefile.h`.
