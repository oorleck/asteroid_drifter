// audio_game.cpp -- how the game turns what happens into what you hear.
//
// Game::sfx places a sound in the world: louder the closer it is to the spaceman,
// and panned by where it is on screen. Game::sfxUI is for things with no place
// (the depot, the clock). updateAudio runs once a frame and looks after the
// sounds that last: the rocket, the field, the shield, the heartbeat when the suit
// is failing, the ambient bed, the beacon's ping and the clock ticking down.
#include "game.h"
#include <algorithm>

void Game::sfx(Sfx s, dv2 at, float vol, float pitch, float range, float delay) {
    if (!audio::ready()) return;
    const v2 rel = tov2(at - pl.pos);
    const float d = len(rel);
    if (d >= range) return;
    const float k = 1.0f - d / range;
    const float g = vol * k * k;
    if (g < 0.01f) return;
    const v2 right = fromAngle(-cam.angle);                   // screen-right, in world space
    const float pan = clampf(dot(rel, right) / std::max(300.0f, cam.halfW * 0.9f), -1.0f, 1.0f) * 0.85f;
    audio::play(s, g, pan, pitch, delay);
}

void Game::sfxUI(Sfx s, float vol, float pitch, float delay) {
    if (!audio::ready()) return;
    audio::play(s, vol, 0.0f, pitch, delay);
}

void Game::updateAudio(float dt) {
    if (!audio::ready()) return;
    const bool live = !paused;
    const bool playing = state == State::Playing && !sandbox;

    // ---- sounds that last
    const bool alive = !playerGone();
    audio::loop(Sustain::Thruster, (live && alive && pl.thrusting) ? 1.0f : 0.0f, 0.88f + 0.28f * (pl.fuel / tune::FUEL_MAX));
    audio::loop(Sustain::FieldHum, (live && alive && pl.fieldOn) ? 0.5f + 0.4f * (pl.field / 100.0f) : 0.0f,
                0.88f + 0.22f * (pl.field / 100.0f));
    audio::loop(Sustain::ShieldHum, (live && alive && pl.shieldUp) ? 0.8f : 0.0f, 1.0f + 0.2f * pl.shieldFlash);
    const float weak = clampf(1.0f - pl.health / 35.0f, 0.0f, 1.0f);              // 0 above 35%, 1 at nothing
    audio::loop(Sustain::Heartbeat, (live && playing && alive && weak > 0.0f) ? 0.35f + 0.65f * weak : 0.0f, 1.0f + 0.55f * weak);
    audio::loop(Sustain::Ambient, live ? (state == State::Shop ? 0.6f : 0.5f) : 0.12f, 1.0f);

    if (!live) return;

    // ---- the depot opening
    if (state == State::Shop && audioState != State::Shop) sfxUI(Sfx::ShopOpen, 0.9f);
    audioState = state;

    if (!playing) return;

    // ---- the beacon pings, faster and brighter as you close in, and from its side
    pingTimer -= dt;
    if (pingTimer <= 0.0f) {
        const v2 to = tov2(level.goal - pl.pos);
        const float dist = len(to);
        const float closeness = 1.0f - clampf(dist / 3500.0f, 0.0f, 1.0f);
        const float side = dist > 1.0f ? dot(to / dist, fromAngle(-cam.angle)) : 0.0f;
        audio::play(Sfx::BeaconPing, 0.30f + 0.55f * closeness, side * 0.7f, 0.92f + 0.30f * closeness);
        pingTimer = clampf(dist / 1200.0f, 0.6f, 2.8f);
    }

    // ---- the clock, in the last few seconds
    if (level.timeLeft > 8.0f) audioSecond = 99;
    else {
        const int sec = (int)std::ceil(level.timeLeft);
        if (sec != audioSecond && level.timeLeft > 0.0f) {
            sfxUI(level.timeLeft < 4.0f ? Sfx::ClockTickHi : Sfx::ClockTick, 0.9f);
            audioSecond = sec;
        }
    }

    // ---- a missile bearing down on you
    warnTimer -= dt;
    float nearest = 1e9f;
    for (const Missile& m : missiles) if (!m.dead) nearest = std::min(nearest, len(tov2(m.pos - pl.pos)));
    if (nearest < 520.0f && warnTimer <= 0.0f) {
        audio::play(Sfx::MissileWarn, 0.7f, 0.0f, 1.0f + 0.35f * (1.0f - nearest / 520.0f));
        warnTimer = clampf(nearest / 1100.0f, 0.16f, 0.5f);
    }
}
