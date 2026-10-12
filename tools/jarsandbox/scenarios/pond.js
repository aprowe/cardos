/* Pond corner: a shallow dish of water and the small lives around it.
   Hand-built target for the item pipeline (see SCENARIOS in index.html):
   plain things whose bodies meet -- perches, a floating surface, ripples. */
SCENARIOS.pond = {
  title: "Pond corner",
  about: "A shallow clay dish of water in the middle of the jar, cattails behind it. A lily pad drifts on the water with a water lily seated on it; a frog uses the pad, the dish rim, the water and the soil as its places, and snaps at any insect that comes close (it always misses). A water strider skates the surface and bounces off the pad's edge, a tadpole wriggles below and hides under the pad, a dragonfly hovers and perches on the cattail tips and the lily, and a snail creeps over the soil, climbs the dish to sit on the rim, and tumbles off in its shell when startled. Nothing is a pair: a frog landing bobs the pad, which rings the water, which sends the strider skittering, whose wake sends the tadpole under the pad; a splash reaches all nine.",
  chain: [
    "Frog hops onto the lily pad -> signal landed to the pad -> pad dips and sways (frog and lily ride it down) and signals ripple -> dish plays its ring; strider within 30 px skitters away -> strider's wake signals ripple -> tadpole within 24 px darts under the pad",
    "Dragonfly lands on the water lily -> signal landed -> lily nods and, if shut, unfolds -> frog on the pad senses an insect within 30 px -> tongue snap, signal snap to it -> dragonfly darts off high (miss) -> frog blinks and re-settles, signalling landed -> pad bobs again -> ripple",
    "Strider skates near the frog -> frog snaps -> strider skitters, its wake signals ripple -> dish rings, tadpole hides, pad sways",
    "Frog dives into the water (random, or poke) -> only its eyes show -> signal splash -> dish rings, pad bobs, tadpole hides under the pad, strider skitters to the far side, lily shuts (and its shutting signals startle -> a dragonfly in it takes off), dragonfly on any perch darts, reeds sway, snail on the rim tucks in and tumbles off",
    "Snail on the rim tucks in -> physics on: the shell falls and rolls on the soil (roll on), dust on a hard landing -> rests -> peeks out -> crawls; a shell that rolls into the dish wall bumps it -> dish shakes and rings",
    "Snail crawls into the dish wall (walk blocked) -> climbs to the rim; frog hopping onto the rim beside it -> snail senses the frog -> tucks in and tumbles off",
    "Tadpole noses up for air -> signal ripple -> dish rings; a strider within 30 px skitters (wake ripple); pad within 30 px sways",
    "Frog lands on the dish rim -> signal landed to the dish -> dish rings",
    "Dragonfly lands on a cattail tip -> signal landed -> reeds rustle; the dragonfly sways with its tip",
    "Poke the reeds -> rustle, fluff, signal startle -> dragonfly perched on a tip takes off",
    "Poke the lily pad -> it bobs, signals startle and ripple -> frog on it leaps away, lily on it shuts (-> startle -> dragonfly in the lily takes off); dish rings, strider skitters",
    "Poke the dish -> it shakes, rings, signals splash -> everything at the pond startles as for a dive; the frog on the pad leaps",
    "Poke the lily -> it shuts (startle -> dragonfly leaves) and reopens later; poke the frog -> it dives in, or climbs out if already in; poke the strider -> it jumps and skitters; poke the tadpole -> ripple and it hides; poke the snail -> it tumbles; poke the dragonfly -> it darts"
  ],
  gaps: [
    "No ripple, bubble or splash particle. Ripples are dish frames, so every ring is centred on the dish whatever caused it; the tadpole's bubble is drawn in its own 'gulp' frame.",
    "No transparency, clipping or runtime layer. 'Under water' is faked: the tadpole is layer 1 so the pad (layer 2) hides it, and the swimming frog is a frame with only its eyes. That frog is still layer 3, so it draws over the pad; the dive picks a spot clear of the pad to hide this.",
    "Can't ask whether a perch is free, or pick one perch of an item. Leaping from the pad to 'perch pad' found none free and fell to a random ground spot, so the frog checks has(perched, pad) first. The snail climbs to computed rim coordinates (d.x +/- 27) instead of a perch, so it never crosses the dish to the far one.",
    "Can't read another item's state or frame (is the lily open? is the frog in the water?). The dragonfly lands on a shut bud, and the lily opens for it instead.",
    "Walking on a solid's top needs feet exactly on it. A hop cut short by 'go' left the strider in mid-air, where walk ignores the edges and it skated off into the air, so its states put y back on the water first. The pad's bob writes y by hand and restores a stored base.",
    "Signals reach everyone. Receivers filter by dist(other) (ripple) or perched == other (startle), and senders must avoid echoes: the pad answers ripple with a sway, never another ripple.",
    "'other' is replaced by the next event, so snap targets go in a var (prey). find() only gives the nearest item, never a random one with the trait."
  ],
  items: [

/* ---------------- Reeds: cattails behind the dish ---------------- */
{name: "Reeds", w: 16, h: 32,
 palette: ['#000000', '#26381a', '#4f8a32', '#86bf4e', '#e8dcb0', '#6b3d1f', '#94582a', '#3a6a28'],
 frames: [
  {name: 'still', rows: [
'........3.......',
'........3.......',
'........3.......',
'...3....3.......',
'...3...151......',
'...3...565......',
'...3...565......',
'..151..565......',
'..565..565......',
'..565..565......',
'..565..151......',
'..565...2.......',
'..565...2...3...',
'..151...2...3...',
'...2....2...3...',
'...2....2..151..',
'...2.7..2..565..',
'...2.7..2..565..',
'...2..7.2..565..',
'...2..7.2..151..',
'7..2..7.2...2...',
'.7.2..7.2...2..7',
'.7.2..72....2..7',
'.7.2...2....2.7.',
'..72...2...72.7.',
'..72...2...72.7.',
'..72...2...2.27.',
'..72...2...2.27.',
'...2...2..72.2..',
'...27..2..72.2..',
'..1271.21.1221..',
'.11111111111111.']},
  {name: 'rustle', rows: [
'.........3......',
'.........3......',
'........3.......',
'..3.....3.......',
'..3....151......',
'...3...565......',
'...3...565......',
'..151..565......',
'..565..565......',
'..565..565......',
'..565..151......',
'..565...2.......',
'..565...2....3..',
'..151...2....3..',
'...2....2...3...',
'...2....2..151..',
'...2.7..2..565..',
'...2.7..2..565..',
'...2..7.2..565..',
'...2..7.2..151..',
'7..2..7.2...2...',
'.7.2..7.2...2..7',
'.7.2..72....2..7',
'.7.2...2....2.7.',
'..72...2...72.7.',
'..72...2...72.7.',
'..72...2...2.27.',
'..72...2...2.27.',
'...2...2..72.2..',
'...27..2..72.2..',
'..1271.21.1221..',
'.11111111111111.']}],
 script: `# Reeds. Cattails at the back of the dish; insects perch on the tips.
traits reed, plant
layer 0                # behind the dish
scale 2                # drawn 32 x 64
perch 3, 3             # the three cattail tips
perch 8, 0
perch 12, 12

anim rustle = rustle still rustle still @ 8

state idle:
  frame still
  loop
    wait random(3, 8)
    sway pick(1, 1, 2) random(15, 30) / 10   # a breeze
  end
end

on signal landed:      # something settled on a tip
  sway 2 1s
  play rustle once
end

on signal splash:      # water slopped against the stems
  sway 1 0.8s
end

on poke:
  sway 4 1.6s
  play rustle once
  emit leaf 3          # cattail fluff
  signal startle       # whatever sits on a tip takes off
end
`,
 start: {x: 86, y: null}},

/* ---------------- Water dish: shallow clay, water on top ---------------- */
{name: "Water dish", w: 32, h: 16,
 palette: ['#000000', '#4a2a1e', '#a8583a', '#d08a5c', '#2f6f8a', '#4f9db5', '#b8e6ee', '#7a3a24'],
 frames: [
  {name: 'still', rows: [
'................................',
'................................',
'................................',
'................................',
'................................',
'................................',
'....111111111111111111111111....',
'..1133333333333333333333333311..',
'.134444444444444444444444444431.',
'13555555555555555555555555555531',
'13555566655555555555556655555531',
'13333333333333333333333333333331',
'12222222222222222222222222222221',
'.172222222222222222222222222271.',
'..1777777777777777777777777771..',
'....111111111111111111111111....']},
  {name: 'shine', rows: [
'................................',
'................................',
'................................',
'................................',
'................................',
'................................',
'....111111111111111111111111....',
'..1133333333333333333333333311..',
'.134444444444444444444444444431.',
'13555555555555555555555555555531',
'13555555555666555555555566555531',
'13333333333333333333333333333331',
'12222222222222222222222222222221',
'.172222222222222222222222222271.',
'..1777777777777777777777777771..',
'....111111111111111111111111....']},
  {name: 'ripple1', rows: [
'................................',
'................................',
'................................',
'................................',
'................................',
'................................',
'....111111111111111111111111....',
'..1133333333333333333333333311..',
'.134444444446666666644444444431.',
'13555555556555555555565555555531',
'13555555555566666666555555555531',
'13333333333333333333333333333331',
'12222222222222222222222222222221',
'.172222222222222222222222222271.',
'..1777777777777777777777777771..',
'....111111111111111111111111....']},
  {name: 'ripple2', rows: [
'................................',
'................................',
'................................',
'................................',
'................................',
'................................',
'....111111111111111111111111....',
'..1133333333333333333333333311..',
'.134444666444444444444666444431.',
'13555655555555555555555555655531',
'13555566665555555555556666555531',
'13333333333333333333333333333331',
'12222222222222222222222222222221',
'.172222222222222222222222222271.',
'..1777777777777777777777777771..',
'....111111111111111111111111....']}],
 script: `# Water dish. Shallow clay with water in it: the water is a surface
# things float and skate on, and it rings whenever anything disturbs it.
traits dish, water, furniture
layer 0
scale 2                # drawn 64 x 32
solid 4, 10, 24, 6     # the water surface (and the wall below it)
perch 3, 7             # the rim, left and right
perch 28, 7

anim ripple  = ripple1 ripple2 still @ 6
anim shimmer = shine still @ 3

state calm:
  frame still
  loop
    wait random(3, 7)
    play shimmer once  # light moving on the water
  end
end

on signal ripple: play ripple once end    # something stirred the surface
on signal splash: play ripple once end    # something fell in
on signal landed: play ripple once end    # a frog on the rim

on bump:               # a shell rolled into the wall
  shake 0.3s
  play ripple once
end

on poke:
  shake 0.5s
  play ripple once
  signal splash        # the water slops: everyone at the pond startles
end
`,
 start: {x: 128, y: null}},

/* ---------------- Tadpole: in the water, under the pad when scared ---------------- */
{name: "Tadpole", w: 16, h: 16,
 palette: ['#000000', '#1a2012', '#3a4428', '#6a7448', '#dfe9b8', '#54603a', '#c4ecf2', '#eefbfd'],
 frames: [
  {name: 'swim1', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'..........111...',
'..55....1123341.',
'....55551222221.',
'.........11111..']},
  {name: 'swim2', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'..........111...',
'....55..1123341.',
'.55...551222221.',
'.........11111..']},
  {name: 'gulp', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'...........77...',
'..........7..7..',
'...........77...',
'..........111...',
'...55...1123341.',
'.....5551222221.',
'........6111116.']}],
 script: `# Tadpole. Wriggles about the dish, noses up for air, and hides
# under the lily pad when the water is disturbed.
traits tadpole, critter, swimmer
layer 1                # under the pad, which is layer 2
var d, p, tx

anim swim = swim1 swim2 @ 5
anim dash = swim1 swim2 @ 14

state swim:
  d = find(dish)
  play swim
  repeat random(2, 5)
    tx = clamp(x + random(-18, 18), d.x - 16, d.x + 16)
    move to tx, d.y - random(11, 13) speed 8
    wait random(4, 16) / 10
  end
  if chance(40) then go breathe end
  go swim
end

state breathe:         # nose up, a bubble, a ring on the water
  frame gulp
  wait 0.5
  signal ripple
  wait random(8, 16) / 10
  go swim
end

state hide:
  d = find(dish)
  p = find(pad)
  play dash
  if p then move to p.x + random(-2, 2), d.y - 12 speed 45 end
  play swim
  wait random(3, 7)
  go swim
end

on signal splash: go hide end
on signal ripple:
  if state != hide and dist(other) < 24 then go hide end
end
on near frog within 20:
  if state != hide then go hide end
end
on poke: signal ripple; go hide end
`,
 start: {x: 120, y: 100}},

/* ---------------- Lily pad: floats on the water, dips under weight ---------------- */
{name: "Lily pad", w: 16, h: 16,
 palette: ['#000000', '#1d4a20', '#3b8a38', '#5cb04a', '#96d46c', '#2c6a2c', '#c4ecf2', '#7cc05a'],
 frames: [
  {name: 'pad', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'...11111.1111...',
'.11344441344431.',
'1334444444444331',
'.12222222222221.']},
  {name: 'tilt', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'...11111.1111...',
'.11344441344431.',
'1334444444444331',
'6122222222222216']}],
 script: `# Lily pad. Floats on the dish and drifts; dips when something lands.
traits pad, plant, floating
layer 2
solid 2, 12, 12, 4     # skaters bump into its edge
perch 10, 13           # a seat for a frog
perch 4, 13            # and one for a flower
var base, dir, d

state start:
  base = y
  go drift
end

state drift:
  frame pad
  d = find(dish)
  loop
    dir = pick(left, right)
    if x < d.x - 8 then dir = right end
    if x > d.x + 8 then dir = left end
    walk dir * random(3, 8) speed 2
    wait random(2, 6)
  end
end

state bob:             # pushed under, then floats back up
  frame tilt
  y = base + 2
  wait 0.2
  y = base + 1
  wait 0.25
  frame pad
  y = base
  wait 0.5
  go drift
end

on signal landed:      # a frog came down on it
  sway 2 1s
  signal ripple
  go bob
end
on signal ripple:
  if dist(other) < 30 then sway 1 0.8s end
end
on signal splash: sway 2 1s; go bob end

on poke:
  sway 3 1.2s
  signal startle       # whatever sits on it lets go
  signal ripple
  go bob
end
`,
 start: {x: 138, y: 100}},

/* ---------------- Water strider: skates on the surface ---------------- */
{name: "Strider", w: 16, h: 16,
 palette: ['#000000', '#26262e', '#4a4858', '#8c8aa4', '#d8f4f8', '#7cc4d6', '#101014', '#000000'],
 frames: [
  {name: 'still', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'...1........1...',
'..1.1.122336.1..',
'.1...12222221.1.',
'1...........1..1',
'4...........4..4']},
  {name: 'skate1', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'..1.........1...',
'.1.1..122336.1..',
'1....12222221..1',
'4...........1...',
'5...........4..4']},
  {name: 'skate2', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'....1.......1...',
'...1.1122336.1..',
'..1..12222221..1',
'.1..........1...',
'45..........45.4']}],
 script: `# Water strider. Skates the surface in quick strokes and skitters
# off from anything that disturbs the water near it.
traits strider, insect, skater
var dir, d, from

anim skate = skate1 skate2 @ 10

state skate:
  d = find(dish)
  y = d.y - 12         # back on the surface (a hop may have been cut short)
  frame still
  wait random(4, 25) / 10
  dir = pick(left, right)
  if x < d.x - 14 then dir = right end
  if x > d.x + 14 then dir = left end
  play skate
  walk dir * random(5, 16) speed 22
  frame still
  if blocked then hop -dir * 3 3 end    # bumped the pad's edge
  go skate
end

state skitter:
  d = find(dish)
  y = d.y - 12
  play skate
  dir = sign(x - from.x)               # away from the trouble
  if dir == 0 then dir = pick(left, right) end
  signal ripple                        # its wake
  walk dir * 22 speed 55
  if blocked then walk -dir * 8 speed 40 end
  frame still
  wait 1
  go skate
end

state jump:
  hop 0 10
  from = me
  go skitter
end

on signal splash: from = other; go skitter end
on signal snap: from = other; go skitter end
on signal ripple:
  if state == skate and dist(other) < 30 then from = other; go skitter end
end
on near frog within 20:
  if state == skate then from = other; go skitter end
end
on poke: go jump end
`,
 start: {x: 112, y: 100}},

/* ---------------- Water lily: a flower seated on the pad ---------------- */
{name: "Water lily", w: 16, h: 16,
 palette: ['#000000', '#8a2f5a', '#f27aa4', '#ffc2d6', '#fff4f7', '#f5c531', '#2f7a34', '#5fb04c'],
 frames: [
  {name: 'bud', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'.......1........',
'......121.......',
'......232.......',
'.....12321......',
'.....22322......',
'.....72327......',
'......777.......',
'.....67776......']},
  {name: 'half', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'.......3........',
'.....2.3.2......',
'.....23432......',
'....2234322.....',
'....1223221.....',
'....7122217.....',
'.....77777......',
'....6677766.....']},
  {name: 'open', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'.......3........',
'..2....3....2...',
'..32..343..23...',
'...32.343.23....',
'.2..3234323..2..',
'.3323355532332..',
'..32235553223...',
'...222333222....',
'....7777777.....',
'...667777766....']}],
 script: `# Water lily. Sits on the lily pad; opens slowly, shuts when disturbed.
traits flower, plant, bloom
layer 3
perch 7, 9             # the heart of the flower: somewhere to land

anim opening = bud half open @ 1.5
anim closing = open half bud @ 6

state start:
  frame bud
  move to perch pad speed 300          # take its seat on the pad
  go closed
end

state closed:
  frame bud
  wait random(5, 12)
  go unfold
end

state unfold:
  play opening once
  go bloom
end

state bloom:
  frame open
  loop
    wait random(4, 9)
    sway 1 1.5s
    if chance(15) then go shut end
  end
end

state shut:
  signal startle       # anything in the petals is shaken off
  play closing once
  go closed
end

on signal landed:      # a visitor coaxes it open
  sway 2 1s
  if state == closed then go unfold end
end
on signal startle:
  if perched == other and state == bloom then go shut end
end
on signal splash:
  if state == bloom then go shut end
end
on poke: go shut end
`,
 start: {x: 134, y: 97}},

/* ---------------- Frog ---------------- */
{name: "Frog", w: 16, h: 16,
 palette: ['#000000', '#1d3a1a', '#3f8a2e', '#74bf45', '#d6eaa0', '#f3d04a', '#141414', '#e8697f'],
 frames: [
  {name: 'sit', rows: [
'................',
'................',
'................',
'................',
'................',
'..111......111..',
'.15661....15661.',
'.15551111115551.',
'1333333333333331',
'1313333333333131',
'1331111111111331',
'.13344444444331.',
'1233444444443321',
'1223344444433221',
'1222333333332221',
'.11111111111111.']},
  {name: 'blink', rows: [
'................',
'................',
'................',
'................',
'................',
'..111......111..',
'.13331....13331.',
'.11111111111111.',
'1333333333333331',
'1313333333333131',
'1331111111111331',
'.13344444444331.',
'1233444444443321',
'1223344444433221',
'1222333333332221',
'.11111111111111.']},
  {name: 'puff', rows: [
'................',
'................',
'................',
'................',
'................',
'..111......111..',
'.15661....15661.',
'.15551111115551.',
'1333333333333331',
'1313333333333131',
'1331111111111331',
'.14444444444441.',
'1444444444444441',
'1244444444444421',
'1223444444443221',
'.11111111111111.']},
  {name: 'leap', rows: [
'................',
'................',
'..111......111..',
'.15661....15661.',
'.15551111115551.',
'1333333333333331',
'1313333333333131',
'1331111111111331',
'.13344444444331.',
'..134444444431..',
'..123344443321..',
'.12211111111221.',
'.121........121.',
'.121........121.',
'1221........1221',
'111..........111']},
  {name: 'swim', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'..111......111..',
'.15661....15661.',
'.15551111115551.',
'.44133333333144.']},
  {name: 'snap', rows: [
'.......77.......',
'.......77.......',
'........7.......',
'.......7........',
'.......7........',
'..111..7...111..',
'.15661.7..15661.',
'.15551171115551.',
'1333333733333331',
'1311111711111131',
'1366666766666631',
'.13111111111131.',
'1233444444443321',
'1223344444433221',
'1222333333332221',
'.11111111111111.']}],
 script: `# Frog. Sits, blinks, croaks; hops between the lily pad, the dish rim,
# the water and the ground; snaps at insects that come close, and misses.
traits frog, critter, hopper
var where, prey, calm, d, p

anim blink = blink sit @ 8
anim croak = puff sit puff sit @ 5

state sit:
  frame sit
  if perched then signal landed to perched end   # the pad dips, the dish rings
  repeat random(3, 6)
    wait random(8, 22) / 10
    where = random(1, 4)
    if where == 1 then play blink once
    elif where == 2 then play croak once; emit note 1
    elif where == 3 then face -facing          # eyes to the other side
    elif grounded then hop random(-12, 12) 4
    end
  end
  go leap
end

state leap:
  frame leap
  where = random(1, 20)
  if where <= 9 and has(perched, pad) then where = 10 end   # already on it
  if where <= 9 then move to perch pad speed 75 arc 18
  elif where <= 13 then move to perch dish speed 75 arc 16
  elif where <= 17 then go dive
  else move to pick(random(40, 62), random(174, 214)), soil speed 70 arc 16
  end
  if grounded then emit dust 2 end
  go sit
end

state dive:            # plop: only the eyes stay above the water
  frame leap
  d = find(dish); p = find(pad)
  where = d.x + random(-18, 18)
  if p and abs(where - p.x) < 14 then where = clamp(p.x + pick(-16, 16), d.x - 18, d.x + 18) end
  move to where, d.y - 11 speed 70 arc 14   # clear of the pad
  frame swim; signal splash
  repeat random(2, 4)
    wait random(12, 30) / 10
    move to clamp(x + random(-12, 12), d.x - 18, d.x + 18), y speed 6
  end
  go climb
end

state climb:
  frame leap
  move to perch pad speed 50 arc 10
  go sit
end

state snap:
  face toward prey; frame snap
  signal snap to prey
  wait 0.3
  frame sit; hop 0 3   # settles back, empty-mouthed
  play blink once
  calm = time + 6
  go sit
end

on near insect within 30:
  if state == sit and time > calm then prey = other; go snap end
end
on signal splash: if state == sit then go leap end end
on signal startle: if perched == other then go leap end end
on poke: if state == dive then go climb else go dive end end
`,
 start: {x: 186, y: null}},

/* ---------------- Snail ---------------- */
{name: "Snail", w: 16, h: 16,
 palette: ['#000000', '#3b2416', '#a0602c', '#dc9e58', '#6b3a1c', '#b9a47a', '#e6d6ac', '#2a2422'],
 frames: [
  {name: 'crawl1', rows: [
'................',
'................',
'................',
'................',
'...........7..7.',
'...........6..6.',
'...1111....6..6.',
'..133221...6.6..',
'.13324421..666..',
'.13243421..6666.',
'.13242421.66666.',
'.12224421566666.',
'..1222215666666.',
'..5555555555555.',
'................',
'................']},
  {name: 'crawl2', rows: [
'................',
'................',
'................',
'................',
'............7.7.',
'............6.6.',
'...1111....6.6..',
'..133221...6.6..',
'.13324421..666..',
'.13243421..6666.',
'.13242421.66666.',
'.12224421566666.',
'..1222215666666.',
'.55555555555555.',
'................',
'................']},
  {name: 'tuck', rows: [
'................',
'................',
'................',
'.....11111......',
'...112333211....',
'..13333222221...',
'..13322444221...',
'..13244224421...',
'..13242442421...',
'..13242222421...',
'..12244444221...',
'..12222222221...',
'...112222211....',
'.....11111......',
'................',
'................']},
  {name: 'peek', rows: [
'................',
'................',
'................',
'.....11111......',
'...112333211....',
'..13333222221...',
'..13322444221...',
'..13244224421.7.',
'..13242442421.6.',
'..13242222421.6.',
'..122444442216..',
'..1222222222166.',
'...11222221166..',
'.....11111555...',
'................',
'................']}],
 script: `# Snail. Creeps over the soil, climbs the dish to sit on its rim, and
# pulls into its shell when startled -- which, on the rim, is a tumble.
traits snail, critter, slow
body 6                 # the shell, when it rolls
layer 3
var dir = 1, d, side

anim crawl = crawl1 crawl2 @ 3
anim peek  = tuck peek crawl1 @ 3

state crawl:
  physics off; roll off; angle = 0
  d = find(dish)
  if chance(60) then dir = sign(d.x - x) end
  repeat random(2, 4)
    play crawl
    walk dir * random(6, 14) speed 3
    frame crawl1
    if blocked and abs(x - d.x) < 40 then go climb end   # the dish wall
    if blocked or chance(25) then dir = -dir end
    wait random(15, 40) / 10
  end
  go crawl
end

state climb:
  side = sign(x - d.x)
  play crawl
  move to d.x + side * 27, d.y - 16 speed 4 arc 2    # up the wall
  frame crawl1; face -side                           # looking in at the water
  repeat random(4, 7)
    wait random(20, 40) / 10
    if chance(30) then play peek once end
  end
  play crawl
  move to d.x + side * 38, soil + 2 speed 4 arc 2    # and down again
  go crawl
end

state tumble:          # in its shell: a ball the physics rolls
  frame tuck
  physics on; roll on; bounce 30; friction 50
  vy = -25
  if y < soil then vx = sign(x - d.x) * 35 else vx = random(-20, 20) end
  wait until grounded and speed < 4
  wait random(15, 30) / 10
  go peek
end

state peek:
  physics off; roll off; angle = 0
  play peek once
  go crawl
end

state hide:            # on the ground: tuck in and wait it out
  frame tuck; wait random(2, 4); go peek
end

on near frog within 22:
  if state == crawl then go hide elif state == climb then go tumble end
end
on signal splash:
  if state == climb then go tumble elif state == crawl then go hide end
end
on hit: if impact > 30 then emit dust 2 end end
on poke: go tumble end
`,
 start: {x: 44, y: null}},

/* ---------------- Dragonfly ---------------- */
{name: "Dragonfly", w: 16, h: 16,
 palette: ['#000000', '#18303a', '#1f8f86', '#5fd8c0', '#e6f6f8', '#a6d4e0', '#2a56b0', '#ffffff'],
 frames: [
  {name: 'fly1', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'...444....444...',
'..45554..45554..',
'...4554..4554...',
'.....454.454....',
'.......444......',
'.........222.66.',
'1323323322236676',
'.........222666.',
'................',
'................']},
  {name: 'fly2', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'.........222.66.',
'1323323322236676',
'.......44222666.',
'.....4554.454...',
'...4554...4554..']},
  {name: 'rest', rows: [
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'................',
'..445554.455544.',
'.........222.66.',
'1323323322236676',
'.........222666.',
'.........1.1....',
'........1...1...']}],
 script: `# Dragonfly. Darts and hovers over the water, perches on reed tips
# and the lily, and is always too quick for the frog.
traits dragonfly, insect, flyer
var spot

anim buzz = fly1 fly2 @ 20

state hover:
  play buzz
  repeat random(2, 4)
    move to clamp(x + random(-40, 40), 50, 200), random(38, 78) speed 70 arc random(-8, 8)
    repeat random(3, 6)              # holding still in the air
      move to x + random(-2, 2), y + random(-1, 1) speed 6
    end
  end
  go land
end

state land:
  spot = random(1, 10)
  if spot <= 5 then move to perch reed speed 60 arc -6
  elif spot <= 8 then move to perch flower speed 55 arc -6
  else move to perch rim speed 80 arc 10
  end
  go rest
end

state rest:
  frame rest
  if perched then signal landed to perched end
  repeat random(3, 8)
    wait random(6, 16) / 10
    if chance(30) then play buzz; wait 0.3; frame rest end   # wing flick
  end
  go hover
end

state dart:            # gone in a blink
  play buzz
  move to clamp(x + pick(-60, 60), 20, 220), random(16, 40) speed 150 arc 6
  go hover
end

on signal snap: go dart end
on signal startle: if perched == other then go dart end end
on signal splash: if onperch then go dart end end
on poke: go dart end
`,
 start: {x: 200, y: 40}},

  ]
};
