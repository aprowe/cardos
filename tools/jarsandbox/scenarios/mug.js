/* The mug: a hand-built Jarscript 3 scenario for the sandbox (tools/jarsandbox).
   A tipped-over mug, the mouse living in it, and the busy little world around it.
   Sprites are rows of palette digits ('.' is slot 0, see-through). */
SCENARIOS.mug = {
  title: "The mug",
  about: "A tipped-over enamel mug lies on its side in the moss, a mouse living in its dark mouth. The mouse's eyes shine from inside; it peeks out, scurries over to nibble a sugar cube that sits in a teaspoon, and runs home. A frog hops between the mug's top, the clover and the soil, croaks, and snaps its tongue at a fly that buzzes about landing on sweet things. A ladybug crawls along the tops of the mug and the spoon and flies off whenever the frog comes close; an ant patrols by its nest and marches over to carry crumbs home. Chains: the frog landing on the mug rattles it and the mouse peeks out; poking the mug rocks it and the mouse bolts while the frog leaps off; tapping the spoon flips the sugar into the air, and where it lands crumbs scatter, which brings the ant over and the mouse out.",
  chain: [
    "Frog leaps onto the mug -> signal landed -> Mug clunks (shake, dust) and signals rattle -> Mouse inside peeks out of the mouth",
    "Poke the mug -> it rocks (clonk!) and signals startle -> Mouse inside bolts out and away (eek!), then runs home; Frog, Fly or Ladybug perched on top take off",
    "Fly comes within 22 px of the sitting Frog -> Frog's tongue shoots out, signal snap to the fly -> Fly zips away (bzz!); the Frog turns to watch it whenever it is within 48",
    "Fly or Ladybug lands on the Clover -> signal landed -> Clover bobs and rustles (and a Frog landing knocks leaves loose)",
    "Poke the Clover -> it shakes and drops leaves, signal startle -> whoever perched on it (Frog, Fly, Ladybug) takes off; a Mouse out nearby runs home",
    "Poke the Teaspoon -> it seesaws (tink!) and signals clink -> the Sugar cube in its bowl flips into the air and tumbles; a Fly close by zips off",
    "Sugar cube comes down hard on the soil, glass, mug or spoon -> sparkle and dust, signal crumbs -> Ant marches over, tugs a crumb free and carries it home; Mouse in the mug comes out to forage",
    "Sugar cube rolls into the mug -> bump -> Mug shakes and signals rattle -> Mouse peeks out",
    "Sugar cube lands on the spoon -> bump -> Teaspoon says tink and shakes",
    "Mouse nibbles the Sugar cube -> signal nibble -> the cube shakes, sparkles, sometimes loses a bite, and signals crumbs -> Ant comes for them",
    "Fly lands on the Sugar cube / Teaspoon -> signal landed -> the cube glints / the spoon jiggles",
    "Frog comes within 24 px of the crawling Ladybug -> Ladybug opens her wing cases and flies off",
    "Frog comes within 20 px of the Mouse out on the soil -> eep! -> Mouse dashes home into the mug",
  ],
  gaps: [
    "No way to hide part of a sprite or be 'inside' another: the Mouse is drawn in front of the Mug and fakes being inside with an 'eyes' frame (two pale pixels that sit in the mug's dark mouth) and a 'peek' frame (head only). A clip/behind-this-item flag, or reading the owner's pixels, would make this real.",
    "One thing cannot push another: the Teaspoon cannot fling the Sugar cube, so it broadcasts clink and the cube flings itself if dist(other) < 30. A `push ITEM vx, vy` (or the spoon's angle tilting what rests on its solid) would do it honestly.",
    "Signals go to one item or to all; there is no 'signal within R'. Startle and clink go to everyone and each listener checks perched == other or dist(other) itself.",
    "`walk` on a perch that is not on a solid walks off into the air (nothing stops it). The Ladybug only walks when on furniture (has(perched, furniture)) and the Mug's and Spoon's perches are placed exactly on the top of their solid boxes so `walk` stops at the edges.",
    "Any solid blocks walkers at ground level (their 16 px box overlaps it), so the Ant and Mouse cross the jar with `move to X, jarh` paths, which ignore solids and pass through the spoon. A walker that climbs over a low solid would help.",
    "Only one solid per item: the Mug's handle and the Spoon's raised bowl cannot be separate boxes.",
    "Perches and solids do not rotate with `angle` (or flip with facing): the rocking Mug and seesawing Spoon carry their riders level.",
    "`other` is only meaningful inside the handler; the Frog copies it into `bug` before `go snap`.",
    "Cannot read another item's state or vars: the Mug cannot know the Mouse is home, so it rattles to all and the Mouse decides. A `.state` field would let a scene ask.",
    "No spawning: crumbs are particles (spark, dust) and the Ant's carried crumb is a frame; the Sugar cube cannot really get smaller than 'bite2' or be used up.",
  ],
  items: [
    {
      name: "Clover", w: 32, h: 32,
      palette: ['#000000', '#1f3d26', '#3f8a3e', '#6dbb4f', '#a8dd7a', '#2f6b33', '#f3eef6', '#e7a6c8'],
      frames: [
        {name: 'still', rows: [
          '................................',
          '.........11..11.................',
          '.......1133113311...............',
          '.......1333333331......1111.....',
          '.......1433333331.....167761....',
          '.......1336336331....17777771...',
          '.......1133333311....17767761...',
          '.........113311......16777771...',
          '....1111...11...1111.17767771...',
          '...1333311....1133331.177761....',
          '...13333331..13333431..1171.....',
          '...13336331..13363331....5......',
          '...13333331..13333331....5......',
          '....1333631..1363331....55......',
          '.....13333311333331.....5.......',
          '.....111431.5333331.....5.......',
          '..111333131.513331......5.......',
          '..14333311..5.111......55.......',
          '..163361...55..........5........',
          '...1111....5...........5........',
          '111....111.5..........55........',
          '1631111361.5..........5.........',
          '1333313331.5.........55.........',
          '.13631631.5..........55.........',
          '.14311331.555.......555.........',
          '..11.531...55.......55..........',
          '.....55....55......555..........',
          '......5....55......55...........',
          '......5....55.....555...........',
          '......55...555....55............',
          '.......5....55...555............',
          '.......5....55...55.............',
        ]},
        {name: 'rustle', rows: [
          '................................',
          '..........11..11................',
          '........1133113311..............',
          '........1333333331......11111...',
          '........1433333331.....1677671..',
          '........1336336331....177777771.',
          '........1133333311....177677671.',
          '..........113311......167777771.',
          '.....1111...11...1111.177767771.',
          '....1333311....1133331.1777761..',
          '....13333331..13333431..11711...',
          '....13336331..13363331....5.....',
          '....13333331..13333331....5.....',
          '.....1333631..1363331.....5.....',
          '......13333311333331.....55.....',
          '......113431.5333331.....5......',
          '..1111331331.513331......5......',
          '..143333111.55.111......55......',
          '..163361....5...........5.......',
          '...1111.....5..........55.......',
          '111....111.55..........5........',
          '1631111361.5...........5........',
          '1333313331.5..........5.........',
          '.13631631..5..........55........',
          '.14311331..55........55.........',
          '..11.531...55.......555.........',
          '.....55....55.......55..........',
          '......5....55......555..........',
          '......5....55......55...........',
          '......55...555....55............',
          '.......5....55...555............',
          '.......5....55...55.............',
        ]},
      ],
      start: {x: 28, y: null},
      script: `# Clover. A sprig with a pink blossom. Things perch on its leaves;
# it bobs when they land and shakes them off when poked.
traits plant, clover
layer 0
perch 12, 2            # the top leaf
perch 5, 8
perch 18, 8
perch 25, 3            # the blossom
perch 4, 20            # the little sprig

anim rustle = rustle still rustle still @ 10

state idle:
  frame still
  loop
    wait random(5, 11)
    sway 1 1.6s        # a breeze
  end
end

on signal landed:
  sway 3 1.2s
  play rustle once
  if has(other, jumper) or chance(30) then emit leaf random(1, 2) end
end

on poke:
  sway 4 1.5s
  play rustle once
  emit leaf 3
  signal startle       # whatever sits on it takes off
end
`,
    },
    {
      name: "Mug", w: 32, h: 32,
      palette: ['#000000', '#2a2430', '#f2e9d8', '#cdbd9f', '#4f86c6', '#e0605a', '#4b3b36', '#ffffff'],
      frames: [
        {name: 'mug', rows: [
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '.............11.................',
          '...........112211...............',
          '..........12222221..............',
          '..........12111121..............',
          '.........121....121.............',
          '.........121....121.............',
          '......11122211112221111.........',
          '.....12222222222222244411.......',
          '....1222222222222224446641......',
          '....1777777777777774466661......',
          '...127777777777777446666641.....',
          '...122222222222222446116661.....',
          '...1222222222222244611166641....',
          '...1222222222222244611166641....',
          '...1222222225252244611116641....',
          '...1222222255555244611116641....',
          '...1222222225552244611116641....',
          '...1222222222522244611116641....',
          '...1222222222222244611116641....',
          '...1222222222222244611116641....',
          '...1222222222222244611166641....',
          '...133333333333333441116661.....',
          '...133333333333333446116641.....',
          '....1333333333333334466661......',
          '....1333333333333334446641......',
          '.....13333333333333344411.......',
          '......11111111111111111.........',
        ]},
      ],
      start: {x: 96, y: null},
      script: `# Mug. Tipped over on its side: a top to stand on, a dark mouth
# to hide in. Poke it and it rocks; whoever is inside bolts.
traits mug, cave, furniture
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
  if has(other, flyer) then return end   # a fly or a ladybug weighs nothing
  shake 0.3s
  emit dust 1
  signal rattle        # heard inside
end

on bump:               # the sugar cube rolled into it
  if impact > 20 then shake 0.25s; signal rattle end
end
`,
    },
    {
      name: "Teaspoon", w: 32, h: 16,
      palette: ['#000000', '#3e4550', '#b4bfca', '#e6ecf1', '#8793a1', '#ffffff', '#97a5b3', '#000000'],
      frames: [
        {name: 'spoon', rows: [
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '................................',
          '......................111111....',
          '....................1135666211..',
          '.111111111111111111136666666621.',
          '1233333333333333333366666666621.',
          '1222222222222222222146666666421.',
          '.11111111111111111111444444221..',
          '......................11111111..',
        ]},
      ],
      start: {x: 172, y: null},
      script: `# Teaspoon. Lies on the soil. Tap it and it seesaws with a tink,
# flipping whatever sits in its bowl.
traits spoon, furniture, metal
layer 1
solid 0, 11, 31, 4
perch 4, 11
perch 13, 11
perch 26, 11           # in the bowl
var k

state still:
  angle = 0
  frame spoon
  loop
    wait random(6, 14)
    if chance(30) then emit spark 1 end   # catches the light
  end
end

state seesaw:
  say "tink!" 0.8s
  signal clink         # the sugar flips, a fly close by leaves
  k = 8
  repeat 3
    angle = -k         # bowl end up
    wait 0.1
    angle = k / 2
    wait 0.1
    k = k / 2
  end
  angle = 0
  go still
end

on poke: go seesaw end

on bump:               # the sugar cube came down on it
  if impact > 25 then say "tink" 0.6s; shake 0.2s end
end

on signal landed: shake 0.15s end
`,
    },
    {
      name: "Sugar cube", w: 16, h: 16,
      palette: ['#000000', '#9a93a8', '#ffffff', '#ebe7f0', '#cfc8db', '#ffe98a', '#dcd6e4', '#000000'],
      frames: [
        {name: 'cube', rows: [
          '................',
          '................',
          '................',
          '.....11111111...',
          '....13333353311.',
          '...111111111431.',
          '...122222221441.',
          '...122622221441.',
          '...122222621441.',
          '...126222221441.',
          '...122222221441.',
          '...12222222141..',
          '...111111111....',
          '................',
          '................',
          '................',
        ]},
        {name: 'bite1', rows: [
          '................',
          '................',
          '................',
          '.....11111......',
          '....1333331.11..',
          '...11111111431..',
          '...122222221441.',
          '...122622221441.',
          '...122222621441.',
          '...126222221441.',
          '...122222221441.',
          '...12222222141..',
          '...111111111....',
          '................',
          '................',
          '................',
        ]},
        {name: 'bite2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '......111.......',
          '.....12221.11...',
          '....1222221441..',
          '...112262221441.',
          '...126222221441.',
          '...122222221441.',
          '...12222222141..',
          '...111111111....',
          '................',
          '................',
          '................',
        ]},
      ],
      start: {x: 182, y: 108},
      script: `# Sugar cube. Sits in the teaspoon and glints. Nibbled by the mouse,
# landed on by the fly; a tap on the spoon flips it into the air.
traits sugar, sweet, food
layer 2
body 4
perch 8, 3
var bites

state rest:
  physics on
  roll off
  bounce 30
  friction 90
  frame bites + 1
  loop
    wait random(4, 9)
    if chance(40) then emit spark 1 end   # a glint
  end
end

state tumble:                # in the air, turning over
  physics on
  roll on
  wait 0.2s
  wait until grounded and speed < 3
  roll off
  angle = round(angle / 90) * 90         # comes to rest on a face
  go rest
end

on signal clink:             # the spoon was tapped
  if dist(other) < 30 then
    vx = (other.x - x) + random(-12, 12)   # up and back over the spoon
    vy = -random(110, 150)
    go tumble
  end
end

on signal nibble:            # the mouse
  shake 0.3s
  emit spark 1
  if bites < 2 and chance(35) then bites += 1; frame bites + 1 end
  signal crumbs
end

on signal landed:            # the fly
  emit spark 1
end

on hit:                      # came down on the soil or the glass
  if impact > 40 then emit dust 2; emit spark 2; signal crumbs end
end

on bump:                     # came down on the mug or the spoon
  if impact > 40 then emit spark 1; signal crumbs end
end

on poke:
  vx = random(-40, 40)
  vy = -random(70, 100)
  go tumble
end
`,
    },
    {
      name: "Mouse", w: 16, h: 16,
      palette: ['#000000', '#2b2330', '#a39488', '#e3d7ca', '#ee9fae', '#121218', '#fff4c4', '#7d6e64'],
      frames: [
        {name: 'run1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......14441....',
          '.......14441....',
          '....1112242211..',
          '...12222222251..',
          '..1222222222221.',
          '..1222233332214.',
          '4.12223333331...',
          '.4.121211121....',
          '..4.4.4...4.4...',
        ]},
        {name: 'run2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......14441....',
          '.......14441....',
          '....1112242211..',
          '...12222222251..',
          '..1222222222221.',
          '.42222233332214.',
          '..11223333331...',
          '..4.11211211....',
          '....4.4..4.4....',
        ]},
        {name: 'sit', rows: [
          '................',
          '................',
          '................',
          '................',
          '.......111......',
          '......14441.....',
          '......144421....',
          '.......1422211..',
          '.......12222521.',
          '......1222222224',
          '....11223333111.',
          '...122233331....',
          '...12222331.....',
          '..42222221......',
          '.4.112111.......',
          '...4.4..4.4.....',
        ]},
        {name: 'nibble', rows: [
          '................',
          '................',
          '................',
          '................',
          '.......111......',
          '......14441.....',
          '......144421....',
          '.......1422211..',
          '.......12222521.',
          '......1222222114',
          '....112233334...',
          '...1222333344...',
          '...12222331.....',
          '..42222221......',
          '.4.112111.......',
          '...4.4..4.4.....',
        ]},
        {name: 'peek', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '..........111...',
          '.........14441..',
          '.........144421.',
          '.......7.1422221',
          '......777222521.',
          '.......72222224.',
          '........721111..',
          '........77......',
          '................',
          '................',
        ]},
        {name: 'eyes', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '........6.6.....',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
        ]},
      ],
      start: {x: 101, y: 107},
      script: `# Mouse. Lives in the mug: its eyes shine in the dark, it peeks out
# of the mouth, scurries off to nibble sugar, and runs home.
traits mouse, critter
layer 2
var home, food, tx

anim run   = run1 run2 @ 12
anim munch = nibble sit nibble sit @ 6

state inside:          # only two shining eyes in the dark
  home = find(cave)
  if home == 0 then wait 2s; go inside end
  move to home.x + 5, home.y - 5 speed 30
  face right
  frame eyes
  wait random(3, 7)
  if chance(55) then go peek end
  go out
end

state peek:            # head out of the mouth, looking about
  face right
  frame peek
  move to home.x + 9, home.y - 2 speed 14
  repeat random(2, 4)
    wait random(6, 16) / 10
    if chance(15) then say "squeak" 0.8s end
  end
  if chance(60) then go out end
  frame eyes
  move to home.x + 5, home.y - 5 speed 14
  go inside
end

state out:             # scurry out and forage
  face right
  play run
  move to home.x + 9, home.y - 2 speed 30
  move to home.x + 26, jarh speed 40 arc 2
  food = find(sweet)
  if food and chance(70) then
    tx = food.x - 10
    if x > food.x then tx = food.x + 10 end
    move to tx, jarh speed 40 arc 2
    face toward food
    play munch
    repeat random(2, 4)
      wait random(8, 14) / 10
      signal nibble to food
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
  move to home.x + 24, jarh speed 55 arc 2
  move to home.x + 9, home.y - 2 speed 30
  go inside
end

state bolt:            # startled out of the mug and away
  say "eek!" 1s
  face right
  play run
  move to home.x + 9, home.y - 2 speed 60
  move to home.x + random(45, 90), jarh speed 80 arc 4
  frame sit
  wait random(2, 4)
  go dash
end

on signal rattle:      # a thump on the roof
  if state == inside then go peek end
end

on signal crumbs:      # sugar crumbs somewhere: worth a look
  if state == inside or state == peek then go out end
end

on signal startle:
  if has(other, cave) and (state == inside or state == peek) then go bolt
  elif state == out and dist(other) < 40 then go dash end
end

on near frog within 20:
  if state == out then say "eep!" 0.8s; go dash end
end

on poke:
  say "squeak!" 0.8s
  if state == inside then go peek
  elif state == out then go dash end
end
`,
    },
    {
      name: "Frog", w: 16, h: 16,
      palette: ['#000000', '#1f2a1c', '#6fb34a', '#cfe38a', '#3f7a34', '#121218', '#f6f0d0', '#ef7f8e'],
      frames: [
        {name: 'sit', rows: [
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......16651....',
          '.......16551.1..',
          '.....1122222121.',
          '....12222222221.',
          '...1222222222221',
          '..14422222111111',
          '..1444422233311.',
          '.124444422331...',
          '.12444442221311.',
          '.1222222111.11..',
          '11111111..11111.',
        ]},
        {name: 'blink', rows: [
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......12221....',
          '.......11111.1..',
          '.....1122222121.',
          '....12222222221.',
          '...1222222222221',
          '..14422222111111',
          '..1444422233311.',
          '.124444422331...',
          '.12444442221311.',
          '.1222222111.11..',
          '11111111..11111.',
        ]},
        {name: 'puff', rows: [
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......16651....',
          '.......16551.1..',
          '.....1122222121.',
          '....12222222221.',
          '...1222222222221',
          '..14422222111111',
          '..14444222333331',
          '.12444442333331.',
          '.1244444223131..',
          '.1222222111.11..',
          '11111111..11111.',
        ]},
        {name: 'tongue', rows: [
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......16651....',
          '.......16551.1..',
          '.....1122222121.',
          '....12222222221.',
          '...1222222222221',
          '..14422222111111',
          '..1444422233377.',
          '.124444422331.77',
          '.12444442221311.',
          '.1222222111.11..',
          '11111111..11111.',
        ]},
        {name: 'leap', rows: [
          '................',
          '................',
          '................',
          '................',
          '........111.....',
          '.......16651....',
          '......126551....',
          '....1122222211..',
          '..112222222221..',
          '114222222111111.',
          '1441422223331...',
          '.11.1411113111..',
          '1....1....1..1..',
          '1.........1...1.',
          '................',
          '................',
        ]},
      ],
      start: {x: 60, y: null},
      script: `# Frog. Sits on the mug, croaks, watches the fly, leaps about.
# A fly that comes too close gets the tongue (and always escapes).
traits frog, critter, jumper
layer 2
var bug, n, where

anim croak = puff sit puff sit @ 5
anim blink = blink sit @ 8

state sit:
  on near fly within 22:     # close enough: the tongue!
    bug = other
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
    move to perch mug speed 80 arc 20
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
  face toward bug
  frame tongue
  signal snap to bug
  wait 0.3s
  frame sit
  if chance(50) then say "blep" 0.8s end
  wait 1.2s
  go sit
end

on near fly within 48:       # follows it with its eyes
  if state == sit then face toward other end
end

on poke:
  say "ribbit!" 1s
  go leap
end

on signal startle:           # the thing under it moved
  if perched == other then go leap end
end
`,
    },
    {
      name: "Fly", w: 16, h: 16,
      palette: ['#000000', '#1a1c24', '#3b4152', '#7d8aa6', '#c8402e', '#e4f0f8', '#a9c3d6', '#000000'],
      frames: [
        {name: 'sit', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '.....66.........',
          '....6556........',
          '.....6556.......',
          '......12221.....',
          '.....1232241....',
          '......12221.....',
          '.....1.1.1......',
        ]},
        {name: 'rub', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '.....66.........',
          '....6556........',
          '.....6556.......',
          '......12221.....',
          '.....12322411...',
          '......122211....',
          '.....1.1........',
        ]},
        {name: 'fly1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '....666.........',
          '...65556........',
          '....65556.......',
          '.....6556.......',
          '......12221.....',
          '.....1232241....',
          '......12221.....',
          '................',
          '................',
        ]},
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
          '......12221.....',
          '.....1232241....',
          '....6612221.....',
          '...655566.......',
          '....666.........',
        ]},
      ],
      start: {x: 40, y: 50},
      script: `# Fly. Buzzes in loops, lands on sweet things and rubs its hands,
# zips off when the frog's tongue comes out.
traits fly, flyer, bug
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
    move to perch mug speed 50 arc 8
  elif where <= 9 then
    move to perch spoon speed 50 arc 8
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

on signal clink:             # the spoon rang right beside it
  if dist(other) < 40 then go zip end
end

on signal startle:
  if perched == other then go buzz end
end

on poke:
  go zip
end
`,
    },
    {
      name: "Ladybug", w: 16, h: 16,
      palette: ['#000000', '#1a1418', '#d8342c', '#f58a6a', '#2a2230', '#2a2230', '#e9dfc4', '#ffffff'],
      frames: [
        {name: 'walk1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '....11111.......',
          '...1233221......',
          '..12321222111...',
          '..122221221551..',
          '..121222221571..',
          '..11111111111...',
          '...1..1..1......',
        ]},
        {name: 'walk2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '....11111.......',
          '...1233221......',
          '..12321222111...',
          '..122221221551..',
          '..121222221571..',
          '..11111111111...',
          '..1..1..1.......',
        ]},
        {name: 'wing1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '..666...........',
          '.66661111.......',
          '.6661233221.....',
          '...12321222111..',
          '...122221221551.',
          '...121222221571.',
          '...11111111111..',
          '....1..1..1.....',
          '................',
          '................',
        ]},
        {name: 'wing2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '.....11111......',
          '....1233221.....',
          '...12321222111..',
          '...122221221551.',
          '...121222221571.',
          '.666111111111...',
          '66661..1..1.....',
          '.666............',
          '................',
        ]},
      ],
      start: {x: 140, y: null},
      script: `# Ladybug. Crawls along the tops of things, opens her wing cases
# and flies to a leaf; keeps well away from the frog.
traits ladybug, flyer, bug
layer 2
var dir, n, where

anim crawl = walk1 walk2 @ 8
anim fly   = wing1 wing2 @ 16

state crawl:
  dir = pick(left, right)
  n = random(2, 4)
  repeat n
    if onperch and not has(perched, furniture) then
      wait random(10, 20) / 10   # on a leaf: no walking off it
      face -facing
    else
      play crawl
      walk dir * random(6, 18) speed 6
      frame walk1
      if blocked or chance(30) then dir = -dir end
      wait random(6, 14) / 10
    end
  end
  go fly
end

state fly:
  play fly
  where = random(1, 10)
  if where <= 4 then
    move to perch plant speed 32 arc 16
  elif where <= 7 then
    move to perch mug speed 32 arc 14
  elif where <= 8 then
    move to perch spoon speed 32 arc 10
  else
    move to ground speed 30 arc 12
  end
  frame walk1
  if perched then signal landed to perched end
  go crawl
end

on near frog within 24:
  if state == crawl then go fly end
end

on signal startle:
  if perched == other then go fly end
end

on poke:
  say "!" 0.6s
  go fly
end
`,
    },
    {
      name: "Ant", w: 16, h: 16,
      palette: ['#000000', '#1e1612', '#6a3420', '#a0583a', '#1e1612', '#fffaf0', '#ddd4c2', '#000000'],
      frames: [
        {name: 'walk1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '...........1....',
          '............1...',
          '..111..1...111..',
          '.12321.1..1231..',
          '.122211211121...',
          '..111..111..1...',
          '..1..1.1..1.....',
        ]},
        {name: 'walk2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '...........1....',
          '............1...',
          '..111..1...111..',
          '.12321.1..1231..',
          '.122211211121...',
          '..111..111..1...',
          '...1..1..1..1...',
        ]},
        {name: 'carry1', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '...........1.55.',
          '............1565',
          '..111..1...1166.',
          '.12321.1..1231..',
          '.122211211121...',
          '..111..111..1...',
          '..1..1.1..1.....',
        ]},
        {name: 'carry2', rows: [
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '................',
          '...........1.55.',
          '............1565',
          '..111..1...1166.',
          '.12321.1..1231..',
          '.122211211121...',
          '..111..111..1...',
          '...1..1..1..1...',
        ]},
      ],
      start: {x: 218, y: null},
      script: `# Ant. Patrols by its nest on the right; when crumbs fall it marches
# over, tugs one free and carries it home.
traits ant, bug, critter
layer 2
var nest = 218, food, n, haul

anim walk  = walk1 walk2 @ 12
anim carry = carry1 carry2 @ 12

state patrol:
  n = random(2, 4)
  repeat n
    play walk
    walk pick(left, right) * random(6, 18) speed 14
    if abs(x - nest) > 20 then walk nest - x speed 14 end
    frame walk1
    wait random(3, 9) / 10
  end
  if chance(15) then go forage end
  go patrol
end

state forage:
  food = find(sweet)
  if food == 0 then go patrol end
  play walk
  move to food.x + pick(-7, 7), jarh speed 16 arc 1
  face toward food
  frame walk1
  repeat 3
    shake 0.2s               # tugging a crumb free
    wait 0.4s
  end
  haul += 1
  go home
end

state home:
  play carry
  move to nest + random(-6, 6), jarh speed 12 arc 1
  frame walk1                # crumb set down
  if haul % 3 == 0 then emit heart 1 end
  wait 1s
  go patrol
end

state startled:
  say "!" 0.6s
  frame walk1
  hop 0 5
  go patrol
end

on signal crumbs:
  if state == patrol then go forage end
end

on poke: go startled end
`,
    },
  ],
};
