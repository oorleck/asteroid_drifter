// sounds.cpp -- the synthesis of every sound in the game.
//
// The picture is neon line-work with a soft bloom, so the sounds are clean tones
// and sweeps (sine, triangle, a little saw), noise blasts that are filtered and
// shaped rather than raw, and, on anything that explodes or rings, a scatter of
// tiny bright blips that stands in for the sparks. The mixer adds a space reverb.
//
// Everything is built once at start-up from the code below into short mono
// buffers, then normalised to the peak level in the table at the bottom.
#include "audio.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace {

using Buf = std::vector<float>;
constexpr float SR  = (float)audio::RATE;
constexpr float TAU = 6.28318530718f;

struct Noise {
    uint32_t s;
    explicit Noise(uint32_t seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    float w() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s >> 8) * (2.0f / 16777216.0f) - 1.0f; }
    float u() { return w() * 0.5f + 0.5f; }
};

// A one-pole low-pass with a cut-off that can change every sample.
struct LP {
    float y = 0.0f;
    float run(float x, float fc) {
        const float a = 1.0f - std::exp(-TAU * std::min(fc, SR * 0.45f) / SR);
        y += a * (x - y);
        return y;
    }
};

// A state-variable filter: low-pass, band-pass and high-pass at once.
struct SVF {
    float lo = 0.0f, bp = 0.0f;
    float low = 0, band = 0, high = 0;
    void run(float x, float fc, float damp) {
        const float f = 2.0f * std::sin(3.14159265f * std::min(fc, SR * 0.16f) / SR);
        lo += f * bp;
        high = x - lo - damp * bp;
        bp += f * high;
        low = lo;  band = bp;
    }
};

inline float saw(float ph)   { const float x = ph / TAU; return 2.0f * (x - std::floor(x + 0.5f)); }
inline float tri(float ph)   { return 2.0f * std::fabs(saw(ph)) - 1.0f; }
inline float atk(float t, float a) { return t >= a ? 1.0f : t / a; }

template <class F> Buf gen(float sec, F f) {
    Buf b((size_t)(sec * SR));
    for (size_t i = 0; i < b.size(); ++i) b[i] = f((float)i / SR);
    return b;
}

void addTo(Buf& dst, const Buf& src, float at, float gain = 1.0f) {
    const size_t o = (size_t)(at * SR);
    for (size_t i = 0; i < src.size() && o + i < dst.size(); ++i) dst[o + i] += src[i] * gain;
}

// A struck-metal or glassy tone: partials that are not whole multiples, each
// dying a little faster than the one below.
Buf bell(float f, float dur, float decay, bool metallic = true) {
    static const float mR[5] = { 1.0f, 2.76f, 5.40f, 8.93f, 13.3f };
    static const float hR[5] = { 1.0f, 2.0f, 3.0f, 4.2f, 5.4f };
    static const float amp[5] = { 1.0f, 0.55f, 0.28f, 0.14f, 0.07f };
    const float* R = metallic ? mR : hR;
    return gen(dur, [&](float t) {
        float x = 0.0f;
        for (int k = 0; k < 5; ++k)
            x += std::sin(TAU * f * R[k] * t) * amp[k] * std::exp(-t * decay * (1.0f + 0.9f * k));
        return x * atk(t, 0.002f);
    });
}

// Tiny bright blips scattered through a time range: the sparks.
void sparkle(Buf& b, Noise& n, int count, float t0, float t1, float fLo, float fHi, float amp, float decay) {
    for (int i = 0; i < count; ++i) {
        const float u = n.u();
        const float at = t0 + (t1 - t0) * u * u;                    // more of them early on
        const float f = fLo + (fHi - fLo) * n.u();
        const float a = amp * (0.4f + 0.6f * n.u());
        const size_t o = (size_t)(at * SR);
        for (size_t k = 0; o + k < b.size() && k < (size_t)(0.12f * SR); ++k) {
            const float t = (float)k / SR;
            b[o + k] += std::sin(TAU * f * t) * a * std::exp(-t * decay) * atk(t, 0.0008f);
        }
    }
}

// One routine for every kind of bang: a burst of noise whose brightness falls
// away, a sine thump underneath, a crack on top, and the sparkle.
Buf explosion(float dur, float decay, float fc0, float fcRate, float fcFloor,
              float subF0, float subF1, float subRate, float subAmp,
              int sparks, float sparkAmp, uint32_t seed, float drive) {
    Noise n(seed), c(seed * 7u + 3u);
    LP a, b;
    float ph = 0.0f;
    Buf out = gen(dur, [&](float t) {
        const float fc = fcFloor + fc0 * std::exp(-t * fcRate);
        const float body = b.run(a.run(n.w(), fc), fc);
        const float env = std::exp(-t * decay) * atk(t, 0.002f);
        const float f = subF1 + (subF0 - subF1) * std::exp(-t * subRate);
        ph += TAU * f / SR;
        const float sub = std::sin(ph) * std::exp(-t * decay * 0.7f) * subAmp;
        const float crack = t < 0.014f ? c.w() * (1.0f - t / 0.014f) : 0.0f;
        return std::tanh((body * env * 2.6f + sub + crack * 0.5f) * drive);
    });
    Noise sp(seed ^ 0xABCDEFu);
    sparkle(out, sp, sparks, 0.01f, dur * 0.6f, 2600.0f, 8500.0f, sparkAmp, 55.0f);
    return out;
}

// Repeats a stretch of a buffer so the end runs into the start with no seam.
Buf loopify(const Buf& raw, size_t fade) {
    const size_t n = raw.size() - fade;
    Buf out(raw.begin(), raw.begin() + (std::ptrdiff_t)n);
    for (size_t i = 0; i < fade; ++i) {
        const float w = (float)i / (float)fade;
        out[i] = raw[i] * w + raw[n + i] * (1.0f - w);
    }
    return out;
}

// ------------------------------------------------------------- your weapons --
Buf makeRifle() {
    float ph = 0.0f;  Noise n(11);
    return gen(0.13f, [&](float t) {
        ph += TAU * (520.0f + 1900.0f * std::exp(-t * 30.0f)) / SR;                 // a bright zip downward
        const float tone = std::sin(ph) + 0.33f * std::sin(2.0f * ph);
        const float click = t < 0.004f ? n.w() * (1.0f - t / 0.004f) : 0.0f;
        return std::exp(-t * 34.0f) * atk(t, 0.0007f) * tone * 0.9f + click * 0.45f;
    });
}

Buf makeShell() {
    float ph = 0.0f, ph2 = 0.0f;  Noise n(12);  LP lp;
    Buf b = gen(0.42f, [&](float t) {
        ph  += TAU * (72.0f + 380.0f * std::exp(-t * 15.0f)) / SR;                  // a heavy thump
        ph2 += TAU * (900.0f + 1800.0f * std::exp(-t * 9.0f)) / SR;                  // the shimmer of a homing shell
        const float body = std::sin(ph) + 0.4f * std::sin(2.0f * ph);
        const float puff = lp.run(n.w(), 1400.0f) * std::exp(-t * 18.0f) * 1.3f;
        const float sh = std::sin(ph2) * std::exp(-t * 16.0f) * 0.26f;
        return std::tanh((body * std::exp(-t * 9.0f) + puff + sh) * 1.5f) * atk(t, 0.001f);
    });
    return b;
}

Buf makeSalvo() {
    Noise n(13);
    SVF f[5];
    return gen(0.62f, [&](float t) {
        float x = 0.0f;
        for (int k = 0; k < 5; ++k) {
            const float u = t - 0.052f * k;                                         // five missiles leave one after another
            if (u < 0.0f || u > 0.34f) continue;
            f[k].run(n.w(), 700.0f + 3600.0f * std::min(1.0f, u / 0.3f), 0.5f);
            const float env = (1.0f - std::exp(-u * 90.0f)) * std::exp(-u * 8.0f);
            x += f[k].band * env * 1.6f + std::sin(TAU * (1000.0f + 2600.0f * u) * u) * env * 0.16f;
        }
        return x;
    });
}

// A crystalline shell: a deep thump as it leaves, and a glassy shimmer that rises
// after it, the sound of something about to come apart.
Buf makeFractalFire() {
    float ph = 0.0f;  Noise n(15);  LP lp;
    Buf b = gen(0.75f, [&](float t) {
        ph += TAU * (60.0f + 130.0f * std::exp(-t * 16.0f)) / SR;
        const float thump = std::sin(ph) * std::exp(-t * 9.0f) + lp.run(n.w(), 900.0f) * std::exp(-t * 26.0f) * 0.5f;
        float glass = 0.0f;
        for (int k = 0; k < 3; ++k) {                                           // three partials, climbing a little
            const float f = (1900.0f + 900.0f * k) * (1.0f + 0.45f * std::min(1.0f, t / 0.5f));
            glass += std::sin(TAU * f * t) * (0.5f - 0.1f * k);
        }
        const float shimmer = 0.6f + 0.4f * std::sin(TAU * 21.0f * t);
        return (thump * 1.1f + glass * shimmer * 0.30f * atk(t, 0.04f) * std::exp(-std::max(0.0f, t - 0.25f) * 6.0f)) * atk(t, 0.001f);
    });
    Noise s(16);  sparkle(b, s, 8, 0.05f, 0.5f, 3500.0f, 8500.0f, 0.06f, 40.0f);
    return b;
}

// A quick glassy chirp: a shell dividing in two.
Buf makeFractalSplit() {
    return gen(0.2f, [](float t) {
        auto blip = [](float u, float f0, float f1) {
            if (u < 0.0f) return 0.0f;
            const float f = f0 + (f1 - f0) * std::min(1.0f, u / 0.05f);
            return std::sin(TAU * f * u) * std::exp(-u * 30.0f) * atk(u, 0.0008f);
        };
        return blip(t, 1500.0f, 2500.0f) + 0.8f * blip(t - 0.035f, 1900.0f, 3100.0f) + 0.05f * std::sin(TAU * 5200.0f * t) * std::exp(-t * 60.0f);
    });
}

Buf makeNukeThrow() {
    float ph = 0.0f, ph2 = 0.0f;  Noise n(14);  LP lp;
    return gen(0.4f, [&](float t) {
        ph  += TAU * (70.0f + 95.0f * std::exp(-t * 22.0f)) / SR;                   // the dull thunk of it leaving
        ph2 += TAU * (280.0f + 900.0f * t) / SR;                                    // and a rising whine as it arms
        const float thunk = std::sin(ph) * std::exp(-t * 14.0f) + lp.run(n.w(), 500.0f) * std::exp(-t * 30.0f) * 0.6f;
        const float arm = std::sin(ph2) * (t > 0.08f ? std::exp(-(t - 0.08f) * 7.0f) * 0.32f : 0.0f);
        return (thunk + arm) * atk(t, 0.001f);
    });
}

Buf makeNukeBeep() {
    return gen(0.11f, [](float t) {
        const float p = TAU * 1568.0f * t;
        return std::tanh(1.7f * (std::sin(p) + 0.32f * std::sin(2.0f * p))) * std::exp(-t * 34.0f) * atk(t, 0.002f);
    });
}

Buf makeNukeBoom() {
    Buf b = explosion(3.6f, 1.05f, 2600.0f, 1.5f, 55.0f, 78.0f, 22.0f, 1.9f, 1.5f, 70, 0.10f, 101, 1.9f);
    // a second, lower layer of rumble that lingers
    Noise n(102);  LP a, c;
    Buf rum = gen(3.6f, [&](float t) {
        const float body = c.run(a.run(n.w(), 130.0f + 70.0f * std::exp(-t * 1.2f)), 200.0f);
        return body * std::exp(-t * 0.9f) * atk(t, 0.05f) * 3.0f;
    });
    addTo(b, rum, 0.0f, 0.7f);
    return b;
}

// -------------------------------------------------------------- explosions --
Buf makeExplodeS() { return explosion(0.55f, 8.5f, 5200.0f, 10.0f, 200.0f, 150.0f, 46.0f, 13.0f, 0.9f, 4, 0.10f, 21, 1.5f); }
Buf makeExplodeM() { return explosion(1.0f,  5.0f, 4200.0f,  6.0f, 140.0f, 120.0f, 38.0f,  9.0f, 1.1f, 9, 0.11f, 22, 1.7f); }
Buf makeExplodeL() { return explosion(1.8f,  2.7f, 3600.0f,  3.6f,  90.0f,  95.0f, 30.0f,  5.0f, 1.3f, 18, 0.12f, 23, 1.8f); }

// ---------------------------------------------------------------- the enemy --
Buf makeEnemyShot() {
    float p1 = 0.0f, p2 = 0.0f;  LP lp;
    return gen(0.17f, [&](float t) {
        const float f = 250.0f + 780.0f * std::exp(-t * 20.0f);                     // lower and buzzier than yours
        p1 += TAU * f / SR;  p2 += TAU * f * 1.012f / SR;
        const float x = lp.run(saw(p1) * 0.6f + saw(p2) * 0.6f, 2300.0f);
        return x * std::exp(-t * 22.0f) * atk(t, 0.001f);
    });
}

Buf makeMissileLaunch() {
    Noise n(31);  SVF f;  float ph = 0.0f, ph2 = 0.0f;
    return gen(0.75f, [&](float t) {
        f.run(n.w(), 350.0f + 2100.0f * (1.0f - std::exp(-t * 3.5f)), 0.45f);
        ph  += TAU * (260.0f + 900.0f * (1.0f - std::exp(-t * 4.0f))) / SR;
        ph2 += TAU * (95.0f * std::exp(-t * 6.0f) + 42.0f) / SR;
        const float env = (1.0f - std::exp(-t * 40.0f)) * std::exp(-t * 3.2f);
        return f.band * env * 2.2f + std::sin(ph) * env * 0.26f + std::sin(ph2) * std::exp(-t * 12.0f) * 0.6f;
    });
}

Buf makeMissileWarn() {
    return gen(0.16f, [](float t) {
        const float on = (t < 0.055f || (t > 0.09f && t < 0.145f)) ? 1.0f : 0.0f;     // bi-bip
        const float local = t < 0.09f ? t : t - 0.09f;
        return std::tanh(2.4f * std::sin(TAU * 1240.0f * t)) * on * atk(local, 0.003f) * std::exp(-local * 9.0f);
    });
}

Buf makeEnemyHit() {
    Noise n(33);
    return gen(0.13f, [&](float t) {
        const float x = std::sin(TAU * 1830.0f * t) * std::exp(-t * 42.0f)
                      + std::sin(TAU * 2740.0f * t) * std::exp(-t * 55.0f) * 0.7f
                      + std::sin(TAU * 3910.0f * t) * std::exp(-t * 72.0f) * 0.5f;
        return (x + (t < 0.002f ? n.w() : 0.0f) * 0.6f) * atk(t, 0.0005f);
    });
}

// ------------------------------------------------------ rocks and spaceman --
Buf makeRockHit() {
    Noise n(41);  SVF f;
    return gen(0.08f, [&](float t) {
        f.run(n.w(), 2500.0f + 900.0f * n.w(), 0.9f);
        return (f.band * 2.2f * std::exp(-t * 68.0f) + std::sin(TAU * 205.0f * t) * std::exp(-t * 60.0f) * 0.4f) * atk(t, 0.0005f);
    });
}

Buf makeRockSplit() {
    Noise n(42), c(43);  LP a, b;  float ph = 0.0f;
    return gen(0.6f, [&](float t) {
        ph += TAU * (85.0f + 190.0f * std::exp(-t * 10.0f)) / SR;
        const float crack = (c.w() - 0.4f * a.run(c.w(), 3000.0f)) * std::exp(-t * 46.0f) * 0.7f;
        const float rumble = b.run(n.w(), 450.0f * std::exp(-t * 4.0f) + 90.0f) * std::exp(-t * 6.0f) * 3.0f;
        return (crack + rumble + std::sin(ph) * std::exp(-t * 9.0f) * 0.55f) * atk(t, 0.001f);
    });
}

Buf makeJump() {
    Noise n(44);  SVF f;  float ph = 0.0f;
    return gen(0.22f, [&](float t) {
        const float u = std::min(1.0f, t / 0.16f);
        ph += TAU * (210.0f + 440.0f * u * u) / SR;                                 // a soft upward chirp
        f.run(n.w(), 1500.0f, 0.8f);
        return (std::sin(ph) * std::exp(-t * 11.0f) * 0.9f + f.band * std::exp(-t * 18.0f) * 0.7f) * atk(t, 0.005f);
    });
}

Buf makeLand() {
    Noise n(45);  LP lp;  float ph = 0.0f;
    return gen(0.16f, [&](float t) {
        ph += TAU * (55.0f + 70.0f * std::exp(-t * 30.0f)) / SR;
        return (std::sin(ph) * std::exp(-t * 22.0f) + lp.run(n.w(), 900.0f) * std::exp(-t * 55.0f) * 0.7f) * atk(t, 0.001f);
    });
}

Buf makeFuelEmpty() {
    return gen(0.16f, [](float t) {
        float x = 0.0f;
        for (int k = 0; k < 2; ++k) {                                               // a dry double click
            const float u = t - 0.075f * k;
            if (u < 0.0f) continue;
            x += std::tanh(2.0f * std::sin(TAU * (170.0f - 30.0f * k) * u)) * std::exp(-u * 60.0f) * atk(u, 0.0008f);
        }
        return x;
    });
}

Buf makeHurt() {
    Noise n(46);  float ph = 0.0f;  LP lp;
    return gen(0.4f, [&](float t) {
        ph += TAU * (90.0f + 540.0f * std::exp(-t * 8.0f)) / SR;                    // a harsh falling zap
        const float zap = std::tanh(3.0f * (saw(ph) * 0.5f + std::sin(ph) * 0.5f));
        const float crackle = lp.run(n.w(), 5000.0f) * ((n.u() < 0.3f) ? 1.0f : 0.0f) * 0.5f;
        return (zap * std::exp(-t * 9.0f) + crackle * std::exp(-t * 12.0f)) * atk(t, 0.001f);
    });
}

// ------------------------------------------------------------------ the run --
Buf makeDeath() {
    Buf b = explosion(2.4f, 2.0f, 3400.0f, 3.0f, 80.0f, 100.0f, 30.0f, 4.0f, 1.3f, 30, 0.12f, 51, 1.8f);
    float ph = 0.0f;  Noise n(52);
    Buf glide = gen(2.4f, [&](float t) {
        ph += TAU * (40.0f + 1050.0f * std::exp(-t * 2.4f)) / SR;                   // the suit's power draining away
        const float trem = 0.6f + 0.4f * std::sin(TAU * (30.0f - 18.0f * t) * t);
        return std::sin(ph) * std::exp(-t * 1.5f) * atk(t, 0.01f) * trem + n.w() * 0.05f * std::exp(-t * 2.0f);
    });
    addTo(b, glide, 0.0f, 0.55f);
    return b;
}

Buf makeGameOver() {
    // three notes stepping down, each a long soft tone, over a low drone
    static const float notes[3] = { 220.0f, 174.61f, 146.83f };
    Buf b((size_t)(3.4f * SR), 0.0f);
    for (int k = 0; k < 3; ++k) {
        const float f = notes[k];
        Buf t = gen(2.2f, [&](float u) {
            return (std::sin(TAU * f * u) + 0.4f * tri(TAU * f * 2.003f * u) + 0.2f * std::sin(TAU * f * 3.0f * u))
                   * std::exp(-u * 1.6f) * atk(u, 0.02f);
        });
        addTo(b, t, 0.62f * k, 1.0f);
    }
    Buf drone = gen(3.4f, [](float t) {
        return (std::sin(TAU * 73.42f * t) + 0.5f * std::sin(TAU * 110.0f * t)) * atk(t, 0.9f) * std::exp(-std::max(0.0f, t - 1.6f) * 1.2f);
    });
    addTo(b, drone, 0.0f, 0.5f);
    return b;
}

Buf makeRespawn() {
    static const float f[3] = { 523.25f, 783.99f, 1046.5f };
    Buf b((size_t)(0.95f * SR), 0.0f);
    for (int k = 0; k < 3; ++k) addTo(b, bell(f[k], 0.7f, 5.0f, false), 0.09f * k, 0.8f);
    Noise n(53);  sparkle(b, n, 8, 0.05f, 0.6f, 3000.0f, 7000.0f, 0.06f, 40.0f);
    return b;
}

Buf makeLivesRestored() {
    static const float f[3] = { 659.25f, 830.61f, 987.77f };
    Buf b((size_t)(1.1f * SR), 0.0f);
    for (int k = 0; k < 3; ++k) addTo(b, bell(f[k], 0.9f, 4.0f, true), 0.13f * k, 0.7f);
    Noise n(54);  sparkle(b, n, 10, 0.1f, 0.9f, 3500.0f, 8000.0f, 0.05f, 38.0f);
    return b;
}

Buf makeLevelStart() {
    Noise n(55);  SVF f;  float ph = 0.0f;
    Buf b = gen(1.4f, [&](float t) {
        ph += TAU * (62.0f + 60.0f * std::exp(-t * 6.0f)) / SR;
        f.run(n.w(), 3200.0f * std::exp(-t * 2.4f) + 350.0f, 0.55f);                // a wash of air falling away
        return std::sin(ph) * std::exp(-t * 4.5f) * 1.1f + f.band * std::exp(-t * 3.2f) * atk(t, 0.02f) * 1.2f;
    });
    addTo(b, bell(660.0f, 1.2f, 3.4f, false), 0.02f, 0.35f);
    addTo(b, bell(990.0f, 1.0f, 4.2f, false), 0.02f, 0.22f);
    return b;
}

Buf makeLevelComplete() {
    static const float f[5] = { 523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f };      // C E G C E, rising
    Buf b((size_t)(2.2f * SR), 0.0f);
    for (int k = 0; k < 5; ++k) addTo(b, bell(f[k], 1.4f, 3.0f, false), 0.10f * k, 0.6f);
    addTo(b, gen(1.6f, [](float t) { return std::sin(TAU * 130.81f * t) * atk(t, 0.04f) * std::exp(-t * 2.0f); }), 0.2f, 0.5f);
    Noise n(56);  sparkle(b, n, 18, 0.2f, 1.6f, 3000.0f, 8500.0f, 0.06f, 34.0f);
    return b;
}

Buf makeBeaconPing() {
    return gen(1.0f, [](float t) {
        auto one = [](float u) {
            if (u < 0.0f) return 0.0f;
            return (std::sin(TAU * 987.77f * u) + 0.25f * std::sin(TAU * 1975.5f * u) * std::exp(-u * 4.0f))
                   * atk(u, 0.012f) * std::exp(-u * 5.0f);
        };
        return one(t) + 0.34f * one(t - 0.21f) + 0.12f * one(t - 0.42f);              // a sonar ping and its echoes
    });
}

Buf makeClockTick() {
    Noise n(57);
    return gen(0.08f, [&](float t) {
        return (std::sin(TAU * 1400.0f * t) * std::exp(-t * 85.0f) + (t < 0.0015f ? n.w() * 0.5f : 0.0f)) * atk(t, 0.0005f);
    });
}

Buf makeClockTickHi() {
    Noise n(58);
    return gen(0.1f, [&](float t) {
        const float p = TAU * 2100.0f * t;
        return (std::tanh(1.5f * std::sin(p)) * std::exp(-t * 70.0f) + (t < 0.0015f ? n.w() * 0.5f : 0.0f)) * atk(t, 0.0005f);
    });
}

// ---------------------------------------------------------------- the depot --
Buf makeShopOpen() {
    Noise n(61);  SVF f;
    Buf b = gen(1.2f, [&](float t) {
        f.run(n.w(), 700.0f + 2200.0f * std::min(1.0f, t / 0.6f), 0.7f);
        return f.band * std::sin(3.14159f * std::min(1.0f, t / 1.1f)) * 0.9f;         // an airy swell...
    });
    addTo(b, bell(659.25f, 1.0f, 3.5f, false), 0.25f, 0.55f);                        // ...that settles on a chord
    addTo(b, bell(987.77f, 1.0f, 3.8f, false), 0.31f, 0.4f);
    return b;
}

Buf makeShopHover() {
    return gen(0.06f, [](float t) { return (std::sin(TAU * 2200.0f * t) + 0.3f * std::sin(TAU * 4400.0f * t)) * std::exp(-t * 120.0f) * atk(t, 0.0005f); });
}

Buf makeShopBuy() {
    Buf b((size_t)(0.7f * SR), 0.0f);
    addTo(b, bell(1318.5f, 0.6f, 6.0f, true), 0.0f, 0.8f);
    addTo(b, bell(1975.5f, 0.6f, 6.0f, true), 0.075f, 0.9f);
    Noise n(62);  sparkle(b, n, 6, 0.06f, 0.4f, 4000.0f, 9000.0f, 0.06f, 44.0f);
    return b;
}

Buf makeShopDeny() {
    float p = 0.0f;  LP lp;
    return gen(0.26f, [&](float t) {
        p += TAU * 98.0f / SR;
        const float on = (t < 0.09f || (t > 0.13f && t < 0.22f)) ? 1.0f : 0.0f;
        return lp.run(std::tanh(2.0f * saw(p)), 900.0f) * on * atk(std::fmod(t, 0.13f), 0.004f);
    });
}

Buf makeShopContinue() {
    Noise n(63);  SVF f;  float ph = 0.0f;
    Buf b = gen(0.7f, [&](float t) {
        ph += TAU * (260.0f + 1500.0f * std::min(1.0f, t / 0.28f)) / SR;
        f.run(n.w(), 900.0f + 3000.0f * std::min(1.0f, t / 0.3f), 0.6f);
        const float env = std::exp(-std::max(0.0f, t - 0.22f) * 12.0f) * atk(t, 0.01f);
        return (std::sin(ph) * 0.7f + f.band * 0.8f) * env * (t < 0.3f ? 1.0f : 0.0f);
    });
    addTo(b, bell(1568.0f, 0.5f, 7.0f, false), 0.24f, 0.7f);
    return b;
}

Buf makePause() {
    float ph = 0.0f;
    return gen(0.14f, [&](float t) {
        ph += TAU * (880.0f - 260.0f * std::min(1.0f, t / 0.1f)) / SR;
        return std::sin(ph) * std::exp(-t * 26.0f) * atk(t, 0.002f);
    });
}

// -------------------------------------------------- force field and shield --
Buf makeFieldOn() {
    Noise n(71);  SVF hp;  float ph = 0.0f;
    return gen(0.65f, [&](float t) {
        const float u = std::min(1.0f, t / 0.45f);
        ph += TAU * (110.0f * std::pow(6.6f, u)) / SR;                              // rises through three octaves
        hp.run(n.w(), 5000.0f, 0.9f);
        const float shimmer = 0.5f + 0.5f * std::sin(TAU * 22.0f * t);
        const float env = std::sin(3.14159f * std::min(1.0f, t / 0.62f) * 0.5f + 0.0f) * std::exp(-std::max(0.0f, t - 0.4f) * 9.0f);
        return (std::sin(ph) * 0.8f + std::sin(ph * 2.0f) * 0.25f * shimmer + hp.high * 0.10f * u) * env;
    });
}

Buf makeFieldOff() {
    float ph = 0.0f;  Noise n(72);  SVF hp;
    return gen(0.5f, [&](float t) {
        const float u = std::min(1.0f, t / 0.4f);
        ph += TAU * (110.0f * std::pow(6.0f, 1.0f - u)) / SR;
        hp.run(n.w(), 5000.0f, 0.9f);
        return (std::sin(ph) * 0.8f + hp.high * 0.08f * (1.0f - u)) * std::exp(-t * 6.0f) * atk(t, 0.005f);
    });
}

Buf makeFieldEmpty() {
    Noise n(73);  SVF hp;  float ph = 0.0f;
    return gen(0.4f, [&](float t) {
        hp.run(n.w(), 3500.0f, 0.8f);
        ph += TAU * (420.0f * std::exp(-t * 7.0f) + 70.0f) / SR;
        const float gate = n.u() < (0.75f - t) ? 1.0f : 0.0f;                        // it sputters out
        return (hp.high * 0.5f * gate + std::sin(ph) * 0.5f) * std::exp(-t * 6.0f) * atk(t, 0.002f);
    });
}

Buf makeShieldUp() {
    Noise n(74);
    return gen(0.26f, [&](float t) {
        const float glide = 1.0f + 0.12f * (1.0f - std::exp(-t * 40.0f));            // a bright metallic shing
        const float x = std::sin(TAU * 1750.0f * glide * t) + 0.7f * std::sin(TAU * 2620.0f * glide * t)
                      + 0.4f * std::sin(TAU * 4130.0f * t) * std::exp(-t * 30.0f);
        return (x * std::exp(-t * 15.0f) + (t < 0.002f ? n.w() : 0.0f) * 0.5f) * atk(t, 0.002f);
    });
}

Buf makeShieldBlock() {
    Noise n(75);
    return gen(0.32f, [&](float t) {
        static const float fr[4] = { 1120.0f, 1735.0f, 2490.0f, 3620.0f };
        static const float dc[4] = { 14.0f, 18.0f, 24.0f, 32.0f };
        float x = 0.0f;
        for (int k = 0; k < 4; ++k) x += std::sin(TAU * fr[k] * t) * std::exp(-t * dc[k]) / (1.0f + 0.4f * k);
        return (x + (t < 0.003f ? n.w() * 0.8f : 0.0f)) * atk(t, 0.0004f);
    });
}

Buf makeShieldBreak() {
    Noise n(76), s(77);  SVF hp;
    Buf b = gen(0.7f, [&](float t) {
        hp.run(n.w(), 4200.0f, 0.7f);
        const float fall = 1.0f - 0.7f * std::min(1.0f, t / 0.5f);                    // glass failing, pitch sagging
        const float x = std::sin(TAU * 2300.0f * fall * t) * std::exp(-t * 9.0f) * 0.5f
                      + std::sin(TAU * 3700.0f * fall * t) * std::exp(-t * 12.0f) * 0.35f
                      + hp.high * std::exp(-t * 14.0f) * 0.7f
                      + std::sin(TAU * 90.0f * t) * std::exp(-t * 16.0f) * 0.7f;
        return x * atk(t, 0.001f);
    });
    sparkle(b, s, 14, 0.02f, 0.5f, 3000.0f, 9000.0f, 0.09f, 40.0f);
    return b;
}

// ---------------------------------------------------------------- warships --
Buf makeShipAlert() {
    // a low, brassy horn: three saws a tritone apart under a slowly opening filter
    float p1 = 0.0f, p2 = 0.0f, p3 = 0.0f, vib = 0.0f;  LP lp, lp2;
    return gen(3.0f, [&](float t) {
        vib += TAU * 4.5f / SR;
        const float v = 1.0f + 0.006f * std::sin(vib);
        p1 += TAU * 55.0f * v / SR;  p2 += TAU * 77.8f * v / SR;  p3 += TAU * 110.0f * 1.004f * v / SR;
        const float fc = 240.0f + 800.0f * std::sin(3.14159f * std::min(1.0f, t / 2.4f) * 0.5f);
        const float x = lp2.run(lp.run(saw(p1) + 0.7f * saw(p2) + 0.6f * saw(p3), fc), fc * 1.2f);
        const float env = atk(t, 0.55f) * std::exp(-std::max(0.0f, t - 1.6f) * 1.5f);
        return (x * 1.6f + std::sin(p1) * 0.7f) * env;
    });
}

Buf makeShipHit() {
    Noise n(81);  LP lp;
    return gen(0.42f, [&](float t) {
        static const float fr[4] = { 318.0f, 489.0f, 762.0f, 1180.0f };
        static const float dc[4] = { 11.0f, 15.0f, 22.0f, 30.0f };
        float x = 0.0f;
        for (int k = 0; k < 4; ++k) x += std::sin(TAU * fr[k] * t) * std::exp(-t * dc[k]) / (1.0f + 0.5f * k);
        return (x + lp.run(n.w(), 700.0f) * std::exp(-t * 40.0f) * 0.8f) * atk(t, 0.0008f);
    });
}

Buf makeShipDeath() {
    // a chain of blasts running down the hull, then the whole ship going, and a long groan
    Buf b((size_t)(4.6f * SR), 0.0f);
    static const float when[6] = { 0.0f, 0.34f, 0.66f, 1.05f, 1.45f, 1.95f };
    for (int k = 0; k < 5; ++k)
        addTo(b, explosion(1.0f + 0.1f * k, 5.0f - 0.3f * k, 4200.0f, 6.0f, 140.0f, 125.0f - 8.0f * k, 40.0f, 9.0f, 1.0f, 8, 0.10f, 300u + k, 1.7f), when[k], 0.8f);
    addTo(b, explosion(2.6f, 1.9f, 3800.0f, 2.6f, 70.0f, 90.0f, 26.0f, 3.5f, 1.4f, 34, 0.12f, 399, 1.9f), when[5], 1.2f);
    float ph = 0.0f;
    addTo(b, gen(4.6f, [&](float t) {
        ph += TAU * (28.0f + 65.0f * std::exp(-t * 1.0f)) / SR;                     // metal groaning as it breaks
        return std::sin(ph) * atk(t, 0.3f) * std::exp(-t * 0.7f) * (0.75f + 0.25f * std::sin(TAU * 7.0f * t));
    }), 0.0f, 0.6f);
    return b;
}

Buf makeCannonCharge() {
    float ph = 0.0f;  Noise n(82);  SVF hp;
    return gen(0.75f, [&](float t) {
        const float u = t / 0.75f;
        ph += TAU * (150.0f * std::pow(10.0f, u)) / SR;                             // a whine climbing to the shot
        hp.run(n.w(), 4500.0f, 0.9f);
        const float trem = 0.7f + 0.3f * std::sin(TAU * (16.0f + 34.0f * u) * t);
        return (std::sin(ph) * 0.8f + std::sin(ph * 2.01f) * 0.25f + hp.high * 0.10f * u) * trem * (0.15f + 0.85f * u) * atk(t, 0.02f);
    });
}

Buf makeCannonFire() {
    Buf b = explosion(0.9f, 4.4f, 3000.0f, 8.0f, 110.0f, 112.0f, 34.0f, 14.0f, 1.5f, 7, 0.10f, 91, 2.0f);
    float ph = 0.0f;  LP lp;
    addTo(b, gen(0.5f, [&](float t) {
        ph += TAU * 95.0f / SR;
        return lp.run(saw(ph), 700.0f) * std::exp(-t * 7.0f) * atk(t, 0.002f);      // a heavy buzz under the boom
    }), 0.0f, 0.6f);
    return b;
}

Buf makeFlakFire() {
    Noise n(83);  LP lp;  float ph = 0.0f;
    Buf b = gen(0.4f, [&](float t) {
        ph += TAU * (210.0f * std::exp(-t * 14.0f) + 70.0f) / SR;
        return (lp.run(n.w(), 2600.0f * std::exp(-t * 12.0f) + 300.0f) * std::exp(-t * 20.0f) * 2.4f
                + std::tanh(2.0f * saw(ph)) * std::exp(-t * 26.0f) * 0.6f) * atk(t, 0.001f);
    });
    Noise s(84);  sparkle(b, s, 5, 0.01f, 0.18f, 2500.0f, 6000.0f, 0.10f, 60.0f);      // the pellets flying out
    return b;
}

// -------------------------------------------------------------------- loops --
Buf makeThruster() {
    // Two seconds that meet themselves: 2.3 s of source with 0.3 s crossfaded over the join.
    Noise n(91), m(92);  LP rum, mod;  SVF f;
    Buf raw = gen(2.3f, [&](float t) {
        const float rumble = rum.run(n.w(), 420.0f) * 2.4f;
        f.run(n.w(), 1700.0f, 0.55f);
        const float wob = 0.75f + 0.25f * std::tanh(mod.run(m.w(), 3.5f) * 14.0f);
        const float tone = std::sin(TAU * 68.0f * t) * (0.55f + 0.45f * std::sin(TAU * 26.0f * t)) * 0.30f;
        return rumble + f.band * 0.45f * wob + tone;
    });
    return loopify(raw, (size_t)(0.3f * SR));
}

Buf makeFieldHum() {
    return gen(1.0f, [](float t) {                                                  // whole cycles per second: it loops exactly
        const float shimmer = 0.5f + 0.5f * std::sin(TAU * 7.0f * t);
        const float x = std::sin(TAU * 110.0f * t) * 0.5f + std::sin(TAU * 165.0f * t) * 0.3f + std::sin(TAU * 220.0f * t) * 0.2f
                      + std::sin(TAU * 880.0f * t) * 0.08f * shimmer + std::sin(TAU * 1320.0f * t) * 0.05f * (1.0f - shimmer);
        return x * (0.85f + 0.15f * std::sin(TAU * 3.0f * t));
    });
}

Buf makeShieldHum() {
    return gen(1.0f, [](float t) {
        const float x = std::sin(TAU * 196.0f * t) * 0.4f + std::sin(TAU * 294.0f * t) * 0.2f
                      + std::sin(TAU * 392.0f * t) * 0.15f + std::sin(TAU * 1176.0f * t) * 0.04f;
        return x * (0.7f + 0.3f * std::sin(TAU * 12.0f * t));
    });
}

Buf makeHeartbeat() {
    Noise n(93);  LP lp;
    return gen(1.0f, [&](float t) {
        auto thump = [](float u, float f, float a) {
            if (u < 0.0f) return 0.0f;
            return std::sin(TAU * f * u) * std::exp(-u * 20.0f) * atk(u, 0.006f) * a;
        };
        return thump(t, 52.0f, 1.0f) + thump(t - 0.27f, 46.0f, 0.7f) + lp.run(n.w(), 160.0f) * (t < 0.05f ? std::exp(-t * 60.0f) * 0.3f : 0.0f);
    });
}

Buf makeAmbient() {
    // Eight seconds of a slow, low chord. Every frequency is a multiple of 1/8 Hz so
    // the tones loop without a seam; the airy noise is crossfaded.
    Noise n(94);  SVF f;
    Buf raw = gen(9.0f, [&](float t) {
        const float swell1 = 0.65f + 0.35f * std::sin(TAU * 0.125f * t);
        const float swell2 = 0.65f + 0.35f * std::sin(TAU * 0.25f * t + 1.3f);
        const float x = std::sin(TAU * 55.0f * t) * 0.5f + std::sin(TAU * 55.125f * t) * 0.4f
                      + std::sin(TAU * 82.5f * t) * 0.32f * swell1 + std::sin(TAU * 110.25f * t) * 0.22f * swell2
                      + std::sin(TAU * 165.0f * t) * 0.09f * swell1;
        f.run(n.w(), 1800.0f + 700.0f * std::sin(TAU * 0.125f * t), 0.6f);
        return x + f.band * 0.05f * swell2;
    });
    return loopify(raw, (size_t)(1.0f * SR));
}

// ----------------------------------------------------------------- the table --
struct Entry { const char* name; float peak; int voices; float gap; float wet; Buf (*make)(); };

const Entry SFX[] = {
    { "rifle",          0.42f, 5, 0.000f, 0.10f, makeRifle },
    { "shell",          0.65f, 3, 0.000f, 0.22f, makeShell },
    { "salvo",          0.55f, 2, 0.100f, 0.25f, makeSalvo },
    { "fractal-fire",   0.55f, 2, 0.150f, 0.40f, makeFractalFire },
    { "fractal-split",  0.32f, 6, 0.030f, 0.35f, makeFractalSplit },
    { "nuke-throw",     0.50f, 2, 0.000f, 0.15f, makeNukeThrow },
    { "nuke-beep",      0.45f, 3, 0.000f, 0.10f, makeNukeBeep },
    { "nuke-boom",      1.00f, 1, 0.000f, 0.60f, makeNukeBoom },
    { "explode-s",      0.60f, 6, 0.030f, 0.25f, makeExplodeS },
    { "explode-m",      0.80f, 4, 0.050f, 0.35f, makeExplodeM },
    { "explode-l",      0.95f, 3, 0.100f, 0.50f, makeExplodeL },
    { "enemy-shot",     0.30f, 6, 0.000f, 0.12f, makeEnemyShot },
    { "missile-launch", 0.50f, 3, 0.050f, 0.25f, makeMissileLaunch },
    { "missile-warn",   0.40f, 1, 0.300f, 0.05f, makeMissileWarn },
    { "enemy-hit",      0.30f, 4, 0.050f, 0.10f, makeEnemyHit },
    { "rock-hit",       0.22f, 4, 0.045f, 0.10f, makeRockHit },
    { "rock-split",     0.50f, 3, 0.080f, 0.30f, makeRockSplit },
    { "jump",           0.30f, 1, 0.100f, 0.10f, makeJump },
    { "land",           0.35f, 1, 0.100f, 0.08f, makeLand },
    { "fuel-empty",     0.30f, 1, 0.200f, 0.05f, makeFuelEmpty },
    { "hurt",           0.60f, 2, 0.100f, 0.15f, makeHurt },
    { "death",          0.95f, 1, 0.000f, 0.50f, makeDeath },
    { "game-over",      0.70f, 1, 0.000f, 0.70f, makeGameOver },
    { "respawn",        0.50f, 1, 0.000f, 0.50f, makeRespawn },
    { "lives-restored", 0.50f, 1, 0.000f, 0.50f, makeLivesRestored },
    { "level-start",    0.60f, 1, 0.000f, 0.40f, makeLevelStart },
    { "level-complete", 0.60f, 1, 0.000f, 0.55f, makeLevelComplete },
    { "beacon-ping",    0.30f, 2, 0.400f, 0.60f, makeBeaconPing },
    { "clock-tick",     0.35f, 1, 0.100f, 0.05f, makeClockTick },
    { "clock-tick-hi",  0.40f, 1, 0.100f, 0.05f, makeClockTickHi },
    { "shop-open",      0.45f, 1, 0.000f, 0.60f, makeShopOpen },
    { "shop-hover",     0.18f, 2, 0.030f, 0.15f, makeShopHover },
    { "shop-buy",       0.50f, 2, 0.050f, 0.40f, makeShopBuy },
    { "shop-deny",      0.40f, 1, 0.100f, 0.05f, makeShopDeny },
    { "shop-continue",  0.50f, 1, 0.000f, 0.40f, makeShopContinue },
    { "pause",          0.30f, 1, 0.100f, 0.10f, makePause },
    { "field-on",       0.50f, 1, 0.200f, 0.30f, makeFieldOn },
    { "field-off",      0.45f, 1, 0.200f, 0.30f, makeFieldOff },
    { "field-empty",    0.40f, 1, 0.300f, 0.20f, makeFieldEmpty },
    { "shield-up",      0.38f, 1, 0.150f, 0.20f, makeShieldUp },
    { "shield-block",   0.50f, 3, 0.050f, 0.25f, makeShieldBlock },
    { "shield-break",   0.60f, 1, 0.300f, 0.35f, makeShieldBreak },
    { "ship-alert",     0.60f, 1, 0.000f, 0.60f, makeShipAlert },
    { "ship-hit",       0.45f, 3, 0.100f, 0.35f, makeShipHit },
    { "ship-death",     1.00f, 1, 0.000f, 0.60f, makeShipDeath },
    { "cannon-charge",  0.40f, 3, 0.000f, 0.15f, makeCannonCharge },
    { "cannon-fire",    0.80f, 3, 0.050f, 0.40f, makeCannonFire },
    { "flak-fire",      0.50f, 4, 0.050f, 0.20f, makeFlakFire },
};
static_assert(sizeof(SFX) / sizeof(SFX[0]) == (size_t)Sfx::Count, "the sound table must match the Sfx list");

const Entry LOOPS[] = {
    { "thruster",  0.22f, 1, 0.0f, 0.12f, makeThruster },
    { "field-hum", 0.30f, 1, 0.0f, 0.20f, makeFieldHum },
    { "shield-hum",0.25f, 1, 0.0f, 0.15f, makeShieldHum },
    { "heartbeat", 0.55f, 1, 0.0f, 0.05f, makeHeartbeat },
    { "ambient",   0.25f, 1, 0.0f, 0.60f, makeAmbient },
};
static_assert(sizeof(LOOPS) / sizeof(LOOPS[0]) == (size_t)Sustain::Count, "the loop table must match the Sustain list");

// Takes the DC offset and any slow drift out, softens the very first and last
// samples so nothing clicks, and sets the peak level.
void finish(audio::Sound& snd, const Entry& e, bool isLoop) {
    Buf& b = snd.s;
    if (!isLoop) {
        float y = 0.0f, x1 = 0.0f;                      // a one-pole high-pass at ~25 Hz
        const float a = 1.0f - TAU * 25.0f / SR;
        for (float& v : b) { const float x = v; y = a * (y + x - x1); x1 = x; v = y; }
        // Every sound is brought to rest with a soft half-cosine over its last 40 ms
        // (the cannon's wind-up is the exception: it is cut off as the shot goes).
        const bool abrupt = std::strcmp(e.name, "cannon-charge") == 0;
        const size_t fi = std::min<size_t>(b.size() / 2, 24);
        const size_t fo = std::min<size_t>(b.size() / 2, (size_t)((abrupt ? 0.004f : 0.04f) * SR));
        for (size_t i = 0; i < fi; ++i) b[i] *= (float)i / fi;
        for (size_t i = 0; i < fo; ++i) b[b.size() - 1 - i] *= 0.5f - 0.5f * std::cos(3.14159265f * (float)i / (float)fo);
    }
    float peak = 1e-9f;
    for (float v : b) peak = std::max(peak, std::fabs(v));
    const float g = e.peak / peak;
    for (float& v : b) v *= g;
    snd.info = { e.name, e.peak, e.voices, e.gap, e.wet };
}

}  // namespace

namespace audio {

void buildBank(Bank& out) {
    out.sfx.clear();  out.loops.clear();
    for (const Entry& e : SFX)   { Sound s; s.s = e.make(); finish(s, e, false); out.sfx.push_back(std::move(s)); }
    for (const Entry& e : LOOPS) { Sound s; s.s = e.make(); finish(s, e, true);  out.loops.push_back(std::move(s)); }
}

}  // namespace audio
