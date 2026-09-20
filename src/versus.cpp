// versus.cpp -- the shoot-each-other mode: an arena in the asteroid field, players
// who respawn on rocks, a frag limit, and bots to practise on.
//
// The local player is `pl`, exactly as in the single-player game, so everything
// written for one player still works. Everyone else is a Peer (game.h): a bot here,
// or a human whose commands arrive from the network. Both are moved by the same
// stepPlayer() as the local player, so the physics is identical for everyone.
//
// Rules: 100 suit, no healing. A rifle round does 5, a shell 42 plus a blast. You
// come back 3 seconds after dying, on a rock away from the others, protected for 2.
// A wall of pushing acceleration marks the edge of the arena. First to 10 frags wins.
#include "game.h"
#include <algorithm>
#include <cstdio>

namespace {
// Each player has a colour, so a bullet and a body are both readable as someone's.
const Col PALETTE[8] = {
    Col(0.85f, 0.95f, 1.00f),      // 0  cool white
    Col(1.00f, 0.45f, 0.35f),      // 1  red
    Col(0.40f, 1.00f, 0.55f),      // 2  green
    Col(1.00f, 0.85f, 0.30f),      // 3  amber
    Col(0.75f, 0.50f, 1.00f),      // 4  violet
    Col(0.30f, 0.85f, 1.00f),      // 5  cyan
    Col(1.00f, 0.55f, 0.85f),      // 6  pink
    Col(0.85f, 0.70f, 0.45f),      // 7  bronze
};
}

Player* Game::playerById(int id) {
    if (pl.id == id) return &pl;
    for (Peer& p : peers) if (p.body.id == id) return &p.body;
    return nullptr;
}

void Game::addKillMsg(const char* text, Col c) {
    KillMsg k;
    snprintf(k.text, sizeof k.text, "%s", text);
    k.ttl = 5.0f;
    k.col = c;
    killFeed.insert(killFeed.begin(), k);
    if (killFeed.size() > 6) killFeed.pop_back();
}

// ------------------------------------------------------------------ spawning --
// Finds a patch of sky on a rock to stand in. `turn` is where round the rock to start looking.
bool Game::standOnRock(int slot, Player& p) {
    const Body& b = world.bodies[slot];
    const float start = vsRng.angle();
    for (int i = 0; i < 16; ++i) {
        const float a = start + i * TAUF / 16.0f;
        const dv2 at(b.pos.x + std::cos(a) * (b.radius + 22.0), b.pos.y + std::sin(a) * (b.radius + 22.0));
        if (world.probe(at, 12.0f, nullptr, nullptr) >= 0) continue;
        p.pos = at;
        p.up = v2(std::cos(a), std::sin(a));
        p.vel = b.vel;
        return true;
    }
    return false;
}

void Game::respawnPlayer(Player& p) {
    // Candidates: real rocks well inside the arena.
    static std::vector<int> cand;
    cand.clear();
    for (int s : world.active) {
        const Body& b = world.bodies[s];
        if (b.alive && b.radius >= 45.0f && len(b.pos) < rules::ARENA_RADIUS * 0.85) cand.push_back(s);
    }
    // Try to be somewhere the others are not; give up on that if nothing suits.
    bool placed = false;
    for (int pass = 0; pass < 2 && !placed && !cand.empty(); ++pass) {
        for (int tries = 0; tries < 40 && !placed; ++tries) {
            const int s = cand[(size_t)vsRng.i(0, (int)cand.size() - 1)];
            Player probe = p;
            if (!standOnRock(s, probe)) continue;
            bool crowded = false;
            if (pass == 0) {
                eachPlayer([&](Player& o) {
                    if (&o == &p || o.dead) return;
                    if (len(probe.pos - o.pos) < rules::SPAWN_APART) crowded = true;
                });
            }
            if (crowded) continue;
            p.pos = probe.pos;  p.up = probe.up;  p.vel = probe.vel;
            placed = true;
        }
    }
    if (!placed) { p.pos = dv2(vsRng.sym(200.0f), vsRng.sym(200.0f));  p.vel = v2(0, 0); }

    p.dead = false;
    p.health = 100.0f;
    p.fuel = 100.0f;  p.fuelLocked = false;
    p.protect = rules::SPAWN_PROTECT;
    p.respawnIn = 0.0f;
    p.grounded = false;  p.ground = BodyRef();
    p.thrusting = false;  p.thrustGlow = 0.0f;  p.hurtGlow = 0.0f;
    p.fireCd = p.heavyCd = p.jumpCd = p.coyote = 0.0f;
    p.sinceHurt = 99.0f;
    if (&p == &pl) cam.pos = pl.pos;
}

void Game::resetMatch() {
    ++match.round;
    match.over = false;
    match.winner = -1;
    match.overTime = 0.0f;
    killFeed.clear();
    bullets.clear();
    eachPlayer([&](Player& p) { p.frags = 0;  p.deaths = 0; });
    eachPlayer([&](Player& p) { respawnPlayer(p); });
}

void Game::startVersus(Renderer& r, int bots) {
    ++runCount;
    r.arenaReset();
    world.init(baseSeed + (uint64_t)(runCount - 1) * 1013ull);
    bullets.clear();  parts.clear();  waves.clear();
    enemies.clear();  ebullets.clear();  missiles.clear();
    nukes.clear();    pmissiles.clear();  ships.clear();
    peers.clear();  killFeed.clear();
    sandbox = true;                               // no level layer: no beacon, clock, enemies or depot
    versus = true;
    state = State::Playing;
    stateTime = 0;
    match = Match();

    cam.pos = dv2(0, 0);
    cam.angle = 0;
    world.streamChunks(dv2(0, 0), rules::ARENA_RADIUS + 1500.0, 100000);   // the whole arena, at once
    world.step(1.0f / 60.0f, dv2(0, 0));
    world.syncGeometry(r);

    pl = Player();
    pl.id = 0;
    snprintf(pl.name, sizeof pl.name, "YOU");
    pl.tint = PALETTE[0];
    pl.hasHoming = true;                          // everyone has the shell in a match
    for (int i = 0; i < bots; ++i) {
        Peer p;
        p.bot = true;
        p.body.id = i + 1;
        snprintf(p.body.name, sizeof p.body.name, "BOT %d", i + 1);
        p.body.tint = PALETTE[(i + 1) % 8];
        p.body.hasHoming = true;
        p.botClock = vsRng.range(0.0f, 2.0f);
        peers.push_back(p);
    }
    resetMatch();
    match.round = 1;
    cam.pos = pl.pos;
}

// -------------------------------------------------------------------- damage --
void Game::damagePlayer(Player& p, float dmg, v2 kick, int attacker) {
    if (!versus) {
        if (&p == &pl) hurtPlayer(dmg, kick, attacker);
        return;
    }
    if (netClient || p.dead || match.over || dmg <= 0.0f) return;     // a client never decides damage
    if (p.protect > 0.0f) return;                 // just arrived: nothing touches them yet
    const bool self = &p == &pl;
    p.vel += kick;
    p.sinceHurt = 0.0f;
    p.hurtGlow = std::min(1.0f, p.hurtGlow + dmg * 0.04f);
    if (self) {
        damageTaken += dmg;
        sfxUI(Sfx::Hurt, clampf(dmg / 22.0f, 0.35f, 1.0f), sfxRng.range(0.92f, 1.08f));
        shake = std::max(shake, std::min(0.8f, dmg * 0.03f));
        if (invincible) return;                   // test hook
    } else {
        sfx(Sfx::Hurt, p.pos, clampf(dmg / 30.0f, 0.3f, 0.8f), sfxRng.range(0.95f, 1.15f), 1400.0f);
    }
    p.health = std::max(0.0f, p.health - dmg);
    if (p.health <= 0.0f) playerDied(p, attacker);
}

// What everyone sees when someone dies: the line in the feed, the bang, the sound.
// A client runs this too, when the host says so; only the host scores.
void Game::killEffects(int killer, int victim, dv2 at) {
    const Player* v = playerById(victim);
    const Player* k = playerById(killer);
    char buf[64];
    const char* vn = v ? v->name : "?";
    if (k && k != v) {
        snprintf(buf, sizeof buf, "%s  >  %s", k->name, vn);
        addKillMsg(buf, k->tint);
    } else {
        snprintf(buf, sizeof buf, "%s  FELL", vn);
        addKillMsg(buf, v ? v->tint : pal::HUD);
    }
    const Col c = v ? v->tint : pal::HUD;
    boom(at, 46.0f, c);
    spawnSparks(at, v ? v->vel : v2(0, 0), 70, 420.0f, mix(c, Col(1, 1, 1), 0.4f), 1.2f);
    if (v && v == &pl) { shake = 1.0f; sfxUI(Sfx::Death, 0.9f); }
}

void Game::playerDied(Player& victim, int killer) {
    if (victim.dead) return;
    victim.dead = true;
    victim.health = 0.0f;
    victim.thrusting = false;
    victim.respawnIn = rules::RESPAWN_TIME;
    ++victim.deaths;

    Player* k = playerById(killer);
    if (k && k != &victim) ++k->frags;
    else                   victim.frags = std::max(0, victim.frags - 1);          // a suicide costs a frag
    killEffects(killer, victim.id, victim.pos);
    netKilled(killer, victim.id, victim.pos);

    if (k && k != &victim && k->frags >= rules::FRAG_LIMIT && !match.over) {
        match.over = true;
        match.winner = k->id;
        match.overTime = 0.0f;
        sfxUI(k == &pl ? Sfx::LevelComplete : Sfx::GameOver, 0.9f, 1.0f, 0.5f);
    }
}

bool Game::bulletHitsPlayers(Bullet& b) {
    if (match.over || netClient) return false;                // a client only draws: the host decides who was hit
    bool hit = false;
    eachPlayer([&](Player& p) {
        if (hit || p.dead || p.id == b.owner) return;
        const float reach = rules::PLAYER_HIT_R + b.caliber * 0.5f + 2.0f;
        const double dx = p.pos.x - b.pos.x, dy = p.pos.y - b.pos.y;
        if (dx * dx + dy * dy > (double)reach * reach) return;
        hit = true;
        ++vsHits;
        spawnSparks(b.pos, b.vel * -0.1f, 6, 150.0f, p.tint, 0.3f);
        if (p.protect > 0.0f) return;                            // absorbed by the spawn shield
        if (b.heavy) {
            explodeOwner = b.owner;
            boom(b.pos, 26.0f, b.col);
            explode(b.pos, rules::HEAVY_SPLASH_R, 0.0f, rules::VS_HEAVY_DAMAGE, 0.0f, 0.0f, false);
        } else {
            damagePlayer(p, rules::VS_RIFLE_DAMAGE, norm(b.vel) * rules::VS_HIT_KICK, b.owner);
        }
    });
    return hit;
}

// ---------------------------------------------------------------------- bots --
// A sparring partner, not a champion: it aims at where you will be, but wobbles,
// fires in bursts, jumps about, and flies at you with the rocket when you are far.
void Game::botThink(Peer& b, float dt) {
    Player& me = b.body;
    PlayerCmd& c = b.cmd;
    c.fire = false;
    c.thrust = false;

    // The nearest living opponent.
    Player* target = nullptr;
    double best = 1e30;
    eachPlayer([&](Player& o) {
        if (&o == &me || o.dead) return;
        const double d = len2(o.pos - me.pos);
        if (d < best) { best = d; target = &o; }
    });
    if (!target || match.over) return;

    const v2 rel = tov2(target->pos - me.pos);
    const float dist = len(rel);
    const float t = dist / tune::BULLET_V;
    const v2 lead = rel + (target->vel - me.vel) * t;

    // A slow random wobble in the aim, bigger the further away the target is.
    b.botClock += dt;
    b.botAimErr += (vsRng.sym(1.0f) * 0.03f - b.botAimErr * 0.6f * dt) * (dt * 60.0f);
    const float wobble = clampf(b.botAimErr, -0.09f, 0.09f) * (0.6f + dist / 1200.0f);
    c.aim = std::atan2(lead.y, lead.x) + wobble;
    c.move = 0.0f;

    // Fire in bursts, and only with a clear line.
    b.botBurst -= dt;
    if (b.botBurst <= -0.6f) b.botBurst = 0.8f;
    if (b.botBurst > 0.0f && dist < 1500.0f && clearLine(me.pos, target->pos)) c.fire = true;

    // The odd shell at middling range.
    if (dist > 250.0f && dist < 1100.0f && me.heavyCd <= 0.0f && vsRng.f() < dt * 0.5f && clearLine(me.pos, target->pos))
        ++c.heavySeq;

    // Getting about. Walking is off, so the only way off a rock is to jump along its
    // surface normal, and the only way to steer is the rocket, which pushes toward
    // the aim. So: when distant away, jump the moment the way up is at all toward the
    // target (or, failing that, after a wait), then fly at them; when close, stay put
    // and shoot, hopping now and then to be harder to hit.
    b.botJumpCd -= dt;
    const v2 toT = dist > 1.0f ? rel / dist : v2(1, 0);
    if (me.grounded && b.botJumpCd <= 0.0f) {
        const bool distant = dist > 550.0f;
        const float facing = dot(me.up, toT);
        if (distant ? (facing > -0.15f || b.botJumpCd < -2.5f) : vsRng.f() < dt * 0.6f) {
            ++c.jumpSeq;
            b.botJumpCd = distant ? 0.8f : vsRng.range(1.5f, 3.5f);
        }
    }
    if (!me.grounded && dist > 500.0f && me.fuel > 6.0f) {
        const float closing = dot(me.vel - target->vel, toT);         // how fast we are already closing
        if (closing < 320.0f) c.thrust = true;
    }
}

// -------------------------------------------------------------------- update --
void Game::updateVersus(float dt) {
    for (KillMsg& k : killFeed) k.ttl -= dt;
    killFeed.erase(std::remove_if(killFeed.begin(), killFeed.end(), [](const KillMsg& k) { return k.ttl <= 0.0f; }),
                   killFeed.end());
    if (netClient) return;             // everything below is the host's job: a client is told the result

    if (match.over) {
        match.overTime += dt;
        if (match.overTime > rules::VS_MATCH_OVER) resetMatch();
    }

    // The opponents. Bots think; a human's command has already arrived by other means.
    for (Peer& pe : peers) {
        Player& p = pe.body;
        if (p.dead) {
            p.respawnIn -= dt;
            if (p.respawnIn <= 0.0f && !match.over) respawnPlayer(p);
            continue;
        }
        if (pe.bot) botThink(pe, dt);
        stepPlayer(p, pe.cmd, dt);
    }
    if (pl.dead) {
        pl.respawnIn -= dt;
        if (pl.respawnIn <= 0.0f && !match.over) respawnPlayer(pl);
    }

    // The wall: past the edge of the arena something pushes you back in.
    eachPlayer([&](Player& p) {
        if (p.dead) return;
        const double r = len(p.pos);
        if (r <= rules::ARENA_RADIUS) return;
        const v2 inward = tov2(dv2(-p.pos.x / r, -p.pos.y / r));
        p.vel += inward * (rules::ARENA_PUSH * (float)((r - rules::ARENA_RADIUS) / 500.0) * dt);
    });
}

// ----------------------------------------------------------------------- hud --
void Game::drawVersusHud(Renderer& r) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float m = 20.0f * s;
    auto centred = [&](const char* txt, float h, float y, Col c, float inten) {
        r.text(v2(W * 0.5f - r.textWidth(h, txt) * 0.5f, y), h, txt, c, inten);
    };
    char buf[96];

    // ---- the connection, top left
    if (net) {
        const std::string st = netStatus();
        const bool bad = netClient && (!netConnected());
        r.text(v2(m, 158.0f * s), 10.0f * s, st.c_str(), bad ? pal::WARN : Col(0.55f, 0.85f, 0.9f), bad ? 2.0f : 1.3f);
    }

    // ---- the scoreboard, top centre
    static std::vector<const Player*> order;
    order.clear();
    order.push_back(&pl);
    for (const Peer& p : peers) order.push_back(&p.body);
    std::sort(order.begin(), order.end(), [](const Player* a, const Player* b) { return a->frags > b->frags; });
    snprintf(buf, sizeof buf, "FIRST TO %d", rules::FRAG_LIMIT);
    centred(buf, 10.0f * s, m + 10.0f * s, pal::HUD, 1.2f);
    float y = m + 32.0f * s;
    for (const Player* p : order) {
        snprintf(buf, sizeof buf, "%-8s %2d", p->name, p->frags);
        const bool me = p == &pl;
        centred(buf, (me ? 15.0f : 12.0f) * s, y, p->dead ? mix(p->tint, Col(0.3f, 0.3f, 0.3f), 0.6f) : p->tint, me ? 2.2f : 1.5f);
        y += (me ? 22.0f : 18.0f) * s;
    }

    // ---- who killed whom, down the right
    float ky = 210.0f * s;
    for (const KillMsg& k : killFeed) {
        const float a = clampf(k.ttl / 1.0f, 0.0f, 1.0f);
        r.text(v2(W - m - r.textWidth(11.0f * s, k.text), ky), 11.0f * s, k.text, k.col, 1.7f * a);
        ky += 17.0f * s;
    }

    // ---- opponents: a tag over each one in view, an arrow to each one that is not
    const float pxPerUnit = W / (2.0f * cam.halfW);
    for (const Peer& pe : peers) {
        const Player& p = pe.body;
        if (p.dead) continue;
        drawEdgeMarker(r, p.pos, p.tint, p.name, true, 1.0f, 30.0f * pxPerUnit);
        const v2 sp = worldToScreen(r, p.pos);
        if (sp.x < 0 || sp.y < 0 || sp.x > W || sp.y > H) continue;
        const float th = 9.0f * s;
        r.text(v2(sp.x - r.textWidth(th, p.name) * 0.5f, sp.y - 34.0f * s), th, p.name, p.tint, 1.5f);
        const float bw = 34.0f * s;
        const float f = clampf(p.health / 100.0f, 0.0f, 1.0f);
        const v2 a(sp.x - bw * 0.5f, sp.y - 28.0f * s), b(sp.x + bw * 0.5f, sp.y - 28.0f * s);
        r.line(a, b, Col(0.35f, 0.1f, 0.1f), 1.4f);
        r.line(a, a + (b - a) * f, p.tint, 2.2f);
    }

    // ---- the state of you
    if (pl.dead && !match.over) {
        centred("ELIMINATED", 40.0f * s, H * 0.36f, pal::WARN, 2.3f);
        snprintf(buf, sizeof buf, "BACK IN %d", (int)std::ceil(std::max(0.0f, pl.respawnIn)));
        centred(buf, 16.0f * s, H * 0.36f + 34.0f * s, pal::HUD, 1.7f);
    }
    if (len(pl.pos) > rules::ARENA_RADIUS && !pl.dead && std::fmod(time, 0.8f) < 0.55f)
        centred("LEAVING THE ARENA", 18.0f * s, H * 0.30f, pal::WARN, 2.2f);

    if (match.over) {
        const Player* w = playerById(match.winner);
        snprintf(buf, sizeof buf, "%s WINS", w ? w->name : "NOBODY");
        centred(buf, 56.0f * s, H * 0.34f, w ? w->tint : pal::HUD, 2.5f);
        snprintf(buf, sizeof buf, "NEW MATCH IN %d", (int)std::ceil(std::max(0.0f, rules::VS_MATCH_OVER - match.overTime)));
        centred(buf, 15.0f * s, H * 0.34f + 40.0f * s, pal::HUD, 1.7f);
        if (match.overTime > 1.0f) centred(netClient ? "WAITING FOR THE HOST" : "PRESS ENTER TO START NOW", 12.0f * s, H * 0.34f + 64.0f * s, Col(1, 1, 1), 1.5f);
    }
}
