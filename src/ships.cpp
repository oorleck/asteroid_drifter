// ships.cpp -- the big generated warships: building one from a seed, flying it,
// arming it, hurting it, blowing it up, and drawing it.
//
// A ship is a hull (an outline that stops bullets and shoves the player) plus a
// set of weapons. Each weapon is an ordinary Enemy of kind Hardpoint that the ship
// carries around, so bullets, blasts and homing weapons all work on it with no
// special cases. Destroying weapons chips the hull too; the hull dying is what
// destroys the ship.
#include "game.h"
#include <algorithm>
#include <cstdio>

namespace {

float dist2d(dv2 a, dv2 b) {
    const double dx = a.x - b.x, dy = a.y - b.y;
    return (float)std::sqrt(dx * dx + dy * dy);
}

Col hsv(float h, float s, float v) {
    h -= std::floor(h);
    const float k = h * 6.0f;
    const int   i = (int)k;
    const float f = k - i;
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
        case 0:  return Col(v, t, p);
        case 1:  return Col(q, v, p);
        case 2:  return Col(p, v, t);
        case 3:  return Col(p, q, v);
        case 4:  return Col(t, p, v);
        default: return Col(v, p, q);
    }
}

// The outline's half-width at x, read off the upper edge (nose to tail).
float halfWidthAt(const std::vector<v2>& upper, float x) {
    for (size_t i = 1; i < upper.size(); ++i) {
        const v2 a = upper[i - 1], b = upper[i];
        if (x <= a.x && x >= b.x) {
            const float span = a.x - b.x;
            const float t = span > 1e-3f ? (a.x - x) / span : 0.0f;
            return lerpf(a.y, b.y, t);
        }
    }
    return upper.empty() ? 0.0f : upper.back().y;
}

const char* const NAME_A[] = { "IRON", "VOID", "CRIMSON", "PALE", "GRIM", "ASHEN", "HOLLOW", "STORM",
                               "SILENT", "IVORY", "COLD", "WRAITH", "OBSIDIAN", "SCARLET", "BROKEN", "LONG" };
const char* const NAME_B[] = { "KESTREL", "MAW", "LANCE", "HARBINGER", "TALON", "WARDEN", "REAPER", "VESPER",
                               "ANVIL", "COMET", "LEVIATHAN", "SHRIKE", "BASILISK", "CHORUS", "EMBER", "OWL" };

}  // namespace

// ------------------------------------------------------------- generation --
// Everything about a ship comes from its seed: the silhouette (one of four
// families, each with its own random wings and steps), the colour, the name, and
// which of the four weapons sit where. Weapons come in mirrored pairs, so a ship
// looks built rather than scattered, with one on the nose if the count is odd.
void generateShip(Ship& s, uint64_t seed, int level, const Difficulty& D) {
    Rng r(seed);
    s = Ship();

    // Every third level brings a ship, each bigger and tougher than the last: its tier is 1, 2, 3...
    // The first is about six turrets long and broad in proportion; each tier is 1.4 times the length.
    const int tier = std::max(1, level / rules::SHIP_EVERY);
    const float L = std::min(rules::SHIP_LENGTH_CAP, rules::SHIP_BASE_LENGTH * std::pow(rules::SHIP_LENGTH_GROWTH, (float)(tier - 1)))
                  * r.range(1.0f - rules::SHIP_LENGTH_JITTER, 1.0f + rules::SHIP_LENGTH_JITTER);
    const float W = L * (r.range(0.14f, 0.30f) + 0.12f / (float)tier);   // the small ones are stubbier, so they are big in every direction
    const int style = r.i(0, 3);          // 0 dart, 1 cruiser, 2 carrier, 3 wedge
    s.style = style;
    s.tier = tier;
    s.mountR = clampf(L * 0.055f, 11.0f, rules::SHIP_WEAPON_RADIUS);
    s.riflePass = rules::SHIP_RIFLE_FACTOR / (1.0f + rules::SHIP_ARMOUR_GROWTH * (float)(tier - 1));
    s.nukeShare = rules::SHIP_NUKE_SHARE / (1.0f + 2.0f * rules::SHIP_ARMOUR_GROWTH * (float)(tier - 1));
    s.col = hsv(r.f(), r.range(0.55f, 0.90f), 1.0f);
    snprintf(s.name, sizeof s.name, "%s %s", NAME_A[r.i(0, 15)], NAME_B[r.i(0, 15)]);

    // ---- the upper edge, nose to tail, as (x, half-width)
    const int N = r.i(6, 9);
    std::vector<v2> upper;
    const float noseX = L * 0.5f, tailX = -L * 0.5f;
    const float forkHW = W * r.range(0.16f, 0.30f);
    const float notch  = L * r.range(0.08f, 0.15f);
    if (style == 2) upper.push_back(v2(noseX, forkHW));                // the prongs of a forked bow

    float prevStep = 0.75f;
    for (int k = 1; k <= N; ++k) {
        const float t = (float)k / N;
        float w;
        switch (style) {
            case 0:  w = W * (0.12f + 0.88f * std::pow(t, 0.7f)) * (1.0f - 0.22f * t * t * t * t); break;
            case 1: {
                static const float steps[3] = { 0.55f, 0.78f, 1.0f };
                const float ramp = clampf(t / 0.22f, 0.25f, 1.0f);
                prevStep = steps[r.i(0, 2)];
                w = W * prevStep * ramp;
            } break;
            case 2:  w = W * (0.55f + 0.45f * std::sin(PIF * std::min(1.0f, t * 0.9f + 0.1f))); break;
            default: w = W * (0.10f + 0.90f * t); break;
        }
        w *= r.range(0.90f, 1.10f);
        const float x = lerpf(noseX, tailX, t);
        if (style == 1 && k > 1)                                       // blocky steps: a vertical edge, then along
            upper.push_back(v2(upper.back().x, w));
        upper.push_back(v2(x, w));
    }

    // ---- wings: a spike out to the side at one station, swept back
    if (style == 0 || r.f() < 0.35f) {
        const int k = r.i(2, std::max(2, (int)upper.size() - 3));
        if (k + 1 < (int)upper.size() && upper[k].x - upper[k + 1].x > L * 0.04f) {
            const float sweep = (upper[k].x - upper[k + 1].x) * r.range(0.35f, 0.75f);
            const v2 base = upper[k];
            upper[k] = v2(base.x, base.y + W * r.range(0.35f, 0.85f));
            upper.insert(upper.begin() + k + 1, v2(base.x - sweep, base.y * 0.85f));
        }
    }

    // ---- the outline, counter-clockwise: nose, upper edge, tail, lower edge back
    if (style == 2) s.hull.push_back(v2(noseX - notch, 0.0f));         // the notch between the prongs
    else            s.hull.push_back(v2(noseX, 0.0f));
    for (const v2& p : upper) s.hull.push_back(p);
    if (style == 3) s.hull.push_back(v2(tailX + L * 0.07f, 0.0f));     // an engine notch in the stern
    for (int i = (int)upper.size() - 1; i >= 0; --i) s.hull.push_back(v2(upper[i].x, -upper[i].y));

    s.radius = 0.0f;
    for (const v2& p : s.hull) s.radius = std::max(s.radius, len(p));

    // ---- a second, smaller outline and a few panel lines, for depth
    for (const v2& p : s.hull) s.inner.push_back(v2(p.x * 0.90f, p.y * 0.68f));
    for (int k = 1; k < (int)upper.size() - 1; ++k) {
        if (r.f() > 0.55f) continue;
        const v2 a = upper[k];
        s.lines.push_back(v2(a.x * 0.90f, a.y * 0.68f));
        s.lines.push_back(v2(a.x * 0.90f, -a.y * 0.68f));
    }
    s.lines.push_back(v2(noseX * 0.55f, 0.0f));
    s.lines.push_back(v2(tailX * 0.85f, 0.0f));                        // the spine

    // The bridge: a small diamond forward of centre.
    {
        const float bx = L * r.range(0.06f, 0.22f), bl = L * 0.045f, bw = std::max(6.0f, W * 0.12f);
        s.bridge[0] = v2(bx + bl, 0.0f);   s.bridge[1] = v2(bx, bw);
        s.bridge[2] = v2(bx - bl, 0.0f);   s.bridge[3] = v2(bx, -bw);
    }

    // The engines, spread across the stern.
    {
        const float tailHW = halfWidthAt(upper, tailX);
        const int n = r.i(2, 4);
        for (int i = 0; i < n; ++i) {
            const float f = n == 1 ? 0.0f : (float)i / (n - 1) * 2.0f - 1.0f;
            s.engines.push_back(v2(tailX + (style == 3 ? L * 0.02f : 0.0f), f * tailHW * 0.62f));
        }
    }

    // ---- the weapons: mirrored pairs on the edge of the hull, and a bow gun if odd
    const int count = clampi(3 + tier + r.i(0, 2), 3, 9);
    // Each ship has its own doctrine: how much it favours each kind of weapon, so one is
    // all guns and another mostly missile pods and cannon.
    const float doctrine[4] = { r.range(0.3f, 1.9f), r.range(0.3f, 1.9f), r.range(0.3f, 1.9f), r.range(0.3f, 1.9f) };
    auto pickType = [&]() {
        const float wGun = 4.0f * doctrine[0], wFlak = 2.5f * doctrine[1];
        const float wMissile = std::min(4.0f, 1.2f + 0.25f * level) * doctrine[2];
        const float wCannon = (level >= 3 ? 1.6f : 0.7f) * doctrine[3];
        float x = r.f() * (wGun + wFlak + wMissile + wCannon);
        if ((x -= wGun) < 0.0f) return ShipWeapon::Gun;
        if ((x -= wFlak) < 0.0f) return ShipWeapon::Flak;
        if ((x -= wMissile) < 0.0f) return ShipWeapon::Missile;
        return ShipWeapon::Cannon;
    };
    // And every weapon has its own character: quick and light, or slow and heavy, or fast rounds.
    auto character = [&](ShipWeapon& w) {
        w.rate  = r.range(0.65f, 1.5f);                  // cooldown: lower is quicker
        w.power = r.range(0.75f, 1.35f) / std::sqrt(w.rate);   // the quick ones hit lighter, the slow ones harder
        w.speed = r.range(0.85f, 1.2f);
        w.shots = w.type == ShipWeapon::Gun ? r.i(-1, 2) : 0;
    };
    const bool laser = level >= rules::SHIP_FIRST_LEVEL && r.f() < rules::LASER_SHIP_CHANCE;
    std::vector<float> taken;
    for (int i = 0; i < count / 2; ++i) {
        float x = 0.0f;
        for (int tries = 0; tries < 40; ++tries) {
            x = lerpf(noseX * 0.72f, tailX * 0.82f, r.f());
            bool clash = false;
            for (float t : taken) if (std::fabs(t - x) < std::max(L * 0.075f, s.mountR * 2.3f)) clash = true;
            if (!clash) break;
        }
        taken.push_back(x);
        const float y = halfWidthAt(upper, x) * 0.90f;                 // right on the edge: bullets reach it
        ShipWeapon proto;
        proto.type = pickType();
        character(proto);                                              // a mirrored pair shares its character
        for (int side = -1; side <= 1; side += 2) {
            ShipWeapon w = proto;
            w.local = v2(x, y * side);
            s.weapons.push_back(w);
        }
    }
    if (count & 1) {
        ShipWeapon w;
        w.type = laser ? ShipWeapon::Laser : pickType();               // the odd one out on the bow is the ray, if there is one
        character(w);
        w.local = v2(noseX * 0.86f, 0.0f);
        s.weapons.push_back(w);
    } else if (laser) {
        ShipWeapon w;
        w.type = ShipWeapon::Laser;
        w.local = v2(noseX * 0.86f, 0.0f);
        s.weapons.push_back(w);
    }

    s.maxHp = s.hp = (rules::SHIP_HULL_BASE + rules::SHIP_HULL_PER_LEVEL * (float)level) * std::pow(rules::SHIP_HULL_GROWTH, (float)(tier - 1));
    s.hullShare = rules::SHIP_WEAPONS_SHARE / std::max<size_t>(1, s.weapons.size());
    (void)D;
}

// ------------------------------------------------------------------ queries --
Ship* Game::findShip(int id) {
    for (Ship& s : ships) if (s.alive && s.id == id) return &s;
    return nullptr;
}

// Signed distance from p to the hull, negative inside. `outward` gets the
// direction that leads away from the hull at the nearest point.
float Game::hullDistance(const Ship& s, dv2 p, v2* outward) const {
    const v2 rel = tov2(p - s.pos);
    const float c = std::cos(-s.angle), sn = std::sin(-s.angle);
    const v2 q = rot(rel, c, sn);                                      // into the hull's frame
    const float rr = len(q);
    if (rr > s.radius + 60.0f) {
        if (outward) *outward = rr > 1e-3f ? rel / rr : v2(1, 0);
        return rr - s.radius;
    }
    float best = 1e30f;
    v2 nearest = q;
    bool inside = false;
    const size_t n = s.hull.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const v2 a = s.hull[j], b = s.hull[i];
        // crossing count for the inside test
        if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        const v2 ab = b - a;
        const float l2 = len2(ab);
        const float t = l2 > 1e-6f ? clampf(dot(q - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        const v2 c2 = a + ab * t;
        const float d2 = len2(q - c2);
        if (d2 < best) { best = d2; nearest = c2; }
    }
    const float d = std::sqrt(best);
    if (outward) {
        v2 dir = inside ? nearest - q : q - nearest;
        const float l = len(dir);
        dir = l > 1e-4f ? dir / l : (rr > 1e-3f ? q / rr : v2(1, 0));
        *outward = rot(dir, s.angle);                                  // and back out to the world
    }
    return inside ? -d : d;
}

Ship* Game::shipAt(dv2 p) {
    for (Ship& s : ships) {
        if (!s.alive) continue;
        const v2 rel = tov2(p - s.pos);
        if (len2(rel) > s.radius * s.radius) continue;
        if (hullDistance(s, p) <= 0.0f) return &s;
    }
    return nullptr;
}

// --------------------------------------------------------------- spawn / die --
void Game::spawnShip() {
    Ship s = level.ship;
    s.id = nextId++;
    s.alive = true;
    s.pos = s.anchor;
    s.angle = s.baseAngle;
    const Difficulty& D = level.diff;
    for (ShipWeapon& w : s.weapons) {
        Enemy e;
        e.kind = Enemy::Hardpoint;
        e.id = nextId++;
        e.alive = true;
        e.pos = dv2(s.pos.x + rot(w.local, s.angle).x, s.pos.y + rot(w.local, s.angle).y);
        e.maxHp = e.hp = rules::SHIP_WEAPON_HP * D.hpScale * std::pow(rules::SHIP_WEAPON_GROWTH, (float)(s.tier - 1));
        e.radius = s.mountR;
        e.hasGun     = w.type == ShipWeapon::Gun;
        e.hasMissile = w.type == ShipWeapon::Missile;
        e.gunCd = rng.range(1.0f, 2.6f);
        e.missileCd = rng.range(3.0f, 6.0f);
        e.phase = rng.angle();
        e.aim = s.angle;
        e.shipId = s.id;
        e.weapon = (int)w.type;
        e.cdScale = rules::SHIP_FIRE_SLOW * w.rate;
        e.power = w.power;  e.speedMul = w.speed;  e.shotsBonus = w.shots;
        if (w.type == ShipWeapon::Laser) {                 // the ray: a bigger, tougher mount, that waits a while before its first shot
            e.radius = s.mountR * 1.35f;
            e.maxHp = e.hp = e.maxHp * 1.5f;
            e.gunCd = rng.range(2.5f, 5.0f);
        }
        e.tint = s.col;
        w.enemyId = e.id;
        enemies.push_back(e);
    }
    ships.push_back(s);
    level.shipSpawned = true;
    sfxUI(Sfx::ShipAlert, 0.9f);
}

void Game::damageShip(Ship& s, float dmg, dv2 at) {
    if (!s.alive) return;
    s.hp -= dmg;
    s.flash = 1.0f;
    spawnSparks(at, s.vel, 3, 150.0f, s.col, 0.3f);
    sfx(Sfx::ShipHit, at, 0.8f, sfxRng.range(0.9f, 1.1f), 2500.0f);
    if (s.hp <= 0.0f) killShip(s);
}

void Game::killShip(Ship& s) {
    if (!s.alive) return;
    s.alive = false;
    s.hp = 0.0f;
    quietBooms = true;                                // the ship has one big sound of its own, below
    for (ShipWeapon& w : s.weapons)
        if (Enemy* e = findEnemy(w.enemyId)) {
            if (e->alive) boom(e->pos, 20.0f, s.col);
            e->alive = false;
        }

    // A run of explosions across the hull, then the whole thing going up.
    const float c = std::cos(s.angle), sn = std::sin(s.angle);
    auto world_of = [&](v2 local) { const v2 q = rot(local, c, sn); return dv2(s.pos.x + q.x, s.pos.y + q.y); };
    for (int i = 0; i < 16; ++i) {
        const v2 lp = s.hull[(size_t)rng.i(0, (int)s.hull.size() - 1)] * rng.range(0.1f, 0.9f);
        ring(world_of(lp), rng.range(45.0f, 110.0f), 0.6f, i & 1 ? Col(1.0f, 0.9f, 0.7f) : s.col, 1.3f, i * 0.07f);
        if (i < 6) boom(world_of(lp), rng.range(30.0f, 60.0f), s.col);
    }
    ring(s.pos, s.radius * 0.8f, 0.9f, Col(1.0f, 1.0f, 1.0f), 2.2f, 0.25f);
    ring(s.pos, s.radius * 1.5f, 1.4f, s.col, 1.8f, 0.30f);
    spawnSparks(s.pos, s.vel, 220, 700.0f, s.col, 1.6f);
    spawnSparks(s.pos, s.vel, 100, 420.0f, Col(1.0f, 0.95f, 0.85f), 1.1f);

    // The hull breaks along its own edges and tumbles apart.
    const size_t n = s.hull.size();
    for (size_t i = 0; i < n && parts.size() < 60000; ++i) {
        const v2 a = s.hull[i], b = s.hull[(i + 1) % n];
        const v2 mid = (a + b) * 0.5f;
        const dv2 wp = world_of(mid);
        Particle q;
        q.pos = wp;
        const v2 out = norm(rot(mid, c, sn));
        q.vel = s.vel + out * rng.range(70.0f, 260.0f) + rng.disc() * 40.0f;
        q.maxLife = q.life = rng.range(3.0f, 6.5f);
        q.size = clampf(len(b - a) * 0.4f, 7.0f, 46.0f);
        q.ang = rng.angle();
        q.angVel = rng.sym(1.6f);
        q.col = s.col;
        q.kind = 1;
        parts.push_back(q);
    }

    quietBooms = false;
    sfx(Sfx::ShipDeath, s.pos, 1.0f, 1.0f, 7000.0f);
    shake = std::max(shake, 1.6f);
    flash = std::max(flash, 0.35f);
    ++kills;
    const int pay = rules::CREDIT_SHIP_BASE + rules::CREDIT_SHIP_PER_LEVEL * level.number;
    earn(pay);
    char buf[64];
    snprintf(buf, sizeof buf, "%s DESTROYED   +%d CREDITS", s.name, pay);
    say(buf);
}

// -------------------------------------------------------------------- update --
void Game::fireShipBullet(const Enemy& e, dv2 muzzle, float ang, float speedMul, float dmgMul,
                          float size, float life) {
    if ((int)ebullets.size() >= rules::MAX_EBULLETS) return;
    const Difficulty& D = level.diff;
    EnemyBullet b;
    b.pos = muzzle;
    b.life = life;
    b.damage = D.bulletDamage * dmgMul * e.power;
    b.size = size;
    b.vel = fromAngle(ang) * (D.bulletSpeed * speedMul * e.speedMul) + e.vel;
    ebullets.push_back(b);
    ++enemyShots;
    spawnSparks(muzzle, e.vel + fromAngle(ang) * 60.0f, 2, 80.0f, e.tint, 0.14f);
}

// ---------------------------------------------------------------- the ray --
// A thick beam that cuts through everything in its line. It starts to shoot when you
// come within range: two seconds of charging (a thin line shows where it points, and it
// keeps following you until the last moment), then the ray, then seven seconds before
// it can start again. e.burst is the state: 0 recharging, 1 charging, 2 firing.
namespace {
// How far p is from the segment a..b, and how far along it p falls.
float distToRay(dv2 a, v2 dir, float length, dv2 p) {
    const v2 rel((float)(p.x - a.x), (float)(p.y - a.y));
    const float along = clampf(dot(rel, dir), 0.0f, length);
    return len(rel - dir * along);
}
}

void Game::updateLaser(Ship& s, Enemy& e, float dt) {
    const Difficulty& D = level.diff;
    const float dist = dist2d(pl.pos, e.pos);
    auto follow = [&](float rate) {
        const v2 rel = tov2(pl.pos - e.pos);
        const float want = std::atan2(rel.y, rel.x);
        const float turn = D.turretTurn * rate * dt;
        e.aim = wrapAngle(e.aim + clampf(wrapAngle(want - e.aim), -turn, turn));
    };

    if (e.burst == 0) {                                   // recharging, or waiting for something to shoot at
        e.gunCd -= dt;
        if (s.aggro) follow(0.5f);
        if (s.aggro && e.gunCd <= 0.0f && dist < rules::LASER_TRIGGER && state == State::Playing) {
            e.burst = 1;
            e.burstCd = rules::LASER_CHARGE;
            sfx(Sfx::LaserCharge, e.pos, 1.0f, 1.0f, 3200.0f);
        }
        return;
    }
    if (e.burst == 1) {                                   // charging
        e.burstCd -= dt;
        if (e.burstCd > rules::LASER_LOCK) follow(0.6f);  // and then it holds still: that is the moment to move
        if (e.burstCd <= 0.0f) {
            e.burst = 2;
            e.burstCd = rules::LASER_FIRE_TIME;
            e.beamHit = 0.0f;
            sfx(Sfx::LaserFire, e.pos, 1.0f, 1.0f, 4200.0f);
            shake = std::max(shake, 0.5f);
        }
        return;
    }

    // ---- firing: the ray is on
    e.burstCd -= dt;
    const v2 dir = fromAngle(e.aim);
    const dv2 from(e.pos.x + dir.x * (e.radius + 10.0), e.pos.y + dir.y * (e.radius + 10.0));
    const float halfW = rules::LASER_WIDTH * 0.5f;
    const float L = rules::LASER_RANGE;
    shake = std::max(shake, 0.18f);

    // It cuts through rock: a slot the width of the ray, all the way along.
    const float step = halfW * 0.6f;
    for (float d = 0.0f; d < L; d += step) {
        const dv2 p(from.x + dir.x * d, from.y + dir.y * d);
        const int hit = world.solidAt(p);
        if (hit < 0) continue;
        world.damage(hit, p, halfW * 0.9f, 0.15f, rng.u32());
        if (rng.f() < 0.25f) spawnSparks(p, dir * 120.0f, 3, 240.0f, e.tint, 0.5f);
    }
    // ...and through anything that flies: missiles, shots, salvos and shells are cut down in it.
    for (Missile& m : missiles)
        if (!m.dead && distToRay(from, dir, L, m.pos) < halfW + 8.0f) destroyMissile(m);
    for (EnemyBullet& b : ebullets)
        if (distToRay(from, dir, L, b.pos) < halfW + 4.0f) b.life = 0.0f;
    for (Bullet& b : bullets)
        if (distToRay(from, dir, L, b.pos) < halfW + 4.0f) b.life = 0.0f;
    for (PMissile& m : pmissiles)
        if (!m.dead && distToRay(from, dir, L, m.pos) < halfW + 6.0f) m.life = 0.0f;

    // And through you: no shield and no force field stops it, so the only defence is to be elsewhere.
    e.beamHit -= dt;
    if (e.beamHit <= 0.0f && state == State::Playing && distToRay(from, dir, L, pl.pos) < halfW + rules::PLAYER_HIT_R) {
        e.beamHit = 0.2f;
        hurtPlayer(rules::LASER_DPS * 0.2f, v2(0, 0), -1);
    }

    if (e.burstCd <= 0.0f) {
        e.burst = 0;
        e.gunCd = rules::LASER_RECHARGE;
    }
}

void Game::updateShipWeapon(Ship& s, Enemy& e, float dt) {
    const Difficulty& D = level.diff;
    if (e.weapon == ShipWeapon::Laser) { updateLaser(s, e, dt); return; }
    if (!s.aggro) return;
    const float dist = dist2d(pl.pos, e.pos);

    // Lead the shot, the way a turret does.
    float speed = D.bulletSpeed * e.speedMul;
    if (e.weapon == ShipWeapon::Flak)   speed *= rules::FLAK_SPEED;
    if (e.weapon == ShipWeapon::Cannon) speed *= rules::CANNON_SPEED;
    const v2 rel = tov2(pl.pos - e.pos);
    const v2 aimPt = rel + (pl.vel - e.vel) * (dist / std::max(speed, 100.0f));
    const float want = std::atan2(aimPt.y, aimPt.x);
    const float turn = D.turretTurn * 0.9f * dt;
    e.aim = wrapAngle(e.aim + clampf(wrapAngle(want - e.aim), -turn, turn));
    const bool aimed = std::fabs(wrapAngle(want - e.aim)) < 0.05f + D.aimError;

    const v2 ad = fromAngle(e.aim);
    const dv2 muzzle(e.pos.x + ad.x * (e.radius + 8.0f), e.pos.y + ad.y * (e.radius + 8.0f));

    switch (e.weapon) {
    case ShipWeapon::Gun:
    case ShipWeapon::Missile:
        enemyShoot(e, muzzle, dist, dt, aimed);
        break;
    case ShipWeapon::Flak:
        e.gunCd -= dt;
        if (e.gunCd <= 0.0f && aimed && dist < D.gunRange * 0.95f && clearLine(muzzle, pl.pos)) {
            sfx(Sfx::FlakFire, muzzle, 0.9f, sfxRng.range(0.95f, 1.08f), 2200.0f);
            for (int i = 0; i < rules::FLAK_PELLETS; ++i) {
                const float off = (i - (rules::FLAK_PELLETS - 1) * 0.5f) * rules::FLAK_SPREAD;
                fireShipBullet(e, muzzle, e.aim + off + rng.sym(D.aimError * 0.3f),
                               rules::FLAK_SPEED, rules::FLAK_DAMAGE, 0.9f, 1.8f);
            }
            e.gunCd = rules::FLAK_INTERVAL * e.cdScale * rng.range(0.85f, 1.3f);
        }
        break;
    case ShipWeapon::Cannon:
        e.gunCd -= dt;
        if (e.burst == 0) {
            // Wind up first: the glow on the barrel is the warning.
            if (e.gunCd <= 0.0f && aimed && dist > 200.0f && dist < D.gunRange * 1.3f) {
                e.burst = 1;
                e.burstCd = rules::CANNON_CHARGE;
                sfx(Sfx::CannonCharge, e.pos, 0.9f, 1.0f, 2600.0f);
            }
        } else {
            e.burstCd -= dt;
            if (e.burstCd <= 0.0f) {
                fireShipBullet(e, muzzle, e.aim, rules::CANNON_SPEED, rules::CANNON_DAMAGE, 2.4f, 3.4f);
                spawnSparks(muzzle, e.vel + ad * 120.0f, 10, 200.0f, e.tint, 0.3f);
                sfx(Sfx::CannonFire, muzzle, 1.0f, sfxRng.range(0.95f, 1.05f), 3000.0f);
                e.burst = 0;
                e.gunCd = rules::CANNON_INTERVAL * e.cdScale * rng.range(0.9f, 1.25f);
            }
        }
        break;
    }
}

void Game::updateShips(float dt) {
    const Difficulty& D = level.diff;
    for (Ship& s : ships) {
        if (!s.alive) continue;
        const dv2 oldPos = s.pos;
        const float oldAngle = s.angle;
        s.phase += dt;
        s.flash = std::max(0.0f, s.flash - dt * 4.0f);

        // Drift lazily around the berth, and swing the bow toward the player once awake.
        const float k = rules::SHIP_DRIFT;
        s.pos = dv2(s.anchor.x + std::sin(s.phase * 0.13f + s.baseAngle) * k,
                    s.anchor.y + std::cos(s.phase * 0.09f + s.baseAngle) * k * 0.6f);
        const float dist = dist2d(pl.pos, s.pos);
        if (!s.aggro) { if (dist < D.aggroRange) s.aggro = true; }
        else if (dist > D.aggroRange * 2.2f) s.aggro = false;
        float want = s.baseAngle + 0.25f * std::sin(s.phase * 0.07f);
        if (s.aggro) want = std::atan2((float)(pl.pos.y - s.pos.y), (float)(pl.pos.x - s.pos.x));
        const float turn = rules::SHIP_TURN * dt;
        s.angle = wrapAngle(s.angle + clampf(wrapAngle(want - s.angle), -turn, turn));
        s.vel    = tov2(s.pos - oldPos) / std::max(dt, 1e-4f);
        s.angVel = wrapAngle(s.angle - oldAngle) / std::max(dt, 1e-4f);

        // Wounded ships smoulder.
        if (s.hp < s.maxHp * 0.4f && rng.f() < 0.35f) {
            const v2 lp = s.hull[(size_t)rng.i(0, (int)s.hull.size() - 1)] * rng.range(0.2f, 0.8f);
            const v2 q = rot(lp, s.angle);
            spawnSparks(dv2(s.pos.x + q.x, s.pos.y + q.y), s.vel + q * 0.2f, 1, 60.0f, Col(1.0f, 0.6f, 0.3f), 0.5f);
        }

        // Carry the weapons along with it.
        for (ShipWeapon& w : s.weapons) {
            Enemy* e = findEnemy(w.enemyId);
            if (!e || !e->alive) continue;
            const v2 q = rot(w.local, s.angle);
            e->pos = dv2(s.pos.x + q.x, s.pos.y + q.y);
            e->vel = s.vel + perp(q) * s.angVel;
            updateShipWeapon(s, *e, dt);
        }

        // The hull is solid to the spaceman: he bounces off it.
        if (state == State::Playing) {
            v2 out;
            const float d = hullDistance(s, pl.pos, &out);
            const float hit = rules::PLAYER_HIT_R;
            if (d < hit) {
                pl.pos.x += (double)out.x * (hit - d);
                pl.pos.y += (double)out.y * (hit - d);
                const v2 hv = s.vel + perp(tov2(pl.pos - s.pos)) * s.angVel;
                const v2 rv = pl.vel - hv;
                const float vn = dot(rv, out);
                if (vn < 0.0f) pl.vel -= out * (vn * (1.0f + rules::SHIP_PUSH_SPEED));
                pl.grounded = false;
            }
        }
    }
    ships.erase(std::remove_if(ships.begin(), ships.end(),
                               [](const Ship& s) { return !s.alive; }), ships.end());
}

// ------------------------------------------------------------------ drawing --
void Game::drawShips(Renderer& r) {
    for (const Ship& s : ships) {
        if (!s.alive) continue;
        const v2 c0 = camRel(s.pos);
        if (!inView(c0, s.radius + 60.0f)) continue;

        const float ca = std::cos(s.angle), sa = std::sin(s.angle);
        auto place = [&](v2 local) { return c0 + rot(local, ca, sa); };
        const Col c = mix(s.col, Col(1, 1, 1), s.flash * 0.8f);
        const float I = 2.3f + 1.6f * s.flash;

        v2 pts[96];
        const int n = (int)std::min<size_t>(s.hull.size(), 96);
        for (int i = 0; i < n; ++i) pts[i] = place(s.hull[(size_t)i]);
        r.poly(pts, n, true, c, I);
        for (int i = 0; i < n; ++i) pts[i] = place(s.inner[(size_t)i]);
        r.poly(pts, n, true, c, I * 0.42f);
        for (size_t i = 0; i + 1 < s.lines.size(); i += 2)
            r.line(place(s.lines[i]), place(s.lines[i + 1]), c, I * 0.34f);

        v2 br[4];
        for (int i = 0; i < 4; ++i) br[i] = place(s.bridge[i]);
        r.poly(br, 4, true, mix(c, Col(1, 1, 1), 0.5f), I * 0.9f);

        // Exhaust: flickering flames out of the stern.
        for (const v2& e : s.engines) {
            const v2 a = place(e);
            const v2 back = fromAngle(s.angle + PIF);
            const float sk = clampf(s.radius / 350.0f, 0.35f, 1.3f);      // the small ships have small flames
            const float flick = (22.0f + rng.f() * 30.0f + (s.aggro ? 14.0f : 0.0f)) * sk;
            r.line(a, a + back * flick, mix(s.col, Col(1.0f, 0.85f, 0.6f), 0.6f), 2.8f);
            r.circle(a, 5.0f * sk, 8, c, I * 0.7f);
        }
    }
}

// The ray itself: a thin sight line while it charges (brighter, then locked and doubled by
// the edges of the ray-to-be for the last moment), and a thick slab of light while it fires.
void Game::drawLaserBeams(Renderer& r) {
    for (const Ship& s : ships) {
        if (!s.alive) continue;
        for (const ShipWeapon& w : s.weapons) {
            if (w.type != ShipWeapon::Laser) continue;
            const Enemy* e = findEnemy(w.enemyId);
            if (!e || e->burst == 0) continue;
            const v2 dir = fromAngle(e->aim), n = perp(dir);
            const dv2 from(e->pos.x + dir.x * (e->radius + 10.0), e->pos.y + dir.y * (e->radius + 10.0));
            const v2 p0 = camRel(from);
            const v2 p1 = p0 + dir * rules::LASER_RANGE;
            const float halfW = rules::LASER_WIDTH * 0.5f;
            const Col hot = mix(e->tint, Col(1.0f, 0.95f, 0.9f), 0.55f);

            if (e->burst == 1) {
                const float t = 1.0f - clampf(e->burstCd / rules::LASER_CHARGE, 0.0f, 1.0f);
                const bool locked = e->burstCd <= rules::LASER_LOCK;
                const Col c = mix(e->tint, Col(1.0f, 0.3f, 0.25f), 0.5f);
                r.line(p0, p1, c, locked ? 2.4f : 0.7f + 1.0f * t);
                if (locked || t > 0.5f) {                              // where the edges of the ray will be
                    const float k = locked ? 1.0f : (t - 0.5f) * 2.0f;
                    r.line(p0 + n * halfW, p1 + n * halfW, c, 0.6f + 1.0f * k);
                    r.line(p0 - n * halfW, p1 - n * halfW, c, 0.6f + 1.0f * k);
                }
                // The glow gathering at the muzzle.
                r.circle(p0, 8.0f + 46.0f * (1.0f - t), 16, hot, 0.8f + 2.6f * t);
                r.point(p0, 6.0f + 16.0f * t, hot, 1.0f + 3.0f * t);
            } else {
                const float f = clampf(e->burstCd / rules::LASER_FIRE_TIME, 0.0f, 1.0f);
                const float fade = std::min(1.0f, f * 4.0f);             // it thins away over the last quarter
                const float hw = halfW * (0.55f + 0.45f * fade);
                const float flick = 0.9f + 0.2f * std::sin(time * 90.0f);
                for (int k = -4; k <= 4; ++k) {                          // a slab of light, built of parallel lines
                    const bool core = std::abs(k) <= 1;
                    const v2 off = n * (hw * (float)k / 4.0f);
                    r.line(p0 + off, p1 + off, core ? Col(1.6f, 1.55f, 1.5f) : mix(e->tint, hot, 0.4f), (core ? 4.4f : 3.6f) * flick * fade + 0.4f);
                }
                r.line(p0 + n * hw, p1 + n * hw, e->tint, 3.0f * fade + 0.5f);
                r.line(p0 - n * hw, p1 - n * hw, e->tint, 3.0f * fade + 0.5f);
                r.circle(p0, 26.0f * fade + 8.0f, 16, hot, 3.0f * fade);
                r.point(p0, 26.0f * fade, Col(1.6f, 1.55f, 1.5f), 4.0f * fade);
            }
        }
    }
}

// The mounts are drawn with the enemies, since that is what they are.
void Game::drawWeaponMount(Renderer& r, const Enemy& e) {
    const v2 p = camRel(e.pos);
    const Col c = mix(e.tint, Col(1, 1, 1), e.flash);
    const float I = 2.0f + 1.8f * e.flash;
    const float R = e.radius * 0.7f;
    const float S = clampf(e.radius / 20.0f, 0.55f, 1.0f);      // the small ships have small mounts, with shorter barrels
    const v2 ad = fromAngle(e.aim), n = perp(ad);

    v2 base[6];
    for (int i = 0; i < 6; ++i) base[i] = p + fromAngle(i * TAUF / 6.0f + 0.5f) * R;
    r.poly(base, 6, true, c, I);

    switch (e.weapon) {
    case ShipWeapon::Gun:
        for (int k = -1; k <= 1; k += 2)
            r.line(p + n * (3.0f * S * k), p + ad * (R + 15.0f * S) + n * (3.0f * S * k), c, I * 1.1f);
        break;
    case ShipWeapon::Missile:
        for (int k = -1; k <= 1; ++k) {
            const v2 o = n * (5.0f * S * k);
            r.line(p + o, p + ad * (R + 9.0f * S) + o, pal::MISSILE, I * 0.9f);
        }
        r.line(p + ad * (R + 9.0f * S) - n * (7.0f * S), p + ad * (R + 9.0f * S) + n * (7.0f * S), c, I * 0.8f);
        break;
    case ShipWeapon::Flak:
        for (int k = -1; k <= 1; ++k) {
            const v2 d = fromAngle(e.aim + 0.32f * k);
            r.line(p + d * (R * 0.4f), p + d * (R + 12.0f * S), c, I);
        }
        break;
    case ShipWeapon::Laser: {                        // the ray: a heavy lens that fills with light, and a ring that shows the recharge
        const float chg = e.burst == 1 ? 1.0f - clampf(e.burstCd / rules::LASER_CHARGE, 0.0f, 1.0f) : (e.burst == 2 ? 1.0f : 0.0f);
        const Col lc = mix(c, Col(1.0f, 0.9f, 0.8f), chg);
        const v2 tip = p + ad * (R + 20.0f * S);
        for (int k = -1; k <= 1; k += 2) {
            r.line(p + n * (9.0f * S * k), tip + n * (4.0f * S * k), lc, I * 1.2f);
            r.line(tip + n * (4.0f * S * k), tip + ad * (12.0f * S), lc, I);
        }
        r.circle(tip, (5.0f + 9.0f * chg) * S, 14, lc, I * (0.8f + 1.6f * chg));
        if (e.burst == 0) {
            const float ready = 1.0f - clampf(e.gunCd / rules::LASER_RECHARGE, 0.0f, 1.0f);
            r.arc(p, e.radius + 9.0f, -PIF * 0.5f, -PIF * 0.5f + TAUF * ready, 24, mix(c, Col(1, 1, 1), ready >= 1.0f ? 0.6f : 0.0f), 1.6f);
        }
    } break;
    default: {                                       // Cannon: a fat barrel that glows as it charges
        const float charge = e.burst == 1 ? 1.0f - clampf(e.burstCd / rules::CANNON_CHARGE, 0.0f, 1.0f) : 0.0f;
        const Col bc = mix(c, Col(1.0f, 0.9f, 0.6f), charge);
        for (int k = -1; k <= 1; k += 2)
            r.line(p + n * (4.0f * S * k), p + ad * (R + 24.0f * S) + n * (4.0f * S * k), bc, I * (1.0f + charge));
        r.line(p + ad * (R + 24.0f * S) - n * (6.0f * S), p + ad * (R + 24.0f * S) + n * (6.0f * S), bc, I);
        if (charge > 0.0f) {
            r.circle(p + ad * (R + 24.0f * S), (26.0f * (1.0f - charge) + 4.0f) * S, 14, Col(1.0f, 0.85f, 0.5f), 1.0f + 2.4f * charge);
            r.point(p + ad * (R + 24.0f * S), 8.0f + 8.0f * charge, Col(1.0f, 0.9f, 0.6f), 3.0f * charge);
        }
    } break;
    }
    const float eye = 0.5f + 0.5f * std::sin(time * 9.0f + e.phase);
    r.circle(p, 3.0f, 8, c, I * (0.7f + 0.6f * eye));

    if (e.hp < e.maxHp) {
        const v2 camRight = fromAngle(-cam.angle);
        const v2 camUp = perp(camRight);
        const v2 bc = p + camUp * (e.radius + 14.0f);
        const float f = clampf(e.hp / e.maxHp, 0.0f, 1.0f);
        r.line(bc - camRight * 12.0f, bc + camRight * 12.0f, Col(0.4f, 0.1f, 0.1f), 1.6f);
        r.line(bc - camRight * 12.0f, bc - camRight * 12.0f + camRight * (24.0f * f), c, 2.4f);
    }
}

// The warship's health bar across the top, and a pointer to it while it is off screen.
void Game::drawShipHud(Renderer& r) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float m = 20.0f * s;
    const Ship* nearest = nullptr;
    float best = 1e30f;
    for (const Ship& sh : ships) {
        if (!sh.alive) continue;
        const float d = dist2d(pl.pos, sh.pos);
        if (d < best) { best = d; nearest = &sh; }
        drawEdgeMarker(r, sh.pos, sh.col, "WARSHIP", true, 1.25f,
                       sh.radius * ((float)r.fbw / (2.0f * cam.halfW)));
    }
    if (!nearest || best > 2600.0f) return;

    const float bw = 300.0f * s, bh = 6.0f * s;
    const float x = W * 0.5f - bw * 0.5f, y = m + 134.0f * s;
    char buf[64];
    snprintf(buf, sizeof buf, "WARSHIP   %s", nearest->name);
    const float th = 11.0f * s;
    r.text(v2(W * 0.5f - r.textWidth(th, buf) * 0.5f, y - 6.0f * s), th, buf, nearest->col, 1.6f);
    const v2 box[4] = { v2(x, y), v2(x + bw, y), v2(x + bw, y + bh), v2(x, y + bh) };
    r.poly(box, 4, true, nearest->col, 1.1f);
    const float f = clampf(nearest->hp / nearest->maxHp, 0.0f, 1.0f);
    r.line(v2(x + 1, y + bh * 0.5f), v2(x + 1 + (bw - 2) * f, y + bh * 0.5f), nearest->col, 2.4f);
}
