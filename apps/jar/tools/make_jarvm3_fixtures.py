"""Write the JarScript 3 fixtures: compiled scripts, worlds and the traces the
Python machine (apps/jar/server/jarvm3.py) prints for them. apps/jar/test/test_jv3.c runs
the same worlds through apps/jar/jv3.h and needs the same traces, byte for
byte.

  python apps/jar/tools/make_jarvm3_fixtures.py          write apps/jar/test/fixtures/jv3_*
  python apps/jar/tools/make_jarvm3_fixtures.py --check  say whether they are current

Files, per fixture NAME:
  jv3_NAME.world        the world, one directive a line (below)
  jv3_NAME_SLOT.bin     each scripted item's bytes
  jv3_NAME.trace        what happens, a line each

The .world format (integers, raw 12.4 unless noted):
  seed S
  ticks N
  bodies N                              most physics bodies at once
  ledge X Y W H                         a solid box of the jar's (raw)
  sense K VALUE                         jar sense K reads VALUE (raw) for every slot;
                                        unlisted read 0, except hour, -16
  item SLOT X Y W H SCALE TRAITS_HEX NFRAMES BINFILE|- MEM0 .. MEM7
                                        W, H in px; '-' is an unscripted item
  event TICK SLOT EV ARG OTHER          queued before tick TICK (1-based) runs
Items are put and placed in the order listed. The host: ground is the soil,
112 px, everywhere; act, say, emit, log and fault only print.

The .trace format: during a tick, as they happen,
  say s=SLOT ticks=T "TEXT"
  emit s=SLOT k=KIND n=N
  act s=SLOT k=K a=ARGRAW
  log s=SLOT "TEXT"
  fault s=SLOT pc=PC
and after each tick, for each scripted slot in order,
  t=TICK s=SLOT x=X y=Y vx=VX vy=VY a=ANGLESTEP st=STATE fr=FRAME0 fc=FACING
    w=WAITTICKS h=HALTED f=FAULTS v=V0,..,V7          (one line)

`adapt_sandbox(src, frames)` turns a sandbox scenario script (tools/
jarsandbox/scenarios) into one the device takes: traits mapped to the 32
words, frames cut to 4, names that are now words renamed. The fixtures'
ported creatures were made with it and then edited by hand.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, ROOT)

from apps.jar.server import jarvm3 as J  # noqa: E402

OUT = os.path.join(ROOT, "apps", "jar", "test", "fixtures")

# ---- the sandbox's words, mapped onto the 32 traits ------------------------------

SANDBOX_TRAITS = dict(
    branch="wood", spider="bug", mushroom="plant", fungus="plant", dew="wet", water="wet",
    caterpillar="bug", crawler="climber", ladybird="bug", beetle="bug", fern="plant",
    moth="flyer", castle="stone", furniture="stone", moss="plant", chest="wood",
    ornament="shiny", snail="shell", slow="sleepy", ladybug="bug", pillbug="bug",
    roller="shell", newt="critter", cricket="bug", hopper="critter", clover="plant",
    mug="nest", cave="nest", spoon="shiny", metal="shiny", sugar="sweet", mouse="prey",
    frog="hunter", jumper="critter", fly="bug", ant="bug", reed="plant", dish="wet",
    tadpole="swimmer", pad="plant", floating="wet", strider="bug", insect="bug",
    skater="swimmer", bloom="flower", dragonfly="flyer")


def adapt_sandbox(src, frames):
    """(source, frame names) the device's compiler takes."""
    def tr(w):
        return SANDBOX_TRAITS.get(w, w)

    keep = frames[:J.MAX_FRAMES]
    for i, f in enumerate(frames[J.MAX_FRAMES:]):
        to = keep[i % len(keep)]
        src = re.sub(r"\b(frame|play)(\s+)%s\b" % f, r"\g<1>\g<2>%s" % to, src)
        src = re.sub(r"^(anim\s+\w+\s*=.*)$",
                     lambda m: re.sub(r"\b%s\b" % f, to, m.group(1)), src, flags=re.M)
    names = re.findall(r"^\s*state\s+(\w+)", src, re.M)
    for line in re.findall(r"^var\s+(.*)$", src, re.M):
        names += [n.split("=")[0].strip() for n in line.split("#")[0].split(",")]
    for n in names:
        if n in J.RESERVED:                 # a frame of that name keeps it
            src = "\n".join(l if l.lstrip().startswith(("anim", "traits")) else
                            re.sub(r"(?<!frame )(?<!play )\b%s\b" % n, n + "_", l)
                            for l in src.split("\n"))

    def traits_line(m):
        words = [tr(w.strip()) for w in m.group(2).split("#")[0].split(",") if w.strip()]
        return m.group(1) + ", ".join(dict.fromkeys(words))
    src = re.sub(r"^(traits\s+)(.*)$", traits_line, src, flags=re.M)
    src = re.sub(r"\b(find|count)\((\w+)", lambda m: m.group(1) + "(" + tr(m.group(2)), src)
    src = re.sub(r"\bhas\(([^,()]+(?:\([^()]*\))?),\s*(\w+)\)",
                 lambda m: "has(%s, %s)" % (m.group(1), tr(m.group(2))), src)
    src = re.sub(r"\b(near|perch)\s+(\w+)", lambda m: m.group(1) + " " + tr(m.group(2)), src)
    return src, keep


# ---- the fixtures ---------------------------------------------------------------------

T = J.TRAITS


def tmask(*words):
    m = 0
    for w in words:
        m |= 1 << T[w]
    return m


ARITH = r"""
# Every operator and function, at its edges.
var a, b, c
state main:
  log 2000 + 100, -2000 - 100, 3.5 * -2.25, -7 / 2, 7 / -2, -7 % 3, 7 % -3, -7.5 % 2
  wait
  log 5 / 0, 5 % 0, -(-2048), 0.1 * 0.1, 2047 * 2, 1 / 3, -1 / 3, 2000 / 0.5
  wait
  log round(2.5), round(-2.5), round(2047.9), floor(-0.5), abs(-2048), sign(-0.2), clamp(5, 1, 3), clamp(0, 3, 1)
  wait
  log min(-1, 2), max(-1, 2), sqrt(2), sqrt(-1), sqrt(2047), sin(30), cos(60), sin(-90)
  wait
  log cos(200), sin(720.5), 0.3, 1 == 1, 1 != 1, 2 < 1, 2 <= 2, 3 > 2
  wait
  log 3 >= 4, 1 and 0, 1 or 0, not 0, not 5, -0.0625, 1.0625 * 1.0625, 0.03
  wait
  log random(1, 6), random(6, 1), random(0.5, 1), random(-3, -1), chance(50), chance(100), pick(1, 2, 3), pick(7)
  wait
  a = 10
  a += 5
  a -= 0.5
  a *= 2
  a /= 4
  b = 0
  repeat 2.5
    b += 1
  end
  repeat -1
    b += 100
  end
  c = 0
  while c < 5
    c += 1.5
  end
  log "a b c", a, b, c
  say "v=" + 1.5 + "," + -0.0625 + "!" 1s
  wait
  say "a long piece of text: " + 1234.5 + "!" 0.1
  say 1 + 2 + "x" + 1 + 2
  log (0).x, (99).y, me.id, (me).w, soil, jarw, jarh, hour
  wait
  log facing, state, frame, statetime, time, me, other, impact
  log me.vx, me.vy, me.h, me.speed, (2).x, (2).h, (2).speed, (2).id
  log angleto(me), dist(me), dist(0), dist(x, y - 8), find(food), count(food), has(me, critter), has(0, critter)
  angle = -90
  log angle
  angle = 725
  log angle, spin
end
"""

STATES = r"""
# States, go from bodies and handlers, every timers, wait until, return.
var n, m, t, e
state a:
  n += 1
  go b
end
state b:
  m += 1
  if m % 6 != 0 then go a end
  log "b", n, m, statetime
  wait 0.25
  return
  log "after return", statetime
  go c
end
state c:
  on every 0.3s:
    e += 1
    log "c every", e
  end
  log "c", statetime
  wait until statetime >= 1
  log "c done", statetime, time
  go a
end
state d:
  on tick: t += 1 end
  log "d"
  wait 0.1s
  wait 300ms
  wait
  go c
end
every 0.5s:
  log "any every", n, m
end
on poke: go d end
on signal back: go b end
on gift: go c end
"""

MOVE_OWNER = r"""
traits roost, wood
perch 4, 2
perch 12, 2
var k
state still:
  wait 6
  move to 150, 112 speed 20
  log "owner moved", x
end
on land: log "owner landed" end
"""

MOVE_BIRD = r"""
traits bird, flyer
var k
state s:
  move to 60, 80 speed 40 arc 10
  log "at", x, y, facing
  move to perch roost speed 80 arc -5
  log "perch", onperch, perched, x, y
  hop
  hop 10 3
  log "hopped", onperch, x, y
  wait 0.5
  wait 7
  log "rode", x, y
  move to ground speed 120
  log "ground", x, y, grounded
  walk 300 speed 50
  log "glass", blocked, x
  walk -20
  log "walked", blocked, x, facing
  move to perch speed 100
  log "rim", onperch, perched, x, y
  hop 20 6
  log "rim hop", x, y
  move to find(roost) speed 30 arc -4
  face toward find(roost)
  log "item", x, y, facing
  face -3
  face 0
  move to 300, -50
  log "clamped", x, y
  move to 0
  log "nobody", x, y
end
on land: log "land", other, onperch end
"""

PHYS_BALL = r"""
traits stone
body 6
var k
state fall:
  physics on
  roll on
  bounce 70
  friction 30
  vx = 50
  vy = -30
  wait until grounded and speed < 1
  log "rest", x, y, angle, physics
  physics off
  gravity 80
  vx = -40
  log "off", vx, speed
  wait 1
  physics on
  vx = -200
  wait 2
  log "again", x, y, vx, vy
end
on hit: log "hit", impact, x, y end
on bump: log "bump", other, impact end
"""

PHYS_BOX = r"""
traits wood
solid 0, 6, 16, 10
state s:
  wait 1000
end
on bump: log "box bumped", other, impact end
"""

PHYS_LATE = r"""
traits stone
state s:
  wait 0.5
  physics on
  log "cap", physics
  spin = 90
  physics off
  spin = 90
  wait 1
  log "spun", angle
  spin = -400
  wait 0.5
  log "spun back", angle
end
"""

SIG_SENDER = r"""
traits noisy
var n
state s:
  loop
    wait 1
    n += 1
    signal ping
    signal pong to find(bug)
    signal self to me
    signal nobody to 0
  end
end
on signal self: log "self", other end
on signal back: log "sender got back", other end
"""

SIG_LISTENER = r"""
traits prey
var p, q
state s:
  wait 1000
end
on signal ping:
  p += 1
  log "ping", other, p
  signal back to other
end
on near bug within 20: log "near bug", other, dist(other) end
on near noisy: log "near noisy", other end
"""

SIG_WALKER = r"""
traits bug
var r
state s:
  loop
    walk 60 speed 40
    walk -60 speed 40
  end
end
on signal pong: r += 1; log "pong", other, r end
on signal ping: log "walker ping" end
"""

BUDGET_SMALL = r"""
var n, m
state s:
  loop
    n += 1
  end
end
on poke:
  while true
    m += 1
  end
end
on tick: m = m end
"""

BUDGET_FULL = r"""
var n
state s:
  loop
    n += 1
  end
end
state pad:
""" + "\n".join("  n = n + %d * 2 - 1" % i for i in range(1, 70)) + "\nend\n"

NEAR2 = r"""
# Two near handlers on one trait at different radii: each hears only the
# arrivals its own radius saw (D3). And angleto, every half second.
traits critter
var close, far
state watch:
  on near bug within 22:
    close += 1
    log "close", other, dist(other), close
  end
  loop
    log "aim", angleto(2), angleto(3), angleto(4)
    wait 0.5
  end
end
on near bug within 48:
  far += 1
  log "far", other, dist(other), far
end
"""

NEAR2_BUG = r"""
traits bug
state s:
  loop
    walk 100 speed 40
    walk -100 speed 40
  end
end
"""

BURST = r"""
# 25 steps a poke and 50 a tick: two pokes run, the rest cannot start and
# are dropped -- never a fault (D4).
var m, n
state s:
  n += 1
  wait 100
end
on poke:
  m += 1
  m += 1
  m += 1
  m += 1
  m += 1
  m += 1
end
"""

FAULT = r"""
var n
state s:
  n += 1
  wait 0.2
  n = 1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + (9 + (10 + (11 + (12 + (13 + (14 + (15 + (16 + 17)))))))))))))))
  log "never"
end
on poke:
  n = 1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + (9 + (10 + (11 + (12 + (13 + (14 + (15 + (16 + 17)))))))))))))))
end
on signal x: log "still running", n end
"""

ANIM = r"""
var k
anim walk = 1 2 3 2 @ 6
anim blink = 4 1 @ 10
anim slow = 2 3 @ 1
state s:
  play walk
  wait 0.7
  log "walk", frame
  play walk
  wait 0.3
  log "kept", frame
  play blink once
  log "blinked", frame
  frame 2
  log "still", frame
  k = 3.4
  frame k
  log "framee", frame
  k = 9
  frame k
  log "out of range", frame
  play walk loop
  wait 0.2
  stop
  wait 0.5
  log "stopped", frame
  play slow
  wait 3
  log "slow", frame
end
on poke: play blink once; log "poke anim", frame end
"""

EVENTS_SRC = r"""
var n
state s:
  log "senses", hour, daytime, weather, days, berries, fed, world, zone
  log "more", gift, playing, stuck, sulking
  wait 1000
end
on time night: log "night"; glow on end
on time: log "time", other; glow toggle end
on weather rainy: log "rain"; sound splash end
on weather: log "weather" end
on chime 4: log "chime 4"; boost 20 end
on chime: log "chime 15"; boost end
on world ants: log "ants"; eat end
on world: log "world any" end
on berry: throw; drop end
on new: log "new", other; nudge end
on gift: quake end
on shipped: fly; home end
on jam fixed: log "fixed"; sound pop end
on jam: log "jam any" end
on poke: n += 1; log "poke", n; sound tune end
on land: log "land" end
"""

# ---- ported creatures (apps/jar/tools/jarsandbox/scenarios/mug.js, adapted) ----------

MUG = r"""
# Mug. Tipped over on its side: a top to stand on, a dark mouth
# to hide in. Poke it and it rocks; whoever is inside bolts.
traits nest, stone
layer 1
solid 3, 11, 25, 21    # the whole body: others stand on its top
perch 7, 11            # three spots along the top
perch 14, 11
perch 21, 11
var k

state still:
  angle = 0
  frame mug
end

state rock:            # rolls a little side to side, settling
  say "clonk!" 0.8s
  emit dust 3
  k = 7
  repeat 4
    angle = k
    wait 0.12
    angle = -k * 2 / 3
    wait 0.12
    k = k * 2 / 3
  end
  angle = 0
  go still
end

on poke:
  signal startle       # the mouse bolts, anything on top jumps off
  go rock
end

on signal landed:      # something set down on top
  if has(other, flyer) then return end   # a fly weighs nothing
  shake 0.3s
  emit dust 1
  signal rattle        # heard inside
end

on bump:
  if impact > 20 then shake 0.25s; signal rattle end
end
"""

MOUSE = r"""
# Mouse. Lives in the mug: its eyes shine in the dark, it peeks out
# of the mouth, scurries off to nibble sugar, and runs home.
traits prey, critter
layer 2
var den, snack, tx

anim run   = run1 run2 @ 12
anim munch = peek sit peek sit @ 6

state inside:          # only two shining eyes in the dark
  den = find(nest)
  if den == 0 then wait 2s; go inside end
  move to den.x + 5, den.y - 5 speed 30
  face right
  frame peek
  wait random(3, 7)
  if chance(55) then go peeking end
  go out
end

state peeking:         # head out of the mouth, looking about
  face right
  frame peek
  move to den.x + 9, den.y - 2 speed 14
  repeat random(2, 4)
    wait random(6, 16) / 10
    if chance(15) then say "squeak" 0.8s end
  end
  if chance(60) then go out end
  move to den.x + 5, den.y - 5 speed 14
  go inside
end

state out:             # scurry out and forage
  face right
  play run
  move to den.x + 9, den.y - 2 speed 30
  move to den.x + 26, jarh speed 40 arc 2
  snack = find(sweet)
  if snack and chance(70) then
    tx = snack.x - 10
    if x > snack.x then tx = snack.x + 10 end
    move to tx, jarh speed 40 arc 2
    face toward snack
    play munch
    repeat random(2, 4)
      wait random(8, 14) / 10
      signal nibble to snack
    end
  else
    walk random(-20, 40) speed 30
    frame sit
    wait random(10, 25) / 10
  end
  go dash
end

state dash:            # home, quick, into the mouth
  play run
  move to den.x + 24, jarh speed 55 arc 2
  move to den.x + 9, den.y - 2 speed 30
  go inside
end

state bolt:            # startled out of the mug and away
  say "eek!" 1s
  face right
  play run
  move to den.x + 9, den.y - 2 speed 60
  move to den.x + random(45, 90), jarh speed 80 arc 4
  frame sit
  wait random(2, 4)
  go dash
end

on signal rattle:      # a thump on the roof
  if state == inside then go peeking end
end

on signal startle:
  if has(other, nest) and (state == inside or state == peeking) then go bolt
  elif state == out and dist(other) < 40 then go dash end
end

on near hunter within 20:
  if state == out then say "eep!" 0.8s; go dash end
end

on poke:
  say "squeak!" 0.8s
  if state == inside then go peeking
  elif state == out then go dash end
end
"""

FROG = r"""
# Frog. Sits on the mug, croaks, watches the fly, leaps about.
# A fly that comes too close gets the tongue (and always escapes).
traits hunter, critter
layer 2
var bug_, n, where

anim croak = puff sit puff sit @ 5
anim blink = blink sit @ 8

state sit:
  on near bug within 22:     # close enough: the tongue!
    bug_ = other
    go snap
  end
  frame sit
  n = random(3, 6)
  repeat n
    wait random(10, 24) / 10
    where = random(1, 10)
    if where <= 3 then
      play croak once
      emit note 1
    elif where <= 6 then
      play blink once
    else
      face -facing
    end
  end
  go leap
end

state leap:
  frame leap
  where = random(1, 10)
  if where <= 5 then
    move to perch nest speed 80 arc 20
  elif where <= 7 then
    move to perch plant speed 80 arc 22
  else
    move to ground speed 70 arc 18
  end
  frame sit
  if perched then signal landed to perched else emit dust 2 end
  go sit
end

state snap:
  face toward bug_
  frame puff
  signal snap to bug_
  wait 0.3s
  frame sit
  if chance(50) then say "blep" 0.8s end
  wait 1.2s
  go sit
end

on near flyer within 48:     # follows it with its eyes
  if state == sit then face toward other end
end

on poke:
  say "ribbit!" 1s
  go leap
end

on signal startle:           # the thing under it moved
  if perched == other then go leap end
end
"""

FLY = r"""
# Fly. Buzzes in loops, lands on sweet things and rubs its hands,
# zips off when the frog's tongue comes out.
traits bug, flyer
layer 3
var n, where

anim buzz = fly1 fly2 @ 20
anim rub  = sit rub sit rub @ 8

state buzz:
  play buzz
  repeat random(1, 3)        # a loop or two in the air
    move to x + random(-40, 40), y - random(-10, 25) speed 55 arc random(-12, 12)
  end
  where = random(1, 10)
  if where <= 4 then
    move to perch sweet speed 50 arc 10
  elif where <= 6 then
    move to perch plant speed 50 arc -10
  elif where <= 8 then
    move to perch nest speed 50 arc 8
  else
    move to perch rim speed 60 arc 12
  end
  go sit
end

state sit:
  frame sit
  if perched then signal landed to perched end
  n = random(3, 7)
  repeat n
    wait random(4, 12) / 10
    if chance(50) then play rub once else face -facing end
  end
  go buzz
end

state zip:                   # nearly eaten: off, fast
  say "bzz!" 0.8s
  play buzz
  move to random(20, 220), random(20, 70) speed 120 arc -15
  go buzz
end

on signal snap: go zip end

on signal startle:
  if perched == other then go buzz end
end

on poke:
  go zip
end
"""

# ---- apps/jar/tools/jarsandbox/scenarios/branch.js: the caterpillar falls onto the mushrooms

BRANCH = r"""
# Branch. A mossy fallen branch leaning across the jar, forked at the top.
traits wood, plant
layer 0
scale 2
perch 7, 18
perch 12, 13
perch 13, 1
perch 25, 5
var gust

anim rustle = rustle still rustle still @ 10

state idle:
  frame still
  loop
    wait random(6, 14)
    gust = random(1, 3)
    sway gust 1.8s
    if gust == 3 and chance(50) then emit leaf 1 end
  end
end

on signal landed:
  sway 2 1s
  play rustle once
  if chance(60) then emit leaf 1 end
end

on signal nibble:
  shake 0.25s
  if chance(40) then emit leaf 1 end
end

on poke:
  sway 6 2s
  shake 0.3s
  play rustle once
  emit leaf 3
  signal startle
end
"""

CATERPILLAR = r"""
# Caterpillar. Inches up the branch to nap in the fork, nibbles, and
# sometimes backs down again; loses its grip and falls when the branch is shaken.
traits bug, critter, climber
body 4
var b, c, n, dir, up

anim crawl = crawl1 crawl2 @ 3

state climb:
  b = find(wood)
  if b == 0 then go wander end
  c = b.x + b.y - 40.5
  if abs(c - x - y) > 3 then go toclimb end
  up = 1
  face right
  while x < b.x + 4
    frame crawl2
    wait random(4, 7) / 10
    frame crawl1
    move to x + 4, c - x - 4 speed 6
    wait random(2, 6) / 10
  end
  go nap
end

state nap:
  up = 1
  move to b.x + 7, b.y - 48 speed 5
  face right
  frame nap
  say "zzz" 2s
  wait random(4, 9)
  go awake
end

state awake:
  up = 1
  frame crawl1
  repeat random(2, 4)
    signal nibble to b
    shake 0.2s
    wait random(5, 12) / 10
  end
  if chance(40) then go descend end
  go nap
end

state descend:
  move to b.x + 4, c - b.x - 4 speed 5
  face right
  while x > b.x - 36
    frame crawl2
    move to x - 4, c - x + 4 speed 7
    wait random(3, 7) / 10
  end
  go wander
end

state wander:
  up = 0
  if y < soil - 1 then move to x - 4, soil speed 8 end
  repeat random(1, 3)
    dir = pick(left, right)
    play crawl
    walk dir * random(6, 16) speed 4
    frame crawl1
    wait random(8, 20) / 10
  end
  go toclimb
end

state toclimb:
  up = 0
  b = find(wood)
  if b == 0 then go wander end
  play crawl
  walk b.x - 40 - x speed 5
  frame crawl1
  if blocked then
    move to x + sign(b.x - 40 - x) * 20, soil speed 8 arc 10
    go toclimb
  end
  go climb
end

state freeze:
  frame twig
  wait 1
  wait until find(bird, 44) == 0
  wait random(6, 14) / 10
  if x > b.x + 4 then go awake end
  go climb
end

state fall:
  up = 0
  frame nap
  physics on
  roll on
  bounce 35
  friction 90
  vx = random(-20, 30)
  vy = -15
  wait until grounded and speed < 3
  wait 0.6
  physics off
  roll off
  angle = 0
  vx = 0
  vy = 0
  y = y - 4
  frame crawl1
  wait 0.8
  go toclimb
end

on near bird within 36:
  if up == 1 and state != freeze then go freeze end
end

on signal startle:
  if up == 1 and has(other, wood) then go fall end
end

on hit: if impact > 40 then emit dust 2 end end

on poke:
  say "!" 0.8s
  if up == 1 then go freeze end
  go wander
end
"""

MUSHROOMS = r"""
# Mushrooms. Whatever drops onto the big cap squashes it.
traits plant, soft
scale 2
solid 1, 3, 11, 4
perch 6, 2

anim squish = squash still @ 8

state still:
  frame still
  loop
    wait random(10, 24)
    if chance(40) then emit dust 1 end
  end
end

state puff:
  play squish once
  emit dust 5
  signal puff
  wait 1
  go still
end

on bump:
  if impact > 12 then go puff end
end

on poke:
  shake 0.3s
  go puff
end
"""


def P(x):
    """px to raw"""
    return int(round(x * 16))


def item(slot, x, y, src=None, w=16, h=16, scale=1, traits=0, frames=1, mem=None, nostack=False):
    if isinstance(frames, int):
        names, n = None, frames
    else:
        names, n = list(frames), len(frames)
    return {"slot": slot, "x": P(x), "y": P(y), "w": w, "h": h, "scale": scale,
            "traits": traits, "nframes": n, "names": names, "src": src,
            "mem": mem or [0] * 8, "nostack": nostack}


FIXTURES = [
    {"name": "arith", "seed": 12345, "ticks": 12,
     "items": [item(0, 100, 112, ARITH, traits=tmask("critter", "food"), frames=2),
               item(1, 130, 112, None, w=16, h=32, scale=2, traits=tmask("food"))]},
    {"name": "states", "seed": 7, "ticks": 140,
     "items": [item(0, 100, 112, STATES)],
     "events": [(40, 0, "poke", 0, 0), (60, 0, "gift", 0, 0), (60, 0, "signal", "back", 0),
                (100, 0, "poke", 0, 0)]},
    {"name": "move", "seed": 99, "ticks": 520,
     "items": [item(0, 100, 112, MOVE_OWNER, w=16, h=16, traits=tmask("roost", "wood")),
               item(1, 30, 112, MOVE_BIRD, traits=tmask("bird", "flyer"), frames=2)]},
    {"name": "physics", "seed": 5, "ticks": 300, "bodies": 1,
     "ledges": [(P(150), P(80), P(40), P(4))],
     "items": [item(0, 120, 60, PHYS_BALL, traits=tmask("stone")),
               item(1, 200, 112, PHYS_BOX, traits=tmask("wood")),
               item(2, 40, 112, PHYS_LATE, traits=tmask("stone"))]},
    {"name": "signals", "seed": 3, "ticks": 140,
     "items": [item(0, 60, 112, SIG_SENDER, traits=tmask("noisy")),
               item(1, 120, 112, SIG_LISTENER, traits=tmask("prey")),
               item(2, 90, 112, SIG_WALKER, traits=tmask("bug")),
               item(3, 128, 112, None, traits=tmask("bug"))]},
    {"name": "budget", "seed": 1, "ticks": 30,
     "items": [item(0, 60, 112, BUDGET_SMALL), item(1, 120, 112, BUDGET_FULL)],
     "events": [(5, 0, "poke", 0, 0), (6, 0, "poke", 0, 0)]},
    {"name": "near2", "seed": 4, "ticks": 220,
     "items": [item(0, 120, 112, NEAR2, traits=tmask("critter")),
               item(1, 70, 112, NEAR2_BUG, traits=tmask("bug")),
               item(2, 60, 112, None, traits=tmask("stone")),
               item(3, 120, 40, None, traits=tmask("stone"))]},
    {"name": "burst", "seed": 1, "ticks": 8,
     "items": [item(0, 60, 112, BURST)],
     "events": [(t, 0, "poke", 0, 0) for t in (2, 3, 4) for _ in range(8 if t < 4 else 4)]},
    {"name": "fault", "seed": 1, "ticks": 40,
     "items": [item(0, 60, 112, FAULT, nostack=True)],
     "events": [(3 + k, 0, "poke", 0, 0) for k in range(5)] + [
         (20, 0, "signal", "x", 0), (25, 0, "poke", 0, 0), (26, 0, "poke", 0, 0),
         (30, 0, "signal", "x", 0)]},
    {"name": "anim", "seed": 1, "ticks": 140,
     "items": [item(0, 60, 112, ANIM, frames=4)],
     "events": [(90, 0, "poke", 0, 0)]},
    {"name": "events", "seed": 1, "ticks": 6,
     "senses": [("hour", 14 * 16), ("daytime", 2 * 16), ("weather", 4 * 16), ("days", 3 * 16),
                ("berries", 5 * 16), ("world", 16), ("zone", 2 * 16), ("playing", 16)],
     "items": [item(0, 60, 112, EVENTS_SRC, mem=[16, 0, 0, 0, 0, 0, 0, 0])],
     "events": [(1, 0, "time", 4, 0), (1, 0, "time", 1, 0), (1, 0, "weather", 4, 0),
                (2, 0, "weather", 2, 0), (2, 0, "chime", 8, 0), (2, 0, "chime", 15, 0),
                (2, 0, "chime", 7, 0), (3, 0, "world", 1, 0), (3, 0, "world", 3, 0),
                (3, 0, "berry", 0, 0), (3, 0, "new", 0, 1), (4, 0, "gift", 0, 0),
                (4, 0, "shipped", 0, 0), (4, 0, "jam", 2, 0), (4, 0, "jam", 1, 0),
                (5, 0, "poke", 0, 0), (5, 0, "land", 0, 0), (5, 0, "poke", 0, 0)]},
    {"name": "mug", "seed": 2026, "ticks": 600,
     "items": [item(0, 172, 112, MUG, w=32, h=32, traits=tmask("nest", "stone"), frames=["mug"]),
               item(1, 60, 112, MOUSE, traits=tmask("prey", "critter"),
                    frames=["run1", "run2", "sit", "peek"]),
               item(2, 40, 50, FROG, traits=tmask("hunter", "critter"),
                    frames=["sit", "blink", "puff", "leap"]),
               item(3, 140, 112, FLY, traits=tmask("bug", "flyer"),
                    frames=["sit", "rub", "fly1", "fly2"]),
               item(4, 120, 112, None, traits=tmask("sweet", "food"))],
     "events": [(200, 0, "poke", 0, 0), (350, 2, "poke", 0, 0), (420, 1, "poke", 0, 0),
                (500, 3, "poke", 0, 0)]},
    {"name": "branch", "seed": 77, "ticks": 600,
     "items": [item(0, 92, 112, BRANCH, w=32, h=32, scale=2, traits=tmask("wood", "plant"),
                    frames=["still", "rustle"]),
               item(1, 72, 91.5, CATERPILLAR, traits=tmask("bug", "critter", "climber"),
                    frames=["crawl1", "crawl2", "nap", "twig"]),
               item(2, 134, 112, MUSHROOMS, scale=2, traits=tmask("plant", "soft"),
                    frames=["still", "squash"])],
     "events": [(260, 0, "poke", 0, 0), (480, 2, "poke", 0, 0), (520, 1, "poke", 0, 0)]},
]


# ---- running a world -------------------------------------------------------------------

def build(fx):
    """{slot: bytes} for a fixture's scripted items."""
    out = {}
    for it in fx["items"]:
        if it["src"] is not None:
            out[it["slot"]] = J.compile_script(it["src"], it["nframes"], it["names"],
                                               check_stack=not it["nostack"])
    return out


def world_text(fx, bins):
    name = fx["name"]
    lines = ["seed %d" % fx["seed"], "ticks %d" % fx["ticks"], "bodies %d" % fx.get("bodies", 4)]
    for l in fx.get("ledges", []):
        lines.append("ledge %d %d %d %d" % l)
    for k, v in fx.get("senses", []):
        lines.append("sense %d %d" % (J.SENSES[k], v))
    for it in fx["items"]:
        f = "jv3_%s_%d.bin" % (name, it["slot"]) if it["slot"] in bins else "-"
        lines.append("item %d %d %d %d %d %d %08x %d %s %s" % (
            it["slot"], it["x"], it["y"], it["w"], it["h"], it["scale"], it["traits"],
            it["nframes"], f, " ".join(str(m) for m in it["mem"])))
    for tick, slot, ev, arg, other in fx.get("events", []):
        if ev == "signal" and isinstance(arg, str):
            arg = J.fnv_signal(arg)
        lines.append("event %d %d %d %d %d" % (tick, slot, J.EVENTS[ev], arg, other))
    return "\n".join(lines) + "\n"


def run_world(text, read_bin):
    """The trace a world gives. `read_bin(name)` returns a .bin's bytes."""
    out = []
    seed = ticks = 0
    bodies = 4
    ledges, senses, items, events = [], {}, [], {}
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "seed":
            seed = int(w[1])
        elif w[0] == "ticks":
            ticks = int(w[1])
        elif w[0] == "bodies":
            bodies = int(w[1])
        elif w[0] == "ledge":
            ledges.append(tuple(int(v) for v in w[1:5]))
        elif w[0] == "sense":
            senses[int(w[1])] = int(w[2])
        elif w[0] == "item":
            items.append(w[1:])
        elif w[0] == "event":
            t, slot, ev, arg, other = (int(v) for v in w[1:6])
            events.setdefault(t, []).append((slot, ev, arg, other))
        else:
            raise ValueError("a world line I do not know: %r" % line)

    def sense(slot, k):
        if k in senses:
            return senses[k]
        return -16 if J.SENSE_NAME[k] == "hour" else 0

    jar = J.Jar(
        seed=seed, sense=sense, max_bodies=bodies, ledges=ledges,
        say=lambda s, text, t: out.append('say s=%d ticks=%d "%s"' % (s, t, text)),
        emit=lambda s, k, n: out.append("emit s=%d k=%d n=%d" % (s, k, n)),
        act=lambda s, k, a: out.append("act s=%d k=%d a=%d" % (s, k, a)),
        log=lambda s, text: out.append('log s=%d "%s"' % (s, text)),
        fault=lambda s, pc: out.append("fault s=%d pc=%d" % (s, pc)))
    for w in items:
        slot, x, y, iw, ih, sc = (int(v) for v in w[:6])
        traits = int(w[6], 16)
        nframes = int(w[7])
        script = None if w[8] == "-" else read_bin(w[8])
        mem = [int(v) for v in w[9:17]]
        jar.put(slot, x, y, iw, ih, sc, traits, nframes, script, mem)
    for w in items:
        jar.place(int(w[0]))
    for t in range(1, ticks + 1):
        for slot, ev, arg, other in events.get(t, []):
            jar.event(slot, ev, arg, other)
        jar.tick()
        for it in jar.items:
            if it and it.s:
                out.append("t=%d s=%d x=%d y=%d vx=%d vy=%d a=%d st=%d fr=%d fc=%d w=%d h=%d f=%d v=%s"
                           % (t, it.slot, it.x, it.y, it.vx, it.vy, it.angle, it.state, it.frame,
                              it.facing, it.wait, 1 if it.halted else 0, it.faults,
                              ",".join(str(m) for m in it.mem)))
    return "\n".join(out) + "\n"


def generate():
    """{file name: bytes} for every fixture."""
    files = {}
    for fx in FIXTURES:
        bins = build(fx)
        for slot, b in bins.items():
            files["jv3_%s_%d.bin" % (fx["name"], slot)] = b
        wt = world_text(fx, bins)
        files["jv3_%s.world" % fx["name"]] = wt.encode("ascii")
        trace = run_world(wt, lambda n: files[n])
        files["jv3_%s.trace" % fx["name"]] = trace.encode("ascii")
    return files


def main(argv):
    files = generate()
    if "--check" in argv:
        stale = []
        for n, b in files.items():
            p = os.path.join(OUT, n)
            if not os.path.exists(p) or open(p, "rb").read() != b:
                stale.append(n)
        print("stale: " + ", ".join(stale) if stale else "fixtures are current")
        return 1 if stale else 0
    os.makedirs(OUT, exist_ok=True)
    for n, b in sorted(files.items()):
        with open(os.path.join(OUT, n), "wb") as f:
            f.write(b)
    for n, b in sorted(files.items()):
        if n.endswith(".bin"):
            print("%-24s %5d bytes" % (n, len(b)))
    print("%d files in %s" % (len(files), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
