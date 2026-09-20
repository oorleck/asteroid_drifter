// enemies.cpp -- everything that shoots back: turrets, drones, bullets, homing
// missiles, explosions, and the nuke.
#include "game.h"
#include <algorithm>
#include <cstdio>

static float dist2d(dv2 a, dv2 b) {
    const double dx = a.x - b.x, dy = a.y - b.y;
    return (float)std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------- effects --
void Game::ring(dv2 pos, float radius, float life, Col c, float weight, float delay) {
    if (waves.size() > 80) return;
    Shockwave w;
    w.pos = pos;  w.radius = radius;  w.life = life;
    w.col = c;    w.weight = weight;  w.delay = delay;
    waves.push_back(w);
}

void Game::boom(dv2 pos, float size, Col c) {
    ring(pos, size * 1.7f, 0.45f, c, 1.0f);
    spawnSparks(pos, v2(0, 0), (int)clampf(size * 0.9f, 8.0f, 120.0f), size * 7.0f, c, 0.7f);
    if (quietBooms) return;
    // Every explosion in the game goes through here, so this is where they get their sound.
    const float pitch = sfxRng.range(0.92f, 1.1f);
    if (size < 22.0f)      sfx(Sfx::ExplodeS, pos, 0.55f, pitch, 1500.0f);
    else if (size < 40.0f) sfx(Sfx::ExplodeS, pos, 1.0f, pitch * 0.9f, 2000.0f);
    else if (size < 70.0f) sfx(Sfx::ExplodeM, pos, 1.0f, pitch, 2600.0f);
    else                   sfx(Sfx::ExplodeL, pos, 1.0f, pitch, 3400.0f);
}

void Game::updateWaves(float dt) {
    size_t w = 0;
    for (size_t i = 0; i < waves.size(); ++i) {
        Shockwave s = waves[i];
        s.age += dt;
        if (s.age > s.life + s.delay) continue;
        waves[w++] = s;
    }
    waves.resize(w);
}

// ---------------------------------------------------------------- spawning --
void Game::spawnTurret(const Slot& s, dv2 pos, v2 normal, int body) {
    const Difficulty& D = level.diff;
    Enemy e;
    e.kind = Enemy::Turret;
    e.id = nextId++;
    e.alive = true;
    e.pos = pos;
    e.normal = normal;
    e.aim = std::atan2(normal.y, normal.x);
    e.maxHp = e.hp = rules::TURRET_HP * D.hpScale;
    e.radius = rules::TURRET_RADIUS;
    e.hasGun = s.hasGun;
    e.hasMissile = s.hasMissile;
    if (!e.hasGun && !e.hasMissile) e.hasGun = true;
    e.gunCd = rng.range(0.6f, 1.6f);
    e.missileCd = rng.range(2.0f, 5.0f);
    e.phase = rng.angle();
    e.seed = s.seed;

    const Body& b = world.bodies[body];
    e.mount = world.ref(body);
    e.localPos = b.toLocal(pos);
    e.localNormal = rot(normal, b.cosA, -b.sinA);
    e.vel = b.velAt(tov2(pos - b.pos));
    enemies.push_back(e);
}

void Game::spawnDrone(const Slot& s, dv2 pos) {
    const Difficulty& D = level.diff;
    Enemy e;
    e.kind = Enemy::Drone;
    e.id = nextId++;
    e.alive = true;
    e.pos = pos;
    e.vel = rng.disc() * 20.0f;
    e.maxHp = e.hp = rules::DRONE_HP * D.hpScale;
    e.radius = rules::DRONE_RADIUS;
    e.hasGun = s.hasGun;
    e.hasMissile = s.hasMissile;
    if (!e.hasGun && !e.hasMissile) e.hasGun = true;
    e.gunCd = rng.range(0.8f, 2.0f);
    e.missileCd = rng.range(3.0f, 6.0f);
    e.phase = rng.angle();
    e.aim = rng.angle();
    e.seed = s.seed;
    enemies.push_back(e);
}

// ------------------------------------------------------------------- mounts --
// A turret rides its rock. If the rock was split it re-attaches to whichever
// piece is now holding it up; if the ground was shot out from under it, it falls.
bool Game::refreshMount(Enemy& e) {
    Body* b = world.get(e.mount);
    if (!b) {
        const dv2 under(e.pos.x - e.normal.x * 3.0, e.pos.y - e.normal.y * 3.0);
        const int h = world.solidAt(under);
        if (h < 0) return false;
        b = &world.bodies[h];
        e.mount = world.ref(h);
        e.localPos = b->toLocal(e.pos);
        e.localNormal = rot(e.normal, b->cosA, -b->sinA);
    }
    e.pos = b->toWorld(e.localPos);
    e.normal = b->dirToWorld(e.localNormal);
    e.vel = b->velAt(tov2(e.pos - b->pos));
    return b->f.sample(e.localPos - e.localNormal * 3.0f) > 0.5f;
}

// Is the straight line between two points free of rock?
bool Game::clearLine(dv2 a, dv2 b, float skip) const {
    const v2 d = tov2(b - a);
    const float L = len(d);
    if (L < 1.0f) return true;
    const v2 dir = d / L;
    for (float s = skip; s < L; s += 9.0f) {
        const dv2 p(a.x + dir.x * s, a.y + dir.y * s);
        if (world.solidAt(p) >= 0) return false;
    }
    return true;
}

// ------------------------------------------------------------------ damage --
void Game::damageEnemy(Enemy& e, float dmg, dv2 at) {
    if (!e.alive) return;
    e.hp -= dmg;
    e.flash = 1.0f;
    e.aggro = true;
    sfx(Sfx::EnemyHit, at, 0.7f, sfxRng.range(0.9f, 1.2f), 1500.0f);
    if (e.kind == Enemy::Hardpoint)                     // a hit wakes the whole ship
        if (Ship* s = findShip(e.shipId)) s->aggro = true;
    const Col kindCol = e.kind == Enemy::Hardpoint ? e.tint : (e.kind == Enemy::Turret ? pal::ENEMY : pal::DRONE);
    spawnSparks(at, v2(0, 0), 4, 170.0f, kindCol, 0.3f);
    if (e.hp > 0.0f) return;

    e.alive = false;
    ++kills;
    earn(e.kind == Enemy::Turret ? rules::CREDIT_TURRET
       : e.kind == Enemy::Drone  ? rules::CREDIT_DRONE : rules::CREDIT_WEAPON);
    boom(e.pos, e.kind == Enemy::Turret ? 28.0f : 24.0f, kindCol);
    shake = std::max(shake, 0.25f);
    // A warship loses a slice of hull with every gun it loses.
    if (e.kind == Enemy::Hardpoint)
        if (Ship* s = findShip(e.shipId)) damageShip(*s, s->maxHp * s->hullShare, e.pos);
}

// Shooting a missile down pays nothing, so it is never worth farming.
void Game::destroyMissile(Missile& m) {
    if (m.dead) return;
    m.dead = true;
    boom(m.pos, 17.0f, pal::MISSILE);
}

// A player bullet is checked against enemies and missiles. Returns true when the
// bullet is used up.
bool Game::bulletHitsTargets(Bullet& b) {
    if (sandbox) return false;
    for (Enemy& e : enemies) {
        if (!e.alive) continue;
        const float reach = e.radius + b.caliber;
        const double dx = e.pos.x - b.pos.x, dy = e.pos.y - b.pos.y;
        if (dx * dx + dy * dy > (double)reach * reach) continue;
        if (b.heavy) explode(b.pos, rules::HEAVY_SPLASH_R, rules::HEAVY_DAMAGE, 0, 0, 0, false);
        else         damageEnemy(e, rules::RIFLE_DAMAGE, b.pos);
        return true;
    }
    // A warship's hull is armour: a rifle round mostly bounces off it, a shell bursts on it.
    for (Ship& s : ships) {
        if (!s.alive) continue;
        const float bound = s.radius + b.caliber;
        if (len2(tov2(b.pos - s.pos)) > bound * bound) continue;
        if (hullDistance(s, b.pos) > b.caliber * 0.5f) continue;
        if (b.heavy) explode(b.pos, rules::HEAVY_SPLASH_R, rules::HEAVY_DAMAGE, 0, 0, 0, false);
        else {
            damageShip(s, rules::RIFLE_DAMAGE * rules::SHIP_RIFLE_FACTOR, b.pos);
            spawnSparks(b.pos, b.vel * -0.1f, 2, 120.0f, Col(1.0f, 0.9f, 0.7f), 0.25f);
        }
        return true;
    }
    for (Missile& m : missiles) {
        if (m.dead) continue;
        const float reach = 11.0f + b.caliber;
        const double dx = m.pos.x - b.pos.x, dy = m.pos.y - b.pos.y;
        if (dx * dx + dy * dy > (double)reach * reach) continue;
        destroyMissile(m);
        return true;
    }
    return false;
}

// One explosion routine for everything: missiles, splash, and the nuke.
void Game::explode(dv2 pos, float radius, float enemyDmg, float playerDmg,
                   float carveR, float impulse, bool nuke) {
    for (Enemy& e : enemies) {
        if (!e.alive) continue;
        const float d = dist2d(e.pos, pos) - e.radius;
        if (d >= radius) continue;
        const float f = 1.0f - clampf(d / radius, 0.0f, 1.0f);
        damageEnemy(e, enemyDmg * (0.4f + 0.6f * f), e.pos);
    }
    // Blasts go straight through the armour. A nuke is capped so it takes a share of the
    // hull, not all of it: two of them, or a nuke and some patience, finish a ship.
    if (enemyDmg > 0.0f) {
        for (Ship& s : ships) {
            if (!s.alive) continue;
            const float d = std::max(0.0f, hullDistance(s, pos));
            if (d >= radius) continue;
            const float f = 1.0f - d / radius;
            damageShip(s, std::min(enemyDmg, s.maxHp * rules::SHIP_NUKE_SHARE) * (0.4f + 0.6f * f), pos);
        }
    }
    for (Missile& m : missiles) {
        if (m.dead) continue;
        if (dist2d(m.pos, pos) < radius * 0.8f) destroyMissile(m);
    }
    for (EnemyBullet& b : ebullets)
        if (dist2d(b.pos, pos) < radius) b.life = 0.0f;

    if (playerDmg > 0.0f && state == State::Playing) {
        const v2 rel = tov2(pl.pos - pos);
        const float d = len(rel);
        const float reach = radius * (nuke ? rules::NUKE_REACH : 1.0f);
        if (d < reach) {
            const float f = 1.0f - d / reach;
            const v2 dir = d > 1e-3f ? rel / d : v2(0, 1);
            // A nuke hurts in proportion to how close you stood; a missile hurts
            // properly even on a near miss.
            const float raw = nuke ? playerDmg * f : playerDmg * (0.3f + 0.7f * f);
            // The blast shield soaks up what goes off inside its arc, and the shove
            // shrinks with it.
            const float dmg = shieldAbsorb(pos, raw);
            const float through = raw > 0.0f ? dmg / raw : 1.0f;
            if (dmg > 0.01f) hurtPlayer(dmg, dir * (nuke ? 1100.0f * f : 380.0f * f) * through, explodeOwner);
        }
    }
    // In a versus match a blast hurts everyone in reach, by the same falloff.
    if (playerDmg > 0.0f && versus) {
        for (Peer& pe : peers) {
            Player& p = pe.body;
            if (p.dead) continue;
            const v2 rel = tov2(p.pos - pos);
            const float d = len(rel);
            const float reach = radius * (nuke ? rules::NUKE_REACH : 1.0f);
            if (d >= reach) continue;
            const float f = 1.0f - d / reach;
            const v2 dir = d > 1e-3f ? rel / d : v2(0, 1);
            const float raw = nuke ? playerDmg * f : playerDmg * (0.3f + 0.7f * f);
            damagePlayer(p, raw, dir * (nuke ? 1100.0f * f : 380.0f * f), explodeOwner);
        }
    }
    if (carveR > 0.0f)
        world.explode(pos, carveR, radius * (nuke ? 2.0f : 1.6f), impulse, rng.u32());
}

void Game::missileBlast(const Missile& m) {
    boom(m.pos, 30.0f, pal::MISSILE);
    shake = std::max(shake, 0.3f);
    explode(m.pos, rules::MISSILE_RADIUS, 0.0f, m.damage, rules::MISSILE_CRATER, 6.0e4f, false);
}

// ------------------------------------------------------------------ shooting --
void Game::fireEnemyBullet(const Enemy& e, dv2 muzzle) {
    if ((int)ebullets.size() >= rules::MAX_EBULLETS) return;
    const Difficulty& D = level.diff;
    EnemyBullet b;
    b.pos = muzzle;
    b.life = 2.6f;
    b.damage = D.bulletDamage;

    float ang;
    if (e.kind != Enemy::Drone) {
        ang = e.aim + rng.sym(D.aimError * 0.6f);
    } else {
        // Aim where the player will be, given how long the bullet takes to arrive.
        const v2 rel = tov2(pl.pos - muzzle);
        const float t = len(rel) / D.bulletSpeed;
        const v2 aimPt = rel + (pl.vel - e.vel) * t;
        ang = std::atan2(aimPt.y, aimPt.x) + rng.sym(D.aimError);
    }
    b.vel = fromAngle(ang) * D.bulletSpeed + e.vel;
    ebullets.push_back(b);
    ++enemyShots;
    spawnSparks(muzzle, e.vel + fromAngle(ang) * 60.0f, 2, 80.0f, pal::ENEMY, 0.12f);
    sfx(Sfx::EnemyShot, muzzle, 0.75f, e.kind == Enemy::Hardpoint ? 0.72f : (e.kind == Enemy::Drone ? 1.2f : 1.0f) * sfxRng.range(0.95f, 1.05f), 1500.0f);
}

void Game::launchMissile(const Enemy& e, v2 dir) {
    const Difficulty& D = level.diff;
    Missile m;
    m.id = nextId++;
    m.pos = dv2(e.pos.x + dir.x * 16.0, e.pos.y + dir.y * 16.0);
    m.vel = dir * 120.0f + e.vel;
    m.life = 8.0f;
    m.maxSpeed = D.missileSpeed;
    m.turn = D.missileTurn;
    m.damage = D.missileDamage;
    ++missilesLaunched;
    missiles.push_back(m);
    spawnSparks(m.pos, e.vel + dir * 90.0f, 8, 120.0f, pal::MISSILE, 0.35f);
    sfx(Sfx::MissileLaunch, m.pos, 0.9f, sfxRng.range(0.95f, 1.05f), 2000.0f);
}

void Game::enemyShoot(Enemy& e, dv2 muzzle, float dist, float dt, bool aimed) {
    const Difficulty& D = level.diff;
    e.gunCd -= dt;
    e.missileCd -= dt;
    e.burstCd -= dt;

    if (e.hasGun) {
        if (e.burst > 0) {
            if (e.burstCd <= 0.0f && aimed) {
                fireEnemyBullet(e, muzzle);
                --e.burst;
                e.burstCd = 0.11f;
            }
        } else if (e.gunCd <= 0.0f && dist < D.gunRange && aimed) {
            if (clearLine(muzzle, pl.pos)) {
                e.burst = D.level < 4 ? 2 : (D.level < 9 ? 3 : 4);
                e.gunCd = D.fireInterval * e.cdScale * rng.range(0.8f, 1.3f);
                e.burstCd = 0.0f;
            } else {
                e.gunCd = 0.35f;                  // blocked: look again shortly
            }
        }
    }
    if (e.hasMissile && e.missileCd <= 0.0f && dist > 300.0f && dist < D.gunRange * 1.5f &&
        (int)missiles.size() < rules::MAX_MISSILES) {
        // A turret lobs it straight out of the rock; a drone fires it at you.
        const v2 dir = e.kind == Enemy::Turret ? e.normal
                     : e.kind == Enemy::Hardpoint ? fromAngle(e.aim) : norm(tov2(pl.pos - e.pos));
        launchMissile(e, dir);
        e.missileCd = D.missileInterval * e.cdScale * rng.range(0.8f, 1.35f);
    }
}

// --------------------------------------------------------------- enemy AI --
void Game::updateTurret(Enemy& e, float dt, float dist) {
    const Difficulty& D = level.diff;
    if (!refreshMount(e)) {
        // Its rock was shot out from under it.
        if (dist < 2600.0f) {
            boom(e.pos, 24.0f, pal::ENEMY);
            earn(rules::CREDIT_TURRET / 2);
            ++kills;
        }
        e.alive = false;
        return;
    }
    if (dist > 3600.0f) return;                       // too far away to matter

    if (!e.aggro) {
        if (dist < D.aggroRange * 0.8f) e.aggro = true;
        else return;
    }

    // Track the player, leading the shot.
    const v2 rel  = tov2(pl.pos - e.pos);
    const v2 relV = pl.vel - e.vel;
    const v2 aimPt = rel + relV * (dist / D.bulletSpeed);
    float want = std::atan2(aimPt.y, aimPt.x);
    const float nAng = std::atan2(e.normal.y, e.normal.x);
    want = nAng + clampf(wrapAngle(want - nAng), -1.45f, 1.45f);   // it cannot aim into its own rock

    const float turn = D.turretTurn * dt;
    e.aim = wrapAngle(e.aim + clampf(wrapAngle(want - e.aim), -turn, turn));
    const bool aimed = std::fabs(wrapAngle(want - e.aim)) < 0.10f + D.aimError;

    const v2 ad = fromAngle(e.aim);
    const dv2 muzzle(e.pos.x + ad.x * 24.0 + e.normal.x * 6.0,
                     e.pos.y + ad.y * 24.0 + e.normal.y * 6.0);
    enemyShoot(e, muzzle, dist, dt, aimed);
}

void Game::updateDrone(Enemy& e, float dt, float dist) {
    const Difficulty& D = level.diff;
    e.phase += dt;

    if (!e.aggro) {
        if (dist < D.aggroRange) e.aggro = true;
        else {                                        // idle: drift
            e.pos.x += (double)e.vel.x * dt;
            e.pos.y += (double)e.vel.y * dt;
            return;
        }
    } else if (dist > D.aggroRange * 2.2f) {
        e.aggro = false;
    }

    const v2 toP = tov2(pl.pos - e.pos);
    const v2 dir = dist > 1e-3f ? toP / dist : v2(1, 0);

    // Hold a fighting distance and weave from side to side.
    const float keep = 300.0f + 70.0f * std::sin(e.phase * 0.6f);
    const float radial = clampf((dist - keep) * 1.3f, -D.droneSpeed * 0.8f, D.droneSpeed);
    const v2 strafe = perp(dir) * (std::sin(e.phase * 0.9f + (float)(e.seed & 255u)) * D.droneSpeed * 0.5f);
    v2 want = dir * radial + strafe;

    // Steer clear of rock ahead of it.
    v2 n;  float depth = 0;
    const v2 ahead = e.vel * 0.7f;
    if (world.probe(dv2(e.pos.x + ahead.x, e.pos.y + ahead.y), e.radius + 30.0f, &n, &depth) >= 0)
        want += n * (D.droneSpeed * 1.3f);

    v2 dv = want - e.vel;
    const float maxA = 650.0f * dt;
    const float dl = len(dv);
    if (dl > maxA) dv = dv * (maxA / dl);
    e.vel += dv;
    e.pos.x += (double)e.vel.x * dt;
    e.pos.y += (double)e.vel.y * dt;

    if (world.probe(e.pos, e.radius + 2.0f, &n, &depth) >= 0) {
        e.pos.x += n.x * depth;
        e.pos.y += n.y * depth;
        const float vn = dot(e.vel, n);
        if (vn < 0.0f) e.vel -= n * (vn * 1.5f);
    }

    e.aim = std::atan2(dir.y, dir.x);
    const dv2 muzzle(e.pos.x + dir.x * 16.0, e.pos.y + dir.y * 16.0);
    enemyShoot(e, muzzle, dist, dt, true);
}

void Game::updateEnemies(float dt) {
    if (sandbox || state != State::Playing) return;
    updateShips(dt);                                  // carries its weapons about and fires them
    for (Enemy& e : enemies) {
        if (!e.alive) continue;
        e.flash = std::max(0.0f, e.flash - dt * 4.0f);
        e.spin += dt;
        if (e.kind == Enemy::Hardpoint) continue;     // its ship looks after it
        const float dist = dist2d(pl.pos, e.pos);
        if (dist > rules::FORGET_RADIUS) { e.alive = false; continue; }
        if (e.kind == Enemy::Turret) updateTurret(e, dt, dist);
        else                         updateDrone(e, dt, dist);
    }
    enemies.erase(std::remove_if(enemies.begin(), enemies.end(),
                                 [](const Enemy& e) { return !e.alive; }), enemies.end());
}

// -------------------------------------------------------------- projectiles --
void Game::updateEnemyBullets(float dt) {
    size_t w = 0;
    const float hitR = rules::PLAYER_HIT_R + 2.0f;
    for (size_t i = 0; i < ebullets.size(); ++i) {
        EnemyBullet b = ebullets[i];
        b.life -= dt;
        if (b.life <= 0.0f) continue;
        b.vel += world.gravityAt(b.pos, 1500.0) * (dt * rules::ENEMY_BULLET_GRAV);

        const float speed = len(b.vel);
        const int steps = std::max(1, (int)(speed * dt / 7.0f) + 1);
        const float sdt = dt / steps;
        bool hit = false;
        for (int s = 0; s < steps && !hit; ++s) {
            b.pos.x += (double)b.vel.x * sdt;
            b.pos.y += (double)b.vel.y * sdt;
            if (state == State::Playing && shieldBlocks(b.pos)) {
                const float left = shieldAbsorb(b.pos, b.damage);   // whatever the charge cannot cover
                if (left > 0.0f) hurtPlayer(left);
                spawnSparks(b.pos, b.vel * -0.15f, 6, 150.0f, pal::SHIELD, 0.3f);
                hit = true;
                break;
            }
            if (state == State::Playing) {
                const double dx = b.pos.x - pl.pos.x, dy = b.pos.y - pl.pos.y;
                if (dx * dx + dy * dy < (double)hitR * hitR) {
                    hurtPlayer(b.damage, norm(b.vel) * 18.0f);
                    spawnSparks(b.pos, v2(0, 0), 6, 140.0f, pal::WARN, 0.3f);
                    hit = true;
                    break;
                }
            }
            const int rock = world.solidAt(b.pos);
            if (rock >= 0) {
                world.damage(rock, b.pos, 2.6f * b.size, 0.3f, rng.u32());   // chips the cover it hits
                spawnSparks(b.pos, b.vel * -0.1f, 3, 90.0f, pal::ENEMY, 0.25f);
                hit = true;
            }
        }
        if (!hit) ebullets[w++] = b;
    }
    ebullets.resize(w);
}

void Game::updateMissiles(float dt) {
    for (Missile& m : missiles) {
        if (m.dead) continue;
        m.age += dt;
        m.life -= dt;
        if (m.life <= 0.0f) { m.dead = true; boom(m.pos, 14.0f, pal::MISSILE); continue; }

        float sp = len(m.vel);
        v2 dir = sp > 1.0f ? m.vel / sp : v2(1, 0);
        if (state == State::Playing) {
            // Home in, leading the target a little. It coasts straight for the
            // first moments so a launch reads as a launch.
            const v2 rel = tov2(pl.pos - m.pos);
            const float d = len(rel);
            const v2 aimPt = rel + pl.vel * (d / std::max(200.0f, sp)) * 0.6f;
            const float want = std::atan2(aimPt.y, aimPt.x);
            const float have = std::atan2(dir.y, dir.x);
            const float turn = m.turn * dt * (m.age < 0.35f ? 0.15f : 1.0f);
            dir = fromAngle(have + clampf(wrapAngle(want - have), -turn, turn));
        }
        sp = std::min(m.maxSpeed, sp + 420.0f * dt);
        m.vel = dir * sp + world.gravityAt(m.pos, 1500.0) * (dt * 0.6f);

        const int steps = std::max(1, (int)(sp * dt / 10.0f) + 1);
        const float sdt = dt / steps;
        bool blew = false;
        for (int s = 0; s < steps && !blew; ++s) {
            m.pos.x += (double)m.vel.x * sdt;
            m.pos.y += (double)m.vel.y * sdt;
            if (state == State::Playing) {
                const double dx = m.pos.x - pl.pos.x, dy = m.pos.y - pl.pos.y;
                const float hr = rules::PLAYER_HIT_R + 7.0f;
                if (dx * dx + dy * dy < (double)hr * hr) blew = true;
            }
            if (!blew && state == State::Playing && shieldBlocks(m.pos)) blew = true;
            if (!blew && world.solidAt(m.pos) >= 0) blew = true;
        }
        if (blew) {
            m.dead = true;             // first, or its own blast would "shoot it down"
            missileBlast(m);
            continue;
        }
        if (rng.f() < 0.9f)
            spawnSparks(m.pos - dir * 9.0f, m.vel * 0.2f, 1, 35.0f, pal::MISSILE, 0.32f);
    }
    missiles.erase(std::remove_if(missiles.begin(), missiles.end(),
                                  [](const Missile& m) { return m.dead; }), missiles.end());
}
