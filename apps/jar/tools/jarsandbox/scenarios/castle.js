/* Aquarium castle: a hand-built Jarscript 3 scenario for the sandbox.
   Nine plain things around a resin castle, linked by trait and by plain
   shared signals (landed, startle, shrug, nibble, chirp). */
SCENARIOS.castle = {
  title: "Aquarium castle",
  about: "A grey resin aquarium castle with a red flag stands in the middle of the jar, with a treasure chest and a moss cushion on the left and a toadstool on the right. The castle is furniture: walls to land on, four battlements to perch on and a dark doorway. Its flag flutters whenever something arrives on it. A snail grazes the moss, making it shiver and drop bits. Now and then the snail crawls to the castle, climbs the wall with its foot on the stone, goes over the top and sits on a tower, naps in its shell, and climbs down again. A ladybug flies between the towers, the moss capsules, the toadstool, the chest lid, the rim and sometimes the snail's shell, where it rides along while the snail crawls. The newt wanders, dozes, flicks its tongue and stalks any bug on the ground. The pill bug sees it coming and scuttles into the castle doorway, curls up, and peeks out until the newt has gone. The cricket leaps up out of reach, onto the toadstool, the chest or a tower, and each of those reacts to being landed on. When the cricket chirps, the newt hears it and goes hunting. Every so often the chest's lid pops open in a burst of sparkles and everything near it jumps. Poke the castle and it shakes: anything on the wall lets go and drops into the soil, anything on the towers takes off, the pill bug curls and the newt freezes. Poke the pill bug and it rolls; if it hits the castle hard enough, the castle shakes and startles everyone again.",
  chain: [
    "Ladybug lands on a castle tower -> signal landed to the castle -> the flag flutters",
    "Snail reaches the top of the castle wall and settles on a battlement -> signal landed -> the flag flutters",
    "Snail grazes the moss -> signal nibble (one a mouthful) -> the moss shivers and drops a leaf bit",
    "Ladybug lands on the snail's shell (only while the snail is on the soil) -> signal landed -> the snail puffs a heart, and the ladybug rides the shell as the snail crawls",
    "Snail starts up the wall, starts down, tucks in or falls -> signal shrug -> a ladybug riding its shell (perched == other) takes off",
    "Poke the castle -> it shakes, flutters and puffs dust -> signal startle -> the snail on the wall lets go and falls (a dust puff where it lands), the snail on top tucks into its shell, the ladybug and cricket on the towers take off, a nearby pill bug curls up, a nearby newt freezes",
    "Newt wanders near the pill bug (on near newt) -> the pill bug runs into the castle doorway and curls up -> it peeks out now and then and only crawls out once no newt is within 40 px",
    "Newt stalks a bug on the soil -> the bug's on near newt fires -> the ladybug flies off, the cricket leaps up, the pill bug hides -> the newt's tongue flick always misses",
    "Cricket leaps onto the toadstool, the chest or a tower -> signal landed -> the toadstool squashes and puffs spores, the chest lid rattles, the castle flag flutters",
    "Cricket chirps (notes) -> signal chirp -> the newt turns toward it and stalks it -> the cricket leaps away",
    "Chest lid pops open on its own -> sparkles -> signal startle -> a cricket or ladybug on its lid takes off, a nearby pill bug curls up, a nearby newt freezes",
    "Poke the pill bug -> it curls and is flung, then tumbles -> bump into the castle -> the castle shakes and flutters, and a hard hit sends startle (as when the castle is poked); bump into the chest -> the lid pops open",
    "Poke the moss or the toadstool -> it puffs spores and sends startle -> anything perched on it or close by reacts",
  ],
  gaps: [
    "No climbing: walk only follows the soil or the top of one solid. The snail climbs with plain `move to` points up the wall, setting `angle` by hand (side * 90, then side * 45 over the top). Rotation pivots on the sprite's centre, so the wall offset (22 px from the castle's centre) was found by trial. A `climb` statement, or walk following a solid's sides, would make this one line.",
    "Perch points ignore facing and rotation. The snail's shell perch had to go at the one column (7) that is on the shell whichever way it faces. While the snail is on the wall, a rider would float in the air, so the snail sends `shrug` before it climbs. Perches that turn and flip with the sprite are the fix.",
    "An item cannot ask who sits on its perches. The snail broadcasts `shrug` and the rider checks `perched == other`. Something like `signal shrug to riders` would be cleaner.",
    "`move to perch TRAIT` with no free perch quietly lands on a random spot on the soil, which is a long trip for a snail. There is no way to ask first; the castle has four perches so that one is always free.",
    "`and`/`or` do not short-circuit, so `s and s.y >= soil` errors when s is 0. Nested if/elif was used instead.",
    "Signals go to one item or to everyone. Every `startle` receiver filters with `dist(other) < N`. `signal startle within 40` would save that line in five scripts.",
    "Items cannot read each other's state. The newt cannot tell that the pill bug is curled up in the doorway: it walks at it, is blocked by the castle wall and gives up (`calm` cooldown).",
    "Layer is fixed at compile time. The pill bug 'inside' the doorway is drawn in front of the dark arch, not behind the jamb. A settable `layer` (or a mask from the castle) would put it truly inside.",
    "No spore, bubble or drip particle. `dust` stands in for spores from the moss and the toadstool, and `spark` for the chest's glints. No time of day either, so a moth at dusk was not possible.",
    "footPad comes from frame 0 only, and physics rests a body circle on the sprite's centre. The pill bug's ball had to be drawn centred on row 8, with every frame ending on row 11, so that it rests, walks and rolls on the same ground line. Feet would make a body's rest position independent of the art.",
    "`walk` comes up short: each tick's step is rounded down to 1/16 px but the full step is taken off the distance, so a walk at 4 px/s covers about 94% of DX. The snail's walk to the foot of the wall missed by 3 px, so that leg is a `move to ..., soil` instead.",
    "Solids block every walker, so a castle and a chest on the soil fence the ground into separate pens. The scene is laid out so the newt and the pill bug share the open ground to the right of the castle; the snail lives between the chest and the castle.",
    "`hop` has a fixed speed and ignores solids, so the cricket's short hops on the soil can pass in front of the chest. Its long leaps use `move to perch` instead.",
  ],
  items: [
/* ------------------------------------------------------------------ */
{name:"Castle", w:32, h:32,
 palette:['#000000','#3b3a52','#7f8299','#aeb2c6','#dfe2ee','#262233','#e0544a','#6da34d'],
 frames:[
  {name:'still', rows:[
'...............1666.............',
'...............1666.............',
'...............166..............',
'...............16...............',
'.........111..1111..111.........',
'.........141..1421..141.........',
'.........141..1421..141.........',
'.........14211433211421.........',
'111.11.114337733333337211.11.111',
'141.11.144444441144444421.11.141',
'141.11.143333335533333321.11.141',
'14214214324333355333332321421421',
'17723333324333355333322333323771',
'17333333324333355333332333333321',
'14331133324333311333332333311321',
'12225522322232223222322232255221',
'14335523324323333323332323355321',
'14335533324733333333332333355321',
'14335533324331111113332333355321',
'12321132223215555551222222311221',
'14323333324155555555122333323321',
'17333333321555555555512333333321',
'14733333321555555555512333333321',
'12223222321555555555512232223271',
'14333323321555555555512323333321',
'14333333321555555555512333333321',
'14333333321555555555512333333321',
'12322232221555555555512222322221',
'17323333321555555555512333323371',
'17733333371555555555512733333771',
'17777333727555555555577333377771',
'11111111111111111111111111111111',
  ]},
  {name:'flap1', rows:[
'...............166666...........',
'...............16666............',
'...............166666...........',
'...............1................',
'.........111..1111..111.........',
'.........141..1421..141.........',
'.........141..1421..141.........',
'.........14211433211421.........',
'111.11.114337733333337211.11.111',
'141.11.144444441144444421.11.141',
'141.11.143333335533333321.11.141',
'14214214324333355333332321421421',
'17723333324333355333322333323771',
'17333333324333355333332333333321',
'14331133324333311333332333311321',
'12225522322232223222322232255221',
'14335523324323333323332323355321',
'14335533324733333333332333355321',
'14335533324331111113332333355321',
'12321132223215555551222222311221',
'14323333324155555555122333323321',
'17333333321555555555512333333321',
'14733333321555555555512333333321',
'12223222321555555555512232223271',
'14333323321555555555512323333321',
'14333333321555555555512333333321',
'14333333321555555555512333333321',
'12322232221555555555512222322221',
'17323333321555555555512333323371',
'17733333371555555555512733333771',
'17777333727555555555577333377771',
'11111111111111111111111111111111',
  ]},
  {name:'flap2', rows:[
'...............166..............',
'...............1666666..........',
'...............166666...........',
'...............1................',
'.........111..1111..111.........',
'.........141..1421..141.........',
'.........141..1421..141.........',
'.........14211433211421.........',
'111.11.114337733333337211.11.111',
'141.11.144444441144444421.11.141',
'141.11.143333335533333321.11.141',
'14214214324333355333332321421421',
'17723333324333355333322333323771',
'17333333324333355333332333333321',
'14331133324333311333332333311321',
'12225522322232223222322232255221',
'14335523324323333323332323355321',
'14335533324733333333332333355321',
'14335533324331111113332333355321',
'12321132223215555551222222311221',
'14323333324155555555122333323321',
'17333333321555555555512333333321',
'14733333321555555555512333333321',
'12223222321555555555512232223271',
'14333323321555555555512323333321',
'14333333321555555555512333333321',
'14333333321555555555512333333321',
'12322232221555555555512222322221',
'17323333321555555555512333323371',
'17733333371555555555512733333771',
'17777333727555555555577333377771',
'11111111111111111111111111111111',
  ]},
 ],
 script:`# Castle. A resin aquarium castle: two towers and a keep to land on,
# a dark doorway to hide in, a flag that flutters when anything arrives.
traits castle, furniture
layer 0
solid 1, 11, 30, 21       # the walls, from the tower walks down
perch 5, 8                # left tower
perch 26, 8               # right tower
perch 10, 4               # keep, left battlement
perch 21, 4               # keep, right battlement

anim flutter = flap1 flap2 flap1 flap2 flap1 flap2 still @ 9
anim breeze  = flap1 still flap1 still @ 4

state stand:
  frame still
  loop
    wait random(7, 16)
    if chance(60) then play breeze once else play flutter once end
  end
end

on signal landed:         # something just arrived on a tower
  play flutter once
end

on bump:                  # a tumbling bug, or a dropped snail
  shake 0.3s
  play flutter once
  if impact > 40 then signal startle end
end

on poke:
  shake 0.5s
  play flutter once
  emit dust 4
  signal startle          # everything on it or near it reacts
end
`,
 start:{x:100, y:null}},
/* ------------------------------------------------------------------ */
{name:"Moss", w:16, h:16,
 palette:['#000000','#22421f','#3d7a35','#5ea343','#97cf62','#9a5a2e','#d8a43c','#7cbd4c'],
 frames:[
  {name:'still', rows:[
'................',
'...66...........',
'...66...........',
'....5...........',
'....5...........',
'....5.......66..',
'.....5......66..',
'.....5..66..5...',
'.....5..66.5....',
'.....5..5..5....',
'...1121312121...',
'..134343234331..',
'.13434373334341.',
'.13374343437331.',
'1322332323223321',
'1111111111111111']},
  {name:'puff', rows:[
'..6..6..........',
'...66...........',
'..6..6..........',
'....5.......6..6',
'....5......6.66.',
'....5......6.66.',
'.....5.6..6...6.',
'.....5..66..5...',
'.....5..66.5....',
'.....5..5..5....',
'...1121312121...',
'..134343234331..',
'.13434373334341.',
'.13374343437331.',
'1322332323223321',
'1111111111111111']},
 ],
 script:`# Moss. A cushion of moss with spore capsules on stalks.
# Ladybugs land on the capsules; the snail grazes the cushion.
traits moss, plant
layer 0
perch 4, 1                # the tall capsule
perch 12, 5               # the short one
perch 8, 10               # the cushion itself

anim spores = puff still @ 3

state grow:
  frame still
  loop
    wait random(5, 12)
    sway 1 1.4s           # a breath of air
    if chance(25) then
      play spores once
      emit dust 2         # spores drift off
    end
  end
end

on signal landed:         # something settled on a capsule
  sway 2 1s
end

on signal nibble:         # the snail takes a mouthful
  sway 1 0.6s
  shake 0.2s
  emit leaf 1
end

on poke:
  sway 3 1.2s
  play spores once
  emit dust 3
  emit leaf 2
  signal startle
end
`,
 start:{x:50, y:null}},
/* ------------------------------------------------------------------ */
{name:"Chest", w:16, h:16,
 palette:['#000000','#2a1a10','#6b3f1f','#9a6232','#c98c4a','#f2c14e','#b3862a','#fff4c0'],
 frames:[
  {name:'shut', rows:[
'................','................','................','................','................',
'...1111111111...',
'..134444444431..',
'.13333363333331.',
'.15555555555551.',
'.13333575333331.',
'.12222656222221.',
'.13333363333331.',
'.12222262222221.',
'.13333363333331.',
'.15555555555551.',
'.11111111111111.']},
  {name:'ajar', rows:[
'................','................','................','................',
'...1111111111...',
'..134444444431..',
'.13333363333331.',
'.15555555555551.',
'.17777777777771.',
'.13333575333331.',
'.12222656222221.',
'.13333363333331.',
'.12222262222221.',
'.13333363333331.',
'.15555555555551.',
'.11111111111111.']},
  {name:'open', rows:[
'................','................',
'..111111111111..',
'.12222222222221.',
'.12222262222221.',
'.12222222222221.',
'.15555555555551.',
'.1.75.7.57.5.71.',
'.17575757575751.',
'.13333575333331.',
'.12222656222221.',
'.13333363333331.',
'.12222262222221.',
'.13333363333331.',
'.15555555555551.',
'.11111111111111.']},
 ],
 script:`# Chest. An aquarium treasure chest. Every so often the lid pops
# open, the coins glint, and anything close by jumps.
traits chest, furniture, ornament
layer 0
solid 1, 6, 14, 10        # the box and the shut lid
perch 7, 5                # on the lid

anim open = ajar open @ 8
anim shut = ajar shut @ 10

state shut:
  frame shut
  wait random(14, 30)
  go pop
end

state pop:
  shake 0.4s
  frame ajar
  wait 0.4s
  play open once
  emit spark 6
  signal startle
  repeat random(2, 4)
    wait random(6, 12) / 10
    emit spark 2          # the coins glint
  end
  wait random(1, 3)
  play shut once
  shake 0.15s
  go shut
end

on signal landed:         # the lid rattles under a visitor
  shake 0.2s
end

on bump:
  shake 0.3s
  if state == shut and impact > 20 then go pop end
end

on poke:
  if state == shut then go pop else shake 0.2s end
end
`,
 start:{x:20, y:null}},
/* ------------------------------------------------------------------ */
{name:"Toadstool", w:16, h:16,
 palette:['#000000','#3a1a14','#a8322a','#e0473a','#ff8a70','#f6ecd8','#cdbf9e','#fff8ec'],
 frames:[
  {name:'still', rows:[
'................','................','................','................',
'.....111111.....',
'...1135333311...',
'..133343353331..',
'.13533333333531.',
'.12333533333321.',
'.11222222222211.',
'...1166666611...',
'......1551......',
'......1551......',
'......1561......',
'.....115561.....',
'.....15555611...']},
  {name:'squash', rows:[
'................','................','................','................','................',
'....11111111....',
'..113533333311..',
'.1333433533331..',
'135333333333531.',
'123335333333321.',
'112222222222211.',
'..11666666661...',
'......1551......',
'......1561......',
'.....115561.....',
'....1555556611..']},
 ],
 script:`# Toadstool. A red cap to land on; it squashes and puffs spores.
traits mushroom, plant
layer 0
perch 8, 4                # the top of the cap

anim squash = squash still @ 8

state grow:
  frame still
  loop
    wait random(8, 18)
    sway 1 1.5s
    if chance(35) then emit dust 2 end    # a puff of spores
  end
end

on signal landed:
  play squash once
  sway 2 0.8s
  emit dust 1
end

on poke:
  play squash once
  sway 3 1.2s
  emit dust 4
  signal startle
end
`,
 start:{x:206, y:null}},
/* ------------------------------------------------------------------ */
{name:"Snail", w:16, h:16,
 palette:['#000000','#3b2a26','#94552c','#cf8a47','#f3c784','#b8a48a','#ddd0b8','#1b1416'],
 frames:[
  {name:'crawl1', rows:[
'................',
'................',
'................',
'................',
'...111111.......',
'..13444431..7.7.',
'.1344333331.5.5.',
'.1432222331.5.5.',
'.1423333231.5.5.',
'.1423223231.656.',
'.1423233231.6661',
'.1342222331.6661',
'..133333311.6661',
'.161111115566661',
'1665555555555551',
'.11111111111111.']},
  {name:'crawl2', rows:[
'................',
'................',
'................',
'................',
'...111111.......',
'..13444431...7.7',
'.1344333331..5.5',
'.1432222331.5.5.',
'.1423333231.5.5.',
'.1423223231.656.',
'.1423233231.6661',
'.1342222331.6661',
'..133333311.6661',
'..61111115566661',
'.165555555555551',
'..1111111111111.']},
  {name:'shell', rows:[
'................',
'................',
'................',
'................',
'................',
'................',
'...111111.......',
'..13444431......',
'.1344333331.....',
'.1432222331.....',
'.1423333231.....',
'.1423223231.....',
'.1423233231.....',
'.1342222331.....',
'..133333311.....',
'...1111111......']},
  {name:'peek', rows:[
'................',
'................',
'................',
'................',
'................',
'................',
'...111111.......',
'..13444431......',
'.1344333331.....',
'.1432222331.....',
'.1423333231.....',
'.1423223231.7.7.',
'.1423233231.5.5.',
'.134222233116661',
'..13333331166661',
'...11111111111..']},
  {name:'munch', rows:[
'................',
'................',
'................',
'................',
'...111111.......',
'..13444431......',
'.1344333331.7.7.',
'.1432222331.5.5.',
'.1423333231.5.5.',
'.1423223231.5.5.',
'.1423233231.656.',
'.1342222331.6661',
'..133333311.6661',
'.161111115566671',
'1665555555555551',
'.11111111111111.']},
 ],
 script:`# Snail. Grazes the moss, and now and then climbs the castle wall
# to sit on a tower. Startle it on the wall and it lets go.
traits snail, critter, slow
body 7
perch 7, 4                # a ladybug may ride on the shell
var c, side

anim crawl = crawl1 crawl2 @ 3
anim munch = munch crawl1 @ 4
anim tuck  = peek shell @ 6
anim peek  = shell peek crawl1 @ 3

state wander:
  if y < soil - 1 then go descend end     # stranded up high
  angle = 0
  play crawl
  walk pick(left, right) * random(6, 20) speed 4
  frame crawl1
  wait random(2, 5)
  if chance(35) and find(moss, 80) then go graze end
  if chance(40) and find(castle, 110) then go climb end
  go wander
end

state graze:
  c = find(moss, 80)
  if c == 0 then go wander end
  play crawl
  walk c.x - x - sign(c.x - x) * 8 speed 4
  face toward c
  repeat random(3, 5)
    play munch
    wait 1.2s
    signal nibble to c
    frame crawl1
    wait random(6, 16) / 10
  end
  go wander
end

state climb:
  c = find(castle)
  if c == 0 then go wander end
  side = sign(x - c.x)
  if side == 0 then side = left end
  play crawl
  move to c.x + side * 22, soil speed 4   # to the foot of the wall
  signal shrug                           # a rider gets off
  face -side
  angle = side * 90                       # foot on the stone, head up
  move to x, c.y - 17 speed 3
  angle = side * 45                       # over the top
  move to c.x + side * 12, c.y - 21 speed 3
  angle = 0
  move to perch castle speed 4 arc 3
  go sit
end

state sit:                                # up on a battlement
  if perched then signal landed to perched end
  frame crawl1
  wait random(5, 10)
  play tuck once                          # a nap in the shell, up high
  wait random(4, 9)
  play peek once
  wait random(2, 4)
  go descend
end

state descend:
  c = find(castle)
  if c == 0 then go fall end
  side = pick(left, right)
  signal shrug
  play crawl
  move to c.x + side * 12, c.y - 21 speed 4 arc 3
  angle = side * 45
  move to c.x + side * 22, c.y - 17 speed 3
  face side
  angle = side * 90                       # head down this time
  move to x, soil speed 3
  angle = 0
  go wander
end

state tuck:
  signal shrug
  angle = 0
  play tuck once
  wait random(3, 6)
  play peek once
  if onperch then go sit end
  go wander
end

state fall:                               # lets go of the wall
  on signal startle: end
  on poke: end
  signal shrug
  angle = 0
  frame shell
  vx = side * 12
  vy = 0
  physics on
  bounce 25
  friction 90
  wait until grounded and speed < 4
  wait 0.4s
  physics off
  vx = 0
  vy = 0
  y = soil
  wait random(2, 4)
  play peek once
  go wander
end

on poke:
  if angle == 90 or angle == 270 then go fall end   # on the wall: lets go
  go tuck
end

on signal startle:
  if perched != other and dist(other) >= 45 then return end   # too far off to care
  if angle == 90 or angle == 270 then go fall end
  go tuck
end

on signal landed:                         # a ladybug on the shell
  emit heart 1
end

on hit:
  if impact > 25 then emit dust 3 end
end
`,
 start:{x:72, y:null}},
/* ------------------------------------------------------------------ */
{name:"Ladybug", w:16, h:16,
 palette:['#000000','#1a1216','#a8232b','#e8473d','#ff9c86','#231c22','#cfe0ec','#f6efe2'],
 frames:[
  {name:'sit', rows:[
'................','................','................','................','................','................','................','................','................','................',
'......11111.....',
'.....1445331....',
'....134333531...',
'....13533333151.',
'....12222253571.',
'.....1.1.1.11...']},
  {name:'step', rows:[
'................','................','................','................','................','................','................','................','................','................',
'......11111.....',
'.....1445331....',
'....134333531...',
'....13533333151.',
'....12222253571.',
'....1.1.1..11...']},
  {name:'open', rows:[
'................','................','................','................','................','................','................',
'...66...........',
'..6666..........',
'...6666.........',
'.....6.11111....',
'.....1445331....',
'....134333531...',
'....13533333151.',
'....12222253571.',
'.....1.1.1.11...']},
  {name:'fly1', rows:[
'................','................','................','................',
'..666...........',
'.66666..........',
'.666666.........',
'..666666........',
'....66666.......',
'......6611111...',
'......1445331...',
'.....134333531..',
'.....13533333151',
'.....12222253571',
'......11111111..',
'................']},
  {name:'fly2', rows:[
'................','................','................','................','................','................','................','................','................',
'.......11111....',
'......1445331...',
'.....134333531..',
'.666613533333151',
'6666612222253571',
'.66666111111111.',
'..666...........']},
 ],
 script:`# Ladybug. Flies from perch to perch -- castle towers, moss capsules,
# the toadstool, the chest lid, a snail's shell -- and lands with a bump.
traits ladybug, bug, flyer
var spot, n, s

anim open  = sit open @ 10
anim fly   = fly1 fly2 @ 18
anim close = open sit @ 10
anim walk  = sit step @ 8

state sit:
  if perched then signal landed to perched end
  repeat random(2, 4)
    wait random(8, 20) / 10
    n = random(1, 3)
    if n == 1 then
      face -facing                  # looks the other way
    elif n == 2 then
      frame open; wait 0.3s; frame sit    # a wing shuffle
    elif grounded then
      play walk
      walk random(-14, 14) speed 6
      frame sit
    end
  end
  go fly
end

state fly:
  play open once
  play fly
  spot = random(1, 12)
  if spot == 9 then                 # the snail, but only on the soil
    s = find(snail)
    if s == 0 then spot = 10 elif s.y < soil then spot = 10 end
  end
  if spot <= 4 then
    move to perch castle speed 40 arc 14
  elif spot <= 6 then
    move to perch moss speed 40 arc 12
  elif spot == 7 then
    move to perch mushroom speed 40 arc 12
  elif spot == 8 then
    move to perch chest speed 40 arc 12
  elif spot == 9 then
    move to perch snail speed 35 arc 10
  elif spot == 10 then
    move to perch rim speed 45 arc 10
  else
    move to ground speed 40 arc 12
  end
  play close once
  go sit
end

on poke:
  go fly
end

on signal startle:
  if state == sit then
    if perched == other or dist(other) < 30 then go fly end
  end
end

on signal shrug:                    # the snail is off up a wall
  if perched == other then go fly end
end

on near newt within 26:
  if state == sit then go fly end
end
`,
 start:{x:128, y:null}},
/* ------------------------------------------------------------------ */
{name:"Pill bug", w:16, h:16,
 palette:['#000000','#2b2a35','#55607a','#8893ad','#bfc8dc','#3a3346','#14141a','#9b7c8e'],
 frames:[
  {name:'ball', rows:[
'................',
'................',
'................',
'................',
'......1111......',
'.....143321.....',
'....14332321....',
'....13323321....',
'....13233231....',
'....12332321....',
'.....122221.....',
'......1111......',
'................',
'................',
'................',
'................']},
  {name:'half', rows:[
'................',
'................',
'................',
'................',
'................',
'................',
'.....111111.....',
'....14343431....',
'...1323232321...',
'...1232323261...',
'....122222221...',
'....5.5.5.5.....',
'................',
'................',
'................',
'................']},
  {name:'walk1', rows:[
'................',
'................',
'................',
'................',
'................',
'....11111111...7',
'..1143434343117.',
'.13232323232321.',
'.12323232323261.',
'.12222222222221.',
'..111111111111..',
'..5.5.5.5.5.5...',
'................',
'................',
'................',
'................']},
  {name:'walk2', rows:[
'................',
'................',
'................',
'................',
'...............7',
'....11111111..7.',
'..1143434343117.',
'.13232323232321.',
'.12323232323261.',
'.12222222222221.',
'..111111111111..',
'...5.5.5.5.5.5..',
'................',
'................',
'................',
'................']},
 ],
 script:`# Pill bug. Crawls about; poked, it rolls into a ball and tumbles.
# When the newt comes near it runs into the castle doorway and curls up.
traits pillbug, bug, roller
body 4                    # the ball: centred on the sprite, 8 px across
var dir, c

anim uncurl = ball half walk1 @ 6
anim curl   = walk1 half ball @ 12
anim crawl  = walk1 walk2 @ 8

state crawl:
  if y < soil then go down end     # landed on a roof
  dir = pick(left, right)
  repeat random(3, 6)
    play crawl
    walk dir * random(10, 34) speed 9
    frame walk1
    if blocked or chance(35) then dir = -dir end
    wait random(5, 18) / 10
  end
  wait random(1, 3)
  go crawl
end

state hide:               # into the castle doorway, quick
  c = find(castle)
  if c == 0 then go curl end
  if abs(c.x - x) > 100 then go curl end
  play crawl
  move to c.x, soil + 4 speed 26
  play curl once
  wait random(4, 8)
  wait until find(newt, 40) == 0
  repeat random(2, 3)     # peeks out, ducks back
    frame half
    wait random(6, 12) / 10
    frame ball
    wait random(10, 20) / 10
  end
  wait until find(newt, 40) == 0
  play uncurl once
  play crawl
  walk pick(left, right) * random(22, 30) speed 9
  go crawl
end

state curl:               # no doorway near: a ball where it stands
  play curl once
  wait random(3, 6)
  wait until find(newt, 30) == 0
  play uncurl once
  go crawl
end

state flung:
  play curl once
  physics on
  roll on
  vx = pick(left, right) * random(70, 120)
  vy = -random(70, 110)
  go tumble
end

state tumble:
  physics on
  roll on
  bounce 55
  friction 45
  frame ball
  loop
    wait until grounded and speed < 4
    wait 1s
    if grounded and speed < 4 then go uncurl end
  end
end

state uncurl:
  physics off
  roll off
  angle = 0
  vx = 0; vy = 0
  play uncurl once
  go crawl
end

state down:               # stranded on the castle or the chest: walk off
  play crawl
  walk dir * 40 speed 9   # stops at the edge
  play curl once
  physics on
  roll on
  vx = dir * 30
  vy = -20
  go tumble
end

on poke:
  if state == hide or state == curl then
    shake 0.3s            # curled tight: it only clenches
    return
  end
  go flung
end

on near newt within 36:
  if state == crawl then go hide end
end

on signal startle:
  if state == crawl and dist(other) < 45 then go curl end
end

on hit:
  if impact > 45 then emit dust 2 end
end
`,
 start:{x:142, y:null}},
/* ------------------------------------------------------------------ */
{name:"Newt", w:16, h:16,
 palette:['#000000','#3a1c10','#c0501c','#ee7a2c','#ffb45c','#d8343a','#140c0a','#f7d65a'],
 frames:[
  {name:'walk1', rows:[
'................','................','................','................','................','................','................','................','................','................',
'................',
'..........1111..',
'....111111334411',
'.111353353336341',
'1222227722222221',
'....1.1...1.1...']},
  {name:'walk2', rows:[
'................','................','................','................','................','................','................','................','................','................',
'................',
'..........1111..',
'....111111334411',
'1111353353336341',
'.222227722222221',
'.....1.1...1.1..']},
  {name:'lick', rows:[
'................','................','................','................','................','................','................','................','................','................',
'................',
'..........1111..',
'....111111334411',
'.111353353336345',
'1222227722222221',
'....1.1...1.1...']},
  {name:'up', rows:[
'................','................','................','................','................','................','................','................','................','................',
'...........1111.',
'..........133441',
'....111111336341',
'.111353353332221',
'1222227722221...',
'....1.1...1.1...']},
  {name:'doze', rows:[
'................','................','................','................','................','................','................','................','................','................',
'................',
'................',
'....11111111111.',
'.111353353332441',
'1222227722222221',
'...11.11..11.11.']},
 ],
 script:`# Newt. An orange eft: wanders, dozes, flicks its tongue, and stalks
# any small bug it sees on the soil. It never catches one.
traits newt, critter, hunter
var prey, d, calm, tries, n

anim walk  = walk1 walk2 @ 6
anim dash  = walk1 walk2 @ 14
anim flick = lick walk1 lick walk1 @ 10

state roam:
  play walk
  walk pick(left, right) * random(10, 36) speed 8
  frame walk1
  wait random(10, 30) / 10
  n = random(1, 10)
  if n <= 2 then play flick once
  elif n == 3 then go doze
  end
  if time > calm then
    prey = find(bug, 70)
    if prey then
      if prey.y >= soil then tries = 0; go stalk end
    end
  end
  go roam
end

state stalk:              # creep, freeze, stare, creep
  if prey == 0 then go roam end
  if prey.y < soil then calm = time + 8; go roam end   # out of reach
  d = prey.x - x
  if abs(d) < 18 then go snap end
  tries += 1
  if tries > 6 then calm = time + 20; go roam end
  face d
  frame up
  wait random(4, 9) / 10
  play walk
  walk sign(d) * min(abs(d) - 14, 12) speed 13
  frame walk1
  if blocked then calm = time + 20; go roam end
  go stalk
end

state snap:               # the tongue flick; the bug is always quicker
  face toward prey
  hop sign(prey.x - x) * 6 3
  play flick once
  calm = time + random(15, 30)
  frame up
  wait 1s
  go roam
end

state doze:
  frame doze
  wait random(5, 12)
  frame up
  wait 0.6s
  go roam
end

state freeze:             # something banged nearby
  frame up
  wait random(10, 25) / 10
  go roam
end

state dash:
  play dash
  walk facing * random(22, 36) speed 40
  go roam
end

on poke:
  face -facing
  go dash
end

on signal chirp:          # a cricket somewhere: dinner?
  if state == roam and time > calm then
    prey = other
    tries = 0
    face toward other
    go stalk
  end
end

on signal startle:
  if dist(other) < 50 and state != dash then go freeze end
end
`,
 start:{x:226, y:null}},
/* ------------------------------------------------------------------ */
{name:"Cricket", w:16, h:16,
 palette:['#000000','#1e140c','#4a3018','#7a5028','#b07a3e','#5c3c1e','#2a1c10','#e8d8b0'],
 frames:[
  {name:'sit', rows:[
'................',
'................',
'................',
'.666............',
'....666.........',
'.......66.......',
'.........66.....',
'...........6....',
'.11.........6...',
'1441.......111..',
'51441111111331..',
'5.1444443333371.',
'5..144433333331.',
'5...1222222221..',
'5....5.5..5.51..',
'55..5.5..5...5..']},
  {name:'chirp', rows:[
'................',
'................',
'................',
'.666............',
'....666.........',
'.......66.......',
'.........66.....',
'......44...6....',
'.11..4441...6...',
'1441.44441.111..',
'51441144111331..',
'5.1444443333371.',
'5..144433333331.',
'5...1222222221..',
'5....5.5..5.51..',
'55..5.5..5...5..']},
  {name:'jump', rows:[
'................',
'................',
'................',
'................',
'................',
'66..............',
'..666...........',
'.....666........',
'........66......',
'..........6111..',
'....111111111331',
'.114444443333337',
'1144433333333331',
'1.11222222222221',
'.5...5.5..5.5...',
'5...............']},
 ],
 script:`# Cricket. Sits, chirps (the newt hears it), hops about, and leaps
# up onto something high when anything comes near.
traits cricket, bug, hopper
var n, where

anim chirp = chirp sit chirp sit @ 12

state sit:
  frame sit
  wait random(15, 40) / 10
  n = random(1, 10)
  if n <= 4 then go sing end
  if n <= 7 then go hopabout end
  go sit
end

state sing:
  repeat random(2, 4)
    play chirp once
    emit note 1
  end
  signal chirp
  go sit
end

state hopabout:
  frame jump
  if onperch then         # hop down off whatever it is on
    move to clamp(x + pick(left, right) * random(16, 30), 12, 228), soil speed 70 arc 12
  else
    hop random(-24, 24) 8
  end
  frame sit
  go sit
end

state leap:               # up, out of reach
  frame jump
  where = random(1, 4)
  if where == 1 then
    move to perch mushroom speed 90 arc 18
  elif where == 2 then
    move to perch chest speed 90 arc 18
  elif where == 3 then
    move to perch castle speed 90 arc 22
  else
    move to clamp(x + pick(left, right) * random(40, 70), 12, 228), soil speed 90 arc 22
  end
  frame sit
  go land
end

state land:
  if perched then signal landed to perched end
  go sit
end

on poke:
  go leap
end

on near newt within 30:
  if state != leap then go leap end
end

on signal startle:
  if state != leap then
    if perched == other or dist(other) < 40 then go leap end
  end
end
`,
 start:{x:176, y:null}},
  ]
};
