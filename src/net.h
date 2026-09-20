// net.h -- multiplayer: the wire protocol, the transport, and world replication.
//
// The shape of the thing:
//
//   * One machine is the **host**. It runs the only real simulation. Clients send
//     what their player is doing and draw what the host tells them.
//   * A rock is a pure function of its radius and seed plus the carves applied to
//     it, so the wire never carries geometry: a rock costs 34 bytes to introduce
//     and a hole costs 18. World::ops (world.h) records those events wherever they
//     come from, and the replicator drains them.
//   * Rock *motion* is the expensive part, because there are hundreds of them. So
//     clients dead-reckon every rock from its last known velocity, and the host
//     only spends bandwidth on one when the client's guess has drifted too far.
//     Most rocks are quietly drifting and cost nothing at all.
//   * Anything that has to survive (a carve, a split) goes on a reliable ordered
//     channel. Anything that is replaced by the next update (positions) does not.
//
// Determinism across machines is not assumed. Both sides hash each rock's field
// now and then; if they ever disagree the host ships that rock's field verbatim
// and the client starts again from it.
//
//   net.h        this: protocol, transport, replicator
//   net.cpp      packet framing, the reliable channel, Winsock UDP, the loopback link
//   net_world.cpp   turning a World into messages and back
#pragma once
#include "world.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

constexpr uint32_t PROTOCOL = 0x41443035u;   // "AD05"
constexpr int      MAX_PACKET = 1200;        // stays under the usual path MTU
constexpr int      MAX_PLAYERS = 8;

enum class Msg : uint8_t {
    Hello = 1,      // client -> host: I would like to join
    Welcome,        // host -> client: you are player N, the match looks like this
    Input,          // client -> host: what my player is doing, every frame
    RockSpawn,      // host -> client: a rock exists (radius + seed rebuild its shape)
    RockCarve,      //                 a hole, in the rock's own frame
    RockSplit,      //                 it came apart; the pieces get consecutive ids
    RockRemove,     //                 it is gone
    RockSettle,     //                 it stayed in one piece but its centre of mass moved
    RockMotion,     //                 where a rock really is, when guessing has drifted
    RockField,      //                 the whole field, to repair a rock that diverged
    RockAudit,      //                 field hashes, so both sides notice a divergence
    RockRepair,     // client -> host: these rocks no longer match yours, send them whole
    PlayerState,    // host -> client: where everyone is
    Shot,           //                 someone fired
    Hit,            //                 someone was hit
    Bye,
};

// ---------------------------------------------------------------- packing --
// Little-endian, tightly packed, no alignment or padding anywhere.
struct Writer {
    std::vector<uint8_t> b;
    void u8 (uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void f32(float v)    { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void f64(double v)   { uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
    void vec(v2 v)       { f32(v.x); f32(v.y); }
    void dvec(dv2 v)     { f64(v.x); f64(v.y); }
    size_t size() const  { return b.size(); }
};

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0, at = 0;
    bool   bad = false;
    Reader() = default;
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool need(size_t k) { if (at + k > n) { bad = true; return false; } return true; }
    uint8_t  u8 () { if (!need(1)) return 0; return p[at++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(p[at] | (p[at+1] << 8)); at += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)p[at+i] << (8*i); at += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)p[at+i] << (8*i); at += 8; return v; }
    float    f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
    double   f64() { uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return v; }
    v2       vec()  { const float x = f32(); return v2(x, f32()); }
    dv2      dvec() { const double x = f64(); return dv2(x, f64()); }
    size_t   left() const { return n > at ? n - at : 0; }
};

// What a rock's motion looks like on the wire: 16 bytes instead of 36. Position
// is fixed-point to a sixteenth of a unit, velocity to an eighth, the angle to
// about a ten-thousandth of a radian. Both sides use the *quantised* values, so
// the host's idea of what the client believes is exact.
struct MotionQ {
    int32_t x = 0, y = 0;
    int16_t vx = 0, vy = 0;
    uint16_t ang = 0;
    int16_t spin = 0;
    static MotionQ from(const Body& b);
    dv2   pos()   const { return dv2(x / 16.0, y / 16.0); }
    v2    vel()   const { return v2(vx / 8.0f, vy / 8.0f); }
    float angle() const { return ang * (6.28318530718f / 65536.0f); }
    float spinRate() const { return spin / 1024.0f; }
    void  write(Writer& w) const;
    static MotionQ read(Reader& r);
};

// ------------------------------------------------------------- transport --
// A Link moves datagrams between two endpoints and knows nothing about the game.
// UdpLink is the real one; LoopLink is two of them wired together in one process,
// which is what the tests use so replication can be checked without a network.
struct Packet {
    std::vector<uint8_t> data;
    int peer = 0;                 // host side: which client it came from / goes to
};

struct Link {
    virtual ~Link() {}
    virtual bool send(const uint8_t* d, size_t n, int peer) = 0;
    virtual bool recv(Packet& out) = 0;
    virtual void poll() {}
    uint64_t bytesSent = 0, bytesRecv = 0, packetsSent = 0, packetsRecv = 0;
};

// Both ends in one process, with optional loss and latency so the protocol can be
// tested against an unkind network without needing one.
struct LoopLink : Link {
    struct Pending { std::vector<uint8_t> data; int peer; double due; };
    LoopLink* other = nullptr;
    std::vector<Pending> queue;      // what has been handed to *this* link to deliver
    double now = 0, latency = 0;
    float  loss = 0;
    uint32_t rng = 12345;
    int    myPeer = 0;

    bool send(const uint8_t* d, size_t n, int peer) override;
    bool recv(Packet& out) override;
    void advance(double dt) { now += dt; }
};

// Real UDP over Winsock. The host binds a port; a client connects to one address.
struct UdpLink : Link {
    bool host = false;
    bool open(int port, bool loopbackOnly = false);   // host: bind and listen (loopback-only for tests: no firewall prompt)
    bool connect(const char* ip, int port);    // client: remember where the host is
    bool send(const uint8_t* d, size_t n, int peer) override;
    bool recv(Packet& out) override;
    void close();
    ~UdpLink() override { close(); }
    std::string lastError;

    // A peer is remembered by address so the host can reply to it.
    struct Peer { uint32_t ip = 0; uint16_t port = 0; bool used = false; };
    Peer peers[MAX_PLAYERS];
    int  peerCount = 0;
private:
    uintptr_t sock = ~(uintptr_t)0;
    int findOrAddPeer(uint32_t ip, uint16_t port);
};

// ------------------------------------------------- reliable ordered channel --
// Everything that changes the world has to arrive, and in order, or the two sides
// stop agreeing about what a rock looks like. This is the smallest thing that
// does that: number every message, repeat what has not been acknowledged, and
// hold anything that arrives early until the gap in front of it is filled.
struct Reliable {
    struct Out { uint32_t seq; std::vector<uint8_t> payload; double sentAt; int tries; };
    std::vector<Out> outbox;
    uint32_t nextSeq = 1, ackedThrough = 0;

    struct In { uint32_t seq; std::vector<uint8_t> payload; };
    std::vector<In> holding;
    uint32_t wantSeq = 1;

    // Pacing: no more than this many bytes go out per flush, and none at all while too
    // much is already in flight. Without it a burst (the whole field on joining, a repair)
    // overflows the receiver's socket buffer or a router queue, and is mostly lost.
    size_t bytesPerFlush = 14000, maxInFlight = 48000;
    double resendAfter = 0.25;         // adapts to the measured round trip (see ack)
    double srtt = 0;                   // smoothed round-trip time, 0 until measured
    uint64_t resent = 0;               // messages sent more than once
    int    maxTries = 40;

    void queue(const std::vector<uint8_t>& payload);
    // Fills `out` with what should go on the wire now (new and overdue messages).
    void collect(double now, std::vector<std::vector<uint8_t>>& out, size_t byteBudget = ~(size_t)0);
    // Takes a numbered message off the wire; returns the ones now ready, in order.
    void accept(uint32_t seq, const uint8_t* d, size_t n, std::vector<std::vector<uint8_t>>& ready);
    void ack(uint32_t through);
    void rttSample(double seconds);   // from timestamps echoed in packets, so resends cannot confuse it
    size_t pending() const { return outbox.size(); }
};

// -------------------------------------------------------------- endpoint --
// One end of a conversation: batches reliable messages into packets, sends the
// lossy ones on their own, and piggybacks a cumulative acknowledgement on every
// packet so there is no separate ack traffic to speak of.
//
//   packet := kind:u8  ack:u32  stamp:u16  echo:u16  hold:u16  body
//   kind 0 = nothing but the ack
//   kind 1 = reliable batch:  { len:u16  seq:u32  payload }...
//   kind 2 = one unreliable message
struct Endpoint {
    Link* link = nullptr;
    int   peer = 0;
    Reliable rel;
    std::vector<std::vector<uint8_t>> ready;        // reliable messages, in order
    std::vector<std::vector<uint8_t>> unreliable;   // lossy messages, as they came
    uint64_t reliableSent = 0, unreliableSent = 0, ackOnlySent = 0, resends = 0;

    // Anything bigger than a packet is split; the ordered channel puts it back together.
    void sendReliable(const std::vector<uint8_t>& payload);
    void sendUnreliable(const std::vector<uint8_t>& payload);
    void flush(double now);       // sends new and overdue reliable messages, and an ack
    double now = 0;               // as of the last flush; lets poll() time acknowledgements
    void poll(double t);          // reads everything waiting
    bool wantsAck = false;
    // Timestamps ride on every packet: our clock in ms, the last one we heard from
    // the peer, and how long we sat on it. That gives a clean round trip whatever
    // has been resent.
    uint16_t lastStamp = 0;  double lastStampAt = 0;  bool haveStamp = false;
    std::vector<uint8_t> assembling;               // a large message, part way through arriving
};

// ------------------------------------------------------------- replication --
// The host side: watches a World and turns what happens to it into messages.
struct HostReplicator {
    World* w = nullptr;
    double motionTolerance = 3.0;     // world units of drift a client may have before correcting
    double angleTolerance  = 0.10;    // radians, likewise
    int    motionsPerTick  = 24;      // cap, so a busy frame cannot flood the link
    double auditPeriod     = 2.0;     // seconds between field-hash audits
    double refreshPeriod   = 4.0;     // a rock is re-sent at least this often, so one lost packet cannot strand it
    double auditAt         = 0;
    uint32_t auditCursor   = 0;       // the last rock the previous audit reached
    uint16_t auditPerMessage = 100;

    // What we believe each client currently thinks, so we can tell when its guess
    // has drifted far enough to be worth a packet.
    struct Believed { MotionQ q; double at = 0; bool known = false; };
    std::unordered_map<uint32_t, Believed> believed;

    void begin(World& world);
    // Drains World::ops into reliable messages.
    void collectShapeChanges(std::vector<std::vector<uint8_t>>& reliableOut);
    // What a client needs to know about rocks that already exist when it joins: a
    // fresh rock is named by radius and seed; one that has been shot is sent whole.
    void collectInitial(std::vector<std::vector<uint8_t>>& reliableOut);
    // Everything a rock's position needs, for the rocks whose guess has gone stale.
    void collectMotion(double now, std::vector<uint8_t>& unreliableOut);
    void collectAudit(double now, std::vector<uint8_t>& reliableOut);
    // The whole field of one rock, to repair a client that has diverged.
    void writeField(uint32_t netId, std::vector<uint8_t>& out);
    uint64_t shapeBytes = 0, motionBytes = 0, auditBytes = 0, fieldBytes = 0;
    int      rocksIntroduced = 0, carvesSent = 0, motionsSent = 0, repairsSent = 0;
};

// The client side: applies all of that to its own World.
struct ClientReplicator {
    World* w = nullptr;
    double now = 0;
    // Rocks the client has been told about but whose motion it is guessing at.
    void begin(World& world);
    bool applyReliable(const uint8_t* d, size_t n);
    bool applyUnreliable(const uint8_t* d, size_t n);
    // Moves every rock along its last known velocity. The host does the real
    // physics; this only keeps things from standing still between corrections.
    void deadReckon(float dt);
    // Rocks whose field no longer matches the host's, found by the last audit.
    std::vector<uint32_t> diverged;
    std::unordered_map<uint32_t, double> repairAsked;   // when each rock was last reported, so a repair in flight is not asked for twice
    // Test hook: how far to nudge each carve, in world units. Stands in for two
    // machines whose floating point differs in the last few bits.
    float jitter = 0;
    Rng   jitterRng{5};
    int rocksKnown = 0, carvesApplied = 0, splitsApplied = 0, repairsApplied = 0, motionsApplied = 0;
};

}  // namespace net
