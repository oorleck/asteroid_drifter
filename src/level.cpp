// level.cpp -- runs, levels, the goal, the difficulty curve and the level HUD.
#include "game.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ------------------------------------------------------------- difficulty --
// Level 1 is gentle enough to learn on; everything then ramps together. The
// numbers are all in rules.h or right here, deliberately in plain sight.
Difficulty makeDifficulty(int level, Rng& rng) {
    Difficulty d;
    const float L = (float)level;
    const float k = (float)(level - 1);

    d.level = level;
    d.distance = std::min(rules::DIST_CAP,
                          rng.range(rules::DIST_MIN, rules::DIST_MAX) * (1.0f + rules::DIST_GROWTH * k));
    // The time limit is the distance over the speed we expect the player to hold.
    const float need = rules::SPEED_BASE + rules::SPEED_GROWTH * k;
    d.timeLimit = clampf(d.distance / need, rules::TIME_MIN, rules::TIME_MAX);

    d.turrets = std::min(3 + 2 * level, 36);
    d.drones  = std::min(1 + (int)(1.3f * k), 22);
    d.missileFrac = clampf(0.08f + 0.10f * k, 0.0f, 0.75f);

    d.aimError     = std::max(0.03f, 0.17f - 0.011f * L);
    d.fireInterval = std::max(0.60f, 2.00f - 0.09f * L);
    d.bulletSpeed  = std::min(950.0f, 560.0f + 22.0f * L);
    d.bulletDamage = std::min(16.0f, 4.5f + 0.55f * L);
    d.missileSpeed = std::min(760.0f, 330.0f + 18.0f * L);
    d.missileTurn  = std::min(3.6f, 1.7f + 0.12f * L);
    d.missileDamage = std::min(50.0f, 28.0f + L);
    d.missileInterval = std::max(3.0f, 8.0f - 0.3f * L);
    d.hpScale      = 1.0f + 0.12f * k;
    d.droneSpeed   = std::min(430.0f, 190.0f + 12.0f * L);
    d.aggroRange   = std::min(2400.0f, 1500.0f + 40.0f * L);
    d.gunRange     = std::min(1500.0f, 700.0f + 40.0f * L);
    d.turretTurn   = std::min(4.0f, 2.2f + 0.08f * L);
    return d;
}

// Which levels have a warship: the first one is on level 2, then mostly the even
// levels, and now and then an odd one later on. A pure function of the run and the
// level number, so a retry brings back exactly the same fight.
static bool wantsShip(uint64_t seed, int number) {
    if (number < rules::SHIP_FIRST_LEVEL) return false;
    if (number == rules::SHIP_FIRST_LEVEL) return true;
    Rng r(hashCombine(seed, (uint64_t)number * 5779ull));
    const float p = (number % 2 == 0) ? rules::SHIP_EVEN_CHANCE : (number >= 7 ? rules::SHIP_ODD_CHANCE : 0.0f);
    return r.f() < p;
}

// -------------------------------------------------------------- run flow --
void Game::say(const char* text) {
    snprintf(message, sizeof message, "%s", text);
    messageTime = 2.4f;
}

void Game::startRun(Renderer& r) {
    ++runCount;
    // A new world: nothing on the GPU or in memory belongs to the old one.
    r.arenaReset();
    world.init(baseSeed + (uint64_t)(runCount - 1) * 1013ull);

    bullets.clear();  parts.clear();
    enemies.clear();  ebullets.clear();  missiles.clear();
    nukes.clear();    pmissiles.clear();   waves.clear();  ships.clear();
    credits = 0;  totalEarned = 0;  kills = 0;  flash = 0;  messageTime = 0;  message[0] = 0;
    lives = rules::LIVES_START;
    shake = 0;
    state = State::Playing;
    stateTime = 0;

    cam.pos = dv2(0, 0);
    cam.angle = 0;
    world.streamChunks(cam.pos, cam.halfW * 1.7 + 1500.0, 100000);   // prime the world
    world.step(1.0f / 60.0f, cam.pos);
    world.syncGeometry(r);
    respawn();
    if (!sandbox) startLevel(1);
}

void Game::startLevel(int number, bool retry) {
    level = Level();
    level.number = number;
    levelRng = Rng(hashCombine(baseSeed ^ ((uint64_t)runCount * 7919ull),
                               (uint64_t)number * 104729ull));
    Rng& rg = levelRng;
    level.diff = makeDifficulty(number, rg);
    const Difficulty& D = level.diff;

    level.start = pl.pos;
    const v2 dir  = fromAngle(rg.angle());
    const v2 side = perp(dir);
    level.goal = dv2(level.start.x + dir.x * D.distance, level.start.y + dir.y * D.distance);
    level.timeLeft = D.timeLimit;
    level.banner = rules::BANNER_TIME;

    // The beacon needs room: keep rocks out of it and clear any already there.
    world.dropZone();
    world.clearZone(level.goal, rules::GOAL_CLEAR);

    // Whatever was hunting the player on the last level does not follow.
    enemies.clear();  ebullets.clear();  missiles.clear();
    nukes.clear();    pmissiles.clear();

    // Lay out the gauntlet. Slots wake up when the player gets close, because
    // turrets need real rocks to sit on and those only exist near the camera.
    auto along = [&](float t, float lateral) {
        return dv2(level.start.x + dir.x * D.distance * t + side.x * lateral,
                   level.start.y + dir.y * D.distance * t + side.y * lateral);
    };
    for (int i = 0; i < D.turrets; ++i) {
        Slot s;
        s.type = Slot::TurretSlot;
        s.pos  = along(0.09f + 0.88f * std::sqrt(rg.f()), rg.sym(700.0f));
        s.hasMissile = rg.f() < D.missileFrac;
        s.hasGun     = !s.hasMissile || rg.f() < 0.6f;
        s.seed = rg.u32();
        level.slots.push_back(s);
    }
    for (int i = 0; i < D.drones; ++i) {
        Slot s;
        s.type = Slot::DroneSlot;
        s.pos  = along(0.14f + 0.86f * rg.f(), rg.sym(650.0f));
        s.hasMissile = rg.f() < D.missileFrac;
        s.hasGun     = !s.hasMissile || rg.f() < 0.5f;
        s.seed = rg.u32();
        level.slots.push_back(s);
    }

    // Every other level or so a warship is berthed somewhere on the way. It is built
    // now (so its size is known and the rocks can be cleared out of its berth) and
    // put into play when the player gets close.
    ships.clear();
    level.hasShip = wantsShip(baseSeed ^ ((uint64_t)runCount * 7919ull), number);
    if (level.hasShip) {
        Rng sr(hashCombine(baseSeed ^ ((uint64_t)runCount * 6151ull), (uint64_t)number * 15485863ull));
        Ship& sh = level.ship;
        generateShip(sh, sr.u32() | ((uint64_t)sr.u32() << 32), number, D);
        sh.anchor = along(sr.range(0.40f, 0.62f), sr.sym(140.0f));
        sh.pos = sh.anchor;
        sh.baseAngle = sr.angle();
        sh.angle = sh.baseAngle;
        sh.berth = sh.radius + rules::SHIP_DRIFT * 1.3f + 140.0f;
        world.clearZone(sh.anchor, sh.berth);

        // Nothing else is laid out inside the berth.
        level.slots.erase(std::remove_if(level.slots.begin(), level.slots.end(), [&](const Slot& sl) {
            const double dx = sl.pos.x - sh.anchor.x, dy = sl.pos.y - sh.anchor.y;
            return dx * dx + dy * dy < (double)sh.berth * sh.berth;
        }), level.slots.end());

        level.diff.timeLimit += rules::SHIP_TIME_BONUS;             // the detour is worth some seconds
        level.timeLeft = level.diff.timeLimit;
    }

    // A breather between levels.
    pl.health = std::min(100.0f, pl.health + rules::LEVEL_HEAL);
    pl.fuel = tune::FUEL_MAX;  pl.fuelLocked = false;
    pl.field = 100.0f;  pl.fieldLocked = false;
    state = State::Playing;
    stateTime = 0;
    bestLevel = std::max(bestLevel, number);
    levelCredits = credits;  levelEarned = totalEarned;
    if (allItems) grantAllItems();
    if (!retry) sfxUI(Sfx::LevelStart, 0.9f);

    // Every fifth level the reserve of suits is topped back up.
    if (!retry && number > 1 && (number - 1) % rules::LIVES_RESET_EVERY == 0 && lives < rules::LIVES_START) {
        lives = rules::LIVES_START;
        say("LIVES RESTORED");
        sfxUI(Sfx::LivesRestored, 1.0f, 1.0f, 0.8f);
    }
}

// A life was lost: the same level again, from where it began. The beacon, the
// gauntlet and the clock come back exactly as they were (a level is a pure
// function of the run and its number); what you bought stays bought, and
// ammunition you burned stays burned. Credits go back to what they were at the
// start, so dying is a cost and not a way to farm turrets.
void Game::retryLevel(Renderer& r) {
    const dv2 start = level.start;
    const int number = level.number;

    credits = levelCredits;  totalEarned = levelEarned;
    bullets.clear();  parts.clear();  waves.clear();

    // Rocks near the start may have been unloaded or shot to pieces meanwhile.
    pl.pos = start;  pl.vel = v2(0, 0);
    cam.pos = start;
    world.streamChunks(cam.pos, cam.halfW * 1.7 + 1500.0, 100000);
    world.step(1.0f / 60.0f, cam.pos);
    world.syncGeometry(r);

    pl.health = 100.0f;  pl.sinceHurt = 99.0f;  pl.hurtGlow = 0.0f;
    pl.fireCd = pl.heavyCd = pl.salvoCd = pl.nukeCd = pl.fractalCd = pl.jumpCd = pl.coyote = 0.0f;
    pl.thrusting = false;  pl.fieldOn = false;  pl.shieldUp = false;  pl.shieldFlash = 0.0f;
    pl.grounded = false;   pl.ground = BodyRef();
    if (pl.hasShield) pl.shield = rules::SHIELD_CAPACITY;      // a fresh suit, a charged plate

    startLevel(number, true);      // rebuilds the identical gauntlet around `start`

    // Stand on the nearest good rock to the start, which is usually the one we left from.
    dv2 p;  v2 n;  int body = -1;
    if (findSurfacePoint(start, 900.0f, 40.0f, p, n, body)) {
        pl.pos = dv2(p.x + n.x * 14.0, p.y + n.y * 14.0);
        pl.up = n;
        pl.vel = world.bodies[body].vel;
    }
    cam.pos = pl.pos;

    char buf[48];
    snprintf(buf, sizeof buf, lives == 1 ? "LAST LIFE" : "%d LIVES LEFT", lives);
    say(buf);
    sfxUI(Sfx::Respawn);
}

void Game::hurtPlayer(float dmg, v2 kick, int attacker) {
    if (state != State::Playing) return;
    if (versus) {                               // a match has its own rules: see damagePlayer
        if (pl.dead || match.over) return;
        damagePlayer(pl, dmg, kick, attacker);
        return;
    }
    damageTaken += dmg;
    pl.vel += kick;
    pl.sinceHurt = 0.0f;
    pl.hurtGlow  = std::min(1.0f, pl.hurtGlow + dmg * 0.04f);
    sfxUI(Sfx::Hurt, clampf(dmg / 22.0f, 0.35f, 1.0f), sfxRng.range(0.92f, 1.08f));
    shake = std::max(shake, std::min(0.8f, dmg * 0.03f));
    if (invincible) return;
    pl.health = std::max(0.0f, pl.health - dmg);
    if (pl.health <= 0.0f && !sandbox) killPlayer("SUIT BREACH");
}

void Game::killPlayer(const char* reason) {
    if (playerGone()) return;
    stateTime = 0;
    gameOverReason = reason;
    pl.health = 0;
    bestLevel = std::max(bestLevel, level.number);
    bestEarned = std::max(bestEarned, totalEarned);
    // Another suit in the reserve means another go at this level; none means the run is over.
    if (--lives > 0) state = State::Dead;
    else { lives = 0; state = State::GameOver; }
    sfxUI(Sfx::Death);
    if (state == State::GameOver) sfxUI(Sfx::GameOver, 0.9f, 1.0f, 1.7f);
    boom(pl.pos, 46.0f, pal::PLAYER);
    spawnSparks(pl.pos, pl.vel, 110, 520.0f, Col(0.7f, 0.95f, 1.0f), 1.5f);
    shake = 1.0f;
}

// ------------------------------------------------------------- level tick --
void Game::updateLevel(float dt) {
    if (sandbox) return;
    stateTime += dt;
    if (level.banner > 0.0f) level.banner -= dt;
    if (messageTime > 0.0f)  messageTime -= dt;
    if (shopNoteTime > 0.0f) shopNoteTime -= dt;

    switch (state) {
    case State::Playing: {
        level.timeLeft = std::max(0.0f, level.timeLeft - dt);
        if (level.timeLeft <= 0.0f && !invincible) { killPlayer("TIME UP"); return; }
        materializeSlots(dt);

        const double dx = pl.pos.x - level.goal.x, dy = pl.pos.y - level.goal.y;
        const double reach = rules::GOAL_RADIUS + rules::PLAYER_HIT_R;
        if (dx * dx + dy * dy < reach * reach) {
            const int bonus = rules::CREDIT_LEVEL * level.number
                            + (int)(level.timeLeft * rules::CREDIT_PER_SEC);
            earn(bonus);
            state = State::LevelComplete;
            stateTime = 0;
            bestLevel = std::max(bestLevel, level.number);
            bestEarned = std::max(bestEarned, totalEarned);
            ring(level.goal, rules::GOAL_RADIUS * 3.0f, 1.2f, pal::GOAL, 1.4f);
            ring(level.goal, rules::GOAL_RADIUS * 2.0f, 0.9f, pal::GOAL, 1.0f, 0.1f);
            spawnSparks(level.goal, v2(0, 0), 80, 420.0f, Col(0.5f, 1.0f, 0.8f), 1.2f);
            char buf[48];
            snprintf(buf, sizeof buf, "BEACON REACHED   +%d CREDITS", bonus);
            say(buf);
            sfxUI(Sfx::LevelComplete);
        }
        break;
    }
    case State::LevelComplete:
        if (stateTime > rules::COMPLETE_TIME) {
            // Everything that was shooting at you stands down while you shop.
            ebullets.clear();  missiles.clear();  pmissiles.clear();  enemies.clear();  ships.clear();
            state = State::Shop;
            stateTime = 0;
            shopNote[0] = 0;
        }
        break;
    case State::Dead:       // the explosion plays out; update() restarts the level
    case State::Shop:       // handled by updateShop()
    case State::GameOver:
        break;
    }
}

// Turns waiting slots into real enemies once the player is close enough.
void Game::materializeSlots(float dt) {
    // The warship wakes up a little before the ordinary enemies do, so it is already
    // hanging there in the dark when you arrive.
    if (level.hasShip && !level.shipSpawned) {
        const double dx = level.ship.anchor.x - pl.pos.x, dy = level.ship.anchor.y - pl.pos.y;
        const double reach = rules::SPAWN_RADIUS + 500.0 + level.ship.radius;
        if (dx * dx + dy * dy < reach * reach) spawnShip();
    }
    for (Slot& s : level.slots) {
        if (s.done) continue;
        const double dx = s.pos.x - pl.pos.x, dy = s.pos.y - pl.pos.y;
        const float dist = (float)std::sqrt(dx * dx + dy * dy);
        if (dist > rules::SPAWN_RADIUS) continue;

        dv2 p;  v2 n;  int body = -1;
        switch (s.type) {
        case Slot::TurretSlot:
            if (findSurfacePoint(s.pos, 650.0f, 34.0f, p, n, body)) {
                spawnTurret(s, p, n, body);
                s.done = true;
            } else {
                // No rock nearby to bolt it to: it becomes a flying drone instead.
                s.waited += dt;
                if (dist < 1200.0f || s.waited > 2.0f) s.type = Slot::DroneSlot;
            }
            break;
        case Slot::DroneSlot: {
            p = s.pos;
            v2 nn;  float depth = 0;
            if (world.probe(p, 24.0f, &nn, &depth) >= 0)
                p += dv2(nn.x * (depth + 40.0f), nn.y * (depth + 40.0f));
            spawnDrone(s, p);
            s.done = true;
            break;
        }
        }
    }
}

// Finds a point on the surface of a real rock near `hint`, and which way is
// "out" there. Rays are fired at the rock from a fan of angles until one finds
// a spot with open space above it.
bool Game::findSurfacePoint(dv2 hint, float searchR, float minRockR,
                            dv2& outPos, v2& outNormal, int& outBody) {
    int best = -1;
    double bestD = 1e30;
    for (int s : world.active) {
        const Body& b = world.bodies[s];
        if (!b.alive || b.radius < minRockR) continue;
        const double dx = b.pos.x - hint.x, dy = b.pos.y - hint.y;
        const double d = std::sqrt(dx * dx + dy * dy) - b.radius;
        if (d < bestD) { bestD = d; best = s; }
    }
    if (best < 0 || bestD > searchR) return false;

    const Body& b = world.bodies[best];
    const v2 toHint = tov2(hint - b.pos);
    const float base = len2(toHint) > 1.0f ? std::atan2(toHint.y, toHint.x) : rng.angle();

    for (int attempt = 0; attempt < 24; ++attempt) {
        const float off = (float)((attempt + 1) / 2) * 0.30f * ((attempt & 1) ? 1.0f : -1.0f);
        const v2 dir = fromAngle(base + off);
        const float R = b.radius + 6.0f;

        dv2 prev(b.pos.x + dir.x * R, b.pos.y + dir.y * R);
        if (world.solidAt(prev) >= 0) continue;          // starts inside some other rock
        bool found = false;
        dv2 inside;
        for (float r = R; r > 0.0f; r -= 3.0f) {
            const dv2 p(b.pos.x + dir.x * r, b.pos.y + dir.y * r);
            const int h = world.solidAt(p);
            if (h == best) { inside = p; found = true; break; }
            if (h >= 0) break;                           // another rock is in the way
            prev = p;
        }
        if (!found) continue;

        // Bisect down to the surface itself.
        dv2 lo = prev, hi = inside;
        for (int i = 0; i < 6; ++i) {
            const dv2 mid((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5);
            if (world.solidAt(mid) == best) hi = mid; else lo = mid;
        }
        const v2 g = b.f.gradient(b.toLocal(hi));
        if (len2(g) < 1e-8f) continue;
        const v2 nOut = b.dirToWorld(norm(g) * -1.0f);

        // Reject spots buried in a crevice: there has to be open space above.
        const dv2 above(lo.x + nOut.x * 26.0, lo.y + nOut.y * 26.0);
        if (world.solidAt(above) >= 0) continue;

        outPos = lo;
        outNormal = nOut;
        outBody = best;
        return true;
    }
    return false;
}

// Credits are the currency of the shop: kills and finished levels pay them out.
void Game::earn(int amount) {
    credits += amount;
    totalEarned += amount;
}
