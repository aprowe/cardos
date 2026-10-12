# Jarscript 3 (a draft)

<!-- The one reference for the language in jarscript3.js. The sandbox's
     Language tab renders this file, and tools/promptlab hands it to Claude
     when a behaviour is written in Jarscript 3. Keep it to what the compiler
     and the machine in index.html actually do. -->

### Shape of a script

A script is a few **declarations**, then **states** and **handlers**. The first state is where the item starts. A state's body runs as a little thread: it can take time (`wait`, `move`, `walk`, `hop`, `play … once`) and is restarted by `go`. A handler (`on poke:`) runs to the end inside one tick and may `go` to a state. Handlers inside a state only listen while in that state and win over top-level ones. Blocks close with `end`; `:` and `then` are optional sugar; `;` separates statements on one line; `#` comments.

```
traits bird, flyer
var looks

anim flap = flap1 flap2 @ 10

state sit:
  play settle once
  wait random(6, 18) / 10
  go fly
end

state fly:
  play flap
  move to perch tree speed 75 arc -22
  go sit
end

on poke: say "tweet!"; go fly end
```

### Units

Positions in jar pixels (240 × 135, y grows downward, soil surface at y 112). `x` is the item's centre, `y` its feet. Velocities in px/s, gravity and friction in px/s², angles in degrees (stored as 256 steps), times in seconds: `2s`, `300ms`, or a bare number. A tick is 50 ms. Every number is kept to 1/16 — what a 16-bit value with 4 fraction bits can hold.

### Declarations

| form | meaning |
|---|---|
| traits a, b | what this item is. Others find it by trait, never by name. |
| var a, b = 3 | up to 8 numbers kept while the item lives. |
| anim NAME = f1 f2 … @ FPS | a named animation from frame names (or numbers, from 1). |
| perch X, Y | a point (in sprite pixels) where another item's feet may land. Others reach it with `move to perch`. |
| solid X, Y, W, H | a box others' physics bodies collide with and can stand on. |
| body R | radius of this item's physics circle (default: half its size). |
| layer N | draw order; lower is further back (default 1). |
| scale 1\|2 | draw the sprite at 2× (a 16×16 that fills 32×32). |
| state NAME: … end | a mode. Its body runs on entry; may contain handlers. |
| on EVENT: … end | a handler; see Events. |

### Events

| handler | when |
|---|---|
| on poke: | someone clicked the item. |
| on tick: | every tick (20 a second) — keep it short. |
| on every 2s: | on a timer (also `every 2s:`). |
| on hit: | its physics body struck the soil, the glass or the lid; `impact` is the speed. |
| on bump: | a body met a solid box: both sides hear it; `other` and `impact` are set. |
| on land: | a `move` finished; if it was a perch, `perched` says whose. |
| on signal NAME: | another item sent `signal NAME`; `other` is the sender. |
| on near TRAIT [within R]: | an item with that trait came within R px (default 24). Fires once per arrival. |

### Statements

| form | does |
|---|---|
| name = expr  (+= -= *= /=) | set a variable or a property: x y vx vy angle spin gravity bounce friction. |
| bounce 55 · friction 40 · gravity 160 · spin 90 | shorthand for setting those four. |
| if c then … elif c then … else … end | choose. |
| repeat N … end · while c … end · loop … end | loops. A loop that never waits runs out of budget and continues next tick. |
| wait T · wait · wait until c | [body only] pause T seconds, one tick, or until c holds (checked each tick). |
| go STATE | switch state; the new state's body starts from the top. |
| frame NAME\|N | show a still frame (stops the anim). |
| play ANIM [once] · stop | loop an anim, or play it once (a body waits for it to finish). |
| physics on\|off · roll on\|off | gravity, bounces, friction and collisions; roll turns distance into rotation. |
| move to X, Y \| perch [TRAIT] \| ground \| ITEM  [speed S] [arc A] | [body only] travel along a curve. `arc` lifts the middle by A px; negative dips it — a swoop. Ends with `on land`. No free perch: lands on the ground. |
| walk DX [speed S] | [body only] crawl DX px along the ground; stops at glass, solids or an edge and sets `blocked`. |
| hop [DX [H]] | [body only] a small jump (in place on a perch). |
| face DIR · face toward ITEM | flip the sprite: any negative is left, positive right. |
| say TEXT [T] · log A, B… | a speech bubble; a console line. |
| emit KIND [N] | particles: leaf, dust, feather, spark, heart, note. Leaves take the item's own greens. |
| sway AMP [T] · shake [T] | a draw-time bend (top moves most — rustling) or a jitter. |
| signal NAME [to ITEM] | wake `on signal NAME` in one item, or every other item. |
| return | leave the handler. |

### Expressions

`+ - * / %`, `== != < <= > >=`, `and or not`, parentheses, `"text"` (joins with +, for `say`). `left` = −1, `right` = 1, `true`, `false`, `none` = 0. A state's name is its number, so `state == crawl` works. Items are numbers too (their id; 0 is nobody), with fields `.x .y .vx .vy .w .h .id .speed`.

| function | gives |
|---|---|
| random(a, b) | a whole number a..b (a decimal if either is one). |
| chance(p) · pick(a, b, …) | 1 with p% odds · one of the values. |
| abs min max sign clamp round floor sqrt | the usual. |
| sin(deg) · cos(deg) | from a table on device. |
| find(TRAIT [, R]) · count(TRAIT) · has(ITEM, TRAIT) | nearest item with a trait (0 if none) · how many · does it have it. |
| dist(ITEM) · dist(x, y) · angleto(ITEM) | distance and direction from me. |

### Senses

| name | is |
|---|---|
| x y vx vy angle spin | my position, velocity, rotation. |
| gravity bounce friction physics | my physics settings. |
| speed · grounded · blocked | \|velocity\| · resting on soil or a solid · the last walk hit something. |
| onperch · perched · facing | on any perch · the item I'm perched on (0 for none or the jar rim) · 1 or −1. |
| other · impact · me | the event's other item · how hard it hit · my own id. |
| state · statetime · frame · time | current state, seconds in it, frame number, seconds since reset. |
| soil · jarw · jarh | 112, 240, 135. |

### Limits the sandbox enforces

- **100 steps per item per tick**, shared by handlers and the body. A handler that runs out is an error; a body that runs out is paused and picks up next tick. The meter on each item shows its peak.
- 8 variables, a 16-value stack, 8 frames, 16×16 to 32×32 sprites, 8 colours with slot 0 see-through.
- Errors name the line. A script that fails to compile leaves the last good version running.
