// audio_check.cpp -- the sound harnesses.
//
//   -soundcheck [-shotfile PREFIX]  analyses every synthesised sound (levels, DC,
//        clipping, silence, loop seams), exercises the mixer without a device (pan,
//        pitch, delay, voice limits, fades, reverb, mute), and writes PREFIX_sounds.png:
//        a picture of every sound, a waveform on top and a spectrogram below, in the
//        order of the list printed on the console, so they can be checked by eye.
//   -soundtest   plays every sound in turn through the real device.
#include "audio.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace audio {
namespace {

int failures = 0;
void check(bool ok, const char* what) {
    printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// ------------------------------------------------------------- a tiny PNG --
uint32_t crcTable[256];
void initCrc() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b;
    put32(b, (uint32_t)data.size());
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), data.begin(), data.end());
    const uint32_t c = crc(0, b.data() + 4, b.size() - 4);
    put32(b, c);
    fwrite(b.data(), 1, b.size(), f);
}
// Writes an uncompressed (stored) PNG: big, but it needs no compressor.
bool writePng(const char* path, int w, int h, const std::vector<uint8_t>& rgb) {
    initCrc();
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w);  put32(ihdr, (uint32_t)h);
    ihdr.push_back(8);  ihdr.push_back(2);  ihdr.push_back(0);  ihdr.push_back(0);  ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);  z.push_back(0x01);
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521u; b = (b + a) % 65521u; }
    for (size_t o = 0; o < raw.size(); o += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));  z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (std::ptrdiff_t)o, raw.begin() + (std::ptrdiff_t)(o + n));
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

// ----------------------------------------------------------------- analysis --
struct Analysis {
    float peak = 0, rms = 0, dc = 0, centroid = 0, seconds = 0, lead = 0, tail = 0, jump = 0, rmsDiff = 0;
    int   nan = 0;
};

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -6.28318530718f / (float)len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;  a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

Analysis analyse(const std::vector<float>& s) {
    Analysis r;
    r.seconds = (float)s.size() / RATE;
    double sum = 0, sq = 0, dsq = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const float v = s[i];
        if (!std::isfinite(v)) { ++r.nan; continue; }
        r.peak = std::max(r.peak, std::fabs(v));
        sum += v;  sq += (double)v * v;
        if (i) dsq += (double)(v - s[i - 1]) * (v - s[i - 1]);
    }
    const double n = (double)s.size();
    r.rms = (float)std::sqrt(sq / n);
    r.dc = (float)(sum / n);
    r.rmsDiff = (float)std::sqrt(dsq / n);
    for (size_t i = 0; i < s.size(); ++i) if (std::fabs(s[i]) > 0.02f * r.peak) { r.lead = (float)i / RATE; break; }
    const size_t t = std::min<size_t>(s.size(), RATE / 100);
    for (size_t i = s.size() - t; i < s.size(); ++i) r.tail = std::max(r.tail, std::fabs(s[i]));
    r.jump = std::fabs(s.front() - s.back());
    // spectral centroid over the loudest half second
    const size_t N = 1024;
    size_t best = 0;  double bestE = -1;
    for (size_t o = 0; o + N <= s.size(); o += N / 2) {
        double e = 0;
        for (size_t i = 0; i < N; ++i) e += (double)s[o + i] * s[o + i];
        if (e > bestE) { bestE = e; best = o; }
    }
    if (s.size() >= N) {
        std::vector<std::complex<float>> a(N);
        for (size_t i = 0; i < N; ++i) a[i] = std::complex<float>(s[best + i] * (0.5f - 0.5f * std::cos(6.28318f * (float)i / N)), 0.0f);
        fft(a);
        double num = 0, den = 0;
        for (size_t k = 1; k < N / 2; ++k) { const double m = std::abs(a[k]); num += m * k * (double)RATE / N; den += m; }
        r.centroid = den > 0 ? (float)(num / den) : 0.0f;
    }
    return r;
}

// One row of the picture: a waveform, and below it a spectrogram (log frequency).
void drawRow(std::vector<uint8_t>& img, int W, int y0, int rowH, const std::vector<float>& s, float maxSec) {
    const size_t total = std::min<size_t>(s.size(), (size_t)(maxSec * RATE));
    const int waveH = rowH / 3;
    const int specH = rowH - waveH - 2;
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= W) return;
        uint8_t* p = &img[((size_t)y * W + (size_t)x) * 3];
        p[0] = r;  p[1] = g;  p[2] = b;
    };
    const size_t N = 512;
    std::vector<std::complex<float>> a(N);
    for (int x = 0; x < W; ++x) {
        const size_t i0 = total * (size_t)x / (size_t)W, i1 = std::max(i0 + 1, total * (size_t)(x + 1) / (size_t)W);
        float lo = 0, hi = 0;
        for (size_t i = i0; i < i1 && i < s.size(); ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
        const int cy = y0 + waveH / 2;
        const int a0 = (int)(-lo * (waveH / 2 - 1)), a1 = (int)(hi * (waveH / 2 - 1));
        for (int y = -a0; y <= a1; ++y) px(x, cy - y, 120, 230, 240);
        // spectrum around this column
        const size_t c = std::min(i0, s.size() > N ? s.size() - N : 0);
        for (size_t i = 0; i < N; ++i) {
            const float v = c + i < s.size() ? s[c + i] : 0.0f;
            a[i] = std::complex<float>(v * (0.5f - 0.5f * std::cos(6.28318f * (float)i / N)), 0.0f);
        }
        fft(a);
        for (int y = 0; y < specH; ++y) {
            const float f0 = 60.0f * std::pow(11000.0f / 60.0f, (float)y / specH);
            const float f1 = 60.0f * std::pow(11000.0f / 60.0f, (float)(y + 1) / specH);
            const size_t k0 = (size_t)(f0 * N / RATE), k1 = std::max(k0 + 1, (size_t)(f1 * N / RATE));
            float m = 0;
            for (size_t k = k0; k < k1 && k < N / 2; ++k) m = std::max(m, std::abs(a[k]));
            const float db = 20.0f * std::log10(m / (N * 0.25f) + 1e-6f);      // 0 dB = a full-scale sine
            const float t = std::max(0.0f, std::min(1.0f, (db + 78.0f) / 66.0f));
            const uint8_t r = (uint8_t)(255 * std::pow(t, 2.2f)), g = (uint8_t)(255 * std::pow(t, 1.3f)), b = (uint8_t)(255 * std::sqrt(t));
            px(x, y0 + rowH - 1 - y, r, g, b);
        }
    }
    for (int x = 0; x < W; ++x) px(x, y0 + rowH - 1 + 0, 30, 30, 40);
}

// ------------------------------------------------------------ mixer helpers --
std::vector<int16_t> mix(int frames) {
    std::vector<int16_t> out((size_t)frames * 2);
    for (int done = 0; done < frames; done += 512) mixInto(out.data() + (size_t)done * 2, std::min(512, frames - done));
    return out;
}
double energy(const std::vector<int16_t>& v, int ch, size_t from = 0, size_t to = ~0ull) {
    double e = 0;
    for (size_t i = from; i < v.size() / 2 && i < to; ++i) { const double x = v[2 * i + ch] / 32768.0; e += x * x; }
    return e;
}
int peak16(const std::vector<int16_t>& v) { int p = 0; for (int16_t x : v) p = std::max(p, std::abs((int)x)); return p; }

}  // namespace

// ---------------------------------------------------------------- the check --
int check(const char* prefix) {
    failures = 0;
    printf("soundcheck:\n");

    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);  QueryPerformanceCounter(&t0);
    startOffline();
    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart;

    Bank bank;
    buildBank(bank);
    size_t samples = 0;
    for (const Sound& s : bank.sfx)   samples += s.s.size();
    for (const Sound& s : bank.loops) samples += s.s.size();
    printf("  synthesised %d sounds and %d loops: %.1f s of audio, %.1f MB, in %.0f ms\n\n",
           (int)bank.sfx.size(), (int)bank.loops.size(), (double)samples / RATE, samples * 4.0 / 1048576.0, ms);
    check(ms < 1500.0, "the whole bank is ready in under a second and a half");

    printf("  %-3s %-16s %6s %6s %6s %7s %6s %7s\n", "#", "name", "secs", "peak", "rms", "centroid", "lead", "tail");
    int bad = 0;
    std::vector<const Sound*> all;
    std::vector<bool> isLoop;
    for (const Sound& s : bank.sfx)   { all.push_back(&s); isLoop.push_back(false); }
    for (const Sound& s : bank.loops) { all.push_back(&s); isLoop.push_back(true); }
    for (size_t i = 0; i < all.size(); ++i) {
        const Sound& s = *all[i];
        const Analysis a = analyse(s.s);
        printf("  %-3d %-16s %6.2f %6.3f %6.3f %6.0fHz %5.0fms %7.4f%s\n", (int)i, s.info.name, a.seconds, a.peak, a.rms,
               a.centroid, a.lead * 1000.0f, a.tail, isLoop[i] ? "  (loop)" : "");
        bool ok = a.nan == 0 && std::fabs(a.peak - s.info.peak) < 0.01f * s.info.peak + 1e-4f &&
                  a.seconds > 0.04f && a.seconds < 10.0f && std::fabs(a.dc) < 0.03f && a.rms > 0.004f && a.rms < 0.6f;
        if (!isLoop[i]) ok = ok && (a.tail < 0.03f * a.peak + 1e-4f || std::strcmp(s.info.name, "cannon-charge") == 0 || std::strcmp(s.info.name, "laser-charge") == 0) && a.lead < 0.06f;   // the wind-up is cut off by the shot
        else            ok = ok && a.jump < 6.0f * a.rmsDiff + 0.004f;                  // the end runs into the start
        if (!ok) { ++bad; printf("      ^ FAILS: nan %d, dc %.4f, seam jump %.4f (rms step %.4f)\n", a.nan, a.dc, a.jump, a.rmsDiff); }
    }
    check(bad == 0, "every sound is finite, on level, centred, starts at once, ends quietly, loops meet");

    // ---- the pictures, a page of thirteen sounds at a time
    {
        const int W = 1000, rowH = 66, perPage = 13;
        const int rows = (int)all.size();
        for (int page = 0; page * perPage < rows; ++page) {
            const int n = std::min(perPage, rows - page * perPage);
            std::vector<uint8_t> img((size_t)W * n * rowH * 3, 8);
            for (int k = 0; k < n; ++k) {
                const size_t i = (size_t)(page * perPage + k);
                drawRow(img, W, k * rowH, rowH - 1, all[i]->s, isLoop[i] ? 3.0f : std::max(0.3f, (float)all[i]->s.size() / RATE));
            }
            char path[600];
            snprintf(path, sizeof path, "%s_sounds%d.png", prefix, page);
            if (writePng(path, W, n * rowH, img)) printf("  picture, sounds %d-%d: %s\n", page * perPage, page * perPage + n - 1, path);
        }
    }

    // ---- the mixer, with no device behind it
    printf("\n  mixer:\n");
    resetOffline();
    {   // silence in, silence out
        const auto out = mix(2048);
        check(peak16(out) == 0, "nothing playing is silence");
    }
    {   // pan
        resetOffline();
        play(Sfx::Rifle, 1.0f, -1.0f);
        auto out = mix(4096);
        const double lE = energy(out, 0), rE = energy(out, 1);
        resetOffline();
        play(Sfx::Rifle, 1.0f, 1.0f);
        out = mix(4096);
        const double lE2 = energy(out, 0), rE2 = energy(out, 1);
        resetOffline();
        play(Sfx::Rifle, 1.0f, 0.0f);
        out = mix(4096);
        const double lE3 = energy(out, 0), rE3 = energy(out, 1);
        check(lE > 8.0 * rE && rE2 > 8.0 * lE2, "panned hard left or right, it comes out of that side");
        check(std::fabs(lE3 / rE3 - 1.0) < 0.25, "centred, both sides are alike");
    }
    {   // pitch: twice as fast is half as long
        resetOffline();
        play(Sfx::Salvo, 1.0f, 0.0f, 2.0f);
        const int frames = (int)bank.sfx[(size_t)Sfx::Salvo].s.size();
        mix(frames / 2 - 1024);
        const int still = stats().voices;
        mix(4096);
        check(still == 1 && stats().voices == 0, "pitch 2 plays the sound in half the time");
    }
    {   // delay
        resetOffline();
        play(Sfx::Rifle, 1.0f, 0.0f, 1.0f, 0.1f);
        const auto out = mix(RATE / 5);
        const double early = energy(out, 0, 0, (size_t)(RATE * 0.09)), late = energy(out, 0, (size_t)(RATE * 0.1), (size_t)(RATE * 0.13));
        check(early < 1e-9 && late > 1e-4, "a delayed sound starts when asked");
    }
    {   // voice limits and gaps
        resetOffline();
        for (int i = 0; i < 20; ++i) play(Sfx::Rifle);
        mix(64);
        check(stats().voices <= info(Sfx::Rifle).voices, "twenty rifle shots at once never use more than its voice limit");
        resetOffline();
        play(Sfx::RockHit);  play(Sfx::RockHit);  play(Sfx::RockHit);
        mix(64);
        check(stats().voices == 1, "a sound with a minimum gap ignores triggers that come too soon");
        resetOffline();
        for (int i = 0; i < 300; ++i) { play((Sfx)(i % (int)Sfx::Count)); }
        mix(64);
        check(stats().voices <= 40, "hundreds of sounds at once never use more than the mixer has");
    }
    {   // loudness: piling on cannot blow past full scale
        resetOffline();
        for (int i = 0; i < 40; ++i) play((Sfx)(i % 3 == 0 ? (int)Sfx::NukeBoom : (i % 3 == 1 ? (int)Sfx::ExplodeL : (int)Sfx::ShipDeath)), 2.0f);
        const auto out = mix(RATE);
        check(peak16(out) <= 32000, "the loudest pile-up stays inside the soft clip");
        resetOffline();
        play(Sfx::NukeBoom);
        const auto boom = mix(RATE);
        resetOffline();
        play(Sfx::Rifle);
        const auto rifle = mix(RATE);
        const double eBoom = energy(boom, 0) + energy(boom, 1), eRifle = energy(rifle, 0) + energy(rifle, 1);
        printf("      loudness (energy) nuke : rifle = %.0f : 1\n", eBoom / std::max(eRifle, 1e-9));
        check(eBoom > 8.0 * eRifle, "a nuke is far louder than a rifle shot");
    }
    {   // loops fade in and out
        resetOffline();
        loop(Sustain::Thruster, 1.0f);
        auto out = mix(RATE / 2);
        const int p1 = peak16(std::vector<int16_t>(out.end() - 4000, out.end()));
        loop(Sustain::Thruster, 0.0f);
        mix(RATE);
        out = mix(4000);
        const int p2 = peak16(out);
        check(p1 > 500, "a loop that is switched on is heard");
        check(p2 < p1 / 30, "and fades away when it is switched off");
        resetOffline();
        loop(Sustain::Ambient, 0.0f);
        check(peak16(mix(4096)) == 0, "a loop at zero costs nothing and says nothing");
    }
    {   // reverb tail
        resetOffline();
        play(Sfx::ExplodeL);
        const int len = (int)bank.sfx[(size_t)Sfx::ExplodeL].s.size();
        const auto out = mix(len + RATE * 3);
        const double tail1 = energy(out, 0, (size_t)len + 2000, (size_t)len + 2000 + 4410);
        const double tail2 = energy(out, 0, (size_t)len + RATE * 2, (size_t)len + RATE * 2 + 4410);
        const double dry = energy(out, 0, 0, (size_t)len), wetTail = energy(out, 0, (size_t)len, out.size() / 2);
        printf("      reverb: energy after the sound ends is %.1f%% of the sound's own\n", 100.0 * wetTail / std::max(dry, 1e-12));
        check(tail1 > 1e-7, "an explosion leaves a reverb tail after the sound itself has ended");
        check(tail2 < tail1 * 0.2, "and the tail dies away");
    }
    {   // mute
        resetOffline();
        loop(Sustain::Ambient, 1.0f);
        mix(RATE / 2);
        toggleMute();
        mix(RATE / 2);
        const int p = peak16(mix(2048));
        toggleMute();
        check(p < 200, "muting fades everything out");
        check(!muted(), "and it can be turned back on");
    }
    shutdown();

    printf("\nsoundcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------- the audition --
int audition(bool quick) {
    if (!init(true)) { printf("soundtest: no audio device\n"); return 1; }
    Bank bank;
    buildBank(bank);
    if (quick) {                                        // just prove the device takes buffers, and quietly
        setMaster(0.12f);
        play(Sfx::ShopHover, 1.0f);
        Sleep(600);
        play(Sfx::ShopBuy, 1.0f);
        Sleep(900);
        const Stats st = stats();
        printf("soundtest (quick): device open, %llu buffers played, %llu underruns\n",
               (unsigned long long)st.buffers, (unsigned long long)st.underruns);
        shutdown();
        return st.buffers > 0 ? 0 : 1;
    }
    printf("soundtest: playing every sound in turn\n");
    for (size_t i = 0; i < bank.sfx.size(); ++i) {
        const Sound& s = bank.sfx[i];
        printf("  %-16s %.2f s\n", s.info.name, (double)s.s.size() / RATE);
        fflush(stdout);
        play((Sfx)i, 1.0f);
        Sleep((DWORD)(std::max(0.5f, (float)s.s.size() / RATE + 0.25f) * 1000.0f));
    }
    for (int i = 0; i < (int)Sustain::Count; ++i) {
        printf("  loop %-11s\n", name((Sustain)i));
        fflush(stdout);
        loop((Sustain)i, 1.0f);
        Sleep(2200);
        loop((Sustain)i, 0.0f);
        Sleep(400);
    }
    Sleep(1500);
    const Stats st = stats();
    printf("  device: %llu buffers played, %llu underruns\n", (unsigned long long)st.buffers, (unsigned long long)st.underruns);
    shutdown();
    return st.buffers > 0 ? 0 : 1;
}

}  // namespace audio
