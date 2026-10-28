/* Branch and caterpillar: a hand-built Jarscript 3 scenario for the sandbox.
   A mossy branch with a busy little world on and around it. Built as a
   target for the item pipeline: plain real things whose bodies interact. */
SCENARIOS.branch = {
  title: "Branch and caterpillar",
  about: "A mossy fallen branch leans across the jar, forked at the top, with a little world on it and under it. A caterpillar inches up the bark a hunch and a stretch at a time, naps curled in the fork, wakes to chew at the moss and sometimes backs down again. A spider hangs on a thread under the long twig, paying out silk and reeling it back; a dewdrop swells on the tip of a hanging leaf until it falls. Under the fork stands a clump of toadstools, and on the soil a ladybird trundles about and climbs the big cap for a look round, beside a fern. A moth rests flat on the bark or the fern and flits in zigzags; a chickadee hops between the branch, the fern, the rim and the soil. The chickadee is the trouble-maker: wherever it lands the perch gives under it, the caterpillar goes stiff as a twig, the spider zips up its thread, the ladybird hides under the fern or plays dead, and the moth is off. Poke the branch and it springs: the bird takes off, the dewdrop lets go, the spider zips up, and the caterpillar loses its grip and tumbles, sometimes onto the toadstool, which puffs out spores and scares off the moth; then it crawls back to the foot of the branch and climbs again.",
  chain: [
    "Chickadee lands on Branch -> signal landed to Branch -> Branch sways (the bird bobs with it), rustles and drops a leaf",
    "Chickadee lands on Fern -> signal landed to Fern -> Fern dips under the weight and rustles",
    "Chickadee comes within 36 of Caterpillar on the branch -> Caterpillar freezes stiff like a twig until no bird is within 44, then climbs on (or wakes in the fork)",
    "Chickadee comes within 36 of Spider -> Spider zips up its thread and tucks its legs in",
    "Chickadee comes down to the soil near Ladybird -> Ladybird scuttles under the Fern (drawn in front, so it is hidden) or, with no fern near, plays dead on its back until the bird is gone, then rocks back over",
    "Chickadee sitting sees Moth within 40 -> darts at it -> Moth (bird within 24) flees in a zigzag",
    "Moth flits within 30 of Spider -> Spider drops to the end of its thread and kicks -> Moth (spider within 20) flees",
    "Caterpillar wakes in the fork -> signal nibble to Branch -> Branch twitches and drops a leaf; after a few nibbles it naps again or backs down the trunk",
    "Poke Branch -> it springs (sway 6), rustles, drops leaves, signal startle -> Chickadee perched on it flies off; Spider zips up; Dewdrop lets go; Caterpillar loses its grip and tumbles (physics, rolling); Moth within 50 flees",
    "Caterpillar falls onto the toadstool cap -> bump -> Mushrooms squash and puff spores -> signal puff -> Moth within 50 flees; Ladybird standing on the cap gets down",
    "Caterpillar lands -> uncurls, crawls back to the foot of the branch (over anything solid in the way) and climbs again",
    "Dewdrop swells (tiny, small, big, trembling) and falls on its own, or at a poke or startle -> lands on the toadstool cap (bump -> puff, as above) or the soil -> signal splash -> Chickadee flies down to sip; Ladybird within 40 hides under the Fern",
    "Caterpillar and Ladybird meet on the soil (within 16) -> Caterpillar rears up stiff; Ladybird says ? and turns back",
    "Ladybird walks into the toadstool stem (walk blocked) -> half the time it climbs onto the cap for a look round",
    "Poke Fern -> it rustles hard and signals startle -> Chickadee perched on it flies off, Ladybird within 36 plays dead, Moth within 50 flees",
    "Poke Mushrooms -> shake and puff -> Moth flees, Ladybird on the cap gets down",
    "Poke Caterpillar -> '!' and it freezes (on the branch) or rears (on the soil); poke Spider -> zips up; poke Moth -> flees shedding scales; poke Ladybird -> plays dead; poke Dewdrop -> falls; poke Chickadee -> 'dee-dee!' and flies"
  ],
  gaps: [
    "No way to move to a particular perch: 'move to perch branch' picks a random free one. The caterpillar needs the fork, so its path is arithmetic on the branch's position (feet on the line y = branch.x + branch.y - 40.5 - x) and a move to a hard-coded spot. A named perch ('perch fork 19, 8' and 'move to perch branch.fork') would make climbing things easy to write.",
    "No walking along a slope: 'walk' is horizontal on the soil or a solid's flat top. Climbing the diagonal branch is a loop of short 'move to x + 4, c - x - 4' steps with frame changes between them (an inching rhythm that happens to suit a caterpillar). A 'climb' along a perch-to-perch path, or a sloped solid, would cover vines, stems and ramps.",
    "A perch puts the visitor's feet on the point, so nothing can hang below one. The spider and the dewdrop are placed with start/x/y under the twig and do not move with the branch's sway; only perched items follow it. A 'hang' kind of perch (sprite below the point) would let both swing with it.",
    "sway pivots at the bottom of the sprite (the top moves most), which is right for plants but wrong for anything hanging. The spider's thread is drawn into a 16x32 sprite at six lengths (frames d0..d5), stepped through with 'frame depth + 1', and it 'turns' with shake. A drawn line (thread/silk) primitive, or a top-pivot sway, would free the frames for animation.",
    "A physics body's centre is the sprite's middle, so a creature that switches physics on must draw its falling frame centred and, after landing, move itself up/down to put its feet back on the ground ('y = y - 4' in the caterpillar's fall). A 'feet' or body-offset declaration would remove the fudge.",
    "Particles always start at fixed places: dust at the soil, leaves from the sprite's top 60%. Spores puffed from a mushroom cap had to be 'emit dust', rising from the soil. An 'emit KIND N at X, Y' (or 'from top') would fix it; a 'spore' or 'drop' kind would suit water and fungi.",
    "No way to remember which state to return to: a freeze that interrupts climbing or napping decides where to go back by position (x past the fork means awake, else climb). 'go back' (to the previous state) would be cleaner.",
    "Items cannot hide or be removed: the dewdrop shows an empty 'gone' frame for a few seconds and teleports back to the leaf tip.",
    "'on near' measures from the sprite's centre, so the spider (16x32) is sensed at the middle of its thread, not where its body is. A 'sense point' declaration, or measuring to the nearest opaque pixel, would make tall sprites behave.",
    "Only physics bodies make 'bump'; a walker that runs into a solid only sets 'blocked' and the solid hears nothing, so the ladybird cannot tell the toadstool it climbed on. Walking into a solid could send both sides a gentle bump."
  ],
  items: [
    {name:"Branch", w:32, h:32,
     palette:["#000000","#2b1d14","#5b3c26","#5f9a3e","#9ccc5a","#80583a","#b08a60","#c8dc6a"],
     frames:[
       {name:"still", rows:[
         "..........113................113",
         ".........137311.............1377",
         "........13731651...........13773",
         "........3731.1651..........3331.",
         "........331..1651.........11655.",
         "..............1651.......166511.",
         "..............1651.....11655133.",
         "...............1651..1166511.373",
         "...............16511166551...373",
         "................155552511....373",
         "...............16655211.......31",
         "..............1665521.........3.",
         ".............1665521............",
         "..........341665521.............",
         "..........43462521..............",
         ".........43462521...............",
         "........43465521................",
         "........3465521.................",
         ".......1665521..................",
         "....341662521...................",
         "....43465521....................",
         "...43465521.....................",
         "..43465521......................",
         "..3462521.......................",
         ".1662521........................",
         "1665521.........................",
         "645421..........................",
         "434341..........................",
         "3343341.........................",
         "33433431........................",
         "33333333........................",
         "13333331........................"]},
       {name:"rustle", rows:[
         "................................",
         ".........113111...............13",
         "........13731651............1377",
         ".......13731.1651..........13773",
         ".......3731..1651.........13315.",
         ".......31.....1651.......166511.",
         "..............1651.....116551.33",
         "...............1651..1166511..37",
         "...............16511166551....37",
         "................155552511.....37",
         "...............16655211.......31",
         "..............1665521.........3.",
         ".............1665521............",
         "..........341665521.............",
         "..........43462521..............",
         ".........43462521...............",
         "........43465521................",
         "........3465521.................",
         ".......1665521..................",
         "....341662521...................",
         "....43465521....................",
         "...43465521.....................",
         "..43465521......................",
         "..3462521.......................",
         ".1662521........................",
         "1665521.........................",
         "645421..........................",
         "434341..........................",
         "3343341.........................",
         "33433431........................",
         "33333333........................",
         "13333331........................"]}
     ],
     script:`# Branch. A mossy fallen branch leaning across the jar, forked at the top.
# Others climb it, perch on it and hang from it; it rustles when a bird
# lands and springs when poked, and whatever is on it reacts.
traits branch, wood, plant
layer 0                # behind everything that uses it
scale 2                # a 32x32 drawn at 64x64
perch 7, 18            # low on the trunk
perch 12, 13           # halfway up
perch 13, 1            # the top of the upright twig
perch 25, 5            # along the long twig
var gust

anim rustle = rustle still rustle still @ 10

state idle:
  frame still
  loop
    wait random(6, 14)
    gust = random(1, 3)
    sway gust 1.8s     # a draught through the jar
    if gust == 3 and chance(50) then emit leaf 1 end
  end
end

on signal landed:      # a bird just landed on me
  sway 2 1s
  play rustle once
  if chance(60) then emit leaf 1 end
end

on signal nibble:      # the caterpillar is chewing at my moss
  shake 0.25s
  if chance(40) then emit leaf 1 end
end

on poke:
  sway 6 2s            # it springs; the tips swing most
  shake 0.3s
  play rustle once
  emit leaf 3
  signal startle       # everything on me (or near) reacts
end
`,
     start:{x:92,y:null}},
    {name:"Spider", w:16, h:32,
     palette:["#000000","#2a2024","#6a4a3a","#a07a5a","#000000","#3e2c26","#f0e6d0","#b4c3c4"],
     frames:[
       {name:"d0", rows:[
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"d1", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"d2", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"d3", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"d4", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"d5", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "..1..12221..1...",
         "...1.11211.1....",
         ".11.1.151.1.11..",
         "...1.11511.1....",
         "..1...161...1...",
         ".......1........"]},
       {name:"kick", rows:[
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         ".......7........",
         "......111.......",
         ".1...12321...1..",
         "..1..12221..1...",
         "...1111211111...",
         ".....11511......",
         "...1111511111...",
         "..1...161....1..",
         ".1.....1.....1.."]},
       {name:"tuck", rows:[
         ".......7........",
         ".......7........",
         "......111.......",
         ".....12321......",
         "....1122211.....",
         "....1111111.....",
         ".....11511......",
         ".....1.6.1......",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]}
     ],
     script:`# Spider. Hangs under the long twig on a thread: pays out silk a little
# at a time and reels it back, drops down to look at a passing moth,
# and zips up out of the way when a bird comes or the branch shakes.
traits spider, critter
layer 0
var depth, goal

anim kick = d5 kick @ 5

state dangle:
  loop
    wait random(15, 40) / 10
    goal = random(0, 5)
    while depth != goal          # down or up one notch at a time
      depth += sign(goal - depth)
      frame depth + 1
      wait 0.35
    end
    if depth == 5 and chance(50) then play kick once end
    shake 0.3s                   # turns on its thread
  end
end

state pounce:          # something flew by: straight down for a look
  while depth < 5
    depth += 1
    frame depth + 1
    wait 0.05
  end
  play kick once
  play kick once
  frame d5
  wait random(10, 20) / 10
  go dangle
end

state zip:             # up the thread in a blink
  while depth > 0
    depth -= 1
    frame depth + 1
    wait 0.05
  end
  frame tuck           # legs pulled in tight
  wait random(3, 6)
  frame d0
  go dangle
end

on near moth within 30:
  if state == dangle then go pounce end
end

on near bird within 36: go zip end

on signal startle:
  if has(other, branch) then go zip end
end

on poke: go zip end
`,
     start:{x:110,y:100}},
    {name:"Mushrooms", w:16, h:16,
     palette:["#000000","#3a2418","#c0573a","#e0875a","#f2e6c8","#cdb994","#8a6a50","#5f9a3e"],
     frames:[
       {name:"still", rows:[
         "................",
         "................",
         "....11111.......",
         "...1334321......",
         "..123342221.....",
         ".12243224221....",
         ".12222222221....",
         ".11666666611....",
         "....14451.......",
         "....14451..111..",
         "....14451.13421.",
         "....14451122221.",
         "....14451166661.",
         "...1444511.141..",
         "..17444517.1417.",
         ".77777777777777."]},
       {name:"squash", rows:[
         "................",
         "................",
         "................",
         "....11111.......",
         "..11334321......",
         ".12233422221....",
         "1122432242221...",
         "1166666666611...",
         "....14451.......",
         "....14451.......",
         "....14451..111..",
         "....14451.13421.",
         "....14451166661.",
         "...1444511.141..",
         "..17444517.1417.",
         ".77777777777777."]}
     ],
     script:`# Mushrooms. A clump of toadstools under the fork. Whatever drops onto
# the big cap squashes it and sends up a puff of spores.
traits mushroom, fungus, plant
scale 2                # a 16x16 drawn at 32x32
solid 1, 3, 11, 4      # the big cap: falling things land on it
perch 6, 2             # a moth or a ladybird can sit on top

anim squish = squash still @ 8

state still:
  frame still
  loop
    wait random(10, 24)
    if chance(40) then emit dust 1 end   # a few spores drift off
  end
end

state puff:
  play squish once
  emit dust 5
  signal puff
  wait 1
  go still
end

on bump:               # something landed on the cap
  if impact > 12 then go puff end
end

on poke:
  shake 0.3s
  go puff
end
`,
     start:{x:134,y:null}},
    {name:"Dewdrop", w:16, h:16,
     palette:["#000000","#4d86a3","#a8dcef","#e6f7fc","#ffffff","#74b6d4","#000000","#000000"],
     frames:[
       {name:"tiny", rows:[
         "................",
         "................",
         "................",
         "................",
         ".......3........",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"small", rows:[
         "................",
         "................",
         "................",
         "................",
         ".......1........",
         "......131.......",
         "......121.......",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"big", rows:[
         "................",
         "................",
         "................",
         "................",
         ".......1........",
         "......131.......",
         ".....14321......",
         ".....13221......",
         ".....12251......",
         "......155.......",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"fall", rows:[
         "................",
         "................",
         "................",
         ".......1........",
         ".......1........",
         "......131.......",
         "......141.......",
         "......121.......",
         "......151.......",
         ".......1........",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"splash", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "....2.....2.....",
         "......3..2......",
         "..2.........3...",
         "................",
         "...1122222211...",
         "................",
         "................",
         "................",
         "................",
         "................"]},
       {name:"gone", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................"]}
     ],
     script:`# Dewdrop. Gathers on the tip of the hanging leaf, swells, trembles and
# falls. It splashes where it lands; a shake brings it down early.
traits dew, water
body 3
var b

state grow:
  physics off
  b = find(branch)
  if b then
    x = b.x + 29       # the tip of the leaf that hangs off the long twig
    y = b.y - 28
  end
  frame tiny
  wait random(8, 16)
  frame small
  wait random(8, 16)
  frame big
  repeat random(3, 6)
    wait random(10, 20) / 10
    shake 0.2s         # heavy now, it trembles
  end
  go fall
end

state fall:
  frame fall
  physics on
  bounce 0
  friction 200
  vx = 0
  vy = 0
  wait until grounded or statetime > 3
  go splash
end

state splash:
  physics off
  frame splash
  signal splash
  wait 0.8
  frame gone
  wait random(3, 6)
  go grow
end

on signal startle:
  if state == grow and has(other, branch) then go fall end
end

on poke:
  if state == grow then go fall end
end
`,
     start:{x:121,y:84}},
    {name:"Caterpillar", w:16, h:16,
     palette:["#000000","#2d4a1e","#76b843","#a8dc6c","#f5d547","#4f8f34","#151515","#e8734a"],
     frames:[
       {name:"crawl1", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "..111111111111..",
         ".13333333355551.",
         "1322222222555651",
         "1242224224555551",
         "122222222225551.",
         "12222222222111..",
         ".1151151151.5..."]},
       {name:"crawl2", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "......11........",
         ".....1331.......",
         "....1322311.....",
         "...132242331111.",
         "..13222222255551",
         ".132222222255651",
         ".122422122455551",
         ".122221122255551",
         "..1221..1222111.",
         "...15....151.5.."]},
       {name:"up1", rows:[
         "................",
         "..........11....",
         ".........1551...",
         "........155551..",
         ".......1355651..",
         "......13255551..",
         ".....132255551..",
         "....132222211...",
         "...13222221.....",
         "..13224221......",
         ".13222221.......",
         "13222221........",
         "1224221.........",
         "122221..........",
         ".1221...........",
         "..11............"]},
       {name:"up2", rows:[
         "................",
         "..........111...",
         ".........15551..",
         "........1555651.",
         ".....1113555551.",
         "....13332555551.",
         "...13222245551..",
         "..13224222211...",
         "..122222221.....",
         "..12222111......",
         "..122221........",
         "..122421........",
         "..122221........",
         "...1111.........",
         "................",
         "................"]},
       {name:"nap", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "..11111111111...",
         ".1333333333551..",
         "132222222255551.",
         "3242224224551151",
         "122222222255551.",
         "122222222225551.",
         ".1111111111111.."]},
       {name:"twig", rows:[
         "................",
         "........1.......",
         ".......1511.....",
         "......155651....",
         "......155551....",
         ".....135551.....",
         ".....124221.....",
         "....132221......",
         "....122221......",
         "...132421.......",
         "...122221.......",
         "..132221........",
         "..12421.........",
         "..12221.........",
         "...111..........",
         "................"]},
       {name:"curl", rows:[
         "................",
         "................",
         "................",
         ".......111......",
         ".....113331.....",
         "....13325551....",
         "...1324255651...",
         "...1222255551...",
         "...1222255521...",
         "...1222222221...",
         "....12242221....",
         ".....122221.....",
         "......1111......",
         "................",
         "................",
         "................"]},
       {name:"down1", rows:[
         "................",
         "................",
         ".........1111...",
         "........133331..",
         ".......1322421..",
         "......13222221..",
         ".....13222221...",
         "....13224221....",
         "...13222221.....",
         "..13222221......",
         ".15554221.......",
         "15555521........",
         "1565551.........",
         "155551..........",
         ".1551...........",
         "..11............"]}
     ],
     script:`# Caterpillar. Inches up the branch to nap in the fork, nibbles, and
# sometimes backs down again. Freezes stiff like a twig when a bird comes
# near; loses its grip and falls when the branch is shaken.
traits caterpillar, critter, crawler
body 4
var b, c, n, dir, up

anim crawl = crawl1 crawl2 @ 3

state climb:           # up the bark, a hunch and a stretch at a time
  b = find(branch)
  if b == 0 then go wander end
  c = b.x + b.y - 40.5 # on the bark my feet sit on the line y = c - x
  if abs(c - x - y) > 3 then go toclimb end
  up = 1
  face right
  while x < b.x + 4
    frame up2
    wait random(4, 7) / 10
    frame up1
    move to x + 4, c - x - 4 speed 6
    wait random(2, 6) / 10
  end
  go nap
end

state nap:             # curled in the crook of the fork
  up = 1
  move to b.x + 7, b.y - 48 speed 5
  face right
  frame nap
  say "zzz" 2s
  wait random(4, 9)
  go awake
end

state awake:           # head up, chewing at the moss beside it
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

state descend:         # backs down the trunk, head first
  move to b.x + 4, c - b.x - 4 speed 5
  face right
  while x > b.x - 36
    frame down1
    move to x - 4, c - x + 4 speed 7
    wait random(3, 7) / 10
  end
  go wander
end

state wander:          # pottering on the soil
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

state toclimb:         # back to the foot of the branch
  up = 0
  b = find(branch)
  if b == 0 then go wander end
  play crawl
  walk b.x - 40 - x speed 5
  frame crawl1
  if blocked then      # something solid in the way: over the top of it
    move to x + sign(b.x - 40 - x) * 20, soil speed 8 arc 10
    go toclimb
  end
  go climb
end

state freeze:          # rigid as a twig until the bird has gone
  frame twig
  wait 1
  wait until find(bird, 44) == 0
  wait random(6, 14) / 10
  if x > b.x + 4 then go awake end
  go climb
end

state fall:            # lost its grip: a tumble and a bump
  up = 0
  frame curl
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
  y = y - 4            # physics held my middle off the ground; feet down
  frame crawl1
  wait 0.8
  go toclimb
end

state rear:            # met something on the soil: rears up, then goes on
  face dir
  frame twig
  shake 0.4s
  wait 1.2
  go wander
end

on near bird within 36:
  if up == 1 and state != freeze then go freeze end
end

on near beetle within 16:
  if up == 0 and (state == wander or state == toclimb) then
    dir = sign(other.x - x)
    go rear
  end
end

on signal startle:
  if up == 1 and has(other, branch) then go fall end
end

on hit: if impact > 40 then emit dust 2 end end

on poke:
  say "!" 0.8s
  if up == 1 then go freeze end
  go rear
end
`,
     start:{x:72,y:91.5}},
    {name:"Ladybird", w:16, h:16,
     palette:["#000000","#1a1416","#d8342c","#f2705a","#2b2226","#f4f0e8","#3b3034","#ffd8c8"],
     frames:[
       {name:"walk1", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         ".....111111.....",
         "....13722121....",
         "...1372212221...",
         "..1222212222144.",
         "..12122221211451",
         "..11111111111441",
         "...6...6...6....",
         "..6...6...6.....",
         "..6...6...6....."]},
       {name:"walk2", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         ".....111111.....",
         "....13722121....",
         "...1372212221...",
         "..1222212222144.",
         "..12122221211451",
         "..11111111111441",
         "...6...6...6....",
         "....6...6...6...",
         "....6...6...6..."]},
       {name:"back1", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "...6...6...6....",
         "...6..6...6.....",
         "....6..6..6.....",
         "..11111111111441",
         "..16666666661451",
         "..1222212222144.",
         "...12212221211..",
         "....122222221...",
         ".....11111111...",
         "................"]},
       {name:"back2", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         "..6...6...6.....",
         "...6...6...6....",
         "...6..6...6.....",
         "..11111111111441",
         "..16666666661451",
         "..1222212222144.",
         "...12212221211..",
         "....122222221...",
         ".....11111111...",
         "................"]}
     ],
     script:`# Ladybird. Trundles about the soil and climbs the toadstool for a look
# round. When a bird comes down it hides under the fern, or plays dead on
# its back until the bird has gone.
traits ladybird, beetle, bug, critter
var dir, n, f

anim walk = walk1 walk2 @ 8
anim kick = back1 back2 @ 6

state wander:
  if onperch or y < soil - 1 then        # get down off the cap first
    play walk
    move to x + pick(-14, 14), soil speed 14 arc 5
  end
  dir = pick(left, right)
  n = random(2, 5)
  repeat n
    play walk
    walk dir * random(8, 30) speed 12
    frame walk1
    if blocked then
      if chance(50) then go climb end    # walked into the toadstool: up it
      dir = -dir
    elif chance(30) then
      dir = -dir
    end
    wait random(4, 14) / 10
  end
  if chance(20) then go climb end
  go wander
end

state climb:           # up the toadstool for a look round
  play walk
  move to perch mushroom speed 10 arc 6
  frame walk1
  repeat random(2, 4)
    wait random(8, 20) / 10
    face -facing
  end
  go wander
end

state dead:            # on its back, legs in the air
  frame back1
  wait random(15, 25) / 10
  wait until find(bird, 40) == 0
  play kick            # legs wave
  wait 1
  hop 0 6              # rocks back over
  frame walk1
  go wander
end

state hide:            # scuttles under the fern
  f = find(fern)
  if f == 0 then go dead end
  play walk
  walk f.x - x speed 30
  frame walk1
  wait until find(bird, 50) == 0
  wait random(2, 4)
  go wander
end

state turn:            # bumped into the caterpillar: turns back
  face -dir
  say "?" 0.8s
  play walk
  walk -dir * 14 speed 12
  go wander
end

on near bird within 30:
  if state != dead and state != hide and other.y > soil - 20 then
    if find(fern, 50) then
      go hide
    else
      go dead
    end
  end
end

on near caterpillar within 16:
  if state == wander then dir = sign(other.x - x); go turn end
end

on signal splash:
  if state == wander and dist(other) < 40 then go hide end
end

on signal startle:
  if state != dead and dist(other) < 36 then go dead end
end

on signal puff:        # the cap it stands on puffed: off it gets
  if perched == other then go wander end
end

on poke: go dead end
`,
     start:{x:150,y:null}},
    {name:"Fern", w:16, h:16,
     palette:["#000000","#24401c","#3f7a2e","#5fa040","#8cc85a","#4a6a28","#b4dc78","#c8dc6a"],
     frames:[
       {name:"still", rows:[
         ".......11.......",
         "......1461......",
         "..11..1341..11..",
         ".1341.1341.1431.",
         ".13431.141.13431",
         "..13431.31.3431.",
         "1..13431313431..",
         "131..1343343..11",
         "13431.13531..131",
         ".134311353.13431",
         "..134335553431..",
         "...1343555431...",
         "....11335331....",
         "......1551......",
         "......1551......",
         ".....155551....."]},
       {name:"rustle", rows:[
         "........11......",
         ".......1461.....",
         "...11..1341.11..",
         "..1341.1341.1431",
         "..13431.141.1343",
         "...13431.31.3431",
         ".1..13431313431.",
         ".131..1343343.11",
         ".13431.1353..131",
         "..134311353.1343",
         "...13433555431..",
         "...1343555431...",
         "....11335331....",
         "......1551......",
         "......1551......",
         ".....155551....."]}
     ],
     script:`# Fern. Arching fronds on the soil. A bird landing bends it; a poke
# rustles it and startles whatever is on it or hiding under it.
traits fern, plant
layer 2                # in front: things hide under it
scale 2
perch 7, 1             # top of the middle frond
perch 2, 2             # the left frond
perch 13, 2            # the right frond

anim rustle = rustle still rustle still @ 9

state idle:
  frame still
  loop
    wait random(5, 12)
    sway 1 2s
  end
end

on signal landed:      # a bird on a frond: it dips under the weight
  sway 4 1.4s
  play rustle once
end

on poke:
  sway 5 1.6s
  play rustle once
  emit leaf 2
  signal startle
end
`,
     start:{x:176,y:null}},
    {name:"Moth", w:16, h:16,
     palette:["#000000","#3b2f26","#c9b28a","#e8dcc0","#8f7a5a","#6b5640","#f6f0e0","#4a3a2c"],
     frames:[
       {name:"rest", rows:[
         "................",
         "................",
         "................",
         "................",
         ".....1....1.....",
         "......1..1......",
         ".......11.......",
         "......1551......",
         ".....125521.....",
         "....12355321....",
         "...1232552321...",
         "..123245542321..",
         ".12322455422321.",
         ".11111111111111.",
         "................",
         "................"]},
       {name:"fly1", rows:[
         "................",
         "................",
         ".....1....1.....",
         "......1..1......",
         ".111...11...111.",
         "12331.1551.13321",
         "1237312552137321",
         "1232325555232321",
         ".12222155122221.",
         "..111215512111..",
         "...1232112321...",
         "...1221..1221...",
         "....11....11....",
         "................",
         "................",
         "................"]},
       {name:"fly2", rows:[
         "................",
         "................",
         "................",
         "................",
         ".....1....1.....",
         "......1..1......",
         "......1551......",
         "....11255211....",
         "...1232552321...",
         "..123215512321..",
         "..122215512221..",
         "...1221551221...",
         "....11.11.11....",
         "................",
         "................",
         "................"]}
     ],
     script:`# Moth. Rests with its wings folded on bark, the fern or the rim, and
# now and then opens them. Flies in zigzags; any bird, spider, shake or
# puff of spores nearby sends it off.
traits moth, flyer, critter
layer 3
var n, where

anim flutter = fly1 fly2 @ 14

state flit:            # a moth never flies straight
  play flutter
  n = random(2, 5)
  repeat n
    move to clamp(x + random(-40, 40), 16, 224), random(30, 96) speed random(25, 40) arc random(-12, 12)
  end
  where = random(1, 10)
  if where <= 5 then
    move to perch branch speed 30 arc 6
  elif where <= 7 then
    move to perch fern speed 30 arc 6
  elif where <= 8 then
    move to perch mushroom speed 30 arc 6
  else
    move to perch rim speed 35 arc 8
  end
  go rest
end

state rest:
  frame rest
  repeat random(3, 6)
    wait random(10, 25) / 10
    if chance(35) then
      frame fly1       # opens its wings: the eyespots show
      wait random(5, 10) / 10
      frame rest
    end
  end
  go flit
end

state flee:
  play flutter
  emit dust 1          # a few scales
  move to clamp(x + pick(-1, 1) * random(30, 60), 16, 224), random(24, 70) speed 75 arc 14
  go flit
end

on near bird within 24: go flee end

on near spider within 20:
  if state != flee then go flee end
end

on signal startle:
  if dist(other) < 50 then go flee end
end

on signal puff:
  if dist(other) < 50 then go flee end
end

on poke:
  emit dust 2
  go flee
end
`,
     start:{x:40,y:50}},
    {name:"Chickadee", w:16, h:16,
     palette:["#000000","#1c1c22","#8a929c","#f4f1e8","#dcae7c","#3a3a40","#c3c9d0","#5d6570"],
     frames:[
       {name:"sit", rows:[
         "................",
         "................",
         "................",
         "................",
         ".........1111...",
         "........111111..",
         "........1113111.",
         "........133331..",
         "...11111133311..",
         ".11222777211131.",
         "12227667723331..",
         ".1127777733441..",
         "..11222234441...",
         "....1111111.....",
         "......5..5......",
         ".....55..55....."]},
       {name:"look", rows:[
         "................",
         "................",
         "................",
         "................",
         ".........1111...",
         "........111111..",
         ".......1131111..",
         "........133331..",
         "...11111133311..",
         ".11222777211131.",
         "12227667723331..",
         ".1127777733441..",
         "..11222234441...",
         "....1111111.....",
         "......5..5......",
         ".....55..55....."]},
       {name:"peck", rows:[
         "................",
         "................",
         "................",
         "................",
         "................",
         "................",
         ".11.............",
         ".1211...........",
         "..12221111......",
         "..122777222111..",
         "...1277667211111",
         "...1277777211311",
         "....122222133331",
         "....11444111111.",
         "......1111....1.",
         ".......5..5....."]},
       {name:"flap1", rows:[
         "................",
         "................",
         "....11..........",
         "...1661.........",
         "...16661.1111...",
         "....1666111111..",
         ".....1661113111.",
         "......16133331..",
         "...11111133311..",
         ".11222222211131.",
         "12222222223331..",
         ".1122222233441..",
         "..11222234441...",
         "....1111111.....",
         "................",
         "................"]},
       {name:"flap2", rows:[
         "................",
         "................",
         "................",
         "................",
         ".........1111...",
         "........111111..",
         "........1113111.",
         "........133331..",
         "...11111133311..",
         ".11222222211131.",
         "12222222223331..",
         ".1122777733441..",
         "..117666671441..",
         "...1666661111...",
         "....16661.......",
         ".....111........"]}
     ],
     script:`# Chickadee. Flits between the branch, the fern, the rim and the soil;
# what it lands on gives under it. Darts at moths, drinks where the dew
# splashes, and is off at the first shake.
traits bird, flyer
layer 3
var looks, where, tgt

anim flap   = flap1 flap2 @ 12
anim settle = flap1 flap2 sit @ 10
anim peck   = peck sit @ 5

state sit:
  if perched then signal landed to perched end
  play settle once
  looks = random(3, 6)
  repeat looks
    wait random(6, 16) / 10
    if grounded and chance(45) then
      play peck once   # pecks at the moss
      if chance(50) then emit dust 1 end
    elif chance(40) then
      frame look       # head turned back
      wait random(4, 9) / 10
      frame sit
    else
      face -facing
    end
    if grounded and chance(30) then hop random(-14, 14) 4 end
    if onperch and chance(25) then hop 0 3 end   # a bob on the twig
  end
  go fly

  on near moth within 40:
    if chance(50) then tgt = other; go chase end
  end
end

state fly:
  play flap
  where = random(1, 10)
  if where <= 4 then
    move to perch branch speed 80 arc -20   # a swoop: dips, then rises
  elif where <= 6 then
    move to perch fern speed 70 arc -14
  elif where <= 8 then
    move to perch rim speed 90 arc 18
  else
    move to ground speed 70 arc 16
  end
  go sit
end

state chase:           # darts at the moth (and always misses)
  play flap
  move to tgt speed 120 arc -8
  go fly
end

state drink:           # down to the splash for a sip
  play flap
  move to tgt.x - 10, soil speed 80 arc 14
  face right
  repeat 2
    play peck once
    wait 0.3
  end
  go sit
end

on poke:
  say "dee-dee!" 1s
  go fly
end

on signal startle:
  if perched == other or (grounded and dist(other) < 30) then go fly end
end

on signal splash:
  if state == sit and chance(85) then tgt = other; go drink end
end
`,
     start:{x:220,y:22}}
  ]
};
