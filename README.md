# Asteroid Drifter

A 2D vector-graphics game in C++17 and OpenGL 3.3. You are a spaceman in an
endless asteroid field with somewhere to be: a **beacon** a few thousand units
away and a clock running down. Turrets, drones and now and then a whole warship fight you on the way, and
between levels you spend what you earn at a **supply depot**.

Every asteroid is fully destructible. Holes are real holes, and when you cut a
rock in two you get two rocks, each with its own mass, spin and gravity.

There is also a **versus mode**, against bots or over the network with up to eight
players, where everyone shoots everyone in an arena of asteroids (see *Versus and
multiplayer*).

**No external dependencies.** No GLFW, no GLAD, no GLM, no CMake required. The
whole thing links against `opengl32`, `gdi32`, `winmm` (sound) and `ws2_32`
(networking), which ship with Windows.

---

## The game

**Each level** puts a beacon at a random distance (roughly 2400-3400 units on
level 1, growing 4% a level) and gives you a time limit worked out from it,
about 40 seconds. The limit is deliberately roomy: the point is to have time to
*fight*, not just run. Reach the beacon and you are paid **credits** for the
level and for every second left on the clock, then the depot opens. Run out of
time or of suit and you lose a life (see below).

**Lives.** You start a run with **3 lives**, drawn as little helmets above the
suit bar. Running out of time or of suit costs one, and the level then restarts
after a short beat: same beacon, same gauntlet, full clock, full suit, back on a
rock near where you began. What you bought stays bought and any ammunition you
burned stays burned, but credits go back to what they were when the level began,
so dying costs you and cannot be used to farm turrets. Lose the last life and
the run is over; **R** or **Enter** starts again from level 1 with nothing.
**Every fifth level** (6, 11, 16...) tops the lives back up to 3.

While the beacon is off-screen, a faint stream of chevrons runs from the
spaceman toward it and a big pulsing arrow sits on the screen edge in the same
direction (kept clear of the suit bar). Both vanish as soon as any part of the
beacon is on screen: once you can see it, you do not need telling.

**The HUD.** The suit is the big bar at the bottom centre, because it is the one
number that ends the run: blue when healthy, amber below 60%, and a pulsing red
below 30%, flaring when you are hit. Aim is a short dotted line pointing from the spaceman toward
the cursor, a bright arrowhead just ahead of him, and a small quiet target that replaces
the mouse cursor (the system pointer is hidden over the game, and returns over the
title bar and window frame).

**Enemies** are laid out along the route and wake up as you approach:

* **Turrets** are bolted to rocks. They track and lead you, fire bursts, and
  from level 2 some carry homing missiles. Shoot the ground out from under one
  and it falls.
* **Drones** fly. They hold a fighting distance, weave, and shoot bullets, and
  later missiles.
* **Homing missiles** turn onto you and explode on the first rock they meet, so
  cover works, and they can be shot down (for no reward: they are not worth
  farming).

Everything scales each level: how many there are, how accurately and often they
shoot, how hard they hit, how fast their missiles fly and turn, and how much
health they have. Level 1 is gentle; level 4 is not.

**Warships.** On level 2, and then about every other level (mostly the even
ones, now and then an odd one from level 7), a big generated warship is berthed
somewhere on the way to the beacon, in a cleared patch of sky. Each one is built
from a random seed, so no two are alike:

* **Shape.** One of four families (dart, blocky cruiser, forked-bow carrier,
  wedge) with random proportions, steps and swept wings, drawn as a double
  outline with panel lines, a bridge and flickering exhaust. Roughly 450-800
  units long: a rock is a pebble beside one.
* **Colour and name.** A random hue, and a name like *OBSIDIAN LEVIATHAN*.
* **Weapons.** 3 to 9 of them in mirrored pairs on the edge of the hull, more on
  later levels. Each is one of four kinds: **guns** (bursts of bullets),
  **missile pods** (homing missiles), **flak** (a fan of five slow pellets) and
  **cannons** (one big, slow, heavy shell, with a glow on the barrel for 3/4 of a
  second before it fires).
* **The hull is armour.** It is solid: you bounce off it. A rifle round does
  under half its damage to it, but blasts (shell splash, salvo, nuke) go straight
  through. A nuke takes 60% of a full hull, so it takes two, or one and a lot of
  patience.
* **Its weapons are ordinary targets.** Every one has its own health, homing
  weapons lock onto them, and each one you destroy takes a slice off the hull too.
  Destroy the hull and the ship breaks apart along its own edges.
* **Reward.** 200 credits plus 40 per level for the ship, and 30 for each weapon
  you shoot off. Killing it is optional, and the clock gets 8 extra seconds on
  levels that have one, since flying round it is a detour.

While one is in range its name and hull bar sit under the clock, and a pointer
tracks it while it is off screen.

**Credits** come from kills (50 per turret, 75 per drone, more for warships) and finishing levels
(100 x the level number, plus 10 per second left).

### The supply depot

You start with a rifle and a rocket, and nothing else. Between levels the world
freezes and the depot opens. Click a row, or press its number:

| Item | Key | Price | What it does |
| --- | --- | --- | --- |
| **Homing shell** | `F` / middle mouse | 300 | The charge shot. A heavy shell that curves onto the nearest enemy in front of it. |
| **Missile salvo** | `G` | 450 (6 salvos), then 150 for 6 more | Five small missiles at once, each sent after a *different* target. With fewer than five targets they double up; incoming enemy missiles count as targets. |
| **Nuke** | `N` | 400 for 3 | A slow grenade, dragged hard by gravity, with a 3.6 s fuse shown on the bomb itself. A huge blast: it vaporises rock, kills everything in its radius and hurts you if you are close. |
| **Force field** | `X` | 500 | A bubble that shoves bullets, missiles, drones and rocks away. Costs energy while it is on. |
| **Blast shield** | hold Left Alt | 350, then 120 to refill | A 30 degree plate held toward the cursor. It stops bullets and missiles that reach it and soaks up blasts that go off inside its arc, spending charge equal to the damage it stops. When it is empty it stays down until you refill it. |

Purchases last for the run. Nukes can only be bought, never found.

### Sound

Every sound is synthesised when the game starts (about 0.8 s, 9 MB), so there are
no sound files. The look is neon line-work with a bloom glow, and the sound is
meant to match it: **clean tones and bright pitch sweeps** (sine, triangle, a
little saw) for anything electronic, **filtered noise blasts** for anything that
explodes with a scatter of tiny high blips on top for the sparks, **struck-metal
partials** for the shield and the warships' hulls, and a wash of **space reverb**
under the big things.

* **Yours:** a zip for the rifle, a heavy thump with a shimmer for the homing
  shell, five staggered whooshes for the salvo, a thunk and a rising whine for the
  nuke, then a beep on every blink of its light (faster and higher as the fuse
  runs down) and a long, deep boom.
* **Theirs:** enemy bullets are lower and buzzier than yours, missiles launch
  with a rising whoosh, and a missile closing on you sets off a bi-bip warning
  that speeds up as it nears. Warship cannons whine up for 3/4 of a second before
  they fire, matching the glow on the barrel.
* **The world:** rocks tick when hit and crack when they split, explosions come in
  three sizes, and the spaceman has a jump chirp, a landing thud, a dry click on an
  empty tank, and a harsh zap when hurt. Below 35% suit there is a heartbeat that
  quickens as it drops.
* **Things that last** (the rocket, the force field, the shield, the heartbeat and
  a very quiet ambient drone) are loops whose volume follows the game and fades
  in and out, so nothing clicks.
* **The run:** a sting for each level, a sonar **ping from the beacon** that comes
  from its side of the stereo field and speeds up and brightens as you close in,
  a tick for each of the last eight seconds (higher for the last four), a rising
  arpeggio for reaching the beacon, a falling chord for losing your last life. The
  depot has its own soft chimes: a tick on hover, a coin for a purchase, a buzz
  for a refusal.
* **Warships:** a low brassy horn when one comes into range, a deep clang on the
  hull, and when one dies a chain of blasts down its length followed by a long
  groan.

Sounds are placed in the world: louder the nearer they are to the spaceman, and
panned by where they are on screen. **`M` turns sound off and on.**


### Versus and multiplayer

A separate mode where the players shoot each other. There are no levels, enemies,
clock or depot: an arena of asteroids, and everyone has the rifle, the shell and
the rocket.

    asteroid.exe -versus 3                 # practise against 3 bots, on your own
    asteroid.exe -host                     # host a match; 4790 is the default port
    asteroid.exe -host 4790 2              # ...with 2 bots in it as well
    asteroid.exe -join 192.168.1.20:4790   # join one
    asteroid.exe -join 192.168.1.20 -name ACE

* **Rules.** 100 suit and no healing. A rifle round does 5, a shell 42 plus a
  blast that hurts everyone in reach (including you). You are out for 3 seconds,
  then reappear on a rock away from the others, untouchable for 2. A wall of
  inward push marks the edge of the arena. A suicide costs a frag. **First to 10
  frags wins**, the result stays up for 8 seconds, and a new match starts (Enter
  starts it at once).
* **Bots** are sparring partners, not champions. They lead their shots but wobble,
  fire in bursts, jump about, and fly at you with the rocket; about 30% of their
  rounds land. Walking is off (see *Tuning*), so a bot can only leave its rock by
  jumping along the surface normal, which took some teaching.
* **The screen.** A scoreboard at the top, a kill feed down the right, a name tag
  and health bar over every opponent in view, an arrow to each one that is not,
  and a connection line (`CONNECTED   PING 28 MS   7 KB/S`) at the top left.
  Shots are tinted with their owner's colour.
* **Hosting.** One machine hosts and the others join by address. On a LAN or over
  a VPN (Tailscale, ZeroTier) that is all there is to it. Over the internet the
  host needs the UDP port forwarded on its router; there is no matchmaking server
  or NAT traversal, deliberately, because that would be something to run and pay
  for. The first `-host` will make Windows ask whether to allow the game through
  the firewall. Up to 8 players.

**How it works.** The host runs the only real simulation, exactly as an offline
match does. A client sends what its player is doing (aim, rocket, fire, and jump
and shell *counts* so a lost packet cannot swallow one) and draws what it is told.
It does not wait to be told where its own spaceman is: it runs the same
`stepPlayer()` locally against the rocks it knows about, and eases toward the
host's version whenever a snapshot arrives. Everyone else glides between
snapshots. Bullets on a client are only pictures; the host decides what they hit.

* **The world is replicated by its history, not its shape.** A rock is a pure
  function of its radius and seed plus the holes cut in it, so the wire never
  carries geometry: a rock costs about 48 bytes to introduce and a hole about 22.
  `World` keeps an optional journal of every change (`World::ops`), and
  `net::HostReplicator` turns it into messages that `net::ClientReplicator`
  replays on a world that started empty. There is one journal and one
  replicator per client, since each remembers what *that* client believes.
* **Rock motion is dead-reckoned.** The host sends a rock's position only when the
  client's guess has drifted, plus a slow refresh. Quantised to 20 bytes a rock.
* **A small reliable channel over UDP** (`net::Endpoint`): ordered, batched into
  packets, cumulative *and selective* acknowledgements piggybacked on everything,
  large messages fragmented, a resend timer that learns the round trip from
  echoed timestamps, and pacing (no more than 14 KB per tick, 60 messages in
  flight). Selective acks cut re-sends by 3-4x on a lossy link.
* **Divergence is audited and repaired.** Every shot-up rock is compared by a
  solid-sample count and centroid, about 50 a second; one that differs by more
  than noise is sent whole. It is deliberately tolerant: an exact hash turned a
  0.001-unit difference in one carve into 62 repairs and 913 KB.

**Measured** with two whole games in one process (`-netgametest`, the client's
world starting empty), including a run over real UDP sockets on the loopback
interface and one with a host and two clients:

| Link | Result |
| --- | --- |
| clean, 2% loss / 40 ms, 8% / 90 ms, 20% / 150 ms | worlds agree exactly at the end; kills, frags and names match on every screen |
| host to one client, while fighting | 7-8 KB/s (loss barely changes it: the resends are a small share) |
| joining | 33-43 KB for 650 rocks |
| a kill announced to a client | instantly on a good link; up to about half a second late at 20% loss / 150 ms (the death itself never is: it rides on the snapshots) |
| client's own spaceman | agrees with the host within 10 units on a clean link, 55 at 90 ms |

Two real processes (`-host ... -loopback` and `-join 127.0.0.1:...`, real windows
and rendering) connected at a 30 ms ping.

Two things this work found that are worth knowing. **A seed does not determine
the world** (`-synctest`): chunk generation asks about the rocks that happen to be
loaded, so the same chunk reached from two directions held different rocks (0 of 9
matched), and a joining client has to be sent its world. And **a snapshot can
beat the Welcome that says which player you are** on a lossy link, which briefly
made a client its own opponent; it now ignores snapshots until welcomed.

**What is not proven.** It has been run between processes on one machine, never
between two computers or through a router. The main risk is a different CPU:
carve arithmetic that differs in the last bits is tolerated up to a point (tested
with nudges of 0.05 units), and beyond it the audit repairs the rock, but a real
cross-machine difference could still be larger than that. Prediction of the
player against a rock that is itself being corrected is simple and untuned for
high latency: expect some rubber-banding above about 150 ms. Only the rifle, the
shell and the rocket exist in a match; the shield, force field, salvo and nukes
do not.

---

## Build

    build.bat            :: Windows - uses MSVC if available, else MinGW-w64
    ./build.sh           #  MSYS2 / Git Bash - MinGW-w64
    ./build.sh debug     #  unoptimised build with symbols

`build.bat` looks for `cl.exe` first (run it from a *Developer Command Prompt
for VS*), and otherwise falls back to `g++` from `C:\msys64\mingw64\bin`,
`C:\msys64\ucrt64\bin` or `C:\mingw64\bin`.

Both toolchains are tested. The binary lands in `build\asteroid.exe`.

Requirements: Windows (sound uses the built-in `winmm`), and a GPU with OpenGL 3.3 core (anything from about 2010
onward). Nothing to install or download.

---

## Controls

| Input | Action |
| --- | --- |
| `A` / `D` or arrows | Walk along the surface, or steer in flight (see *Tuning*: both speeds are currently 0) |
| `W` / `Up` / `Space` | Jump, and it kicks the rock back |
| Mouse | Aim |
| Left mouse | Rapid fire. Bullets tunnel, so hold it to drill through |
| Right mouse / `Shift` | Rocket: thrust toward the cursor, burns fuel |
| `Left Alt` (hold) | Blast shield, once bought. Right Alt / AltGr does nothing |
| `F` / middle mouse | Homing shell, once bought |
| `G` | Missile salvo, once bought |
| `N` | Throw a nuke, once bought |
| `X` | Force field on/off, once bought |
| Wheel / `Q` / `E` | Zoom out and in |
| `C` | Camera: fixed (default) or POV, where the world rolls so you stay upright |
| `R` | Restart the run from level 1 |
| `Enter` / `Space` | Leave the depot; start again on the game-over screen |
| `P` | Pause |
| `M` | Sound on / off |
| `H` / `F1` | Toggle the control list, which only shows what you own |
| `B` / `T` / `V` | Bloom / line weight / v-sync |
| `F11` / `F12` | Fullscreen / save a PNG screenshot |
| `Esc` | Quit |

The rocket burns fuel that refills while you are not using it. The force field
runs off an energy bar: it switches itself off when that empties and will not
relight until it has recovered a little.

---

## Command line

    asteroid.exe [options]

    -seed N          world seed (the whole field is derived from it)
    -w N  -h N       window size                        (default 1600x900)
    -fs              start fullscreen
    -zoom N          starting half-view width in world units
    -povcam          start with the rolling POV camera instead of the fixed one
    -keylog          print key changes (Alt, Space) and a frame heartbeat to the console
    -sandbox         the plain toy: no levels, enemies, clock or shop
    -novsync         uncap the frame rate
    -frames N        run N frames then exit, printing timing stats
    -shot N          write a PNG on frame N
    -shotfile PATH   where that PNG goes                (default shot.png)
    -aim FX FY       hold the cursor at that fraction of the window (0..1)
    -selftest        drive the game automatically, and make it unkillable
    -dbg             trace player state to stdout

Test harnesses. Each prints `PASS`/`FAIL` (or a summary) and exits:

    -shoptest        prices, ownership, magazine limits, clicking, the payout
    -weapontest      homing shell, salvo targeting, force field, blast shield
    -lifetest        lives: losing one, restarting the level, game over, the refill every 5 levels
    -shiptest        warships: schedule, generation, hull, weapons, wreck, retry
    -synctest        does a seed determine the world? (no: see Multiplayer)
    -nettest         replicate a shot-up world to an empty one, with loss and latency
    -udptest         the same protocol over real loopback sockets
    -versus [N]      shoot it out with N bots (default 1) in an arena
    -host [PORT] [BOTS]  host a match (default port 4790), optionally with 0-7 bots
    -join ADDRESS[:PORT] join a match
    -name NAME       what you are called in a match (default PILOT)
    -loopback        with -host: listen on this machine only (for testing; no firewall prompt)
    -versustest      versus rules, respawns, frag limit, bots
    -netgametest     two and three whole games in one process, over lossy links and real sockets
    -nosound         start without sound
    -volume V        master volume, 0 to 1.5                     (default 0.8)
    -soundcheck      analyse every synthesised sound and exercise the mixer, no device needed;
                     writes PREFIX_sounds0..3.png (waveform + spectrogram of each sound)
    -soundgametest   play the game against a silent mixer and check that events make their sounds
    -soundtest       play every sound in turn through the device (-soundquick: one soft tick)
    -shipgallery     save a portrait of eight generated warships (-shotfile PREFIX)
    -enemytest       turrets, drones, missiles, shooting things down
    -nuketest        blast size, falloff, enemy kills, damage by distance
    -leveltest       a pilot bot flies six levels: is the clock fair?
      -peaceful      ...with the enemies removed, to time pure navigation
    -persisttest     damage survives chunks unloading and reloading
    -rockettest      the rocket obeys its fuel budget
    -walktest        A and D move the right way on screen
    -bullettest      how far gravity bends each shot
    -showcase        stage each feature and save screenshots (use -shotfile)

---

## How it works

Four design decisions carry most of the weight.

### 1. An asteroid is a signed field, not a polygon

`src/field.h` / `field.cpp`

Each rock owns a small grid of floats where `d > 0` means solid rock. The grid
resolution scales with the rock, so every asteroid costs about the same
regardless of size (typically 60-110 samples per side, 10-40 KB).

That one structure does four jobs:

* **Shape** - generated from a radial harmonic series plus a few carved craters.
* **Damage** - shooting is `d = min(d, |p - c| - r)`, an exact boolean
  subtraction of a disc. A bullet carves a disc per substep, so a burst drills a
  clean tunnel rather than a string of dots.
* **Collision** - a bilinear sample *is* the penetration depth and its gradient
  *is* the surface normal. Shot-up, concave, hole-riddled rocks collide
  correctly with no convex decomposition and no mesh rebuild.
* **Splitting** - 4-connected labelling of the solid samples. More than one
  component means the rock came apart; each piece is copied into its own
  cropped grid and becomes a body with its own mass, centre of mass and inertia.
  Pieces too small to be worth simulating turn into debris particles instead.

Rendering comes from the same field: marching squares at the zero level set,
with the saddle cases resolved by the cell centre so the contour is watertight.
Because each crossed grid edge yields exactly one vertex, the loops close
exactly, with no point welding. The outer boundary and every hole come out as
separate closed loops, which is exactly what `GL_LINE_LOOP` wants.

When a rock loses material its local origin is moved back onto the new centre of
mass, and the velocity of the point that becomes the new origin is folded into
the body velocity. Without that, shooting one side of a rock would make it
wobble around a centre that no longer exists.

### 2. All asteroids draw in one call

`src/render.h` / `render.cpp`

Contours live in a persistent GPU vertex arena and never move. Allocations are
rounded to powers of two so freed slots are handed straight back out, and a
rock is only re-uploaded when its shape actually changes.

Per frame, the only thing that streams to the GPU is a small table of
camera-relative transforms in a texture buffer. Each vertex carries its body
index and reads its own transform in the vertex shader, so the CPU never
transforms a point. Every visible asteroid is then drawn with a single
`glMultiDrawArrays(GL_LINE_LOOP, ...)`.

Everything else - player, bullets, sparks, stars, HUD - goes through a small
streamed line/point batcher, because there is never much of it.

### 3. The world is doubles, the GPU gets floats

Body positions are `double`, so the field is effectively unbounded. Positions
are made camera-relative before they are narrowed to `float`, which keeps full
precision on screen no matter how far out you fly.

The world streams in chunks around the camera, generated deterministically from
the world seed, so the same seed always gives the same field. Chunks that you
have shot at are serialised on unload and restored on return - fly 80,000 units
away and back and the holes are still there. (`-persisttest` checks exactly
this.) Chunk loading is budgeted per frame and walks outward in rings, so the
nearest chunks always win and generation never causes a hitch.

### 4. Crisp lines plus bloom, not thick lines

The scene renders into an RGBA16F buffer with additive blending and line
smoothing. Colours deliberately exceed 1.0 so a three-level blur pyramid gives
them a real glow, then a filmic tonemap, vignette, slight chromatic separation
and a faint scanline finish it off.

Every contour is also drawn a second time, offset slightly inward and dimmed.
That one detail is what makes a bare outline read as a solid rock rather than a
soap bubble.

The POV camera rolls the whole world so the spaceman's local up is screen up.
That roll is a `uRot` uniform applied in both world-space vertex shaders rather
than a CPU transform, so it costs nothing and automatically covers rocks,
bullets, sparks, debris and the parallax starfield without touching any of the
code that submits them. The HUD is drawn with the roll set to identity, and the
cursor is un-rolled on its way back to a world aim direction. Culling is done in
camera space so it stays exact at any roll angle.

The HUD uses a built-in stroke font (`GLYPHS` in `render.cpp`) so the text is
vectors like everything else - no texture atlas, no font file.

---


---

## How the game layer works

The level, enemy and shop code sits on top of the destruction engine and reuses
it rather than working round it.

* **Turrets ride rocks.** A turret stores its position and outward normal in the
  rock's local space, so it turns and drifts with it. If the rock splits, the
  turret re-attaches to whichever piece is now holding it up; if the ground is
  shot out from under it (the field is sampled beneath it every frame), it falls.
* **Spawning is lazy.** The route is laid out as *slots* when a level starts, but
  a slot only becomes an enemy when you are within 1800 units, because turrets
  need real rocks to sit on and those only exist near the camera. A slot with no
  rock nearby becomes a drone instead.
* **The beacon keeps its space.** A clear zone stops rocks generating inside it
  and removes any already there, so it is never buried.
* **A warship's weapons are Enemies.** Each gun on a ship is an `Enemy` of kind
  `Hardpoint` that the ship repositions every frame from its own drifting,
  turning frame. Because of that, bullets, blasts, homing shells, salvo missiles
  and credits all work on ships with no special cases; only the hull needs its own
  code (point-in-outline and nearest-edge tests, in `src/ships.cpp`).
* **A level is a pure function of the run and its number**, and that includes the
  ship (its seed comes from the same hash). That is why a retry after losing a
  life brings back exactly the same warship.
* **Several clear zones.** The world keeps a list of discs where rocks may not
  generate: the beacon's, and a warship's berth.
* **One explosion routine** serves missiles, salvo warheads, shell splash and the
  nuke. It hurts enemies and the player with falloff, carves every rock in reach
  with a disc, and shoves what is left in proportion to 1/mass, so small pieces
  fly and mountains barely stir.
* **Homing weapons target by id**, not by pointer, so a target that dies or is
  compacted out of a vector cannot leave a dangling reference.
* **The blast shield** is a band, not a line, a little wider than one step of a
  fast bullet, so nothing can skip across it between frames. Blasts are tested by
  the angle of their centre against the plate's arc.
* **The force field** is an outward acceleration that ramps from 30% at the rim
  to full at the core, and is never zero inside, so nothing fast can slip through.
* **Left Alt needs special care on Windows.** Alt is a "system key": left alone,
  tapping it activates the window menu, which beeps and can stall the game, and
  the key-up can go missing. The platform layer routes `WM_SYSKEYDOWN/UP` into the
  normal key table, splits Alt into Left and Right by the extended-key bit, eats
  `WM_SYSCHAR` and `SC_KEYMENU`, handles Alt+F4 itself, and clears every held key
  when the window loses focus so the shield cannot stick on. `-keylog` prints what
  the game receives if you want to check it against a real keyboard.
* **The depot freezes the world** and dims it, drawn with the same vector text as
  everything else; its layout function serves both drawing and mouse hit-testing.

---

## Performance

Measured on a Radeon integrated GPU with `-selftest -novsync`, 2000 frames, with
the level layer, enemies and everything else live:

| Scene | Resolution | Average | Worst | Frames > 16 ms |
| --- | --- | --- | --- | --- |
| Level running, rocks being shot up | 1600x900 | 3.0 ms (335 fps) | 11.9 ms | 0 / 2000 |

Earlier measurements of the destruction engine alone: 746 rocks at 4.4 ms, and
3342 rocks fully zoomed out at 5.7 ms with no frame over 16 ms. Field memory is
about 10 MB for a normal view and 59 MB fully zoomed out. The vertex arena is
pre-sized to 64 MB so it never has to grow mid-session.

This is all single-threaded. Threading the physics step and the per-rock
contour rebuilds is the obvious next win if it is ever needed.

---

## Tuning

Everything about how levels, enemies, weapons and the shop feel is in one file,
`src/rules.h`: distances and time limits, spawn radii, credit payouts, salvo
speed and damage, force-field strength and drain, the nuke's radius and fuse,
the shield's arc and charge, the warships (how often, how big, how much armour, what each weapon does), and every price. The difficulty curve itself is
`makeDifficulty()` in `src/level.cpp`.

The rest of the feel lives in two other blocks:

* `namespace cfg` in `src/world.h` - chunk size, gravity constant, body limit,
  the area below which a fragment becomes debris, simulation radius.
* `namespace tune` in `src/game.cpp` - walk speed, jump height, thrust, fuel
  burn and regeneration, rifle and shell speed, calibre and penetration.

A few notes on what interacts with what:

* **Time limit.** `rules::SPEED_BASE` is the average speed a player is expected
  to hold, and the limit is distance divided by it. A pilot bot (`-leveltest`)
  crosses a level in about 5-17 s at the current thrust, so a ~40 s limit leaves
  most of the clock for fighting. If you make the rocket weaker, raise the limit.
* **Walking.** `WALK_SPEED` and `AIR_ACCEL` are currently 0, so `A`/`D` do not
  move the spaceman and all travel is by jump and rocket. `-walktest` reports
  itself as skipped in that state.
* **Rocket refuelling.** Fuel regenerates even while the button is held, so
  holding it on an empty tank sputters at a duty cycle of regen / (regen + burn).
  `-rockettest` checks that thrust never exceeds that budget.
* Gravity is `G * m * r / (r^2 + soft)^1.5` with softening proportional to the
  rock's radius. Since a rock's mass scales with its area, **surface gravity
  works out roughly the same on every rock** (about 250 u/s^2).
* Bullet **gravity** (`BULLET_GRAV`, `HEAVY_GRAV` in `src/game.h`) is a
  multiplier on the pull of nearby rock. Real-strength gravity bends a 1650 u/s
  round by about 4 units over 330 units of flight, which is invisible, so bullets
  get an exaggerated pull.
* Bullet **calibre** is the hole radius and must stay comfortably above the grid
  cell size, or shots will not register on the biggest rocks. **Penetration
  budget** is how much solid rock a shot can chew through.
* Every rock gets a valid bounding radius the moment it exists. (It used to be
  computed only when its outline was first built, a few rocks per frame, so at
  the start of a level hundreds of rocks briefly behaved as single points.)

---

## Known limitations

* Windows only. The platform layer is one file (`src/main.cpp`), and the
  renderer and simulation have no OS dependencies, so an SDL or X11 backend
  would be a contained job.
* Sound is stereo and placed by screen position and distance only: no Doppler, no occlusion by rock. The mixer feeds the old `waveOut` API through a few short buffers, which is universal but not the lowest possible latency.
* Purchases reset with each run; nothing is saved between sessions.
* Rocks do not attract each other, only the player, bullets and debris. Mutual
  attraction would be easy to add but would collapse the field over time.
* Asteroid-asteroid contact samples one outline against the other's field, which
  is accurate but can miss a contact if one rock is far coarser than the other.
* A very light velocity and spin damping is applied to rocks. It is not physical
  in vacuum; it bleeds off the energy the positional contact solver injects.
* The force field pushes the rocks around it but not the one you are standing on.
* The pilot bot ignores enemies, so its damage figures say how dangerous a level
  is to someone who does not fight back, not how it plays for a person.

---

## Layout

    src/core.h         vectors (float and double), RNG, colour
    src/field.*        the signed field: generation, carving, marching squares,
                       connected components, mass properties
    src/world.*        bodies, chunk streaming and persistence, broadphase,
                       contacts, gravity, destruction, splitting, blasts
    src/render.*       GL 3.3 renderer, vertex arena, bloom chain, stroke font,
                       PNG screenshots
    src/rules.h        every number that shapes levels, enemies, weapons, prices
    src/game.h         the Game class, and the structs it is built from
    src/game.cpp       player, rifle, particles, camera, world drawing, main HUD
    src/level.cpp      runs, levels, goal, difficulty curve, spawn slots
    src/level_hud.cpp  beacon, direction pointers, clock, banners, game over
    src/shop.cpp       the supply depot: goods, prices, layout, drawing
    src/enemies.cpp    turrets, drones, enemy fire, missiles, explosions
    src/enemies_fx.cpp the nuke, and drawing for the combat layer
    src/ships.cpp      the generated warships: outline, weapons, hull, wreck, drawing
    src/weapons.cpp    homing shell, salvo, force field, blast shield
    src/net.h          multiplayer protocol, transport, replication (see Multiplayer)
    src/net.cpp        UDP link, loopback link, reliable channel, endpoint
    src/net_world.cpp  turning a World into messages and back
    src/versus.cpp     versus mode: arena, spawning, damage, frags, bots, the match HUD
    src/net_game.cpp   a match over the network: joining, commands, snapshots, prediction
    src/net_session.h  the state of one end of a match
    src/sounds.cpp     the synthesis of every sound
    src/audio.*        the mixer, reverb and Windows waveOut device
    src/audio_game.cpp how game events become sounds; the sounds that last
    src/audio_check.cpp the -soundcheck and -soundtest harnesses
    src/gl.*           self-contained OpenGL/WGL loader
    src/main.cpp       Win32 window, context, input, frame loop, test harnesses
