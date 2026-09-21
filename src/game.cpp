#include "game.h"
#include <cstdio>
#include <cstring>


static const Col C_PLAYER (0.85f, 0.95f, 1.00f);
static const Col C_VISOR  (0.35f, 0.95f, 1.00f);
static const Col C_FLAME  (1.00f, 0.55f, 0.15f);
static const Col C_BULLET (1.00f, 0.86f, 0.45f);
static const Col C_HEAVY  (1.00f, 0.45f, 0.75f);
static const Col C_HUD    (0.45f, 0.90f, 0.95f);
static const Col C_WARN   (1.00f, 0.42f, 0.30f);

// ------------------------------------------------------------------- setup --
void Game::init(Renderer& r, uint64_t seed) {
    baseSeed = seed;
    cam.halfW = zoomTarget;
    startRun(r);                 // builds the world, drops the player in, starts level 1
}

void Game::respawn() {
    // Drop in next to the nearest decent rock so the first thing you see is
    // something you can land on.
    int best = -1;
    double bestD = 1e30;
    for (int s : world.active) {
        const Body& b = world.bodies[s];
        if (b.radius < 45.0f) continue;
        const double d = len2(b.pos);
        if (d < bestD) { bestD = d; best = s; }
    }
    pl = Player();
    if (best >= 0) {
        const Body& b = world.bodies[best];
        // Walk around the rock until we find a patch of sky to stand on.
        for (int i = 0; i < 16; ++i) {
            const float a = i * TAUF / 16.0f;
            const dv2 p(b.pos.x + std::cos(a) * (b.radius + 22.0),
                        b.pos.y + std::sin(a) * (b.radius + 22.0));
            if (world.probe(p, 12.0f, nullptr, nullptr) < 0) {
                pl.pos = p;
                pl.up  = v2(std::cos(a), std::sin(a));
                break;
            }
        }
        if (pl.pos.x == 0 && pl.pos.y == 0)
            pl.pos = dv2(b.pos.x, b.pos.y + b.radius + 22.0);
        pl.vel = b.vel;                      // ride along with the rock
    }
    bullets.clear();
    parts.clear();
    cam.pos = pl.pos;
    if (allItems) grantAllItems();
}

// --------------------------------------------------------------- particles --
void Game::spawnSparks(dv2 p, v2 base, int n, float speed, Col c, float life) {
    for (int i = 0; i < n; ++i) {
        if (parts.size() >= 60000) break;
        Particle q;
        q.pos = p;
        q.vel = base + rng.dir() * (speed * rng.range(0.25f, 1.0f));
        q.maxLife = q.life = life * rng.range(0.55f, 1.3f);
        q.size = rng.range(1.4f, 3.2f);
        q.col = c;
        q.kind = 0;
        parts.push_back(q);
    }
}

void Game::drainWorldEvents() {
    for (const WorldEvent& e : world.events) {
        switch (e.kind) {
            case WorldEvent::Debris: {
                if (parts.size() >= 60000) break;
                Particle q;
                q.pos = e.pos;
                q.vel = e.vel;
                q.maxLife = q.life = rng.range(4.0f, 9.0f);
                q.size = clampf(e.size * 0.55f, 2.0f, 14.0f);
                q.ang = rng.angle();
                q.angVel = rng.sym(2.4f);
                q.col = e.col;
                q.kind = 1;
                parts.push_back(q);
                spawnSparks(e.pos, e.vel, 5, 70.0f, Col(1.0f, 0.7f, 0.35f), 0.5f);
            } break;
            case WorldEvent::Split:
                ++rocksSplit;
                sfx(Sfx::RockSplit, e.pos, 0.9f, sfxRng.range(0.85f, 1.15f), 1900.0f);
                spawnSparks(e.pos, e.vel, 14, 110.0f, Col(1.0f, 0.8f, 0.45f), 0.7f);
                shake = std::max(shake, 0.35f);
                break;
            case WorldEvent::Impact:
                sfx(Sfx::RockHit, e.pos, 0.45f, sfxRng.range(0.7f, 1.0f), 900.0f);
                spawnSparks(e.pos, e.vel, 3, 50.0f, Col(1.0f, 0.75f, 0.4f), 0.3f);
                break;
        }
    }
    world.events.clear();
}

void Game::updateParticles(float dt) {
    size_t w = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        Particle& q = parts[i];
        q.life -= dt;
        if (q.life <= 0) continue;
        if (q.kind == 1) {
            // Chunks are heavy enough to feel the pull of nearby rock.
            q.vel += world.gravityAt(q.pos, 1200.0) * dt;
            q.ang += q.angVel * dt;
        } else {
            q.vel *= std::exp(-0.7f * dt);
        }
        q.pos.x += (double)q.vel.x * dt;
        q.pos.y += (double)q.vel.y * dt;
        parts[w++] = q;
    }
    parts.resize(w);
}

// ----------------------------------------------------------------- bullets --
void Game::updateBullets(float dt) {
    size_t w = 0;
    for (size_t i = 0; i < bullets.size(); ++i) {
        Bullet b = bullets[i];
        b.life -= dt;
        if (b.life <= 0) {
            if (b.homing || b.gen >= 0) detonateBullet(b);     // the homing weapons only reach so far: they go off when their time is up
            continue;
        }
        b.vel += world.gravityAt(b.pos, 1500.0) * (dt * b.gravScale);
        if (b.homing) steerHoming(b, dt);

        const float speed = len(b.vel);
        // Step finely enough that the carved holes overlap into a clean tunnel.
        const int steps = std::max(1, std::min(28, (int)(speed * dt / (b.caliber * 0.62f)) + 1));
        const float sdt = dt / steps;
        const float stepLen = speed * sdt;
        bool spent = false;

        for (int s = 0; s < steps && !spent; ++s) {
            b.pos.x += (double)b.vel.x * sdt;
            b.pos.y += (double)b.vel.y * sdt;
            if (bulletHitsTargets(b) || (versus && bulletHitsPlayers(b))) { spent = true; break; }
            if (b.gen >= 0) {                         // a fractal shell divides after every so often
                b.flown += sdt;
                if (b.gen < rules::FRACTAL_SPLITS && b.flown >= b.splitEvery) { splitFractal(b); spent = true; break; }
            }
            const int hit = world.solidAt(b.pos);
            if (hit < 0) continue;
            if (netClient) {                          // the host decides what a round does to a rock
                spawnSparks(b.pos, b.vel * -0.1f, b.heavy ? 8 : 3, 120.0f, b.col, 0.3f);
                spent = true;
                break;
            }

            world.damage(hit, b.pos, b.caliber, 0.34f, rng.u32());
            b.budget -= stepLen;

            // Momentum transfer: hardly moves a mountain, launches a pebble.
            Body& bd = world.bodies[hit];
            const v2 rvec = tov2(b.pos - bd.pos);
            const v2 P = norm(b.vel) * (b.caliber * 180.0f);
            bd.vel    += P * bd.invMass;
            bd.angVel += cross(rvec, P) * bd.invInertia;

            if (s == 0) sfx(Sfx::RockHit, b.pos, b.heavy ? 1.0f : 0.7f, sfxRng.range(0.85f, 1.25f), 1300.0f);
            if (s == 0 || (s % 4) == 0)
                spawnSparks(b.pos, b.vel * -0.12f, b.heavy ? 6 : 2,
                            b.heavy ? 220.0f : 130.0f, b.col, 0.35f);
            if (b.budget <= 0.0f) {
                spawnSparks(b.pos, b.vel * -0.2f, b.heavy ? 40 : 9,
                            b.heavy ? 340.0f : 170.0f,
                            b.heavy ? Col(1.0f, 0.6f, 0.3f) : Col(1.0f, 0.85f, 0.5f),
                            b.heavy ? 0.9f : 0.4f);
                if (b.gen >= 0) {                     // a fractal piece bursts in the rock as hard as its strength
                    shake = std::max(shake, 0.25f + 0.35f * std::sqrt(b.power));
                    shellBurst(b);
                } else if (b.heavy) {
                    shake = std::max(shake, 0.6f);
                    // A shell going off in the rock still shreds anything nearby.
                    explodeOwner = b.owner;
                    explode(b.pos, rules::HEAVY_SPLASH_R, rules::HEAVY_SPLASH, versus ? rules::VS_HEAVY_SPLASH : 0.0f, rules::HEAVY_CRATER, rules::HEAVY_KICK, false);
                    blastKick(b.pos, b.owner);                    // and it shoves whoever fired it: a rocket jump
                }
                spent = true;
            }
        }
        if (spent) continue;
        bullets[w++] = b;
    }
    bullets.resize(w);
}

void Game::fire(Player& p, bool heavy) {
    const bool self = &p == &pl;
    const v2 dir = fromAngle(p.aim);
    const v2 d2  = rot(dir, rng.sym(heavy ? 0.004f : 0.017f));
    Bullet b;
    b.pos     = dv2(p.pos.x + dir.x * 15.0, p.pos.y + dir.y * 15.0);
    b.vel     = d2 * (heavy ? tune::HEAVY_V : tune::BULLET_V) + p.vel;
    b.caliber = heavy ? tune::HEAVY_CAL : tune::BULLET_CAL;
    b.gravScale = heavy ? tune::HEAVY_GRAV : tune::BULLET_GRAV;
    b.budget  = heavy ? tune::HEAVY_PEN : tune::BULLET_PEN;
    b.life    = heavy ? rules::HOMING_LIFE : 2.6f;
    b.heavy   = heavy;
    b.homing  = heavy;              // the charge shot is the homing shell
    b.owner   = p.id;
    b.col     = heavy ? C_HEAVY : C_BULLET;
    if (versus) b.col = mix(b.col, p.tint, 0.55f);     // so you can tell whose shot it is
    bullets.push_back(b);
    if (versus) { ++vsShots; if (netHost) netShot(b); }

    p.vel -= d2 * (heavy ? 145.0f : 6.0f);
    if (self) shake = std::max(shake, heavy ? 0.5f : 0.07f);
    if (self) ++shotsFired;
    sfx(heavy ? Sfx::Shell : Sfx::Rifle, p.pos, self ? (heavy ? 1.0f : 0.8f) : (heavy ? 0.9f : 0.6f),
        heavy ? 1.0f : sfxRng.range(0.93f, 1.08f));
    spawnSparks(b.pos, d2 * (heavy ? 90.0f : 40.0f), heavy ? 10 : 3,
                heavy ? 150.0f : 80.0f, b.col, 0.16f);
}

// ------------------------------------------------------------------ player --
// The local player's keyboard and mouse, boiled down to a PlayerCmd, plus the
// things only a single-player run has (the force field, the blast shield, the
// nuke and the salvo). The physics is in stepPlayer, which anyone can be run through.
void Game::updatePlayer(Renderer& r, const Input& in, float dt) {
    // Aim follows the cursor through the same projection the world uses.
    const float nx =  (in.mousePx.x / (float)r.fbw - 0.5f) * 2.0f * cam.halfW;
    const float ny = -(in.mousePx.y / (float)r.fbh - 0.5f) * 2.0f * cam.halfH();
    // The shader rolls world space by cam.angle on its way to the screen, so
    // undo that roll to turn the cursor back into a world direction.
    const v2 off = rot(v2(nx, ny), std::cos(-cam.angle), std::sin(-cam.angle));
    const v2 toCursor((float)(cam.pos.x + off.x - pl.pos.x), (float)(cam.pos.y + off.y - pl.pos.y));
    lastAimDist = len(toCursor);                 // how far away the cross is: the fractal shell splits by it
    v2 aimDir = norm(toCursor);
    if (len2(aimDir) < 0.25f) aimDir = fromAngle(pl.aim);

    // ---- force field (only once bought): X switches it on and off
    if (in.pressed['X'] && pl.hasField) {
        if (pl.fieldOn)                                       { pl.fieldOn = false; sfxUI(Sfx::FieldOff, 0.8f); }
        else if (!pl.fieldLocked && pl.field > 5.0f)          { pl.fieldOn = true; sfxUI(Sfx::FieldOn, 0.9f); }
    }

    // ---- the blast shield: Left Alt holds it up, once bought
    pl.shieldUp = pl.hasShield && pl.shield > 0.0f && in.down[VK_LMENU] && state == State::Playing;
    pl.shieldFlash = approach(pl.shieldFlash, 0.0f, 7.0f, dt);
    if (pl.shieldUp && !prevShieldUp) sfxUI(Sfx::ShieldUp, 0.9f);
    prevShieldUp = pl.shieldUp;

    // ---- everything else becomes a command
    PlayerCmd& c = localCmd;
    c.aim    = std::atan2(aimDir.y, aimDir.x);
    c.thrust = in.mouse[1] || in.down[VK_SHIFT];             // right mouse (or Shift) is the rocket
    c.fire   = in.mouse[0];
    c.move   = 0;
    if (in.down['A'] || in.down[VK_LEFT])  c.move -= 1.0f;
    if (in.down['D'] || in.down[VK_RIGHT]) c.move += 1.0f;
    if (in.pressed[VK_SPACE] || in.pressed['W'] || in.pressed[VK_UP]) ++c.jumpSeq;
    // The charge shot has to be bought: it is the homing shell.
    if (in.pressed['F'] || in.mousePressed[2]) ++c.heavySeq;

    stepPlayer(pl, c, dt);

    pl.nukeCd  -= dt;
    pl.salvoCd -= dt;
    pl.fractalCd -= dt;
    if (in.pressed['N']) throwNuke(aimDir);
    if (in.pressed['G']) fireSalvo(aimDir);
    if (in.pressed['Z']) fireFractal(lastAimDist);
}

// One frame of one player's life: rocket, gravity, contact with rock, walking,
// jumping, healing and weapons. Written for anybody, so the same code moves the
// local player, a bot, and (on the host) a human on the far end of a connection.
void Game::stepPlayer(Player& p, const PlayerCmd& c, float dt) {
    const bool self = &p == &pl;
    p.aim = c.aim;
    const v2 aimDir = fromAngle(p.aim);
    const bool playing = state == State::Playing;

    v2 g = world.gravityAt(p.pos, 2600.0);
    if (floating && self) g = v2(0, 0);                       // test hook

    // ---- rocket
    // Once the tank runs dry the rocket stays off until it has recovered a bit.
    // Without this, the sliver of fuel regenerated on the frame the rocket cut
    // out would re-enable it on the next one, and an empty tank would still
    // thrust every other frame.
    if (p.fuel <= 0.0f)                    p.fuelLocked = true;
    else if (p.fuel >= tune::FUEL_RESTART) p.fuelLocked = false;
    p.thrusting = c.thrust && !p.fuelLocked && p.fuel > 0.0f;
    if (self && c.thrust && !p.thrusting && playing) sfxUI(Sfx::FuelEmpty, 0.8f);   // the dry click
    if (p.thrusting) {
        p.vel += aimDir * (tune::THRUST * dt);
        p.fuel = std::max(0.0f, p.fuel - tune::FUEL_BURN * dt);
        p.thrustGlow = 1.0f;
        if (rng.f() < dt * 90.0f)
            spawnSparks(dv2(p.pos.x - aimDir.x * 10, p.pos.y - aimDir.y * 10),
                        p.vel - aimDir * 260.0f, 2, 90.0f, C_FLAME, 0.30f);
    } else {
        p.fuel = std::min(tune::FUEL_MAX, p.fuel + tune::FUEL_REGEN * dt);
    }
    p.thrustGlow = approach(p.thrustGlow, 0.0f, 9.0f, dt);
    p.hurtGlow   = approach(p.hurtGlow, 0.0f, 3.0f, dt);
    p.protect    = std::max(0.0f, p.protect - dt);

    const float mv = c.move;
    p.vel += g * (dt * tune::PLAYER_GRAV);

    // ---- integrate, substepped so we never tunnel through thin rock
    const float speed = len(p.vel);
    const int   steps = std::max(1, std::min(10, (int)(speed * dt / 3.0f) + 1));
    const float sdt   = dt / steps;
    const bool  wasGrounded = p.grounded;
    float landImpact = 0.0f;
    p.grounded = false;
    for (int s = 0; s < steps; ++s) {
        p.pos.x += (double)p.vel.x * sdt;
        p.pos.y += (double)p.vel.y * sdt;

        v2 n; float depth;
        const int hit = world.probe(p.pos, tune::PLAYER_R + 2.0f, &n, &depth);
        if (hit < 0) continue;
        Body& b = world.bodies[hit];
        const v2 rvec = tov2(p.pos - b.pos);
        const v2 surf = b.velAt(rvec);
        const float pen = depth - 2.0f;
        if (pen > 0) { p.pos.x += n.x * pen; p.pos.y += n.y * pen; }

        v2 rel = p.vel - surf;
        const float vn = dot(rel, n);
        if (vn < 0) {
            const float impact = -vn;
            if (!wasGrounded) landImpact = std::max(landImpact, impact);
            rel -= n * vn;
            if (impact > 300.0f && !wasGrounded) {
                damagePlayer(p, (impact - 300.0f) * 0.06f, v2(0, 0), -1);
                spawnSparks(p.pos, n * 60.0f, 10, 130.0f, C_WARN, 0.4f);
            }
        }
        p.vel = rel + surf;
        p.grounded = true;
        p.up = n;
        p.ground = world.ref(hit);
    }

    if (landImpact > 70.0f && p.grounded) sfx(Sfx::Land, p.pos, clampf(landImpact / 260.0f, 0.3f, 1.0f), sfxRng.range(0.9f, 1.1f));

    // ---- local up: surface normal on the ground, gravity vector in flight
    if (!p.grounded) {
        if (len2(g) > 400.0f)
            p.up = norm(lerp(p.up, norm(g) * -1.0f, clampf(6.0f * dt, 0.0f, 1.0f)));
        p.coyote -= dt;
    } else {
        p.coyote = 0.12f;
    }

    // ---- walking / air control
    // "Right" for the spaceman is 90 degrees clockwise from his up. That is what
    // ends up on the right of the screen once the POV camera has rolled up to
    // screen-up. (perp() rotates the other way, which made D walk left.)
    const v2 right = v2(p.up.y, -p.up.x);
    if (p.grounded) {
        Body* gb = world.get(p.ground);
        const v2 surf = gb ? gb->velAt(tov2(p.pos - gb->pos)) : v2(0, 0);
        v2 rel = p.vel - surf;
        const float vt = dot(rel, right);
        float dv;
        if (mv != 0.0f) dv = clampf(mv * tune::WALK_SPEED - vt,
                                    -tune::WALK_ACCEL * dt, tune::WALK_ACCEL * dt);
        else            dv = clampf(-vt, -tune::WALK_ACCEL * 1.4f * dt,
                                          tune::WALK_ACCEL * 1.4f * dt);
        rel += right * dv;
        p.vel = rel + surf;
        p.legPhase += std::fabs(vt) * dt * 0.05f;
    } else {
        p.vel += right * (mv * tune::AIR_ACCEL * dt);
        p.legPhase = approach(p.legPhase, 0.0f, 3.0f, dt);
    }

    // ---- jump
    p.jumpCd -= dt;
    const bool jumpPressed = c.jumpSeq != p.seenJump;
    p.seenJump = c.jumpSeq;
    if (jumpPressed && (p.grounded || p.coyote > 0) && p.jumpCd <= 0) {
        Body* gb = world.get(p.ground);
        const v2 surf = gb ? gb->velAt(tov2(p.pos - gb->pos)) : v2(0, 0);
        v2 rel = p.vel - surf;
        // The jump goes toward the cursor. It cannot go into the ground, so a cursor
        // below (or almost level with) the surface gives a low leap along it instead.
        v2 jd = aimDir;
        const float lift = dot(jd, p.up);
        if (lift < tune::JUMP_MIN_LIFT) {
            const v2 along = jd - p.up * lift;
            const float al = len(along);
            const v2 side = al > 1e-3f ? along / al : right * (p.facing >= 0.0f ? 1.0f : -1.0f);
            jd = side * std::sqrt(1.0f - tune::JUMP_MIN_LIFT * tune::JUMP_MIN_LIFT) + p.up * tune::JUMP_MIN_LIFT;
        }
        rel += jd * tune::JUMP_SPEED;
        rel += right * (mv * 90.0f);
        p.vel = rel + surf;
        // Kick back against the rock. Big ones shrug it off; pebbles do not.
        if (gb) {
            const v2 rr = tov2(p.pos - gb->pos);
            const v2 P  = jd * (-70.0f * tune::JUMP_SPEED);
            gb->vel    += P * gb->invMass;
            gb->angVel += cross(rr, P) * gb->invInertia;
        }
        p.grounded = false;
        p.coyote = 0;
        p.jumpCd = 0.18f;
        spawnSparks(p.pos, jd * -70.0f, 7, 90.0f, Col(0.8f, 0.9f, 1.0f), 0.3f);
        sfx(Sfx::Jump, p.pos, self ? 0.8f : 0.55f, sfxRng.range(0.95f, 1.06f));
    }

    if (self && debugTrace && ((int)(time * 60.0f) % 45) == 0) {
        const float want = PIF * 0.5f - std::atan2(p.up.y, p.up.x);
        printf("t=%5.1f grounded=%d up=(%+.2f,%+.2f)  camRoll=%+7.1f  want=%+7.1f  "
               "err=%+5.1f deg  halfW=%.0f\n",
               time, (int)p.grounded, p.up.x, p.up.y,
               cam.angle * 180.0f / PIF, want * 180.0f / PIF,
               wrapAngle(want - cam.angle) * 180.0f / PIF, cam.halfW);
        fflush(stdout);
    }

    p.facing = dot(aimDir, right) >= 0 ? 1.0f : -1.0f;
    // The suit heals, but only once nothing has hurt it for a few seconds.
    p.sinceHurt += dt;
    if (p.sinceHurt > rules::REGEN_DELAY && !versus)        // a versus match is not a place to hide and heal
        p.health = std::min(100.0f, p.health + rules::REGEN_RATE * dt);

    // ---- weapons
    p.fireCd  -= dt;
    p.heavyCd -= dt;
    if (c.fire && p.fireCd <= 0) { fire(p, false); p.fireCd = tune::FIRE_RATE; }
    const bool heavyPressed = c.heavySeq != p.seenHeavy;
    p.seenHeavy = c.heavySeq;
    if (p.hasHoming && heavyPressed && p.heavyCd <= 0) {
        fire(p, true);
        p.heavyCd = tune::HEAVY_RATE;
    }

    p.vel *= 0.99F;
}

// ------------------------------------------------------------------ update --
void Game::update(Renderer& r, const Input& in, float dt) {
    if (in.pressed['P'])   { paused = !paused; sfxUI(Sfx::Pause, 0.8f, paused ? 0.8f : 1.2f); }
    if (in.pressed['M'] && audio::ready()) {
        audio::toggleMute();
        if (!sandbox) say(audio::muted() ? "SOUND OFF" : "SOUND ON");
    }
    if (in.pressed['H'])   showHelp = !showHelp;
    if (in.pressed[VK_F1]) showHelp = !showHelp;
    if (in.pressed['C'])   povCamera = !povCamera;
    // A lost life restarts the level once the explosion has had its moment.
    if (!paused && state == State::Dead && stateTime > rules::DEATH_TIME) retryLevel(r);
    // R (or Enter on the game-over screen) starts a fresh run.
    if (versus) {
        if (!netClient && match.over && match.overTime > 1.0f && (in.pressed['R'] || in.pressed[VK_RETURN])) resetMatch();   // on a client the host decides
    } else if (in.pressed['R'] || (state == State::GameOver && stateTime > 0.8f && in.pressed[VK_RETURN])) {
        if (sandbox) respawn();
        else         startRun(r);
    }

    zoomTarget *= std::exp(-in.wheel * 0.14f);
    if (in.down['Q']) zoomTarget *= std::exp(1.1f * dt);
    if (in.down['E']) zoomTarget *= std::exp(-1.1f * dt);
    zoomTarget = clampf(zoomTarget, 150.0f, 4000.0f);

    if (!paused && state == State::Shop) {
        // The world holds still while you shop; only the menu runs.
        time += dt;
        mousePx = in.mousePx;
        updateShop(r, in);
        updateLevel(dt);
        updateWaves(dt);
        updateParticles(dt);
        flash = approach(flash, 0.0f, 2.4f, dt);
    } else if (!paused) {
        time += dt;
        const dv2 prev = pl.pos;
        if (net) netBegin(dt);
        if (!netClient) world.step(dt, pl.pos);        // a client is told where the rocks are
        if (!playerGone()) {
            updatePlayer(r, in, dt);
            updateField(dt);
        }
        if (versus) updateVersus(dt);
        mousePx = in.mousePx;
        updateBullets(dt);
        updateEnemies(dt);
        updateEnemyBullets(dt);
        updateMissiles(dt);
        updatePMissiles(dt);
        updateNukes(dt);
        updateWaves(dt);
        updateLevel(dt);
        updateParticles(dt);
        drainWorldEvents();
        if (net) netEnd(dt);
        distanceTravelled += len(pl.pos - prev);
        shake = approach(shake, 0.0f, 5.0f, dt);
        flash = approach(flash, 0.0f, 2.4f, dt);
    }

    // Camera: follow with a little lead toward the aim and the current drift.
    cam.halfW = approach(cam.halfW, zoomTarget, 7.0f, dt);

    // POV camera: roll the whole world so the spaceman's local up is screen up.
    // Taking the shortest way round matters, or walking past the twelve o'clock
    // point on a rock would spin the view the long way.
    const float wantRoll = povCamera ? (PIF * 0.5f - std::atan2(pl.up.y, pl.up.x)) : 0.0f;
    cam.angle = wrapAngle(cam.angle + wrapAngle(wantRoll - cam.angle) *
                                      (1.0f - std::exp(-9.0f * dt)));

    v2 look = fromAngle(pl.aim) * (cam.halfW * 0.17f) + pl.vel * 0.20f;
    const float lookMax = cam.halfW * 0.42f;
    if (len2(look) > lookMax * lookMax) look = norm(look) * lookMax;
    const double k = 1.0 - std::exp(-8.0 * dt);
    cam.pos.x += (pl.pos.x + look.x - cam.pos.x) * k;
    cam.pos.y += (pl.pos.y + look.y - cam.pos.y) * k;
    if (shake > 0.001f) {
        const v2 j = rng.disc() * (shake * cam.halfW * 0.035f);
        cam.pos.x += j.x;
        cam.pos.y += j.y;
    }

    if (netClient) {}                              // the host sends the rocks: a client must not make its own
    else if (versus) world.streamChunks(dv2(0, 0), rules::ARENA_RADIUS + 1500.0);
    else        world.streamChunks(cam.pos, cam.halfW * 1.7 + 1500.0);
    world.syncGeometry(r);
    updateAudio(dt);
}

// ------------------------------------------------------------------ stars --
void Game::drawStars(Renderer& r) {
    for (int layer = 0; layer < 3; ++layer) {
        const float par  = 0.10f + layer * 0.24f;
        const float cell = std::max(95.0f * (1.0f + layer * 0.7f), cam.halfW * 0.085f);
        const double bx = cam.pos.x * par, by = cam.pos.y * par;
        // Cover the circle enclosing the view, so stars do not pop in at the
        // corners when the POV camera rolls.
        const double hw = cam.viewRadius() + cell, hh = hw;
        const long long i0 = (long long)std::floor((bx - hw) / cell);
        const long long i1 = (long long)std::floor((bx + hw) / cell);
        const long long j0 = (long long)std::floor((by - hh) / cell);
        const long long j1 = (long long)std::floor((by + hh) / cell);
        for (long long j = j0; j <= j1; ++j) {
            for (long long i = i0; i <= i1; ++i) {
                const uint64_t h = hashCombine((uint64_t)i * 0x9E3779B1ull + layer,
                                               (uint64_t)j * 0xC2B2AE35ull);
                Rng sr(h);
                if (sr.f() > 0.72f) continue;
                const double sx = (double)i * cell + sr.f() * cell;
                const double sy = (double)j * cell + sr.f() * cell;
                const float b = sr.range(0.25f, 1.0f) * (0.45f + layer * 0.28f);
                const float tw = 0.85f + 0.15f * std::sin(time * sr.range(1.0f, 4.0f) + sr.angle());
                Col c(0.62f + sr.f() * 0.38f, 0.72f + sr.f() * 0.28f, 0.95f);
                r.point(v2((float)(sx - bx), (float)(sy - by)),
                        0.9f + layer * 0.8f, c, b * tw);
            }
        }
    }
}

// ------------------------------------------------------------- draw player --
void Game::drawPlayerFig(Renderer& r, const Player& p) {
    const v2 up  = p.up;
    const v2 rgt = v2(up.y, -up.x);
    const float f = p.facing;
    const v2 camRel((float)(p.pos.x - cam.pos.x), (float)(p.pos.y - cam.pos.y));
    // p.pos is the centre of the collision disc, so shift the drawing down
    // by the disc radius and the feet land exactly on the rock.
    const float FOOT = tune::PLAYER_R;
    auto L = [&](float x, float y) {
        return camRel + rgt * (x * f) + up * (y - FOOT);
    };

    const float hurt = p.hurtGlow;
    Col body = mix(p.tint, C_WARN, clampf(hurt, 0.0f, 0.9f));
    if (p.protect > 0.0f && std::fmod(p.protect, 0.25f) < 0.12f) body = mix(body, Col(1, 1, 1), 0.6f);   // flickers while protected
    const float gain = 1.35f;

    // legs, swinging while walking
    const float sw = std::sin(p.legPhase * 6.0f) * (p.grounded ? 1.0f : 0.25f);
    const float lift = p.grounded ? 0.0f : 1.6f;
    r.line(L(0, 7.0f), L( 2.4f * sw, 2.6f + lift), body, gain);
    r.line(L(2.4f * sw, 2.6f + lift), L(2.9f * sw + 0.8f, 0.2f + lift), body, gain);
    r.line(L(0, 7.0f), L(-2.4f * sw, 2.6f + lift), body, gain);
    r.line(L(-2.4f * sw, 2.6f + lift), L(-2.9f * sw + 0.8f, 0.2f + lift), body, gain);

    // torso
    const v2 torso[4] = { L(-2.7f, 6.6f), L(-2.3f, 12.4f), L(2.3f, 12.4f), L(2.7f, 6.6f) };
    r.poly(torso, 4, true, body, gain);

    // life-support pack
    const v2 pack[4] = { L(-2.4f, 7.6f), L(-4.6f, 8.2f), L(-4.6f, 12.0f), L(-2.4f, 11.9f) };
    r.poly(pack, 4, true, mix(body, C_HUD, 0.4f), gain * 0.8f);

    // helmet + visor
    const v2 head = L(0.2f, 15.2f);
    r.circle(head, 3.25f, 14, body, gain);
    r.arc(head, 2.35f, p.aim - 0.85f, p.aim + 0.85f, 7, C_VISOR, 1.9f);

    // arm and rifle, aimed with the cursor
    const v2 shoulder = L(1.4f, 11.6f);
    const v2 aimDir = fromAngle(p.aim);
    const v2 hand = shoulder + aimDir * 5.2f;
    r.line(shoulder, hand, body, gain);
    r.line(hand - aimDir * 1.5f, hand + aimDir * 6.5f, mix(body, C_BULLET, 0.5f), 1.6f);
    r.line(hand + aimDir * 2.0f + perp(aimDir) * 1.2f,
           hand + aimDir * 2.0f - perp(aimDir) * 1.2f, body, gain);

    // rocket plume
    if (p.thrustGlow > 0.02f) {
        const float t = p.thrustGlow;
        const v2 root = L(0.0f, 9.0f) - aimDir * 7.0f;
        for (int i = 0; i < 5; ++i) {
            const float jitter = rng.sym(0.30f);
            const float lenF = (9.0f + rng.f() * 22.0f) * t;
            const v2 tip = root - rot(aimDir, jitter) * lenF;
            r.line(root + perp(aimDir) * rng.sym(2.4f), tip,
                   mix(C_FLAME, Col(1.0f, 0.95f, 0.7f), rng.f()), 2.2f * t);
        }
    }

    // where "down" currently is, while airborne
    if (!p.grounded && &p == &pl) {
        const v2 g = world.gravityAt(p.pos, 2600.0);
        if (len2(g) > 250.0f) {
            const v2 d = norm(g);
            const v2 mid = L(0.0f, 9.0f);
            const v2 a0 = mid + d * 20.0f, a1 = mid + d * 34.0f;
            const Col gc(0.30f, 0.65f, 0.85f);
            r.line(a0, a1, gc, 0.7f);
            r.line(a1, a1 - rot(d, 0.45f) * 6.0f, gc, 0.7f);
            r.line(a1, a1 - rot(d, -0.45f) * 6.0f, gc, 0.7f);
        }
    }
}

// --------------------------------------------------------------- draw pass --
void Game::render(Renderer& r) {
    r.cam = cam;
    r.beginScene();
    r.useWorldProjection();
    // Behind the depot the world is dimmed, so the menu text reads cleanly.
    r.setGain(state == State::Shop ? 0.26f : 1.0f);

    drawStars(r);

    world.collectRenderData(cam, r, xforms, firsts, counts);
    r.setBodyXforms(xforms.data(), (int)xforms.size());
    r.flush();                                     // LOD dots and stars first
    r.drawBodies(firsts.data(), counts.data(), (int)firsts.size());

    // The level layer: beacon, hostiles, and everything they throw.
    drawGoal(r);
    drawShips(r);
    drawLaserBeams(r);
    drawEnemies(r);
    drawProjectiles(r);
    drawPMissiles(r);
    drawLocks(r);
    drawNukes(r);
    drawWaves(r);
    drawField(r);
    drawShield(r);

    // particles
    for (const Particle& q : parts) {
        const v2 p((float)(q.pos.x - cam.pos.x), (float)(q.pos.y - cam.pos.y));
        const float t = q.life / q.maxLife;
        if (q.kind == 1) {
            // tumbling rock chip drawn as a little wireframe shard
            const float s = q.size;
            const v2 a = p + rot(v2( s,  0.0f), q.ang);
            const v2 b = p + rot(v2(-s * 0.4f,  s * 0.8f), q.ang);
            const v2 c = p + rot(v2(-s * 0.9f, -s * 0.5f), q.ang);
            const Col col = q.col;
            const float g = clampf(t * 1.4f, 0.0f, 1.2f);
            r.line(a, b, col, g); r.line(b, c, col, g); r.line(c, a, col, g);
        } else {
            r.point(p, q.size * (0.4f + 0.6f * t), q.col, 0.4f + 2.2f * t * t);
        }
    }

    // bullets: a bright streak along the direction of travel
    for (const Bullet& b : bullets) {
        const v2 p((float)(b.pos.x - cam.pos.x), (float)(b.pos.y - cam.pos.y));
        const v2 d = norm(b.vel);
        if (b.gen >= 0) {
            // A fractal piece: a sharp diamond, 35% smaller than it was, that shrinks with every split, and a tail that shortens with it.
            const float sz = std::sqrt(b.power);
            const float rad = (2.4f + 5.0f * sz) * 0.65f;
            const v2 n = perp(d);
            r.line(p, p - d * ((8.0f + 20.0f * sz) * 0.65f), b.col, 1.6f + 0.6f * sz);
            const v2 dia[4] = { p + d * (rad * 1.5f), p + n * rad, p - d * (rad * 1.5f), p - n * rad };
            r.poly(dia, 4, true, b.col, 1.9f);
            r.point(p, (2.0f + 3.0f * sz) * 0.65f, b.col, 2.0f);
            continue;
        }
        const float tail = b.heavy ? 16.0f : 26.0f;
        r.line(p, p - d * tail, b.col, b.heavy ? 2.6f : 2.0f);
        if (b.heavy) r.circle(p, 6.0f, 9, b.col, 2.0f);
    }

    if (!playerGone()) drawPlayerFig(r, pl);
    for (const Peer& pe : peers) if (!pe.body.dead) drawPlayerFig(r, pe.body);
    r.flush();

    r.setGain(1.0f);
    r.useHudProjection();
    drawHud(r);
    if (versus) drawVersusHud(r);

    // A nuke blows the exposure out for a moment, like a flashbulb.
    const float e0 = r.exposure, b0 = r.bloomAmount;
    r.exposure    = e0 * (1.0f + flash * 2.6f);
    r.bloomAmount = b0 * (1.0f + flash * 2.2f);
    r.endScene();
    r.exposure = e0;
    r.bloomAmount = b0;
}

// --------------------------------------------------------------------- hud --
void Game::drawHud(Renderer& r) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float m = 20.0f * s;
    char buf[160];

    auto bar = [&](float x, float y, float w, float h, float frac,
                   Col c, const char* label, bool alert) {
        const Col edge = alert ? C_WARN : c;
        const v2 box[4] = { v2(x, y), v2(x + w, y), v2(x + w, y + h), v2(x, y + h) };
        r.poly(box, 4, true, edge, 1.1f);
        const int n = std::max(1, (int)(w / (5.0f * s)));
        for (int i = 0; i < n; ++i) {
            const float t = (i + 0.5f) / n;
            if (t > frac) break;
            const float px = x + 2.0f * s + t * (w - 4.0f * s);
            r.line(v2(px, y + 2.0f * s), v2(px, y + h - 2.0f * s), c, 1.7f);
        }
        r.text(v2(x, y - 5.0f * s), 9.0f * s, label, edge, 1.25f);
    };

    const float bw = 200.0f * s, bh = 13.0f * s, gap = 30.0f * s;
    float y = m + 12.0f * s;
    bar(m, y, bw, bh, pl.fuel / tune::FUEL_MAX, Col(1.0f, 0.65f, 0.25f),
        pl.fuelLocked ? "ROCKET  RECHARGING" : "ROCKET FUEL", pl.fuelLocked || pl.fuel < 0.2f * tune::FUEL_MAX);
    y += gap;
    // (The suit has moved to the bottom centre of the screen, and is much bigger.)
    // Everything below only appears once it has been bought.
    if (pl.hasField) {
        const char* label = pl.fieldOn ? "FORCE FIELD  ON  [X]"
                          : (pl.fieldLocked ? "FORCE FIELD  RECHARGING" : "FORCE FIELD  [X]");
        bar(m, y, bw, bh, pl.field / 100.0f, pal::FIELD, label, pl.fieldLocked || (pl.fieldOn && pl.field < 20.0f));
        y += gap;
    }
    if (pl.hasShield) {
        const char* label = pl.shieldUp ? "BLAST SHIELD  UP" : (pl.shield <= 0.0f ? "BLAST SHIELD  EMPTY" : "BLAST SHIELD  [LEFT ALT]");
        bar(m, y, bw, bh, pl.shield / rules::SHIELD_CAPACITY, pal::SHIELD, label, pl.shield < 20.0f);
        y += gap;
    }
    if (pl.hasHoming) {
        const float heavyFrac = 1.0f - clampf(pl.heavyCd / tune::HEAVY_RATE, 0.0f, 1.0f);
        bar(m, y, bw, bh, heavyFrac, pal::HOMING, "HOMING SHELL  [F]", false);
        y += gap;
    }
    if (pl.hasSalvo) {
        const float frac = 1.0f - clampf(pl.salvoCd / rules::SALVO_COOLDOWN, 0.0f, 1.0f);
        snprintf(buf, sizeof buf, "MISSILE SALVO  [G]   X%d", pl.salvoAmmo);
        bar(m, y, bw, bh, frac, pal::SALVO, buf, pl.salvoAmmo == 0);
        y += gap;
    }
    if (pl.hasFractal) {
        const float frac = 1.0f - clampf(pl.fractalCd / rules::FRACTAL_COOLDOWN, 0.0f, 1.0f);
        snprintf(buf, sizeof buf, "FRACTAL SHELL  [Z]   X%d", pl.fractalAmmo);
        bar(m, y, bw, bh, frac, pal::FRACTAL, buf, pl.fractalAmmo == 0);
        y += gap;
    }
    if (pl.nukeAmmo > 0) {   // nuke ammunition, one pip per warhead
        r.text(v2(m, y - 5.0f * s), 9.0f * s, "NUKE  [N]", pal::NUKE, 1.25f);
        for (int i = 0; i < std::min(pl.nukeAmmo, rules::NUKE_MAX); ++i) {
            const v2 pc(m + bh * 0.6f + i * bh * 1.7f, y + bh * 0.5f);
            r.circle(pc, bh * 0.5f, 10, pal::NUKE, 1.8f);
            r.circle(pc, bh * 0.18f, 6, pal::NUKE, 2.4f);
        }
        y += gap;
    }

    drawLevelHud(r);

    // ---- right hand telemetry
    auto rightLine = [&](float yy, const char* txt, Col c, float inten) {
        r.text(v2(W - m - r.textWidth(10.0f * s, txt), yy), 10.0f * s, txt, c, inten);
    };
    float ry = m + 12.0f * s;
    const float rstep = 15.0f * s;
    snprintf(buf, sizeof buf, "FPS %d  %.1f MS", (int)(fps + 0.5f), frameMs);
    rightLine(ry, buf, fps < 50 ? C_WARN : C_HUD, 1.2f); ry += rstep;
    snprintf(buf, sizeof buf, "ROCKS %d / SIM %d", world.liveCount, world.simCount);
    rightLine(ry, buf, C_HUD, 1.0f); ry += rstep;
    snprintf(buf, sizeof buf, "LOOPS %d  VERTS %dK", r.statBodyLoops, (r.statBodyVerts + 500) / 1000);
    rightLine(ry, buf, C_HUD, 1.0f); ry += rstep;
    snprintf(buf, sizeof buf, "PARTICLES %d  SHOTS %d", (int)parts.size(), shotsFired);
    rightLine(ry, buf, C_HUD, 1.0f); ry += rstep;
    snprintf(buf, sizeof buf, "FIELD MEM %d MB", (int)(world.fieldBytes() / (1024 * 1024)));
    rightLine(ry, buf, C_HUD, 1.0f); ry += rstep;
    snprintf(buf, sizeof buf, "PIECES BROKEN %d", rocksSplit);
    rightLine(ry, buf, C_HUD, 1.0f); ry += rstep;
    snprintf(buf, sizeof buf, "X %.0f  Y %.0f", pl.pos.x, pl.pos.y);
    rightLine(ry, buf, C_HUD, 0.85f); ry += rstep;
    snprintf(buf, sizeof buf, "ZOOM %.0f M ACROSS", cam.halfW * 2.0f);
    rightLine(ry, buf, C_HUD, 0.85f);

    // ---- status line under the bars
    const float spd = len(pl.vel);
    snprintf(buf, sizeof buf, "%s   %.0f M/S   CAM %s",
             pl.grounded ? "ON SURFACE" : "FREE FALL", spd, povCamera ? "POV" : "FIXED");
    r.text(v2(m, y + 34.0f * s), 11.0f * s, buf,
           pl.grounded ? Col(0.5f, 1.0f, 0.7f) : C_HUD, 1.3f);

    // ---- the suit: the one number that ends the run, so it is big and central
    {
        const float sw = 460.0f * s, sh = 22.0f * s;
        const float x = W * 0.5f - sw * 0.5f;
        const float top = H - 120.0f * s;                        // just above the help lines
        const float frac = clampf(pl.health / 100.0f, 0.0f, 1.0f);
        const bool  critical = pl.health < 30.0f;
        // Cool blue when healthy, amber as it drops, and a pulsing red when it is close.
        Col c = frac > 0.6f ? Col(0.55f, 0.90f, 1.00f)
              : frac > 0.3f ? Col(1.00f, 0.82f, 0.30f)
                            : C_WARN;
        const float pulse = critical ? 0.7f + 0.3f * std::sin(time * 12.0f) : 1.0f;
        const float I = (1.9f + 1.6f * pl.hurtGlow) * pulse;     // flares when you are hit

        const v2 box[4] = { v2(x, top), v2(x + sw, top), v2(x + sw, top + sh), v2(x, top + sh) };
        r.poly(box, 4, true, c, I);
        const int n = std::max(1, (int)(sw / (6.0f * s)));
        for (int i = 0; i < n; ++i) {
            const float t = (i + 0.5f) / n;
            if (t > frac) break;
            const float px = x + 3.0f * s + t * (sw - 6.0f * s);
            r.line(v2(px, top + 3.0f * s), v2(px, top + sh - 3.0f * s), c, I * 1.05f);
        }
        r.text(v2(x, top - 7.0f * s), 14.0f * s, "SUIT", c, I);
        char hp[16];
        snprintf(hp, sizeof hp, "%d", (int)(pl.health + 0.5f));
        r.text(v2(x + sw - r.textWidth(14.0f * s, hp), top - 7.0f * s), 14.0f * s, hp, c, I);

    // ---- lives: one little helmet per life, a faint outline for each one lost
    if (!sandbox) {
        const int total = rules::LIVES_START;
        const float gap = 34.0f * s, rad = 11.0f * s;
        const float y = top - 42.0f * s;
        const bool last = lives == 1 && state != State::GameOver;
        const float pl8 = last ? 0.75f + 0.25f * std::sin(time * 8.0f) : 1.0f;
        for (int i = 0; i < total; ++i) {
            const v2 c(W * 0.5f + (i - (total - 1) * 0.5f) * gap, y);
            if (i < lives) {
                const Col hc = last ? C_WARN : Col(0.55f, 0.90f, 1.00f);
                r.circle(c, rad, 16, hc, 2.0f * pl8);
                r.line(c + v2(-rad * 0.55f, -rad * 0.05f), c + v2(rad * 0.55f, -rad * 0.05f), hc, 1.6f * pl8);   // the visor
                r.line(c + v2(-rad * 0.40f, rad * 0.30f), c + v2(rad * 0.40f, rad * 0.30f), hc, 1.6f * pl8);
            } else {
                r.circle(c, rad, 16, Col(0.45f, 0.55f, 0.6f), 0.45f);
            }
        }
    }
    }

    // ---- aim: a short dotted line pointing from the spaceman toward the cursor, a
    // bright arrowhead just ahead of him, and a small quiet target that is also the
    // mouse cursor (the system one is hidden). The line only needs to say which way
    // he is facing, so it stops well short of the target. Only while playing.
    {
        const v2 c = mousePx;
        if (state == State::Playing) {
            const v2 pp = worldToScreen(r, pl.pos);
            const v2 d = c - pp;
            const float dist = len(d);
            if (dist > 20.0f * s) {
                const v2 dn = d / dist;
                const v2 side = perp(dn);
                const float dash = 6.0f * s, gapL = 10.0f * s;
                const float from = 60.0f * s;
                const float to = std::min(dist - 16.0f * s, from + 100.0f * s);   // never the whole way
                for (float t = from; t + dash < to; t += dash + gapL) {
                    const float fade = 1.0f - 0.85f * (t - from) / std::max(to - from, 1.0f);
                    r.line(pp + dn * t, pp + dn * (t + dash), C_BULLET, 1.7f * fade);
                }
                // arrowhead just ahead of the spaceman
                const v2 a = pp + dn * (44.0f * s);
                r.line(a - dn * (9.0f * s) + side * (8.0f * s), a + dn * (7.0f * s), C_BULLET, 3.0f);
                r.line(a - dn * (9.0f * s) - side * (8.0f * s), a + dn * (7.0f * s), C_BULLET, 3.0f);
            }
        }
        const float k = 9.0f * s, inner = 4.0f * s;
        const Col cc = (pl.hasHoming && pl.heavyCd > 0) ? Col(0.75f, 0.85f, 0.9f) : Col(1.0f, 0.93f, 0.55f);
        r.line(v2(c.x - k, c.y), v2(c.x - inner, c.y), cc, 1.4f);
        r.line(v2(c.x + inner, c.y), v2(c.x + k, c.y), cc, 1.4f);
        r.line(v2(c.x, c.y - k), v2(c.x, c.y - inner), cc, 1.4f);
        r.line(v2(c.x, c.y + inner), v2(c.x, c.y + k), cc, 1.4f);
        r.circle(c, 1.6f * s, 6, cc, 1.3f);
        r.circle(c, 12.0f * s, 20, cc, 0.4f);                    // a faint ring to find it by
    }

    // ---- help: only the things you own are listed
    if (showHelp) {
        char l1[200], l2[240], l3[200], l4[120];
        snprintf(l1, sizeof l1, "MOVE  A / D     JUMP  SPACE OR W     AIM  MOUSE     ROCKET  RIGHT MOUSE OR SHIFT");
        int n = snprintf(l2, sizeof l2, "FIRE  LEFT MOUSE");
        if (pl.hasShield) n += snprintf(l2 + n, sizeof l2 - n, "     BLAST SHIELD  HOLD LEFT ALT");
        if (pl.hasHoming) n += snprintf(l2 + n, sizeof l2 - n, "     HOMING SHELL  F");
        if (pl.hasSalvo)  n += snprintf(l2 + n, sizeof l2 - n, "     MISSILES  G");
        if (pl.hasFractal) n += snprintf(l2 + n, sizeof l2 - n, "     FRACTAL SHELL  Z");
        if (pl.nukeAmmo)  n += snprintf(l2 + n, sizeof l2 - n, "     NUKE  N");
        if (pl.hasField)  n += snprintf(l2 + n, sizeof l2 - n, "     FORCE FIELD  X");
        snprintf(l3, sizeof l3, "CAMERA  C     ZOOM  WHEEL / Q / E     RESTART  R     PAUSE  P     HELP  H");
        snprintf(l4, sizeof l4, "FULLSCREEN  F11     SCREENSHOT  F12     SOUND ON/OFF  M");
        const char* lines[4] = { l1, l2, l3, l4 };
        const float th = 10.5f * s;
        float ly = H - m - th * 1.4f * 4.0f;
        for (const char* l : lines) {
            r.text(v2(m, ly), th, l, Col(0.42f, 0.70f, 0.78f), 0.95f);
            ly += th * 1.45f;
        }
    }
    if (paused) {
        const char* t = "PAUSED";
        const float th = 44.0f * s;
        r.text(v2(W * 0.5f - r.textWidth(th, t) * 0.5f, H * 0.5f), th, t, C_HUD, 1.8f);
    }
}
