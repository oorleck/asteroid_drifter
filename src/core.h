// core.h -- small math + rng helpers shared by every system.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

static const float PIF  = 3.14159265358979323846f;
static const float TAUF = 6.28318530717958647692f;

// ---------------------------------------------------------------- vectors --
struct v2 {
    float x, y;
    v2() : x(0), y(0) {}
    v2(float X, float Y) : x(X), y(Y) {}
};
inline v2 operator+(v2 a, v2 b)   { return v2(a.x + b.x, a.y + b.y); }
inline v2 operator-(v2 a, v2 b)   { return v2(a.x - b.x, a.y - b.y); }
inline v2 operator-(v2 a)         { return v2(-a.x, -a.y); }
inline v2 operator*(v2 a, float s){ return v2(a.x * s, a.y * s); }
inline v2 operator*(float s, v2 a){ return v2(a.x * s, a.y * s); }
inline v2 operator/(v2 a, float s){ return v2(a.x / s, a.y / s); }
inline v2& operator+=(v2& a, v2 b){ a.x += b.x; a.y += b.y; return a; }
inline v2& operator-=(v2& a, v2 b){ a.x -= b.x; a.y -= b.y; return a; }
inline v2& operator*=(v2& a, float s){ a.x *= s; a.y *= s; return a; }
inline float dot(v2 a, v2 b)      { return a.x * b.x + a.y * b.y; }
inline float cross(v2 a, v2 b)    { return a.x * b.y - a.y * b.x; }
inline float len2(v2 a)           { return a.x * a.x + a.y * a.y; }
inline float len(v2 a)            { return std::sqrt(len2(a)); }
inline v2 perp(v2 a)              { return v2(-a.y, a.x); }          // +90 deg
inline v2 norm(v2 a)              { float l = len(a); return l > 1e-20f ? a / l : v2(0, 0); }
inline v2 rot(v2 a, float c, float s) { return v2(a.x * c - a.y * s, a.x * s + a.y * c); }
inline v2 rot(v2 a, float ang)    { return rot(a, std::cos(ang), std::sin(ang)); }
inline v2 fromAngle(float a)      { return v2(std::cos(a), std::sin(a)); }
inline v2 lerp(v2 a, v2 b, float t){ return a + (b - a) * t; }

// Double-precision world position: the world is effectively unbounded, so
// body positions are doubles and everything is converted to camera-relative
// floats just before it reaches the GPU.
struct dv2 {
    double x, y;
    dv2() : x(0), y(0) {}
    dv2(double X, double Y) : x(X), y(Y) {}
    explicit dv2(v2 a) : x(a.x), y(a.y) {}
};
inline dv2 operator+(dv2 a, dv2 b) { return dv2(a.x + b.x, a.y + b.y); }
inline dv2 operator-(dv2 a, dv2 b) { return dv2(a.x - b.x, a.y - b.y); }
inline dv2 operator+(dv2 a, v2 b)  { return dv2(a.x + b.x, a.y + b.y); }
inline dv2 operator-(dv2 a, v2 b)  { return dv2(a.x - b.x, a.y - b.y); }
inline dv2 operator*(dv2 a, double s){ return dv2(a.x * s, a.y * s); }
inline dv2& operator+=(dv2& a, v2 b) { a.x += b.x; a.y += b.y; return a; }
inline dv2& operator+=(dv2& a, dv2 b){ a.x += b.x; a.y += b.y; return a; }
inline v2  tov2(dv2 a)             { return v2((float)a.x, (float)a.y); }
inline double len2(dv2 a)          { return a.x * a.x + a.y * a.y; }
inline double len(dv2 a)           { return std::sqrt(len2(a)); }

// --------------------------------------------------------------- scalars --
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t)  { return a + (b - a) * t; }
inline int   clampi(int v, int lo, int hi)      { return v < lo ? lo : (v > hi ? hi : v); }
inline float sgnf(float v) { return v < 0 ? -1.0f : 1.0f; }
// Frame-rate independent exponential approach: rate = "fraction left per second".
inline float approach(float cur, float target, float rate, float dt) {
    return target + (cur - target) * std::exp(-rate * dt);
}
inline float wrapAngle(float a) {
    while (a >  PIF) a -= TAUF;
    while (a < -PIF) a += TAUF;
    return a;
}

// ------------------------------------------------------------------- rng --
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline uint64_t hashCombine(uint64_t a, uint64_t b) { return splitmix64(a ^ (b * 0x9E3779B97F4A7C15ull)); }

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x243F6A8885A308D3ull) { s = splitmix64(seed ? seed : 1); }
    uint32_t u32() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 32);
    }
    // [0,1)
    float f()                      { return (u32() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b)  { return a + (b - a) * f(); }
    float sym(float m)             { return range(-m, m); }        // [-m,m]
    int   i(int a, int b)          { return a + (int)(u32() % (uint32_t)(b - a + 1)); }
    float angle()                  { return f() * TAUF; }
    v2    dir()                    { return fromAngle(angle()); }
    // Normally distributed, mean 0 and standard deviation 1 (Box-Muller; two draws).
    float normal()                 { const float u = std::max(f(), 1e-7f), v = f(); return std::sqrt(-2.0f * std::log(u)) * std::cos(TAUF * v); }
    // Point in unit disc, uniform.
    v2    disc()                   { return dir() * std::sqrt(f()); }
};

// ------------------------------------------------------------------ color --
struct Col {
    float r, g, b, a;
    Col() : r(1), g(1), b(1), a(1) {}
    Col(float R, float G, float B, float A = 1) : r(R), g(G), b(B), a(A) {}
};
inline Col mix(Col x, Col y, float t) {
    return Col(lerpf(x.r, y.r, t), lerpf(x.g, y.g, t), lerpf(x.b, y.b, t), lerpf(x.a, y.a, t));
}
inline uint32_t packCol(Col c) {
    uint32_t R = (uint32_t)(clampf(c.r, 0, 1) * 255.0f + 0.5f);
    uint32_t G = (uint32_t)(clampf(c.g, 0, 1) * 255.0f + 0.5f);
    uint32_t B = (uint32_t)(clampf(c.b, 0, 1) * 255.0f + 0.5f);
    uint32_t A = (uint32_t)(clampf(c.a, 0, 1) * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}
