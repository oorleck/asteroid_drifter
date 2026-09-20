// enemies_fx.cpp -- the nuke, plus drawing for everything the
// combat layer adds to the world.
#include "game.h"
#include <algorithm>
#include <cstdio>

// ------------------------------------------------------------------- nukes --
// The grenade is slow on purpose. At 170 u/s it is dragged around hard by the
// nearest rock, so a throw is a curve you have to plan, not a straight line.
void Game::throwNuke(v2 aimDir) {
    if (pl.nukeAmmo <= 0 || pl.nukeCd > 0.0f || playerGone()) return;
    --pl.nukeAmmo;
    pl.nukeCd = rules::NUKE_COOLDOWN;

    Nuke n;
    n.pos = dv2(pl.pos.x + aimDir.x * 16.0, pl.pos.y + aimDir.y * 16.0);
    n.vel = aimDir * rules::NUKE_SPEED + pl.vel;
    n.fuse = rules::NUKE_FUSE;
    n.angVel = rng.sym(3.0f);
    nukes.push_back(n);
    pl.vel -= aimDir * 20.0f;
    spawnSparks(n.pos, n.vel, 6, 80.0f, pal::NUKE, 0.25f);
    sfx(Sfx::NukeThrow, n.pos, 1.0f, 1.0f, 2500.0f);
}

void Game::updateNukes(float dt) {
    for (Nuke& n : nukes) {
        if (n.dead) continue;
        const float fuseBefore = n.fuse;
        n.fuse -= dt;
        {   // a beep on every blink of the light, faster and higher as the fuse runs down
            const float frac = clampf(n.fuse / rules::NUKE_FUSE, 0.0f, 1.0f);
            const float hz = 3.0f + 14.0f * (1.0f - frac);
            if (n.fuse > 0.0f && std::floor(fuseBefore * hz) != std::floor(n.fuse * hz))
                sfx(Sfx::NukeBeep, n.pos, 0.9f, 0.85f + 0.75f * (1.0f - frac), 2400.0f);
        }
        n.ang += n.angVel * dt;
        n.vel += world.gravityAt(n.pos, 2600.0) * (dt * rules::NUKE_GRAV);

        const int steps = std::max(1, (int)(len(n.vel) * dt / 4.0f) + 1);
        const float sdt = dt / steps;
        for (int s = 0; s < steps; ++s) {
            n.pos.x += (double)n.vel.x * sdt;
            n.pos.y += (double)n.vel.y * sdt;

            v2 nrm;  float depth = 0;
            const int hit = world.probe(n.pos, rules::NUKE_R_BODY, &nrm, &depth);
            if (hit < 0) continue;
            const Body& b = world.bodies[hit];
            n.pos.x += nrm.x * depth;
            n.pos.y += nrm.y * depth;

            const v2 surf = b.velAt(tov2(n.pos - b.pos));
            v2 rel = n.vel - surf;
            const float vn = dot(rel, nrm);
            if (vn < 0.0f) {
                // Bounce only if it hit hard; otherwise it settles and rolls.
                rel -= nrm * (vn * (std::fabs(vn) > 40.0f ? 1.35f : 1.0f));
                const v2 tang = rel - nrm * dot(rel, nrm);
                rel -= tang * std::min(1.0f, 3.0f * sdt);
                n.angVel *= 0.8f;
            }
            n.vel = rel + surf;
        }
        if (n.fuse <= 0.0f) detonate(n);
    }
    nukes.erase(std::remove_if(nukes.begin(), nukes.end(),
                               [](const Nuke& n) { return n.dead; }), nukes.end());
}

void Game::detonate(Nuke& n) {
    n.dead = true;
    const float R = rules::NUKE_RADIUS;
    explode(n.pos, R, 99999.0f, rules::NUKE_PLAYER_DMG, R * 0.95f, rules::NUKE_IMPULSE, true);

    // A great deal of light and noise-equivalent.
    ring(n.pos, R * 0.45f, 0.55f, Col(1.0f, 1.0f, 1.0f), 2.6f);
    ring(n.pos, R * 1.15f, 0.95f, Col(1.0f, 0.95f, 0.7f), 2.4f);
    ring(n.pos, R * 0.85f, 0.80f, Col(1.0f, 0.60f, 0.2f), 1.8f, 0.06f);
    ring(n.pos, R * 1.60f, 1.50f, Col(1.0f, 0.45f, 0.15f), 1.2f, 0.14f);
    ring(n.pos, R * 2.20f, 2.00f, Col(0.9f, 0.3f, 0.1f), 0.8f, 0.30f);
    spawnSparks(n.pos, v2(0, 0), 260, 1000.0f, Col(1.0f, 0.75f, 0.35f), 1.7f);
    spawnSparks(n.pos, v2(0, 0), 140, 520.0f, Col(1.0f, 1.0f, 0.95f), 1.1f);
    flash = 1.0f;
    shake = 1.9f;
    sfx(Sfx::NukeBoom, n.pos, 1.0f, 1.0f, 6500.0f);
}

// ------------------------------------------------------------------ drawing --
void Game::drawEnemies(Renderer& r) {
    // The camera's own right and up, in world space, so health bars stay level
    // on screen however the POV camera has rolled.
    const v2 camRight = fromAngle(-cam.angle);
    const v2 camUp    = perp(camRight);

    for (const Enemy& e : enemies) {
        if (!e.alive) continue;
        const v2 p = camRel(e.pos);
        if (!inView(p, 60.0f)) continue;

        if (e.kind == Enemy::Hardpoint) { drawWeaponMount(r, e); continue; }
        const Col base = e.kind == Enemy::Turret ? pal::ENEMY : pal::DRONE;
        const Col c = mix(base, Col(1, 1, 1), e.flash);
        const float I = 1.9f + 1.8f * e.flash;
        const float R = e.radius;

        if (e.kind == Enemy::Turret) {
            const v2 n = e.normal, t = perp(n);
            v2 dome[9];
            for (int i = 0; i < 9; ++i) {
                const float a = PIF * i / 8.0f;
                dome[i] = p + t * (std::cos(a) * R * 1.15f) + n * (std::sin(a) * R * 0.85f);
            }
            r.poly(dome, 9, true, c, I);

            const v2 ad = fromAngle(e.aim);
            const v2 root = p + n * (R * 0.45f);
            r.line(root + perp(ad) * 1.7f, root + ad * (R + 13.0f) + perp(ad) * 1.7f, c, I * 1.15f);
            r.line(root - perp(ad) * 1.7f, root + ad * (R + 13.0f) - perp(ad) * 1.7f, c, I * 1.15f);
            const float eye = e.aggro ? 0.75f + 0.25f * std::sin(time * 11.0f + e.phase) : 0.45f;
            r.circle(root, 4.2f, 9, c, I * 1.3f * eye);
            if (e.hasMissile) {                        // a pair of launch tubes
                for (int k = -1; k <= 1; k += 2) {
                    const v2 b0 = p + t * (k * R * 0.62f) + n * 1.5f;
                    r.line(b0, b0 + n * (R * 0.75f), pal::MISSILE, 1.9f);
                }
            }
        } else {
            const v2 ad = fromAngle(e.aim);
            v2 hull[6];
            for (int i = 0; i < 6; ++i) {
                const float rr = (i == 0) ? R * 1.35f : ((i == 3) ? R * 0.75f : R);
                hull[i] = p + fromAngle(e.aim + i * TAUF / 6.0f) * rr;
            }
            r.poly(hull, 6, true, c, I);
            v2 tri[3];
            for (int i = 0; i < 3; ++i)
                tri[i] = p + fromAngle(e.spin * 3.0f + i * TAUF / 3.0f) * (R * 0.5f);
            r.poly(tri, 3, true, c, I * 0.8f);
            r.circle(p + ad * (R * 0.75f), 2.6f, 7, c, I * 1.4f);

            // Thruster flicker opposite to its motion.
            const v2 vd = norm(e.vel);
            const float thrust = clampf(len(e.vel) / 250.0f, 0.2f, 1.2f);
            r.line(p - vd * R, p - vd * (R + 6.0f + rng.f() * 12.0f * thrust),
                   Col(1.0f, 0.7f, 0.4f), 2.0f);
        }

        if (e.hp < e.maxHp) {
            const v2 bc = p + camUp * (R + 17.0f);
            const float f = clampf(e.hp / e.maxHp, 0.0f, 1.0f);
            r.line(bc - camRight * 14.0f, bc + camRight * 14.0f, Col(0.4f, 0.1f, 0.1f), 1.6f);
            r.line(bc - camRight * 14.0f, bc - camRight * 14.0f + camRight * (28.0f * f), c, 2.4f);
        }
    }
}

void Game::drawProjectiles(Renderer& r) {
    for (const EnemyBullet& b : ebullets) {
        const v2 p = camRel(b.pos);
        if (!inView(p, 40.0f)) continue;
        const v2 d = norm(b.vel);
        r.line(p, p - d * (20.0f * b.size), pal::ENEMY, 2.8f * b.size);
        r.point(p, 3.6f * b.size, Col(1.0f, 0.7f, 0.6f), 2.2f);
    }
    for (const Missile& m : missiles) {
        if (m.dead) continue;
        const v2 p = camRel(m.pos);
        if (!inView(p, 60.0f)) continue;
        const v2 d = norm(m.vel), n = perp(d);
        const v2 body[4] = { p + d * 10.0f, p + n * 3.5f - d * 5.0f, p - d * 7.0f, p - n * 3.5f - d * 5.0f };
        r.poly(body, 4, true, pal::MISSILE, 2.6f);
        r.line(p - d * 7.0f, p - d * (15.0f + rng.f() * 11.0f), Col(1.0f, 0.85f, 0.5f), 2.8f);
        // A pulsing ring so an incoming missile cannot be missed.
        r.circle(p, 16.0f + 2.5f * std::sin(time * 13.0f + m.age * 5.0f), 10, pal::WARN, 1.1f);
    }
}

void Game::drawNukes(Renderer& r) {
    for (const Nuke& n : nukes) {
        if (n.dead) continue;
        const v2 p = camRel(n.pos);
        if (!inView(p, rules::NUKE_RADIUS * 1.3f)) continue;

        const float frac = clampf(n.fuse / rules::NUKE_FUSE, 0.0f, 1.0f);
        // Yellow while there is time, red as it runs out; it blinks faster and
        // faster toward zero.
        const float blinkHz = 3.0f + 14.0f * (1.0f - frac);
        const bool  on = std::fmod(n.fuse * blinkHz, 1.0f) < 0.6f;
        const Col c = mix(Col(1.0f, 0.35f, 0.2f), pal::NUKE, frac);
        const float I = on ? 3.2f : 1.4f;

        // The bomb: a circle, a core and three fins that tumble with it.
        r.circle(p, rules::NUKE_R_BODY, 14, c, I);
        r.circle(p, 2.6f, 6, c, I * 1.3f);
        for (int k = 0; k < 3; ++k) {
            const v2 d = fromAngle(n.ang + k * TAUF / 3.0f);
            r.line(p + d * rules::NUKE_R_BODY, p + d * (rules::NUKE_R_BODY + 5.0f), c, I * 0.9f);
        }
        // Fuse gauge: an arc that shrinks as the time runs down.
        r.arc(p, 15.0f, PIF * 0.5f, PIF * 0.5f + TAUF * frac, 24, c, I * 0.9f);

        // In the last two seconds, show exactly how far the blast will reach.
        if (n.fuse < 2.0f) {
            const float k = 1.0f - n.fuse / 2.0f;
            const int dashes = 48;
            for (int i = 0; i < dashes; i += 2) {
                const float a0 = i * TAUF / dashes + time * 0.3f;
                r.arc(p, rules::NUKE_RADIUS, a0, a0 + TAUF / dashes, 3, Col(1.0f, 0.4f, 0.2f), 0.5f + 1.6f * k);
            }
        }
    }
}

// The countdown is text, so it has to stay upright on screen: it is drawn in
// the HUD's coordinate system at the grenade's projected position.
void Game::drawNukeLabels(Renderer& r) {
    const float s = clampf((float)r.fbh / 900.0f, 0.7f, 2.0f);
    for (const Nuke& n : nukes) {
        if (n.dead) continue;
        const v2 sp = worldToScreen(r, n.pos);
        if (sp.x < -40 || sp.y < -40 || sp.x > r.fbw + 40 || sp.y > r.fbh + 40) continue;

        const float frac = clampf(n.fuse / rules::NUKE_FUSE, 0.0f, 1.0f);
        const bool  hot = n.fuse < 1.5f;
        const bool  on  = !hot || std::fmod(n.fuse * 9.0f, 1.0f) < 0.6f;
        char buf[16];
        snprintf(buf, sizeof buf, "%.1f", std::max(0.0f, n.fuse));
        const float th = (13.0f + (hot ? 6.0f * (1.0f - n.fuse / 1.5f) : 0.0f)) * s;
        const Col c = mix(Col(1.0f, 0.3f, 0.2f), pal::NUKE, frac);
        const float w = r.textWidth(th, buf);
        if (on) r.text(v2(sp.x - w * 0.5f, sp.y - 26.0f * s), th, buf, c, 2.6f);
    }
}

void Game::drawWaves(Renderer& r) {
    for (const Shockwave& w : waves) {
        const float age = w.age - w.delay;
        if (age < 0.0f) continue;
        const float t = clampf(age / w.life, 0.0f, 1.0f);
        const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // ease out
        const float R = w.radius * e;
        const v2 p = camRel(w.pos);
        if (!inView(p, R)) continue;
        const float I = w.weight * (1.0f - t) * 3.4f;
        const int segs = R > 150.0f ? 112 : 56;
        r.circle(p, R, segs, w.col, I);
        r.circle(p, R * 0.965f, segs, w.col, I * 0.55f);           // a thicker, softer edge
        r.circle(p, R * 0.93f, segs, w.col, I * 0.25f);
    }
}
