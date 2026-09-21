// rules.h -- tuning for the level, enemy, weapon and shop layer.
// Everything that decides how the game *feels* to play lives here, so it can be
// rebalanced without hunting through the logic.
#pragma once

namespace rules {
    // ---- the goal ----------------------------------------------------------
    static const float GOAL_RADIUS    = 85.0f;    // touch the beacon inside this
    static const float GOAL_CLEAR     = 300.0f;   // no rocks within this of the goal
    static const float COMPASS_STRENGTH = 0.30f;  // brightness of the chevrons around the spaceman (0..1)
    static const float BANNER_TIME    = 3.4f;     // level intro banner
    static const float COMPLETE_TIME  = 1.8f;     // beat before the shop opens

    // Distance and time limit. The limit is the distance divided by the average
    // speed the player is expected to keep up, which creeps upward with the level.
    // It is deliberately roomy: about 40 s, so there is time to fight, not just run.
    static const float DIST_MIN       = 2400.0f;
    static const float DIST_MAX       = 3400.0f;
    static const float DIST_GROWTH    = 0.04f;    // +4% distance per level
    static const float DIST_CAP       = 9500.0f;
    static const float SPEED_BASE     = 72.0f;    // u/s the player must average on level 1
    static const float SPEED_GROWTH   = 3.0f;     // ...plus this much per level
    static const float TIME_MIN       = 34.0f;
    static const float TIME_MAX       = 55.0f;

    // ---- spawning ----------------------------------------------------------
    static const float SPAWN_RADIUS   = 1800.0f;  // slots wake up this close to the player
    static const float FORGET_RADIUS  = 6500.0f;  // enemies this far behind are dropped
    static const int   MAX_EBULLETS   = 420;
    static const int   MAX_MISSILES   = 14;

    // ---- the player --------------------------------------------------------
    static const float PLAYER_HIT_R   = 9.0f;
    static const float REGEN_DELAY    = 4.0f;     // seconds without damage before healing
    static const float REGEN_RATE     = 6.0f;
    static const float LEVEL_HEAL     = 30.0f;
    static const int   LIVES_START    = 3;        // suits in reserve at the start of a run
    static const int   LIVES_RESET_EVERY = 5;     // lives are topped back up to LIVES_START on levels 6, 11, 16...
    static const float DEATH_TIME     = 2.6f;     // the beat after losing a life, before the level restarts

    // ---- what the rifle and homing shell do to enemies ----------------------
    static const float RIFLE_DAMAGE   = 6.0f;
    static const float HEAVY_DAMAGE   = 55.0f;
    static const float HEAVY_SPLASH_R = 70.0f;
    static const float HEAVY_SPLASH   = 45.0f;
    static const float HOMING_TURN    = 1.75f;    // rad/s a homing shell can turn: a turning radius of about 400 units (speed / rate), so it must be aimed
    static const float HOMING_RANGE   = 1300.0f;  // how far away it can pick a target

    // ---- enemies -----------------------------------------------------------
    static const float TURRET_HP      = 45.0f;
    static const float TURRET_RADIUS  = 17.0f;
    static const float DRONE_HP       = 32.0f;
    static const float DRONE_RADIUS   = 14.0f;
    static const float ENEMY_BULLET_GRAV = 2.0f;
    static const float MISSILE_RADIUS = 60.0f;    // blast radius of a homing missile
    static const float MISSILE_CRATER = 17.0f;

    // ---- credits: what a kill and a finished level pay ------------------------
    // Shooting down a missile pays nothing, so it is never worth farming.
    static const int   CREDIT_TURRET  = 50;
    static const int   CREDIT_DRONE   = 75;
    static const int   CREDIT_LEVEL   = 100;      // per level number, on completion
    static const float CREDIT_PER_SEC = 10.0f;    // for every second left on the clock
    static const int   CREDIT_SHIP_BASE = 200;    // a warship, when its hull goes
    static const int   CREDIT_SHIP_PER_LEVEL = 40;
    static const int   CREDIT_WEAPON  = 30;       // each of a warship's guns

    // ---- warships -----------------------------------------------------------
    // Big generated ships berthed on the way to the beacon: every other level or so.
    // Their weapons are Enemies (so every weapon of yours works on them); the hull
    // is armour that soaks up most of a rifle's damage, but blasts go straight in.
    static const int   SHIP_FIRST_LEVEL = 2;      // the first one is on level 2, then about every other level
    static const float SHIP_EVEN_CHANCE = 0.85f;  // an even level (after the first) has one this often...
    static const float SHIP_ODD_CHANCE  = 0.20f;  // ...an odd one (from level 7) this often
    static const float SHIP_TIME_BONUS  = 8.0f;   // extra seconds on the clock for the detour
    static const float SHIP_LENGTH_MIN  = 460.0f;
    static const float SHIP_LENGTH_MAX  = 780.0f;
    static const float SHIP_HULL_BASE   = 420.0f; // hull points on level 0...
    static const float SHIP_HULL_PER_LEVEL = 70.0f; // ...plus this per level
    static const float SHIP_RIFLE_FACTOR = 0.45f; // share of a rifle bullet's damage that gets through the armour
    static const float SHIP_NUKE_SHARE  = 0.6f;   // a nuke at the heart takes this share of a full hull
    static const float SHIP_WEAPON_HP   = 70.0f;  // each gun, before the level scaling
    static const float SHIP_WEAPON_RADIUS = 20.0f;
    static const float SHIP_WEAPONS_SHARE = 0.6f; // destroying every gun takes this share of the hull with it
    static const float SHIP_DRIFT       = 90.0f;  // how far it wanders around its berth
    static const float SHIP_TURN        = 0.16f;  // rad/s it can swing to face you
    static const float SHIP_FIRE_SLOW   = 1.35f;  // its guns fire this much less often than a turret's
    static const float SHIP_PUSH_SPEED  = 0.35f;  // bounce when you hit the hull
    static const int   FLAK_PELLETS     = 5;      // one flak burst is a fan of this many
    static const float FLAK_SPREAD      = 0.13f;  // radians between pellets
    static const float FLAK_SPEED       = 0.80f;  // of the level's bullet speed
    static const float FLAK_DAMAGE      = 0.60f;  // of the level's bullet damage, per pellet
    static const float FLAK_INTERVAL    = 2.4f;
    static const float CANNON_SPEED     = 0.72f;
    static const float CANNON_DAMAGE    = 2.4f;
    static const float CANNON_CHARGE    = 0.75f;  // the warning glow before it fires
    static const float CANNON_INTERVAL  = 3.6f;

    // ---- the multi-target missile salvo ---------------------------------------
    static const int   SALVO_COUNT    = 5;        // small missiles per shot
    static const float SALVO_SPEED    = 560.0f;
    static const float SALVO_TURN     = 2.15f;    // rad/s: a turning radius of about 260 units
    static const float SALVO_DAMAGE   = 26.0f;
    static const float SALVO_BLAST    = 46.0f;
    static const float SALVO_CRATER   = 8.0f;
    static const float SALVO_COOLDOWN = 0.9f;
    static const float SALVO_RANGE    = 1500.0f;
    static const float SALVO_LIFE     = 4.6f;

    // ---- the force field ---------------------------------------------------------
    static const float FIELD_RADIUS   = 250.0f;
    static const float FIELD_DRAIN    = 24.0f;    // energy per second while on
    static const float FIELD_REGEN    = 9.0f;
    static const float FIELD_RESTART  = 25.0f;    // energy needed to relight after running dry
    static const float FIELD_BULLET   = 30000.0f; // outward acceleration at the core, u/s^2
    static const float FIELD_MISSILE  = 11000.0f;
    static const float FIELD_DRONE    = 3800.0f;
    static const float FIELD_ROCK     = 6.0e6f;   // force on a rock, divided by its mass

    // ---- the blast shield -------------------------------------------------------
    // A small armoured plate held out toward the cursor by holding Left Alt
    // button. It only covers a narrow arc, so it is a thing you aim, and it spends
    // charge on whatever it stops. When the charge is gone it stays down until the
    // depot refills it.
    static const float SHIELD_ARC      = 0.5236f;  // 30 degrees, in radians
    static const float SHIELD_RADIUS   = 46.0f;    // how far out from the spaceman it hangs
    static const float SHIELD_CAPACITY = 100.0f;


    // ---- the fractal shell ---------------------------------------------------------
    // A homing shell that splits in two every so often, and every piece homes too. Each
    // generation is half its parent, less a tenth: 0.5 x 0.9 = 0.45. Five splits at most,
    // so one shot can become 32 pieces. It is the most expensive thing in the depot.
    static const float FRACTAL_SPEED      = 560.0f;   // slower than the rifle so you can watch it divide
    static const float FRACTAL_DAMAGE     = 240.0f;   // what the parent would do on a direct hit; most of it is spent before it lands, splitting
    static const float FRACTAL_SPLASH_R   = 85.0f;    // and its blast radius; a piece scales it by the square root of its strength, so the area follows the damage
    static const float FRACTAL_CRATER     = 48.0f;    // the crater a full-strength blast bites out of rock, scaled the same way
    static const float FRACTAL_KICK       = 6.0e4f;   // and the shove it gives loose rock, scaled by strength
    static const float FRACTAL_CHILD      = 0.45f;    // a child's strength as a share of its parent's: 50% minus 10%
    static const int   FRACTAL_SPLITS     = 5;        // generations after the first
    static const float FRACTAL_SPREAD     = 0.225f;   // radians each child leaves the parent's line by (about 13 degrees; it was 0.30)
    static const float FRACTAL_SPEEDUP    = 1.14f;    // each split, a piece gains this much speed: five splits, about 1.9 times the launch speed
    static const float FRACTAL_TURN       = 1.95f;    // rad/s a piece can turn toward its target: a radius of about 290 units
    static const float FRACTAL_CAL        = 11.7f;    // the parent's hole in a rock
    static const float FRACTAL_MIN_CAL    = 2.6f;
    static const float FRACTAL_SPLIT_K    = 1.00f;    // the split timer covers this share of the cursor distance at launch speed: 1.0 puts the first split on the cross (with no gravity)...
    static const float FRACTAL_SPLIT_MIN  = 90.0f;    // ...but never more often than this
    static const float FRACTAL_SPLIT_MAX  = 2500.0f;  // ...or more rarely (further than the cursor can be from you)
    static const float FRACTAL_COOLDOWN   = 2.4f;
    static const int   FRACTAL_LOAD       = 3;        // shells per purchase
    static const int   FRACTAL_MAX        = 12;
    // ---- the nuke ----------------------------------------------------------
    static const float NUKE_FUSE      = 3.6f;     // seconds from throw to blast
    static const float NUKE_SPEED     = 170.0f;   // slow, so gravity drags it hard
    static const float NUKE_GRAV      = 1.7f;
    static const float NUKE_RADIUS    = 340.0f;   // crater radius
    static const float NUKE_REACH     = 1.15f;    // player damage reaches this * radius
    static const float NUKE_PLAYER_DMG = 125.0f;  // at the very centre
    static const float NUKE_IMPULSE   = 3.2e6f;
    static const float NUKE_COOLDOWN  = 0.9f;
    static const int   NUKE_MAX       = 9;
    static const float NUKE_R_BODY    = 7.0f;     // collision radius of the grenade


    // ---- versus mode -----------------------------------------------------------
    static const float ARENA_RADIUS     = 2000.0f;  // beyond this a wall pushes you back
    static const float ARENA_PUSH       = 1500.0f;  // inward acceleration, per 500 units outside
    static const int   FRAG_LIMIT       = 10;       // first to this many wins
    static const float RESPAWN_TIME     = 3.0f;
    static const float SPAWN_PROTECT    = 2.0f;
    static const float SPAWN_APART      = 700.0f;   // try not to appear this close to anyone
    static const float VS_RIFLE_DAMAGE  = 5.0f;     // a rifle round to the suit
    static const float VS_HEAVY_DAMAGE  = 42.0f;    // a shell that hits directly
    static const float VS_HEAVY_SPLASH  = 30.0f;    // and what its blast does at the centre
    static const float VS_HIT_KICK      = 26.0f;    // shove from a rifle round
    static const float VS_MATCH_OVER    = 8.0f;     // seconds the result stays up before a new match
    // ---- the shop ----------------------------------------------------------
    static const int   PRICE_HOMING   = 300;      // unlocks the homing shell
    static const int   PRICE_SALVO    = 450;      // unlocks the salvo, with a first load
    static const int   PRICE_SALVO_AMMO = 150;    // one more load
    static const int   SALVO_LOAD     = 6;        // salvos per purchase
    static const int   SALVO_MAX      = 24;
    static const int   PRICE_NUKE     = 400;      // a pack of NUKE_PACK
    static const int   NUKE_PACK      = 3;
    static const int   PRICE_FIELD    = 500;      // unlocks the force field
    static const int   PRICE_SHIELD   = 350;      // fits the shield, fully charged
    static const int   PRICE_SHIELD_REFILL = 120; // tops it back up
    static const int   PRICE_FRACTAL  = 1400;     // unlocks the fractal shell, with FRACTAL_LOAD of them
    static const int   PRICE_FRACTAL_AMMO = 500;  // FRACTAL_LOAD more
}
