// world.h -- asteroid bodies, streaming chunks, broadphase and destruction.
#pragma once
#include "core.h"
#include "field.h"
#include "render.h"
#include <vector>
#include <unordered_map>

typedef long long i64;

// ---------------------------------------------------------------- tuning --
namespace cfg {
    static const double CHUNK       = 620.0;    // world units per streaming chunk
    static const int    MAX_BODIES  = 4096;
    static const float  DENSITY     = 1.0f;
    static const float  GRAV_CONST  = 258.75f;  // all rocks end up near 650 u/s2 at the surface (it was 100, 150, 225)
    static const float  MIN_PIECE   = 140.0f;   // area below this becomes debris
    static const double SIM_RADIUS  = 5200.0;   // full physics only near the player
    static const int    REBUILD_BUDGET = 14;    // split checks per frame (uploads get 3x)
    static const float  MAX_SEPARATION_SPEED = 55.0f;  // cap on penetration push-out
    static const float  MAX_BODY_SPEED = 600.0f;
    static const float  MAX_BODY_SPIN  = 2.0f;
}

// A rock. Local space has its origin at the centre of mass.
struct Body {
    dv2   pos;
    v2    vel;
    float ang = 0, angVel = 0;
    float mass = 1, invMass = 0;
    float inertia = 1, invInertia = 0;
    float radius = 1;              // bounding radius around the local origin
    float flash = 0;               // impact glow, decays each frame
    Col   color;
    uint32_t seed = 0;
    uint32_t gen  = 0;             // bumped on slot reuse, for stale handles
    uint32_t netId = 0;            // stable across machines; 0 when not in a network game
    float genRadius = 0;           // the radius it was generated with; with the seed, all a fresh rock needs
    bool  alive = false;
    bool  dirty = false;           // field changed: recheck mass and splits
    bool  geomDirty = false;       // contour must be re-extracted and uploaded
    bool  authored = false;        // no longer matches its procedural original
    bool  simulated = true;

    Field f;
    std::vector<v2>    pts;        // contour points, local space
    std::vector<Loop>  loops;
    std::vector<float> heat;       // per-point impact glow
    v2    lastImpact;
    GeomSlot geom;

    float cosA = 1, sinA = 0;      // refreshed once per frame
    void  refreshTrig() { cosA = std::cos(ang); sinA = std::sin(ang); }

    v2  toLocal(dv2 world) const {
        const v2 d((float)(world.x - pos.x), (float)(world.y - pos.y));
        return v2(d.x * cosA + d.y * sinA, -d.x * sinA + d.y * cosA);
    }
    dv2 toWorld(v2 local) const {
        return dv2(pos.x + (double)(local.x * cosA - local.y * sinA),
                   pos.y + (double)(local.x * sinA + local.y * cosA));
    }
    v2  dirToWorld(v2 local) const { return rot(local, cosA, sinA); }
    // Velocity of the material point currently at world offset r.
    v2  velAt(v2 r) const { return vel + perp(r) * angVel; }
};

// Handle that survives a body being destroyed and its slot recycled.
struct BodyRef {
    int slot = -1;
    uint32_t gen = 0;
    void clear() { slot = -1; }
};

// Everything that changes the *shape* of the world, recorded in the order it
// happened. A rock is a pure function of its radius and seed plus the carves
// applied to it, so this journal is enough to rebuild an identical world
// somewhere else; see net.h. Off unless someone turns it on, and it costs one
// branch when it is off.
struct WorldOp {
    enum Kind : uint8_t { Spawn, Carve, Split, Remove, Settle };   // Settle: re-centred on its centre of mass
    Kind     kind;
    uint32_t id = 0;              // net id of the rock it happened to
    uint32_t seed = 0;
    float    radius = 0, wobble = 0;
    v2       local;               // carve centre, in the rock's own frame
    dv2      pos;                 // spawn position
    v2       vel;
    float    ang = 0, angVel = 0;
    // A split hands its surviving pieces consecutive ids, so the pieces only need
    // naming once. Both sides label the components in the same order.
    uint32_t firstChild = 0;
    uint8_t  children = 0;
};

// Something worth drawing a puff of sparks for.
struct WorldEvent {
    enum Kind { Debris, Split, Impact };
    dv2 pos; v2 vel; float size; Kind kind; Col col;
};

struct ContactPoint { int a, b; dv2 p; v2 n; float depth, w; };
struct ChunkCoord { i64 cx, cy; };

struct World {
    std::vector<Body> bodies;
    std::vector<int>  freeSlots;
    std::vector<int>  active;              // slots alive this frame
    std::vector<WorldEvent> events;        // drained by the effects layer
    uint64_t seed = 0x5EEDFACEull;
    int liveCount = 0, simCount = 0, rebuildsThisFrame = 0, contactsThisFrame = 0;

    // ---- journalling, for replicating the world to another machine ---------
    bool     journal = false;      // when on, every shape change is recorded in ops
    std::vector<WorldOp> ops;
    std::vector<uint32_t> fullSync;        // rocks restored from a saved chunk: to be sent whole, not replayed
    uint32_t nextNetId = 1;        // the host hands these out
    std::unordered_map<uint32_t, int> byNetId;    // net id -> slot, kept while journalling
    uint32_t assignNetId(int slot);
    bool     splitting = false;    // inside splitBody: the parent is reported as a Split, not a Remove
    int      slotOfNetId(uint32_t id) const;
    // Carve in the rock's own frame. Replication uses this so a carve lands in
    // exactly the same place whatever the body's transform happens to be.
    bool damageLocal(int slot, v2 localPos, float radius, float wobble, uint32_t rseed);
    // A cheap hash of a rock's field, so two machines can tell they still agree.
    uint32_t fieldHash(int slot) const;        // exact: any sample differing changes it
    // A summary that tolerates noise. Two machines whose arithmetic differs in the last
    // bit will disagree about the odd sample right on a rock's edge, and that is not
    // worth repairing; a missed carve moves the count and the centroid a great deal.
    struct FieldSummary { uint32_t solid = 0; int16_t cx = 0, cy = 0; };
    FieldSummary summarise(int slot) const;
    static bool summariesClose(const FieldSummary& a, const FieldSummary& b);
    // Replication needs to drive these directly. The pieces of a split are listed
    // in the order they were created, which is the order both machines name them
    // in; slot numbers are local to one machine and cannot be used for that.
    void splitNow(int slot) { splitBody(slot); }
    void refinalize(int slot, bool recenter) { finalizeBody(slot, recenter); }
    std::vector<int> lastSplitPieces;

    void init(uint64_t worldSeed);
    void streamChunks(dv2 centre, double viewRadius, int loadBudget = 3);
    void step(float dt, dv2 focus);
    void syncGeometry(Renderer& r);      // budgeted contour rebuild + GPU upload
    void settleDirty();                  // the split/re-centre half of that, which needs no renderer
    // Rebuilds the list of live rocks and the broadphase, without moving anything: what a
    // client needs so that probe() and gravityAt() work on rocks the host is simulating.
    void refreshIndex(dv2 focus);
    void collectRenderData(const Camera& cam, Renderer& r,
                           std::vector<BodyXform>& xf,
                           std::vector<int>& firsts, std::vector<int>& counts);

    // Gravity felt at a world point, summed over nearby rocks.
    v2  gravityAt(dv2 p, double reach = 600.0, int* dominant = nullptr) const;
    // Nearest solid overlap for a point of the given radius; -1 if free.
    int probe(dv2 p, float r, v2* normal, float* depth, int ignore = -1) const;
    // Carve a disc out of one rock. Returns true if rock was actually removed.
    bool damage(int slot, dv2 worldPos, float radius, float wobble, uint32_t rseed);
    // Ray-ish query used by bullets: first body whose field is solid at p.
    int  solidAt(dv2 p, v2* localOut = nullptr) const;

    Body* get(BodyRef h) {
        if (h.slot < 0 || h.slot >= (int)bodies.size()) return nullptr;
        Body& b = bodies[h.slot];
        return (b.alive && b.gen == h.gen) ? &b : nullptr;
    }
    BodyRef ref(int slot) const {
        BodyRef h; h.slot = slot; h.gen = bodies[slot].gen; return h;
    }
    size_t fieldBytes() const;

    int  spawn(dv2 p, float radius, uint32_t rseed);
    // True if no live rock overlaps a disc of this radius at p.
    bool spaceFree(dv2 p, double radius, double margin = 1.12) const;

    // ---- level support ---------------------------------------------------
    // Keeps rocks out of a disc (the beacon, a ship's berth) and removes any already there.
    // Zones accumulate until dropZone().
    void clearZone(dv2 centre, double radius);
    void dropZone() { zones.clear(); }
    bool zoneBlocks(dv2 p, double r) const;
    // A big blast: carves a crater of carveR out of every rock in reach and
    // shoves what is left. impulse is a momentum scale (mass-divided per rock).
    void explode(dv2 c, float carveR, float impulseR, float impulse, uint32_t seed);
    void destroy(int slot);
    void rebuildGeometry(int slot, Renderer& r);
    void markAllDirty();

    // ---- broadphase ----------------------------------------------------
    struct Grid {
        double cell = 640.0;
        dv2    origin;
        int    nx = 0, ny = 0;
        std::vector<int>  head;            // per-cell first index, -1 empty
        std::vector<int>  next;            // linked list over body slots
        void build(const World& w, dv2 centre, double radius);
        template <typename F>
        void forEachNear(const World& w, dv2 p, double r, F fn) const {
            if (nx <= 0) return;
            const int x0 = (int)std::floor((p.x - r - origin.x) / cell);
            const int x1 = (int)std::floor((p.x + r - origin.x) / cell);
            const int y0 = (int)std::floor((p.y - r - origin.y) / cell);
            const int y1 = (int)std::floor((p.y + r - origin.y) / cell);
            for (int y = std::max(0, y0); y <= std::min(ny - 1, y1); ++y)
                for (int x = std::max(0, x0); x <= std::min(nx - 1, x1); ++x)
                    for (int i = head[(size_t)y * nx + x]; i >= 0; i = next[i])
                        fn(i);
        }
    };
    Grid grid;
    double maxBodyRadius = 300.0;

private:
    struct Zone { dv2 pos; double r; };
    std::vector<Zone> zones;         // discs kept free of rock: the beacon, and a ship's berth
    struct SavedBody {
        dv2 pos; v2 vel; float ang, angVel; uint32_t seed; Col color; Field f;
    };
    struct SavedChunk {
        uint64_t stamp = 0;                // for evicting the least recently saved
        std::vector<SavedBody> bodies;
    };
    std::unordered_map<i64, SavedChunk> saved;
    uint64_t saveStamp = 0;
    std::unordered_map<i64, ChunkCoord> loaded;
    std::vector<i64> pendingUnload;
    std::vector<int> dirtyList;
    std::vector<GeomSlot> pendingFree;
    std::vector<ContactPoint> contacts;

    static i64 chunkKey(i64 cx, i64 cy) { return (cx << 32) ^ (cy & 0xFFFFFFFFll); }
    void loadChunk(i64 cx, i64 cy);
    void unloadChunk(i64 cx, i64 cy);
    int  allocSlot();
    void finalizeBody(int slot, bool recenter);
    void narrowphase(int a, int b);
    void solveContacts(float dt);
    void splitBody(int slot);
};
