// audio.h -- sound. Every effect is synthesised at start-up from a few lines of
// maths (oscillators, filtered noise, envelopes), so there are no sound files to
// ship. The look is neon vector line-work with a bloom glow, and the sound follows
// it: clean sine and triangle tones, bright pitch sweeps, filtered-noise blasts
// with a little glittering sparkle on top, and a wash of space reverb.
//
//   audio.h        this: the list of sounds and the small API
//   sounds.cpp     the synthesis of each sound
//   audio.cpp      the mixer, the reverb, and the Windows waveOut device
//   audio_game.cpp how the game turns events into sounds (Game::sfx, updateAudio)
//   audio_check.cpp the -soundcheck / -soundtest harnesses
#pragma once
#include <cstdint>
#include <vector>

enum class Sfx : int {
    // your weapons
    Rifle, Shell, Salvo, FractalFire, FractalSplit, NukeThrow, NukeBeep, NukeBoom,
    // things going bang
    ExplodeS, ExplodeM, ExplodeL,
    // the enemy
    EnemyShot, MissileLaunch, MissileWarn, EnemyHit,
    // rocks and the spaceman
    RockHit, RockSplit, Jump, Land, FuelEmpty, Hurt,
    // the run
    Death, GameOver, Respawn, LivesRestored, LevelStart, LevelComplete,
    BeaconPing, ClockTick, ClockTickHi,
    // the depot
    ShopOpen, ShopHover, ShopBuy, ShopDeny, ShopContinue, Pause,
    // force field and shield
    FieldOn, FieldOff, FieldEmpty, ShieldUp, ShieldBlock, ShieldBreak,
    // warships
    ShipAlert, ShipHit, ShipDeath, CannonCharge, CannonFire, FlakFire, LaserCharge, LaserFire,
    Count
};

// Sounds that run for as long as something is happening. Their volume follows the
// game every frame and fades in and out, so a rocket that cuts out does not click.
enum class Sustain : int { Thruster, FieldHum, ShieldHum, Heartbeat, Ambient, Count };

namespace audio {

constexpr int RATE = 44100;

// What the bank knows about each sound.
struct Info {
    const char* name;
    float peak;         // it is normalised to this, so the levels are set in one table
    int   voices;       // how many may play at once before the oldest is cut
    float gap;          // seconds that must pass before it can be triggered again
    float wet;          // how much of it goes to the reverb
};

struct Sound {
    std::vector<float> s;                     // mono, RATE samples a second
    Info info;
};

// The whole bank, synthesised. Deterministic: the same every run.
struct Bank {
    std::vector<Sound> sfx;                   // indexed by Sfx
    std::vector<Sound> loops;                 // indexed by Sustain
};
void buildBank(Bank& out);

// ---- the device and the mixer
bool  init(bool enabled);                     // false = no sound (and every call below is a no-op)
void  shutdown();
bool  ready();
void  play(Sfx s, float gain = 1.0f, float pan = 0.0f, float pitch = 1.0f, float delay = 0.0f);
void  loop(Sustain l, float gain, float pitch = 1.0f, float pan = 0.0f);   // gain 0 fades it out
void  setMaster(float v);
float master();
void  toggleMute();
bool  muted();
const Info& info(Sfx s);
const char* name(Sustain l);

// ---- for the tests: mix without a device
struct Stats {
    uint64_t buffers = 0, underruns = 0;
    int voices = 0;
    float peak = 0;
    uint32_t plays[64] = {};                  // how many times each Sfx was actually started
    float loops[8] = {};                      // where each Sustain's volume has got to
};
Stats stats();
void  mixInto(int16_t* stereo, int frames);   // what the audio thread does with each buffer
void  startOffline();                         // make a mixer with no device behind it
void  resetOffline();

}  // namespace audio

// The harnesses (audio_check.cpp), run from main: -soundcheck and -soundtest.
namespace audio {
int check(const char* pngPrefix);     // analyse every sound; draw a picture of each. 0 = all good
int audition(bool quick);             // play every sound in turn through the device (quick: one soft tick, to test the device)
}
