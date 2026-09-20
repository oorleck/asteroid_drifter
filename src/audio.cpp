// audio.cpp -- the mixer and the Windows device behind it.
//
// Sounds are pre-rendered mono buffers (sounds.cpp). Playing one takes a voice: a
// read position, a pitch (the step through the buffer), and left/right gains from
// a pan. A second kind of voice, the loop, runs continuously and just has its
// volume nudged toward a target each frame. Everything goes through a small
// stereo reverb (the "space" in the sound) and a soft clip, then to waveOut.
//
// The device is fed by its own thread from a few short buffers, so latency is a
// few tens of milliseconds. All of it is optional: if there is no sound card, or
// the game was started with -nosound, every call here does nothing.
#include "audio.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

namespace audio {
namespace {

constexpr int MAX_VOICES = 40;
constexpr int NBUF = 4;
constexpr int BUF_FRAMES = 512;

struct Voice {
    const float* d = nullptr;
    int    n = 0;
    double pos = 0.0;
    float  step = 1.0f, gl = 0.0f, gr = 0.0f, wet = 0.0f;
    int    delay = 0;
    int    sfx = -1;
    uint64_t serial = 0;
    bool   used = false;
};

struct LoopVoice {
    const float* d = nullptr;
    int    n = 0;
    double pos = 0.0;
    float  gain = 0.0f, target = 0.0f, pitch = 1.0f, pan = 0.0f, wet = 0.0f;
};

// A small Freeverb-style room: four damped combs and two all-passes per side.
struct Reverb {
    static constexpr int NC = 4, NA = 2;
    static constexpr int cl[NC] = { 1557, 1617, 1491, 1422 };
    static constexpr int al[NA] = { 556, 441 };
    static constexpr int SPREAD = 23;
    std::vector<float> comb[2][NC], ap[2][NA];
    int ci[2][NC] = {}, ai[2][NA] = {};
    float cf[2][NC] = {};
    void init() {
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < NC; ++i) { comb[s][i].assign(cl[i] + s * SPREAD, 0.0f); ci[s][i] = 0; cf[s][i] = 0.0f; }
            for (int i = 0; i < NA; ++i) { ap[s][i].assign(al[i] + s * SPREAD, 0.0f);  ai[s][i] = 0; }
        }
    }
    void clear() { init(); }
    void run(float in, float& outL, float& outR) {
        const float fb = 0.90f, damp = 0.35f;
        float o[2];
        for (int s = 0; s < 2; ++s) {
            float acc = 0.0f;
            for (int i = 0; i < NC; ++i) {
                std::vector<float>& b = comb[s][i];
                float& f = cf[s][i];
                const float y = b[(size_t)ci[s][i]];
                f = y * (1.0f - damp) + f * damp;
                b[(size_t)ci[s][i]] = in + f * fb;
                if (++ci[s][i] >= (int)b.size()) ci[s][i] = 0;
                acc += y;
            }
            for (int i = 0; i < NA; ++i) {
                std::vector<float>& b = ap[s][i];
                const float y = b[(size_t)ai[s][i]];
                const float z = acc + y * 0.5f;
                b[(size_t)ai[s][i]] = z;
                acc = y - acc;
                if (++ai[s][i] >= (int)b.size()) ai[s][i] = 0;
            }
            o[s] = acc;
        }
        outL = o[0];  outR = o[1];
    }
};

struct Mixer {
    Bank bank;
    Voice voices[MAX_VOICES];
    LoopVoice loops[(int)Sustain::Count];
    Reverb rev;
    double lastPlay[(int)Sfx::Count] = {};
    uint64_t serial = 0;
    float masterVol = 0.8f, masterNow = 0.8f;
    bool  isMuted = false;
    bool  open = false, offline = false;
    Stats st;
    CRITICAL_SECTION cs;
    bool csInit = false;
};
Mixer M;

// the device
HWAVEOUT hwo = nullptr;
HANDLE   hEvent = nullptr, hThread = nullptr;
WAVEHDR  hdr[NBUF];
int16_t* bufData[NBUF] = {};
volatile bool running = false;

double nowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;  QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

struct Lock {
    Lock()  { EnterCriticalSection(&M.cs); }
    ~Lock() { LeaveCriticalSection(&M.cs); }
};

void ensureCs() { if (!M.csInit) { InitializeCriticalSection(&M.cs); M.csInit = true; } }

void panGains(float pan, float gain, float& l, float& r) {
    const float p = std::max(-1.0f, std::min(1.0f, pan));
    const float a = (p + 1.0f) * 0.7853982f;                 // equal power across the field
    l = std::cos(a) * gain * 1.4142f;
    r = std::sin(a) * gain * 1.4142f;
}

void resetState() {
    for (Voice& v : M.voices) v = Voice();
    for (int i = 0; i < (int)Sustain::Count; ++i) {
        LoopVoice& l = M.loops[i];
        l = LoopVoice();
        l.d = M.bank.loops[(size_t)i].s.data();
        l.n = (int)M.bank.loops[(size_t)i].s.size();
        l.wet = M.bank.loops[(size_t)i].info.wet;
    }
    M.rev.init();
    for (double& t : M.lastPlay) t = -1e9;
    M.serial = 0;
    M.st = Stats();
}

DWORD WINAPI audioThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (running) {
        WaitForSingleObject(hEvent, 40);
        int done = 0;
        for (int i = 0; i < NBUF && running; ++i) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            ++done;
            mixInto(bufData[i], BUF_FRAMES);
            waveOutWrite(hwo, &hdr[i], sizeof hdr[i]);
            ++M.st.buffers;
        }
        if (done >= NBUF) ++M.st.underruns;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------- mix --
void mixInto(int16_t* out, int frames) {
    Lock lock;
    const float kAtt = 0.0008f, kRel = 0.0003f;
    float peak = 0.0f;
    int active = 0;
    for (int i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f, send = 0.0f;

        for (Voice& v : M.voices) {
            if (!v.used) continue;
            if (v.delay > 0) { --v.delay; continue; }
            const int k = (int)v.pos;
            if (k + 1 >= v.n) { v.used = false; continue; }
            const float f = (float)(v.pos - k);
            const float s = v.d[k] * (1.0f - f) + v.d[k + 1] * f;
            l += s * v.gl;  r += s * v.gr;
            send += s * (v.gl + v.gr) * 0.5f * v.wet;
            v.pos += v.step;
            if (i == 0) ++active;
        }
        for (LoopVoice& lv : M.loops) {
            if (lv.gain < 0.0004f && lv.target <= 0.0f) continue;
            lv.gain += (lv.target - lv.gain) * (lv.target > lv.gain ? kAtt : kRel);
            const int k = (int)lv.pos;
            const float f = (float)(lv.pos - k);
            const float s = lv.d[k] * (1.0f - f) + lv.d[(k + 1) % lv.n] * f;
            float gl, gr;  panGains(lv.pan, lv.gain, gl, gr);
            l += s * gl;  r += s * gr;
            send += s * lv.gain * lv.wet;
            lv.pos += lv.pitch;
            if (lv.pos >= lv.n) lv.pos -= lv.n;
        }

        float wl, wr;
        M.rev.run(send * 0.45f, wl, wr);
        l += wl * 0.75f;  r += wr * 0.75f;

        M.masterNow += ((M.isMuted ? 0.0f : M.masterVol) - M.masterNow) * 0.002f;
        const float ol = std::tanh(l * M.masterNow), or_ = std::tanh(r * M.masterNow);
        peak = std::max(peak, std::max(std::fabs(ol), std::fabs(or_)));
        out[2 * i]     = (int16_t)(ol * 32000.0f);
        out[2 * i + 1] = (int16_t)(or_ * 32000.0f);
    }
    M.st.peak = peak;
    for (int i = 0; i < (int)Sustain::Count; ++i) M.st.loops[i] = M.loops[i].gain;
    int n = 0;
    for (const Voice& v : M.voices) if (v.used) ++n;
    M.st.voices = n;
    (void)active;
}

// ---------------------------------------------------------------------- API --
bool init(bool enabled) {
    if (!enabled || M.open) return M.open;
    ensureCs();
    buildBank(M.bank);
    resetState();

    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, (DWORD_PTR)hEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        printf("audio: no output device, running silent\n");
        CloseHandle(hEvent);  hEvent = nullptr;  hwo = nullptr;
        return false;
    }
    for (int i = 0; i < NBUF; ++i) {
        bufData[i] = new int16_t[BUF_FRAMES * 2];
        std::memset(bufData[i], 0, sizeof(int16_t) * BUF_FRAMES * 2);
        std::memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)bufData[i];
        hdr[i].dwBufferLength = BUF_FRAMES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(hwo, &hdr[i], sizeof hdr[i]);
    }
    M.open = true;
    running = true;
    for (int i = 0; i < NBUF; ++i) {                          // prime the queue
        mixInto(bufData[i], BUF_FRAMES);
        waveOutWrite(hwo, &hdr[i], sizeof hdr[i]);
    }
    hThread = CreateThread(nullptr, 0, audioThread, nullptr, 0, nullptr);
    return true;
}

void shutdown() {
    if (!M.open) return;
    if (!M.offline) {
        running = false;
        if (hEvent) SetEvent(hEvent);
        if (hThread) { WaitForSingleObject(hThread, 1000); CloseHandle(hThread); hThread = nullptr; }
        if (hwo) {
            waveOutReset(hwo);
            for (int i = 0; i < NBUF; ++i) { waveOutUnprepareHeader(hwo, &hdr[i], sizeof hdr[i]); delete[] bufData[i]; bufData[i] = nullptr; }
            waveOutClose(hwo);
            hwo = nullptr;
        }
        if (hEvent) { CloseHandle(hEvent); hEvent = nullptr; }
    }
    M.open = false;
}

bool ready() { return M.open; }

void play(Sfx s, float gain, float pan, float pitch, float delay) {
    if (!M.open || gain <= 0.001f) return;
    const int idx = (int)s;
    if (idx < 0 || idx >= (int)Sfx::Count) return;
    const Sound& sd = M.bank.sfx[(size_t)idx];
    Lock lock;
    const double now = nowSeconds();
    if (now - M.lastPlay[idx] < sd.info.gap) return;
    M.lastPlay[idx] = now;
    ++M.st.plays[idx];

    // How many of this one are already going? If it is too many, cut the oldest.
    int same = 0, oldest = -1;
    uint64_t oldestSerial = ~0ull;
    for (int i = 0; i < MAX_VOICES; ++i) {
        const Voice& v = M.voices[i];
        if (!v.used || v.sfx != idx) continue;
        ++same;
        if (v.serial < oldestSerial) { oldestSerial = v.serial; oldest = i; }
    }
    int slot = -1;
    if (same >= sd.info.voices) slot = oldest;
    if (slot < 0) for (int i = 0; i < MAX_VOICES; ++i) if (!M.voices[i].used) { slot = i; break; }
    if (slot < 0) {                                            // everything is busy: cut the oldest of all
        oldestSerial = ~0ull;
        for (int i = 0; i < MAX_VOICES; ++i)
            if (M.voices[i].serial < oldestSerial) { oldestSerial = M.voices[i].serial; slot = i; }
    }
    Voice& v = M.voices[slot];
    v.d = sd.s.data();
    v.n = (int)sd.s.size();
    v.pos = 0.0;
    v.step = std::max(0.25f, std::min(4.0f, pitch));
    panGains(pan, gain, v.gl, v.gr);
    v.wet = sd.info.wet;
    v.delay = (int)(std::max(0.0f, delay) * RATE);
    v.sfx = idx;
    v.serial = ++M.serial;
    v.used = true;
}

void loop(Sustain l, float gain, float pitch, float pan) {
    if (!M.open) return;
    Lock lock;
    LoopVoice& v = M.loops[(int)l];
    v.target = std::max(0.0f, gain);
    v.pitch = std::max(0.25f, std::min(3.0f, pitch));
    v.pan = pan;
}

void  setMaster(float v) { if (M.csInit) { Lock lock; M.masterVol = std::max(0.0f, std::min(1.5f, v)); } else M.masterVol = v; }
float master()           { return M.masterVol; }
void  toggleMute()       { if (M.csInit) { Lock lock; M.isMuted = !M.isMuted; } else M.isMuted = !M.isMuted; }
bool  muted()            { return M.isMuted; }

const Info& info(Sfx s)  { return M.bank.sfx[(size_t)s].info; }
const char* name(Sustain l) { return M.bank.loops[(size_t)l].info.name; }
Stats stats()            { Lock lock; return M.st; }

void startOffline() {
    if (M.open) return;
    ensureCs();
    buildBank(M.bank);
    resetState();
    M.open = true;
    M.offline = true;
    M.masterNow = M.masterVol;
}

void resetOffline() { Lock lock; resetState(); M.masterNow = M.masterVol; }

}  // namespace audio
