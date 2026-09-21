// weapons.cpp -- the things you buy: the homing shell, the multi-target missile
// salvo, the force field and the blast shield.
#include "game.h"
#include <algorithm>
#include <cstdio>

Enemy* Game::findEnemy(int id) {
    if (id == 0) return nullptr;
    for (Enemy& e : enemies) if (e.alive && e.id == id) return &e;
    return nullptr;
}

Missile* Game::findMissile(int id) {
    if (id == 0) return nullptr;
    for (Missile& m : missiles) if (!m.dead && m.id == id) return &m;
    return nullptr;
}

// ------------------------------------------------------------ homing shell --
// The charge shot: it flies like any other shell until an enemy is in front of
// it, then turns onto it. The turn rate is limited, so it can be dodged round a
// rock and it still needs a sensible aim.
void Game::steerHoming(Bullet& b, float dt) {
    Enemy* t = findEnemy(b.targetId);
    if (!t) {
        b.targetId = 0;
        const v2 dir = norm(b.vel);
        float best = 1e30f;
        for (Enemy& e : enemies) {
            if (!e.alive) continue;
            const v2 rel = tov2(e.pos - b.pos);
            const float d = len(rel);
            if (d > rules::HOMING_RANGE || d < 1.0f) continue;
            const float c = dot(rel / d, dir);
            if (c < -0.3f) continue;                       // not behind it
            const float cost = d * (1.6f - c);             // near, and near where it is heading
            if (cost < best) { best = cost; b.targetId = e.id; t = &e; }
        }
        if (!t) return;
    }
    const v2 rel = tov2(t->pos - b.pos);
    const float sp = len(b.vel);
    const float have = std::atan2(b.vel.y, b.vel.x);
    const float want = std::atan2(rel.y, rel.x);
    const float turn = (b.gen >= 0 ? rules::FRACTAL_TURN : rules::HOMING_TURN) * dt;
    b.vel = fromAngle(have + clampf(wrapAngle(want - have), -turn, turn)) * sp;
}

// ------------------------------------------------------------------ salvo --
// Five small missiles, each sent after a different target: the closest enemies
// to where you are aiming first, then incoming enemy missiles. With fewer than
// five targets, the spare missiles double up.
void Game::fireSalvo(v2 aimDir) {
    if (!pl.hasSalvo || pl.salvoAmmo <= 0 || pl.salvoCd > 0.0f || state != State::Playing) return;
    --pl.salvoAmmo;
    pl.salvoCd = rules::SALVO_COOLDOWN;
    sfx(Sfx::Salvo, pl.pos, 1.0f, sfxRng.range(0.96f, 1.04f), 2200.0f);

    struct Cand { int id; float cost; };
    std::vector<Cand> cands;
    for (const Enemy& e : enemies) {
        if (!e.alive) continue;
        const v2 rel = tov2(e.pos - pl.pos);
        const float d = len(rel);
        if (d > rules::SALVO_RANGE || d < 1.0f) continue;
        cands.push_back({ e.id, d * (1.7f - dot(rel / d, aimDir)) });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
    std::vector<int> ids;
    for (const Cand& c : cands) if ((int)ids.size() < rules::SALVO_COUNT) ids.push_back(c.id);

    if ((int)ids.size() < rules::SALVO_COUNT) {
        cands.clear();
        for (const Missile& m : missiles) {
            if (m.dead) continue;
            const float d = (float)len(m.pos - pl.pos);
            if (d < 900.0f) cands.push_back({ m.id, d });
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
        for (const Cand& c : cands) if ((int)ids.size() < rules::SALVO_COUNT) ids.push_back(c.id);
    }

    for (int i = 0; i < rules::SALVO_COUNT; ++i) {
        PMissile m;
        m.targetId = ids.empty() ? 0 : ids[i % ids.size()];
        // Launched in a fan, so five missiles visibly go their separate ways.
        const float fan = (i - (rules::SALVO_COUNT - 1) * 0.5f) * 0.30f;
        const v2 d = rot(aimDir, fan);
        m.pos = dv2(pl.pos.x + d.x * 16.0, pl.pos.y + d.y * 16.0);
        m.vel = d * 230.0f + pl.vel;
        m.life = rules::SALVO_LIFE;
        pmissiles.push_back(m);
    }
    pl.vel -= aimDir * 30.0f;
    shake = std::max(shake, 0.18f);
    spawnSparks(dv2(pl.pos.x + aimDir.x * 18.0, pl.pos.y + aimDir.y * 18.0),
                pl.vel + aimDir * 90.0f, 14, 160.0f, pal::SALVO, 0.3f);
}

void Game::updatePMissiles(float dt) {
    for (PMissile& m : pmissiles) {
        if (m.dead) continue;
        m.age += dt;
        m.life -= dt;
        if (m.life <= 0.0f) { m.dead = true; boom(m.pos, 10.0f, pal::SALVO); continue; }

        // Find what it is chasing; if that is gone, take the nearest thing in reach.
        dv2 target;
        bool have = false;
        if (Enemy* e = findEnemy(m.targetId))        { target = e->pos;  have = true; }
        else if (Missile* em = findMissile(m.targetId)) { target = em->pos; have = true; }
        else if (m.age > 0.2f) {
            float best = 700.0f;
            m.targetId = 0;
            for (Enemy& e : enemies) {
                const float d = (float)len(e.pos - m.pos);
                if (e.alive && d < best) { best = d; m.targetId = e.id; target = e.pos; have = true; }
            }
            for (Missile& em : missiles) {
                const float d = (float)len(em.pos - m.pos);
                if (!em.dead && d < best) { best = d; m.targetId = em.id; target = em.pos; have = true; }
            }
        }

        float sp = len(m.vel);
        v2 dir = sp > 1.0f ? m.vel / sp : v2(1, 0);
        if (have) {
            const v2 rel = tov2(target - m.pos);
            const float want = std::atan2(rel.y, rel.x);
            const float cur  = std::atan2(dir.y, dir.x);
            // Coast straight for a moment so the five paths fan out before they bend.
            const float turn = rules::SALVO_TURN * dt * (m.age < 0.2f ? 0.25f : 1.0f);
            dir = fromAngle(cur + clampf(wrapAngle(want - cur), -turn, turn));
        }
        sp = std::min(rules::SALVO_SPEED, sp + 900.0f * dt);
        m.vel = dir * sp + world.gravityAt(m.pos, 1500.0) * (dt * 0.3f);

        const int steps = std::max(1, (int)(sp * dt / 8.0f) + 1);
        const float sdt = dt / steps;
        bool blew = false;
        for (int s = 0; s < steps && !blew; ++s) {
            m.pos.x += (double)m.vel.x * sdt;
            m.pos.y += (double)m.vel.y * sdt;
            for (const Enemy& e : enemies) {
                if (!e.alive) continue;
                const float reach = e.radius + 6.0f;
                const double dx = e.pos.x - m.pos.x, dy = e.pos.y - m.pos.y;
                if (dx * dx + dy * dy < (double)reach * reach) { blew = true; break; }
            }
            if (!blew) {
                for (Missile& em : missiles) {
                    if (em.dead) continue;
                    const double dx = em.pos.x - m.pos.x, dy = em.pos.y - m.pos.y;
                    if (dx * dx + dy * dy < 16.0 * 16.0) { blew = true; break; }
                }
            }
            if (!blew && world.solidAt(m.pos) >= 0) blew = true;
        }
        if (blew) {
            m.dead = true;
            boom(m.pos, 13.0f, pal::SALVO);
            explode(m.pos, rules::SALVO_BLAST, rules::SALVO_DAMAGE, 0.0f, rules::SALVO_CRATER, 3.0e4f, false);
            continue;
        }
        if (rng.f() < 0.8f)
            spawnSparks(m.pos - dir * 6.0f, m.vel * 0.15f, 1, 30.0f, pal::SALVO, 0.25f);
    }
    pmissiles.erase(std::remove_if(pmissiles.begin(), pmissiles.end(),
                                   [](const PMissile& m) { return m.dead; }), pmissiles.end());
}

// ------------------------------------------------------------- force field --
// A bubble around the player that shoves everything out of it: bullets and
// missiles are turned back, drones are pushed off, rocks are flung, and
// debris scatters. It costs energy while it is on.
void Game::updateField(float dt) {
    if (pl.hasField) {
        if (pl.fieldOn) {
            pl.field -= rules::FIELD_DRAIN * dt;
            if (pl.field <= 0.0f) { pl.field = 0.0f; pl.fieldOn = false; pl.fieldLocked = true; sfxUI(Sfx::FieldEmpty, 0.9f); }
        } else {
            pl.field = std::min(100.0f, pl.field + rules::FIELD_REGEN * dt);
            if (pl.fieldLocked && pl.field >= rules::FIELD_RESTART) pl.fieldLocked = false;
        }
    }
    if (!pl.fieldOn) return;

    const dv2 c = pl.pos;
    const float R = rules::FIELD_RADIUS;
    // Push strength is never quite zero at the rim, or fast things would slip
    // through: it ramps from 30% at the edge to full at the core.
    auto strength = [&](float d) { return 0.3f + 0.7f * (1.0f - d / R); };

    for (EnemyBullet& b : ebullets) {
        const v2 rel = tov2(b.pos - c);
        const float d = len(rel);
        if (d >= R || d < 1e-3f) continue;
        b.vel += (rel / d) * (rules::FIELD_BULLET * strength(d) * dt);
        if (rng.f() < 0.2f) spawnSparks(b.pos, v2(0, 0), 1, 90.0f, pal::FIELD, 0.25f);
    }
    for (Missile& m : missiles) {
        if (m.dead) continue;
        const v2 rel = tov2(m.pos - c);
        const float d = len(rel);
        if (d >= R || d < 1e-3f) continue;
        m.vel += (rel / d) * (rules::FIELD_MISSILE * strength(d) * dt);
    }
    for (Enemy& e : enemies) {
        if (!e.alive || e.kind != Enemy::Drone) continue;    // turrets are bolted down
        const v2 rel = tov2(e.pos - c);
        const float d = len(rel);
        if (d >= R || d < 1e-3f) continue;
        e.vel += (rel / d) * (rules::FIELD_DRONE * strength(d) * dt);
    }
    for (Nuke& n : nukes) {
        if (n.dead) continue;
        const v2 rel = tov2(n.pos - c);
        const float d = len(rel);
        if (d >= R || d < 1e-3f) continue;
        n.vel += (rel / d) * (rules::FIELD_MISSILE * strength(d) * dt);
    }
    for (int s : world.active) {
        Body& b = world.bodies[s];
        if (!b.alive) continue;
        if (pl.grounded && s == pl.ground.slot) continue;    // do not throw the rock you stand on
        const v2 rel = tov2(b.pos - c);
        const float d = len(rel);
        const float gap = d - b.radius;                      // the field meets its near edge
        if (gap >= R || d < 1e-3f) continue;
        const float f = 1.0f - clampf(gap, 0.0f, R) / R;
        b.vel += (rel / d) * (rules::FIELD_ROCK * f * b.invMass * dt);
        const float sp = len(b.vel);
        if (sp > cfg::MAX_BODY_SPEED) b.vel = b.vel * (cfg::MAX_BODY_SPEED / sp);
    }
    for (Particle& q : parts) {
        const v2 rel = tov2(q.pos - c);
        const float d = len(rel);
        if (d >= R || d < 1e-3f) continue;
        q.vel += (rel / d) * (4500.0f * strength(d) * dt);
    }
}

// ------------------------------------------------------------------ drawing --
void Game::drawPMissiles(Renderer& r) {
    for (const PMissile& m : pmissiles) {
        if (m.dead) continue;
        const v2 p = camRel(m.pos);
        if (!inView(p, 40.0f)) continue;
        const v2 d = norm(m.vel), n = perp(d);
        const v2 body[4] = { p + d * 7.0f, p + n * 2.4f - d * 3.5f, p - d * 5.0f, p - n * 2.4f - d * 3.5f };
        r.poly(body, 4, true, pal::SALVO, 2.8f);
        r.line(p - d * 5.0f, p - d * (11.0f + rng.f() * 8.0f), Col(0.8f, 1.0f, 0.95f), 2.6f);
    }
}

// Brackets around whatever a homing weapon has locked onto.
void Game::drawLocks(Renderer& r) {
    auto bracket = [&](dv2 at, float radius, Col c, float spin) {
        const v2 p = camRel(at);
        if (!inView(p, 60.0f)) return;
        for (int k = 0; k < 4; ++k) {
            const float a = spin + k * PIF * 0.5f;
            r.arc(p, radius, a - 0.25f, a + 0.25f, 4, c, 2.6f);
        }
    };
    for (const Bullet& b : bullets) {
        if (!b.homing || b.targetId == 0) continue;
        if (const Enemy* e = findEnemy(b.targetId)) bracket(e->pos, e->radius + 12.0f, pal::HOMING, time * 3.0f);
    }
    for (const PMissile& m : pmissiles) {
        if (m.dead || m.targetId == 0) continue;
        if (const Enemy* e = findEnemy(m.targetId))         bracket(e->pos, e->radius + 9.0f, pal::SALVO, -time * 4.0f);
        else if (const Missile* em = findMissile(m.targetId)) bracket(em->pos, 14.0f, pal::SALVO, -time * 4.0f);
    }
}

void Game::drawField(Renderer& r) {
    if (!pl.fieldOn) return;
    const v2 p = camRel(pl.pos);
    const float R = rules::FIELD_RADIUS;
    const float low = pl.field < 20.0f ? (std::fmod(time * 8.0f, 1.0f) < 0.5f ? 0.35f : 1.0f) : 1.0f;
    const float flicker = (0.85f + 0.15f * std::sin(time * 37.0f)) * low;

    r.circle(p, R, 80, pal::FIELD, 1.5f * flicker);
    for (int k = 0; k < 6; ++k) {                          // a slowly turning lattice
        const float a0 = time * 0.7f + k * TAUF / 6.0f;
        r.arc(p, R * 0.94f, a0, a0 + 0.62f, 7, pal::FIELD, 2.0f * flicker);
        r.arc(p, R * 0.80f, -a0, -a0 + 0.35f, 5, pal::FIELD, 1.2f * flicker);
    }
    for (int i = 0; i < 3; ++i) {                          // ripples travelling outward
        const float f = std::fmod(time * 0.9f + i / 3.0f, 1.0f);
        r.circle(p, R * (0.2f + 0.8f * f), 64, pal::FIELD, 1.3f * (1.0f - f) * flicker);
    }
}

// ------------------------------------------------------------- blast shield --
// A narrow plate held toward the cursor. Bullets and missiles that reach it are
// stopped; blasts that go off within its arc are soaked up. Both cost charge,
// and whatever the charge cannot cover gets through to the suit.
bool Game::shieldBlocks(dv2 p) const {
    if (!pl.shieldUp) return false;
    const v2 rel = tov2(p - pl.pos);
    const float d = len(rel);
    // A band rather than a line, wider than one step of a fast bullet, so nothing
    // can hop across the plate between two frames.
    if (d < rules::SHIELD_RADIUS - 11.0f || d > rules::SHIELD_RADIUS + 8.0f) return false;
    return std::fabs(wrapAngle(std::atan2(rel.y, rel.x) - pl.aim)) <= rules::SHIELD_ARC * 0.5f;
}

float Game::shieldAbsorb(dv2 src, float dmg) {
    if (!pl.shieldUp || pl.shield <= 0.0f || dmg <= 0.0f) return dmg;
    const v2 rel = tov2(src - pl.pos);
    if (len2(rel) <= 1.0f) return dmg;                      // right on top of you: no "side" to shield
    if (std::fabs(wrapAngle(std::atan2(rel.y, rel.x) - pl.aim)) > rules::SHIELD_ARC * 0.5f + 0.04f)
        return dmg;                                         // the blast is outside the plate's arc
    const float take = std::min(dmg, pl.shield);
    pl.shield -= take;
    pl.shieldFlash = 1.0f;
    if (pl.shield <= 0.01f) sfx(Sfx::ShieldBreak, pl.pos, 1.0f, 1.0f, 1500.0f);
    else                    sfx(Sfx::ShieldBlock, pl.pos, clampf(0.45f + take / 10.0f, 0.45f, 1.0f), sfxRng.range(0.9f, 1.15f), 1500.0f);
    return dmg - take;
}

void Game::drawShield(Renderer& r) {
    if (!pl.shieldUp) return;
    const v2 p = camRel(pl.pos);
    const float R = rules::SHIELD_RADIUS;
    const float a0 = pl.aim - rules::SHIELD_ARC * 0.5f, a1 = pl.aim + rules::SHIELD_ARC * 0.5f;
    const float f = pl.shieldFlash;
    const float charge = pl.shield / rules::SHIELD_CAPACITY;
    const float blink = charge < 0.25f ? (std::fmod(time * 9.0f, 1.0f) < 0.5f ? 0.45f : 1.0f) : 1.0f;
    const Col c = mix(pal::SHIELD, Col(1.0f, 1.0f, 1.0f), f * 0.7f);
    const float I = (2.6f + 2.6f * f) * blink;

    // Three close arcs make a heavy plate; the caps and a faint inner arc give it depth.
    for (int k = -1; k <= 1; ++k) r.arc(p, R + k * 2.6f, a0, a1, 10, c, I);
    r.line(p + fromAngle(a0) * (R - 7.0f), p + fromAngle(a0) * (R + 7.0f), c, I);
    r.line(p + fromAngle(a1) * (R - 7.0f), p + fromAngle(a1) * (R + 7.0f), c, I);
    r.arc(p, R - 10.0f, a0, a1, 8, c, 0.9f * blink);
}

// ---------------------------------------------------------- fractal shell --
// One shot, up to 32 pieces. The parent flies like a homing shell, and every so often
// it comes apart into two, which fly apart a little and each carry on homing, each
// on a different target where there is more than one. A child has 0.45 of its
// parent's strength (half, less a tenth), and a piece that is five generations down
// stops splitting. How often it splits depends on how far away you are pointing (on a timer;
// with no gravity the interval is exactly the cursor distance): aim close and it divides quickly into a cloud; aim far and it flies a long way as
// one heavy shell before it starts.
static Col fractalColour(int gen) {
    const float t = clampf(gen / (float)rules::FRACTAL_SPLITS, 0.0f, 1.0f);
    return mix(Col(1.55f, 0.10f, 0.05f), Col(1.45f, 1.30f, 0.12f), t);          // blood red, going to yellow as it comes apart
}

float Game::fractalSplitDistance(float aimDist) {
    return clampf(rules::FRACTAL_SPLIT_K * aimDist, rules::FRACTAL_SPLIT_MIN, rules::FRACTAL_SPLIT_MAX);
}

// It splits on a timer, not by measured distance, so gravity can bend a shot without
// throwing off the rhythm; the timer is set to the time the cursor distance takes at the
// launch speed, which makes it exactly the cursor distance where there is no gravity.
float Game::fractalSplitTime(float aimDist, float speed) {
    return fractalSplitDistance(aimDist) / std::max(1.0f, speed);
}

// How long a piece needs to live to get through all the splits still ahead of it.
static float fractalLife(int gen, float splitTime) {
    const int left = rules::FRACTAL_SPLITS - gen + 1;
    return clampf(left * splitTime + 2.0f, 3.0f, 24.0f);
}

void Game::fireFractal(float aimDist) {
    if (!pl.hasFractal || pl.fractalAmmo <= 0 || pl.fractalCd > 0.0f || state != State::Playing) return;
    --pl.fractalAmmo;
    pl.fractalCd = rules::FRACTAL_COOLDOWN;

    const v2 dir = fromAngle(pl.aim);
    Bullet b;
    b.pos = dv2(pl.pos.x + dir.x * 16.0, pl.pos.y + dir.y * 16.0);
    b.vel = dir * rules::FRACTAL_SPEED + pl.vel;
    b.caliber = rules::FRACTAL_CAL;
    b.gravScale = 6.0f;
    b.budget = tune::HEAVY_PEN;
    b.heavy = true;
    b.homing = true;
    b.owner = pl.id;
    b.gen = 0;
    b.power = 1.0f;
    b.splitEvery = fractalSplitTime(aimDist, len(b.vel));
    b.life = fractalLife(0, b.splitEvery);
    b.col = fractalColour(0);
    bullets.push_back(b);

    pl.vel -= dir * 90.0f;
    shake = std::max(shake, 0.45f);
    sfx(Sfx::FractalFire, pl.pos, 1.0f, sfxRng.range(0.97f, 1.03f), 2400.0f);
    spawnSparks(b.pos, dir * 80.0f, 14, 170.0f, b.col, 0.25f);
}

void Game::splitFractal(const Bullet& parent) {
    const v2 dir = norm(parent.vel);
    const float speed = len(parent.vel);

    // The two best targets from here: the ones nearest, and nearest to where it is heading.
    struct Cand { int id; float cost; float side; };
    std::vector<Cand> cands;
    for (const Enemy& e : enemies) {
        if (!e.alive) continue;
        const v2 rel = tov2(e.pos - parent.pos);
        const float d = len(rel);
        if (d > rules::HOMING_RANGE * 1.3f || d < 1.0f) continue;
        const float c = dot(rel / d, dir);
        if (c < -0.2f) continue;
        cands.push_back({ e.id, d * (1.6f - c), cross(dir, rel / d) });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
    int left = 0, right = 0;                     // target ids for the child that turns left, and the one that turns right
    if (!cands.empty()) {
        left = right = cands[0].id;
        if (cands.size() > 1) {                  // two targets: the one on the left goes to the left-hand child
            const bool firstIsLeft = cands[0].side >= cands[1].side;
            left  = firstIsLeft ? cands[0].id : cands[1].id;
            right = firstIsLeft ? cands[1].id : cands[0].id;
        }
    }

    const float rootP = std::sqrt(rules::FRACTAL_CHILD);
    for (int s = 0; s < 2; ++s) {
        const float turn = (s == 0 ? 1.0f : -1.0f) * rules::FRACTAL_SPREAD;
        Bullet c = parent;
        c.gen = parent.gen + 1;
        c.power = parent.power * rules::FRACTAL_CHILD;
        c.flown = 0.0f;
        c.vel = rot(dir, turn) * (speed * rules::FRACTAL_SPEEDUP);
        c.caliber = std::max(rules::FRACTAL_MIN_CAL, parent.caliber * rootP);
        c.budget = parent.budget * rootP;
        c.life = std::max(parent.life, fractalLife(c.gen, c.splitEvery));
        c.targetId = s == 0 ? left : right;
        c.col = fractalColour(c.gen);
        if ((int)bullets.size() < 900) bullets.push_back(c);
    }
    // A small flash and a dull crack where it divided; the smaller pieces crack softer.
    const float small = std::sqrt(parent.power);
    ring(parent.pos, 22.0f + 36.0f * small, 0.32f, parent.col, 1.0f);
    spawnSparks(parent.pos, parent.vel * 0.3f, 6, 110.0f, parent.col, 0.25f);
    sfx(Sfx::FractalSplit, parent.pos, 0.55f + 0.45f * small, sfxRng.range(0.93f, 1.03f), 1800.0f);
}

// A heavy shell going off where it landed. Ordinary ones use the fixed numbers; a fractal
// piece scales them by its strength, and its blast radius by the square root of it.
void Game::shellBurst(const Bullet& b) {
    if (b.gen >= 0) {
        const float s = std::sqrt(b.power);
        boom(b.pos, 12.0f + 18.0f * s, b.col);
        explode(b.pos, rules::FRACTAL_SPLASH_R * s, rules::FRACTAL_DAMAGE * b.power, 0,
                rules::FRACTAL_CRATER * s, rules::FRACTAL_KICK * b.power, false);
    } else {
        explode(b.pos, rules::HEAVY_SPLASH_R, rules::HEAVY_DAMAGE, 0, 0, 0, false);
    }
}
