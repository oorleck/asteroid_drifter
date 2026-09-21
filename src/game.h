// game.h -- player, weapons, particles, camera, HUD, and the level / enemy layer.
//
// The class is defined here but implemented across several files:
//   game.cpp       player, rifle, particles, camera, world drawing
//   level.cpp      runs, levels, goal, difficulty, spawn slots
//   level_hud.cpp  beacon, pointers, clock, banners, game-over screen
//   shop.cpp       the supply depot between levels
//   enemies.cpp    turrets, drones, enemy fire, explosions
//   enemies_fx.cpp the nuke and drawing for the combat layer
//   weapons.cpp    homing shell, missile salvo, force field, blast shield
#pragma once
#include "world.h"
#include "render.h"
#include "rules.h"
#include "audio.h"
#include "net.h"
#include <string>

struct Input {
    bool  down[256]   = {};
    bool  pressed[256] = {};
    bool  mouse[3]    = {};
    bool  mousePressed[3] = {};
    v2    mousePx;                 // cursor in framebuffer pixels
    float wheel = 0;
    void newFrame() {
        for (int i = 0; i < 256; ++i) pressed[i] = false;
        for (int i = 0; i < 3; ++i)   mousePressed[i] = false;
        wheel = 0;
    }
};

// Bullets feel exaggerated gravity: at real strength a 1650 u/s rifle round bends
// about 4 units over 330 units of flight, which nobody can see.
namespace tune {
    static const float BULLET_GRAV = 9.0f;
    static const float HEAVY_GRAV  = 4.0f;     // the F shell: 60% less pull than the 10 it had
    static const float PLAYER_R    = 6.5f;
    static const float WALK_SPEED  = 0.0f;
    static const float WALK_ACCEL  = 1500.0f;
    static const float AIR_ACCEL   = 0.0f;
    static const float JUMP_SPEED  = 260.0f;
    static const float JUMP_MIN_LIFT = 0.17f;  // a jump leaves the ground at least this far (sine, about 10 degrees) off the surface
    static const float THRUST      = 562.5f;   // 25% weaker than the 750 it was
    static const float FUEL_MAX    = 40.0f;    // the tank (it was 100, then 80); burn and regen are unchanged, so the duty cycle is too: 1.6 s of burn from full
    static const float FUEL_BURN   = 25.0f;
    static const float FUEL_REGEN  = 18.7f;    // 15% slower than the 22 it was
    static const float FUEL_RESTART = 12.0f;   // after running dry, fuel needed before the rocket relights
    static const float BULLET_V    = 1650.0f;
    static const float BULLET_CAL  = 5.2f;
    static const float BULLET_PEN  = 10.0f;    // world units of solid rock a shot can chew through
    static const float FIRE_RATE   = 0.085f;
    static const float HEAVY_V     = 700.0f;
    static const float HEAVY_CAL   = 21.0f;
    static const float HEAVY_PEN   = 50.0f;
    static const float HEAVY_RATE  = 1.4f;
}

// Palette for the level / enemy layer. Colours are deliberately over 1.0 so the
// bloom pass makes them glow.
namespace pal {
    static const Col HUD     (0.45f, 0.90f, 0.95f);
    static const Col WARN    (1.00f, 0.42f, 0.30f);
    static const Col GOAL    (0.30f, 1.00f, 0.72f);
    static const Col ENEMY   (1.00f, 0.30f, 0.22f);
    static const Col DRONE   (1.00f, 0.28f, 0.62f);
    static const Col MISSILE (1.00f, 0.55f, 0.30f);
    static const Col NUKE    (1.00f, 0.92f, 0.30f);
    static const Col PLAYER  (0.85f, 0.95f, 1.00f);
    static const Col HOMING  (1.00f, 0.45f, 0.75f);
    static const Col SALVO   (0.40f, 1.00f, 0.85f);
    static const Col FIELD   (0.35f, 0.85f, 1.00f);
    static const Col SHIELD  (1.00f, 0.80f, 0.30f);
    static const Col FRACTAL (1.25f, 0.20f, 0.10f);
}

struct Bullet {
    dv2   pos;
    v2    vel;
    float life = 0;
    float budget = 0;              // how much solid rock it can still chew through
    float caliber = 5.0f;
    float gravScale = 1.0f;        // multiplier on the pull of nearby rock
    int   owner = -1;              // who fired it: a player id, or -1 for none (versus mode scores by this)
    // The fractal shell: gen is 0 for the parent and counts up with each split (-1 = an ordinary round);
    // power is its strength against the parent's 1.0; it splits when it has been in the air splitEvery seconds since its last split.
    int   gen = -1;
    float power = 1.0f, flown = 0.0f, splitEvery = 0.0f;   // flown: seconds since the last split
    bool  heavy = false;
    bool  homing = false;          // the homing shell steers onto an enemy
    int   targetId = 0;
    Col   col;
};

struct Particle {
    dv2   pos;
    v2    vel;
    float life = 0, maxLife = 1;
    float size = 2.0f;
    float ang = 0, angVel = 0;
    Col   col;
    int   kind = 0;                // 0 spark, 1 tumbling chunk, 2 smoke ring
};

// Everything a player can ask for in one frame, and nothing else. The local
// player's keyboard and mouse are boiled down to this, a bot writes one, and in a
// network game a client sends one to the host every frame. Actions that happen
// once (a jump, a shell) are *counted* rather than flagged, so a lost packet
// cannot swallow one: the host acts whenever the count has moved.
struct PlayerCmd {
    float   aim = 0;                 // world-space angle the rifle points along
    float   move = 0;                // -1..1 along the surface
    bool    thrust = false;          // the rocket, which pushes toward the aim
    bool    fire = false;            // the rifle, held
    uint8_t jumpSeq = 0, heavySeq = 0;
};

struct Player {
    dv2   pos;
    v2    vel;
    v2    up = v2(0, 1);           // local "up": surface normal or anti-gravity
    float aim = 0;                 // world-space aim angle
    float facing = 1;
    bool  grounded = false;
    BodyRef ground;

    float fuel = tune::FUEL_MAX;
    float fireCd = 0, heavyCd = 0, salvoCd = 0, nukeCd = 0, jumpCd = 0, coyote = 0;
    float legPhase = 0, thrustGlow = 0, hurtGlow = 0;
    float health = 100;
    float sinceHurt = 99;          // seconds since the suit last took damage
    bool  thrusting = false;
    bool  fuelLocked = false;      // ran dry: rocket stays off until the tank recovers

    // ---- what has been bought in the shop (nothing, at the start of a run)
    bool  hasHoming = false;       // the homing shell   [F]
    bool  hasSalvo  = false;       // the missile salvo  [G]
    bool  hasField  = false;       // the force field    [X]
    int   salvoAmmo = 0;
    int   nukeAmmo  = 0;           // the nuke           [N]
    bool  hasFractal = false;      // the fractal shell [Z]
    int   fractalAmmo = 0;
    float fractalCd = 0;
    float field = 100;             // force-field energy
    bool  fieldOn = false;
    bool  fieldLocked = false;     // ran dry: stays off until the energy recovers
    bool  hasShield = false;       // the blast shield   [left alt]
    float shield = 0;              // its charge, spent on whatever it stops
    bool  shieldUp = false;        // raised this frame
    float shieldFlash = 0;         // brightens when it takes a hit

    // ---- versus mode
    int   id = 0;                  // the same on every machine: 0 is the host
    char  name[16] = "PILOT";
    Col   tint = Col(0.85f, 0.95f, 1.00f);
    bool  dead = false;
    float respawnIn = 0;           // seconds until it comes back
    float protect = 0;             // spawn protection: damage is ignored while this runs
    int   frags = 0, deaths = 0;
    uint8_t seenJump = 0, seenHeavy = 0;     // the last counts acted on
};

// ------------------------------------------------------------ level layer --
// Everything the difficulty curve controls, worked out once per level.
struct Difficulty {
    int   level = 1;
    float distance = 0, timeLimit = 30;
    int   turrets = 0, drones = 0;
    float missileFrac = 0;         // share of enemies carrying homing missiles
    float aimError = 0.1f;         // radians of random aim error
    float fireInterval = 1.5f;     // seconds between bursts
    float bulletSpeed = 600, bulletDamage = 6;
    float missileSpeed = 340, missileTurn = 1.8f, missileDamage = 30;
    float hpScale = 1;
    float droneSpeed = 200;
    float aggroRange = 1500, gunRange = 900;
    float turretTurn = 2.2f;
    float missileInterval = 7;
};
Difficulty makeDifficulty(int level, Rng& rng);

struct Enemy {
    enum Kind { Turret, Drone, Hardpoint };
    int   id = 0;                  // homing weapons refer to their target by id
    Kind  kind = Turret;
    bool  alive = false;
    dv2   pos;
    v2    vel;
    float hp = 1, maxHp = 1, radius = 12;
    bool  hasGun = true, hasMissile = false;
    float gunCd = 1, missileCd = 3, burstCd = 0;
    int   burst = 0;
    bool  aggro = false;
    float aim = 0;                 // barrel direction / heading
    float flash = 0, phase = 0, spin = 0;
    // A turret rides on a rock: local position and outward normal.
    BodyRef mount;
    v2    localPos, localNormal, normal = v2(0, 1);
    // A hardpoint belongs to a warship: which one, which ShipWeapon::Type, how slowly it fires, its colour.
    int   shipId = 0, weapon = 0;
    float cdScale = 1;
    Col   tint;
    uint32_t seed = 0;
};

struct EnemyBullet {
    dv2   pos;
    v2    vel;
    float life = 0, damage = 5;
    float size = 1;                // a cannon shell is drawn larger
};

struct Missile {
    int   id = 0;
    dv2   pos;
    v2    vel;
    float life = 0, maxSpeed = 350, turn = 1.8f, damage = 30;
    float age = 0;
    bool  dead = false;
};

// A weapon bolted to a warship. It is an Enemy (kind Hardpoint) in every sense (bullets hit it,
// blasts hurt it, homing weapons lock onto it); the ship only decides where it is.
struct ShipWeapon {
    enum Type { Gun, Missile, Flak, Cannon };
    Type   type = Gun;
    v2     local;                  // position in the hull's frame, +x toward the nose
    int    enemyId = 0;
};

// A big generated warship: an outline, a colour, a name and a set of weapons. It
// drifts near its berth, turns slowly to face you, and dies when its hull does.
struct Ship {
    int   id = 0;
    bool  alive = false;
    dv2   anchor, pos;             // where it is berthed, and where it is now
    v2    vel;
    float angle = 0, angVel = 0;   // local +x points along `angle`
    float baseAngle = 0;
    float radius = 300;            // bounds the hull, from the ship's centre
    float berth = 500;             // rocks are kept this far from the anchor
    float hp = 1, maxHp = 1, flash = 0;
    float hullShare = 0.1f;        // hull lost when one weapon is destroyed
    float phase = 0;
    bool  aggro = false;
    Col   col;
    char  name[32] = "";
    int   style = 0;
    std::vector<v2>   hull;        // outline, counter-clockwise, local
    std::vector<v2>   inner;       // a second, smaller outline
    std::vector<v2>   lines;       // panel lines, as pairs of points
    std::vector<v2>   engines;     // where the exhaust comes out
    std::vector<ShipWeapon> weapons;
    v2    bridge[4];               // the little diamond at the front
};
// Builds a random warship: outline, colour, name and weapons, all from the seed (ships.cpp).
void generateShip(Ship& s, uint64_t seed, int level, const Difficulty& d);


// One of the small missiles from the player's salvo.
struct PMissile {
    dv2   pos;
    v2    vel;
    int   targetId = 0;            // an enemy or an enemy missile; 0 = none yet
    float life = 0, age = 0;
    bool  dead = false;
};

struct Nuke {
    dv2   pos;
    v2    vel;
    float fuse = 0, ang = 0, angVel = 0;
    bool  dead = false;
};

// Expanding ring drawn for explosions.
struct Shockwave {
    dv2   pos;
    float age = 0, life = 1, radius = 100, delay = 0;
    Col   col;
    float weight = 1;
};

// A place along the route where something will appear once the player gets
// close enough for the surrounding rocks to exist.
struct Slot {
    enum Type { TurretSlot, DroneSlot };
    Type  type = TurretSlot;
    dv2   pos;
    bool  done = false;
    bool  hasGun = true, hasMissile = false;
    uint32_t seed = 0;
    float waited = 0;
};

struct Level {
    int   number = 0;
    dv2   start, goal;
    Difficulty diff;
    float timeLeft = 30;
    float banner = 0;              // intro banner countdown
    std::vector<Slot> slots;
    bool  hasShip = false;         // a warship is berthed on the way
    bool  shipSpawned = false;
    Ship  ship;                    // built at level start; put into play once the player is near
};

enum class State { Playing, Dead, LevelComplete, Shop, GameOver };   // Dead: a life was lost, the level restarts shortly

// What the supply depot sells.
enum ShopItem { ITEM_HOMING, ITEM_SALVO, ITEM_NUKE, ITEM_FIELD, ITEM_SHIELD, ITEM_FRACTAL, ITEM_COUNT };

struct Game {
    World    world;
    Player   pl;
    Camera   cam;
    std::vector<Bullet>   bullets;
    std::vector<Particle> parts;

    // ---- the level layer
    State    state = State::Playing;
    Level    level;
    int      credits = 0;          // the currency: earned by kills and finished levels
    int      lives = rules::LIVES_START;   // suits left, this one included
    int      levelCredits = 0, levelEarned = 0;   // credits when the level began: a retry goes back to these
    int      totalEarned = 0;      // credits earned this run, whatever was spent
    int      kills = 0, bestLevel = 0, bestEarned = 0;
    float    stateTime = 0;
    const char* gameOverReason = "";
    std::vector<Enemy>       enemies;
    std::vector<Ship>        ships;
    std::vector<EnemyBullet> ebullets;
    std::vector<Missile>     missiles;
    std::vector<PMissile>    pmissiles;
    std::vector<Nuke>        nukes;
    std::vector<Shockwave>   waves;
    int      nextId = 1;

    // Running totals, mostly for the tests and the game-over screen.
    float    damageTaken = 0;
    int      enemyShots = 0, missilesLaunched = 0;
    float    flash = 0;            // white-out after a big blast
    char     message[64] = "";     // short pop-up line
    float    messageTime = 0;

    // ---- the shop
    int      shopHover = -1;       // row under the cursor, or ITEM_COUNT for "continue"
    char     shopNote[64] = "";
    float    shopNoteTime = 0;
    bool     shopNoteBad = false;

    // Test and benchmark switches.
    bool     sandbox = false;      // no levels, enemies or timer: the plain toy
    bool     invincible = false;   // enemies still fight, but nothing kills you
    bool     allItems = false;     // -allitems: everything in the depot is owned from the start, and kept topped up
    void     grantAllItems();
    bool     floating = false;     // ignore gravity, so a test can hold the player still

    float  zoomTarget = 760.0f;
    double distanceTravelled = 0;
    int    rocksSplit = 0, shotsFired = 0;
    bool   showHelp = true;
    bool   povCamera = false;      // off by default: the camera does not rotate. C turns on the rolling POV view
    bool   paused = false;
    float  time = 0;
    float  shake = 0;
    float  fps = 0, frameMs = 0, physMs = 0;
    bool   debugTrace = false;
    v2     mousePx;
    Rng    rng{0xC0FFEEull};

    // scratch reused every frame
    std::vector<BodyXform> xforms;
    std::vector<int> firsts, counts;

    void init(Renderer& r, uint64_t seed);
    void update(Renderer& r, const Input& in, float dt);
    void render(Renderer& r);
    void respawn();

    // ---- level flow (level.cpp)
    void startRun(Renderer& r);
    void startLevel(int number, bool retry = false);
    void retryLevel(Renderer& r);        // after a lost life: the same level again, from the start
    bool playerGone() const { return state == State::Dead || state == State::GameOver || (versus && pl.dead); }
    void hurtPlayer(float dmg, v2 kick = v2(0, 0), int attacker = -1);
    void killPlayer(const char* reason);
    void say(const char* text);
    void earn(int amount);

    // ---- the shop (shop.cpp)
    bool owned(int item) const;
    int  priceOf(int item) const;
    bool canBuy(int item) const;
    bool buy(int item);
    void updateShop(Renderer& r, const Input& in);
    void drawShop(Renderer& r);
    void shopRect(int row, float W, float H, float& x, float& y, float& w, float& h) const;

    // ---- combat (enemies.cpp)
    void explode(dv2 pos, float radius, float enemyDmg, float playerDmg,
                 float carveR, float impulse, bool nuke);
    void damageEnemy(Enemy& e, float dmg, dv2 at);
    bool bulletHitsTargets(Bullet& b);
    bool clearLine(dv2 a, dv2 b, float skip = 0.0f) const;
    void throwNuke(v2 aimDir);

    // ---- weapons (weapons.cpp)
    void fireSalvo(v2 aimDir);
    // ---- the fractal shell (weapons.cpp)
    float lastAimDist = 400.0f;                         // how far the cursor is from the spaceman, in world units
    static float fractalSplitDistance(float aimDist);   // how far a shell would fly between splits with no gravity: the cursor distance
    static float fractalSplitTime(float aimDist, float speed);   // ...and the time that takes at a given launch speed: what it actually splits by
    void fireFractal(float aimDist);
    void splitFractal(const Bullet& parent);
    void detonateBullet(const Bullet& b);              // a shell or fractal piece that has flown its time goes off where it is (the range limit)
    void shellBurst(const Bullet& b);                   // a heavy shell going off where it landed
    void updatePMissiles(float dt);
    void updateField(float dt);
    void steerHoming(Bullet& b, float dt);
    Enemy*   findEnemy(int id);
    Missile* findMissile(int id);
    void drawPMissiles(Renderer& r);
    void drawField(Renderer& r);
    void drawLocks(Renderer& r);
    // The blast shield: does it stop something at this point, and how much of a
    // blast from `src` does it soak up? (returns the damage that gets through)
    bool  shieldBlocks(dv2 p) const;
    float shieldAbsorb(dv2 src, float dmg);
    void  drawShield(Renderer& r);

// Internals. Public so the test harness in main.cpp can drive them directly.
    uint64_t baseSeed = 0;
    int      runCount = 0;
    Rng      levelRng{1};

    void updatePlayer(Renderer& r, const Input& in, float dt);
    void updateBullets(float dt);
    void updateParticles(float dt);
    void drainWorldEvents();
    void spawnSparks(dv2 p, v2 base, int n, float speed, Col c, float life);
    void drawHud(Renderer& r);
    void drawStars(Renderer& r);

    // ---- level.cpp
    void updateLevel(float dt);
    void materializeSlots(float dt);
    bool findSurfacePoint(dv2 hint, float searchR, float minRockR,
                          dv2& outPos, v2& outNormal, int& outBody);
    void drawGoal(Renderer& r);
    void drawLevelHud(Renderer& r);
    void drawEdgeMarker(Renderer& r, dv2 target, Col c, const char* label, bool onlyOffscreen,
                        float size = 1.0f, float hidePx = 0.0f);
    bool beaconInView(Renderer& r) const;
    void drawBeaconCompass(Renderer& r);
    v2   worldToScreen(Renderer& r, dv2 p) const;
    v2   camRel(dv2 p) const { return v2((float)(p.x - cam.pos.x), (float)(p.y - cam.pos.y)); }
    bool inView(v2 rel, float margin) const {
        const float R = cam.viewRadius() + margin;
        return len2(rel) < R * R;
    }

    // ---- enemies.cpp
    void spawnTurret(const Slot& s, dv2 pos, v2 normal, int body);
    void spawnDrone(const Slot& s, dv2 pos);
    void updateEnemies(float dt);
    void updateTurret(Enemy& e, float dt, float dist);
    void updateDrone(Enemy& e, float dt, float dist);
    bool refreshMount(Enemy& e);
    void enemyShoot(Enemy& e, dv2 muzzle, float dist, float dt, bool aimed);
    void missileBlast(const Missile& m);
    void fireEnemyBullet(const Enemy& e, dv2 muzzle);
    void launchMissile(const Enemy& e, v2 dir);
    void destroyMissile(Missile& m);
    void updateEnemyBullets(float dt);
    void updateMissiles(float dt);
    void updateNukes(float dt);
    void updateWaves(float dt);
    void detonate(Nuke& n);
    void drawEnemies(Renderer& r);
    void drawProjectiles(Renderer& r);
    void drawNukes(Renderer& r);
    void drawWaves(Renderer& r);
    void drawNukeLabels(Renderer& r);

    // ---- warships (ships.cpp)
    void spawnShip();                              // puts level.ship into play, weapons and all
    void updateShips(float dt);
    void updateShipWeapon(Ship& s, Enemy& e, float dt);
    void fireShipBullet(const Enemy& e, dv2 muzzle, float ang, float speedMul, float dmgMul, float size, float life);
    Ship* findShip(int id);
    Ship* shipAt(dv2 p);                           // the ship whose hull contains p, if any
    float hullDistance(const Ship& s, dv2 p, v2* outward = nullptr) const;   // negative inside
    void  damageShip(Ship& s, float dmg, dv2 at);
    void  killShip(Ship& s);
    void  drawShips(Renderer& r);
    void  drawShipHud(Renderer& r);
    void  drawWeaponMount(Renderer& r, const Enemy& e);

    // ---- versus (versus.cpp)
    // The local player is always `pl`, so everything written for one player keeps
    // working. Everyone else is a Peer: a bot here, or a human across the network.
    struct Peer {
        Player  body;
        PlayerCmd cmd;
        bool    bot = false;
        float   botClock = 0, botBurst = 0, botAimErr = 0, botStrafe = 1, botJumpCd = 0;
        int     netPeer = -1;          // which connection it is, for humans on the wire
        // On a client, other players are only ever told where they are; these let them glide.
        dv2     netPos;  v2 netVel;  double netAt = 0;  bool netHave = false;
    };
    bool    versus = false;
    bool    netClient = false;         // a client does not decide matches: the host does
    char    localName[16] = "PILOT";   // what this machine calls its player when it joins or hosts
    std::vector<Peer> peers;
    PlayerCmd localCmd;                // the local player's command, kept between frames for its counters
    Rng     vsRng{0x5EED};             // spawn choices; only the host uses it
    struct KillMsg { char text[64]; float ttl; Col col; };
    std::vector<KillMsg> killFeed;
    struct Match { bool over = false; int winner = -1; float overTime = 0; int round = 1; } match;
    int     explodeOwner = -1;         // who a blast in progress belongs to, so its kills are credited
    int     vsShots = 0, vsHits = 0;   // rounds fired, and rounds that landed on a player (diagnostics)

    void startVersus(Renderer& r, int bots);
    void updateVersus(float dt);
    void stepPlayer(Player& p, const PlayerCmd& c, float dt);
    void fire(Player& p, bool heavy);
    void damagePlayer(Player& p, float dmg, v2 kick, int attacker);   // any player, versus rules
    void playerDied(Player& victim, int killer);
    void respawnPlayer(Player& p);
    bool standOnRock(int slot, Player& p);
    bool bulletHitsPlayers(Bullet& b);
    void botThink(Peer& b, float dt);
    Player* playerById(int id);
    template <class F> void eachPlayer(F f) { f(pl); for (Peer& p : peers) f(p.body); }
    void resetMatch();
    void addKillMsg(const char* text, Col c);
    void drawVersusHud(Renderer& r);
    void drawPlayerFig(Renderer& r, const Player& p);

    // ---- network play (net_game.cpp)
    struct NetSession* net = nullptr;
    bool netHost = false;
    bool startHost(Renderer& r, int port, int bots, std::string* err = nullptr, bool loopbackOnly = false);
    bool startClient(Renderer& r, const char* ip, int port, std::string* err = nullptr);
    bool startHostOn(Renderer& r, net::Link* link, int bots);        // over a link somebody else made: the tests
    bool startClientOn(Renderer& r, net::Link* link);
    void netShutdown();
    void netBegin(float dt);       // reads the network and applies it
    void netEnd(float dt);         // sends what changed
    void netShot(const Bullet& b); // host: tell the clients a round was fired
    void netKilled(int killer, int victim, dv2 at);
    void killEffects(int killer, int victim, dv2 at);   // the announcement, the bang and the sound
    Bullet makeBullet(const Player& p, dv2 pos, v2 vel, bool heavy) const;
    std::string netStatus() const;                       // one line for the HUD
    bool netConnected() const;
    float netRtt() const;

    // ---- sound (audio_game.cpp)
    void sfx(Sfx s, dv2 at, float vol = 1.0f, float pitch = 1.0f, float range = 1600.0f, float delay = 0.0f);
    void sfxUI(Sfx s, float vol = 1.0f, float pitch = 1.0f, float delay = 0.0f);
    void updateAudio(float dt);
    Rng   sfxRng{77};              // its own stream, so sound never disturbs the game's dice
    bool  quietBooms = false;      // a warship's death has its own big sound: no per-blast ones
    State audioState = State::Playing;
    float pingTimer = 0, warnTimer = 0;
    int   audioSecond = 99;
    bool  prevShieldUp = false;
    void ring(dv2 pos, float radius, float life, Col c, float weight = 1.0f, float delay = 0.0f);
    void boom(dv2 pos, float size, Col c);
};
