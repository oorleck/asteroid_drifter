// level_hud.cpp -- drawing for the level layer: the goal beacon, off-screen
// pointers, the clock, banners and the game-over screen.
#include "game.h"
#include <cstdio>
#include <cstring>

// World position to a pixel in the HUD's coordinate system. The shader rolls
// the world by cam.angle, so this has to do the same.
v2 Game::worldToScreen(Renderer& r, dv2 p) const {
    const v2 q = rot(camRel(p), std::cos(cam.angle), std::sin(cam.angle));
    const float k = (float)r.fbw / (2.0f * cam.halfW);
    return v2(r.fbw * 0.5f + q.x * k, r.fbh * 0.5f - q.y * k);
}

void Game::drawGoal(Renderer& r) {
    if (sandbox) return;
    const v2 rel = camRel(level.goal);
    const float R = rules::GOAL_RADIUS;
    if (!inView(rel, R * 1.8f)) return;

    const float t = time;
    const float nearness = clampf(1.0f - (float)len(level.goal - pl.pos) / 700.0f, 0.0f, 1.0f);
    const float pulse = 0.5f + 0.5f * std::sin(t * (3.0f + 5.0f * nearness));
    const Col c = pal::GOAL;
    const float inten = 1.7f + 1.1f * pulse + 1.2f * nearness;

    // Outer ring, broken into rotating dashes so it reads as a beacon.
    const int dashes = 20;
    for (int i = 0; i < dashes; ++i) {
        const float a0 = t * 0.6f + i * TAUF / dashes;
        r.arc(rel, R, a0, a0 + TAUF / dashes * 0.62f, 5, c, inten);
    }
    r.circle(rel, R * 0.66f + pulse * 4.0f, 48, c, inten * 0.8f);
    r.circle(rel, R * 0.30f, 32, c, inten * 1.1f);

    // A triangle and a square turning against each other, and ticks around the rim.
    {
        v2 tri[3], sq[4];
        for (int i = 0; i < 3; ++i) tri[i] = rel + fromAngle(t * 1.3f + i * TAUF / 3.0f) * (R * 0.52f);
        for (int i = 0; i < 4; ++i) sq[i]  = rel + fromAngle(-t * 1.9f + i * TAUF / 4.0f) * (R * 0.36f);
        r.poly(tri, 3, true, c, inten * 0.9f);
        r.poly(sq, 4, true, c, inten * 0.8f);
    }
    for (int i = 0; i < 12; ++i) {
        const v2 d = fromAngle(i * TAUF / 12.0f - t * 0.25f);
        r.line(rel + d * (R * 1.06f), rel + d * (R * 1.22f), c, inten * 0.7f);
    }
    // Faint halo rings that breathe outward.
    for (int i = 0; i < 3; ++i) {
        const float f = std::fmod(t * 0.5f + i / 3.0f, 1.0f);
        r.circle(rel, R * (1.25f + 1.4f * f), 56, c, 0.9f * (1.0f - f));
    }
}

// Where an off-screen arrow may sit: a margin in from each edge, and a deeper one
// along the bottom so arrows never land on the suit bar or the help text, and along
// the top so they stay clear of the clock and the warship's health bar.
static void arrowLimits(float W, float H, float s, float margin, float dy, float& limX, float& limY) {
    const float marginBottom = std::max(margin, 175.0f * s);
    const float marginTop    = std::max(margin, 190.0f * s);
    limX = W * 0.5f - margin;
    limY = H * 0.5f - (dy > 0.0f ? marginBottom : marginTop);   // +y is down the screen
}

// An arrow on the edge of the screen pointing at something off-screen. `size`
// scales the arrow and its label: the beacon's is drawn at twice the size of the
// small warnings, and pulses, because it is the thing you are steering by.
void Game::drawEdgeMarker(Renderer& r, dv2 target, Col c, const char* label, bool onlyOffscreen,
                          float size, float hidePx) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f) * size;
    const float margin = 52.0f * s;
    const v2 sp = worldToScreen(r, target);
    const v2 ctr(W * 0.5f, H * 0.5f);
    const v2 d = sp - ctr;
    // Anything you can already see needs no arrow. `hidePx` is how far off the
    // edge the target's centre may be while part of it is still visible.
    if (onlyOffscreen && sp.x > -hidePx && sp.x < W + hidePx && sp.y > -hidePx && sp.y < H + hidePx)
        return;
    float limX, limY;
    arrowLimits(W, H, s / size, margin, d.y, limX, limY);
    const bool on = std::fabs(d.x) < limX && std::fabs(d.y) < limY;
    if (on && onlyOffscreen) return;

    v2 pos = sp;
    if (!on) {
        const float tx = limX / std::max(std::fabs(d.x), 1e-3f);
        const float ty = limY / std::max(std::fabs(d.y), 1e-3f);
        pos = ctr + d * std::min(tx, ty);
    }
    const v2 dn = norm(d);
    const float pulse = size > 1.2f ? 0.75f + 0.25f * std::sin(time * 6.0f) : 1.0f;
    const float I = (size > 1.2f ? 3.0f : 2.4f) * pulse;

    // A solid chevron: three nested strokes so it reads as heavy, not spindly.
    for (int k = 0; k < (size > 1.2f ? 3 : 1); ++k) {
        const float back = k * 7.0f * s;
        const v2 tip = pos + dn * (13.0f * s - back);
        const v2 wl  = pos - dn * (5.0f * s + back) + perp(dn) * (10.0f * s);
        const v2 wr  = pos - dn * (5.0f * s + back) - perp(dn) * (10.0f * s);
        r.line(wl, tip, c, I);
        r.line(wr, tip, c, I);
        if (k == 0) { r.line(wl, pos, c, I * 0.7f); r.line(wr, pos, c, I * 0.7f); }
    }

    if (label[0]) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s %.0f", label, len(target - pl.pos));
        const float th = 9.5f * s;
        const float tw = r.textWidth(th, buf);
        // Step back from the arrow by half the label along the axis it points, so the
        // text never sits underneath it.
        v2 tp = pos - dn * (22.0f * s + std::fabs(dn.x) * tw * 0.5f + std::fabs(dn.y) * th);
        tp.x = clampf(tp.x - tw * 0.5f, 6.0f * s, W - tw - 6.0f * s);
        tp.y = clampf(tp.y + th * 0.5f, th + 6.0f * s, H - 6.0f * s);
        r.text(tp, th, buf, c, 1.5f + 0.8f * (size - 1.0f));
    }
}

// Is any part of the beacon on screen? Its centre may be a little past the edge
// while the ring is still visible, so the test is padded by the ring's size.
bool Game::beaconInView(Renderer& r) const {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float pad = rules::GOAL_RADIUS * 1.3f * (W / (2.0f * cam.halfW));
    const v2 gp = worldToScreen(r, level.goal);
    return gp.x > -pad && gp.x < W + pad && gp.y > -pad && gp.y < H + pad;
}

// A stream of chevrons running from the spaceman toward the beacon. It is drawn
// around the player, where the eye already is, so the way to go is never further
// than a glance away. It is hidden once the beacon is close enough to see.
void Game::drawBeaconCompass(Renderer& r) {
    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const v2 pp = worldToScreen(r, pl.pos);
    const v2 gp = worldToScreen(r, level.goal);
    // Once any part of the beacon is on screen you can simply see where it is, so
    // the hint goes.
    if (beaconInView(r)) return;
    (void)W; (void)H;
    const v2 d = gp - pp;
    const float dist = len(d);
    if (dist < 1.0f) return;
    const v2 dn = d / dist;
    const v2 side = perp(dn);
    const float k = rules::COMPASS_STRENGTH;               // overall visibility, 0..1

    // Chevrons flow outward along the line to the beacon, brightest near the
    // player and fading as they go, so the motion itself says "this way". They
    // are small, thin and faint: a hint the eye can find when it looks, not a
    // shape that competes with the action.
    const float flow = std::fmod(time * 1.6f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const float f = (i + flow) / 4.0f;                      // 0..1 along the run
        const float off = (58.0f + 92.0f * f) * s;
        const float grow = 0.75f + 0.45f * f;
        const float a = (1.0f - f) * (f < 0.12f ? f / 0.12f : 1.0f);   // fade in and out
        if (a <= 0.02f) continue;
        const v2 c = pp + dn * off;
        const float w = 11.0f * s * grow, l = 9.5f * s * grow;
        const v2 tip = c + dn * l;
        const v2 wl = c - dn * (l * 0.35f) + side * w;
        const v2 wr = c - dn * (l * 0.35f) - side * w;
        r.line(wl, tip, pal::GOAL, 4.0f * a * k);
        r.line(wr, tip, pal::GOAL, 4.0f * a * k);
    }

    // The distance, tucked beside the first chevron, in small quiet type.
    char buf[32];
    snprintf(buf, sizeof buf, "%.0f M", len(level.goal - pl.pos));
    const float th = 9.5f * s;
    const v2 tp = pp + dn * (58.0f * s) + side * (26.0f * s);
    r.text(v2(tp.x - (side.x < 0 ? r.textWidth(th, buf) : 0.0f), tp.y + th * 0.4f), th, buf, pal::GOAL, 3.0f * k);
}

void Game::drawLevelHud(Renderer& r) {
    drawNukeLabels(r);
    if (sandbox) return;
    if (state == State::Shop) { drawShop(r); return; }

    const float W = (float)r.fbw, H = (float)r.fbh;
    const float s = clampf(H / 900.0f, 0.7f, 2.0f);
    const float m = 20.0f * s;
    char buf[128];
    auto centred = [&](const char* txt, float h, float y, Col c, float inten) {
        r.text(v2(W * 0.5f - r.textWidth(h, txt) * 0.5f, y), h, txt, c, inten);
    };

    // ---- top centre: level and score, the clock, distance to the beacon
    snprintf(buf, sizeof buf, "LEVEL %d     CREDITS %d", level.number, credits);
    centred(buf, 11.0f * s, m + 12.0f * s, pal::HUD, 1.25f);

    const float tl = level.timeLeft;
    const bool urgent = tl < 8.0f;
    const float pulse = urgent ? 0.65f + 0.35f * std::sin(time * (tl < 4.0f ? 18.0f : 9.0f)) : 1.0f;
    snprintf(buf, sizeof buf, "%.1f", tl);
    centred(buf, 40.0f * s, m + 64.0f * s, urgent ? pal::WARN : Col(0.85f, 1.0f, 1.0f),
            urgent ? 2.3f * pulse : 1.6f);

    // A bar under the clock so the drain is readable at a glance.
    {
        const float bw = 230.0f * s, bh = 5.0f * s;
        const float x = W * 0.5f - bw * 0.5f, y = m + 72.0f * s;
        const v2 box[4] = { v2(x, y), v2(x + bw, y), v2(x + bw, y + bh), v2(x, y + bh) };
        const Col bc = urgent ? pal::WARN : pal::HUD;
        r.poly(box, 4, true, bc, 1.0f);
        const float frac = clampf(tl / std::max(1.0f, level.diff.timeLimit), 0.0f, 1.0f);
        r.line(v2(x + 1, y + bh * 0.5f), v2(x + 1 + (bw - 2) * frac, y + bh * 0.5f), bc, 2.0f);
    }
    snprintf(buf, sizeof buf, "BEACON  %.0f M", len(level.goal - pl.pos));
    centred(buf, 11.0f * s, m + 96.0f * s, pal::GOAL, 1.5f);

    // ---- pointers to things that are off-screen
    if (state == State::Playing) {
        drawBeaconCompass(r);
        drawShipHud(r);
        drawEdgeMarker(r, level.goal, pal::GOAL, "BEACON", true, 2.1f,
                       rules::GOAL_RADIUS * 1.3f * ((float)r.fbw / (2.0f * cam.halfW)));
        for (const Missile& mi : missiles) {
            if (mi.dead) continue;
            if (len(mi.pos - pl.pos) < 1600.0) drawEdgeMarker(r, mi.pos, pal::MISSILE, "", true);
        }
    }

    // ---- pop-up line ("NUKE +3", "BEACON REACHED +450")
    if (messageTime > 0.0f)
        centred(message, 17.0f * s, H * 0.70f, pal::NUKE,
                1.9f * clampf(messageTime * 1.6f, 0.0f, 1.0f));

    // ---- level intro
    if (level.banner > 0.0f && state == State::Playing) {
        const float a = clampf(level.banner / 1.3f, 0.0f, 1.0f);
        snprintf(buf, sizeof buf, "LEVEL %d", level.number);
        centred(buf, 54.0f * s, H * 0.30f, pal::HUD, 2.3f * a);
        snprintf(buf, sizeof buf, "REACH THE BEACON IN %d SECONDS", (int)(level.diff.timeLimit + 0.5f));
        centred(buf, 15.0f * s, H * 0.30f + 36.0f * s, pal::GOAL, 1.9f * a);
        snprintf(buf, sizeof buf, "HOSTILES  %d TURRETS   %d DRONES",
                 level.diff.turrets, level.diff.drones);
        centred(buf, 12.0f * s, H * 0.30f + 60.0f * s, pal::WARN, 1.6f * a);
        if (level.hasShip) {
            snprintf(buf, sizeof buf, "WARSHIP  %s   %d WEAPONS", level.ship.name, (int)level.ship.weapons.size());
            centred(buf, 12.0f * s, H * 0.30f + 82.0f * s, level.ship.col, 1.8f * a);
        }
    }

    if (state == State::LevelComplete) {
        const float a = clampf((rules::COMPLETE_TIME - stateTime) / 0.8f, 0.0f, 1.0f);
        centred("BEACON REACHED", 48.0f * s, H * 0.32f, pal::GOAL, 2.4f * a + 0.3f);
        centred("THE DEPOT OPENS...", 15.0f * s, H * 0.32f + 34.0f * s, pal::HUD, 1.6f);
    }

    if (state == State::Dead) {
        const float a = clampf(stateTime / 0.5f, 0.0f, 1.0f);
        centred("LIFE LOST", 54.0f * s, H * 0.34f, pal::WARN, 2.4f * a);
        centred(gameOverReason, 16.0f * s, H * 0.34f + 34.0f * s, pal::WARN, 1.6f * a);
        if (lives == 1) snprintf(buf, sizeof buf, "ONE LIFE LEFT   -   RETRYING LEVEL %d", level.number);
        else            snprintf(buf, sizeof buf, "%d LIVES LEFT   -   RETRYING LEVEL %d", lives, level.number);
        centred(buf, 14.0f * s, H * 0.34f + 62.0f * s, pal::HUD, 1.6f * a);
    }

    if (state == State::GameOver) {
        centred("GAME OVER", 64.0f * s, H * 0.34f, pal::WARN, 2.5f);
        snprintf(buf, sizeof buf, "%s   -   OUT OF LIVES", gameOverReason);
        centred(buf, 18.0f * s, H * 0.34f + 42.0f * s, pal::WARN, 1.7f);
        snprintf(buf, sizeof buf, "REACHED LEVEL %d     %d CREDITS EARNED", level.number, totalEarned);
        centred(buf, 15.0f * s, H * 0.34f + 76.0f * s, pal::HUD, 1.6f);
        snprintf(buf, sizeof buf, "BEST   LEVEL %d     %d CREDITS EARNED", bestLevel, bestEarned);
        centred(buf, 12.0f * s, H * 0.34f + 98.0f * s, Col(0.55f, 0.8f, 0.9f), 1.3f);
        if (stateTime > 1.0f && std::fmod(stateTime, 1.2f) < 0.8f)
            centred("PRESS ENTER TO TRY AGAIN", 15.0f * s, H * 0.34f + 138.0f * s, Col(1, 1, 1), 1.9f);
    }
}
