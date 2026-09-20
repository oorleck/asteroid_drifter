// net_world.cpp -- turning a World into messages and back again.
//
// The host drains World::ops (every carve, split and removal, wherever it came
// from) into reliable messages, and separately decides which rocks have drifted
// far enough from the client's guess to be worth a position update. The client
// applies all of it to its own World and dead-reckons everything in between.
#include "net.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace net {

// ----------------------------------------------------------------- motion --
MotionQ MotionQ::from(const Body& b) {
    MotionQ q;
    auto clamp16 = [](double v) { return (int16_t)std::max(-32767.0, std::min(32767.0, v)); };
    q.x = (int32_t)std::llround(b.pos.x * 16.0);
    q.y = (int32_t)std::llround(b.pos.y * 16.0);
    q.vx = clamp16(std::llround(b.vel.x * 8.0));
    q.vy = clamp16(std::llround(b.vel.y * 8.0));
    float a = std::fmod(b.ang, 6.28318530718f);
    if (a < 0) a += 6.28318530718f;
    q.ang = (uint16_t)((int)std::lround(a * (65536.0f / 6.28318530718f)) & 0xFFFF);
    q.spin = clamp16(std::llround(b.angVel * 1024.0));
    return q;
}

void MotionQ::write(Writer& w) const {
    w.u32((uint32_t)x);  w.u32((uint32_t)y);
    w.u16((uint16_t)vx); w.u16((uint16_t)vy);
    w.u16(ang);          w.u16((uint16_t)spin);
}

MotionQ MotionQ::read(Reader& r) {
    MotionQ q;
    q.x = (int32_t)r.u32();  q.y = (int32_t)r.u32();
    q.vx = (int16_t)r.u16(); q.vy = (int16_t)r.u16();
    q.ang = r.u16();         q.spin = (int16_t)r.u16();
    return q;
}

// ------------------------------------------------------------------ host --
void HostReplicator::begin(World& world) {
    w = &world;
    w->journal = true;
    w->ops.clear();
    believed.clear();
    // Everything already in the world has to be introduced.
    for (int s = 0; s < (int)w->bodies.size(); ++s) {
        Body& b = w->bodies[s];
        if (!b.alive) continue;
        w->assignNetId(s);
    }
}

void HostReplicator::collectShapeChanges(std::vector<std::vector<uint8_t>>& out) {
    for (const WorldOp& op : w->ops) {
        Writer wr;
        switch (op.kind) {
        case WorldOp::Spawn:
            wr.u8((uint8_t)Msg::RockSpawn);
            wr.u32(op.id);
            wr.dvec(op.pos);
            wr.f32(op.radius);
            wr.u32(op.seed);
            ++rocksIntroduced;
            break;
        case WorldOp::Carve:
            wr.u8((uint8_t)Msg::RockCarve);
            wr.u32(op.id);
            wr.vec(op.local);
            wr.f32(op.radius);
            wr.f32(op.wobble);
            wr.u32(op.seed);
            ++carvesSent;
            break;
        case WorldOp::Split:
            wr.u8((uint8_t)Msg::RockSplit);
            wr.u32(op.id);
            wr.u32(op.firstChild);
            wr.u8(op.children);
            break;
        case WorldOp::Remove:
            wr.u8((uint8_t)Msg::RockRemove);
            wr.u32(op.id);
            break;
        case WorldOp::Settle:
            wr.u8((uint8_t)Msg::RockSettle);
            wr.u32(op.id);
            break;
        }
        shapeBytes += wr.size();
        out.push_back(std::move(wr.b));
    }
    w->ops.clear();
}

void HostReplicator::collectInitial(std::vector<std::vector<uint8_t>>& out) {
    for (int s = 0; s < (int)w->bodies.size(); ++s) {
        Body& b = w->bodies[s];
        if (!b.alive) continue;
        w->assignNetId(s);
        if (!b.authored && b.genRadius > 0.0f) {
            // Never shot: radius and seed rebuild it exactly.
            Writer wr;
            wr.u8((uint8_t)Msg::RockSpawn);
            wr.u32(b.netId);
            wr.dvec(b.pos);
            wr.f32(b.genRadius);
            wr.u32(b.seed);
            shapeBytes += wr.size();
            ++rocksIntroduced;
            out.push_back(std::move(wr.b));
        } else {
            std::vector<uint8_t> f;
            writeField(b.netId, f);
            out.push_back(std::move(f));
        }
    }
}

void HostReplicator::collectMotion(double now, std::vector<uint8_t>& out) {
    // Score every rock by how wrong the client's guess has become, and spend the
    // budget on the worst offenders. A rock drifting untouched costs nothing, apart
    // from the occasional refresh that guards against a lost packet.
    struct Cand { int slot; double err; };
    static std::vector<Cand> cands;
    cands.clear();

    for (int s : w->active) {
        Body& b = w->bodies[s];
        if (!b.alive || !b.netId) continue;
        auto it = believed.find(b.netId);
        if (it == believed.end() || !it->second.known) {
            cands.push_back({ s, 1e18 });            // never told them: highest priority
            continue;
        }
        const Believed& g = it->second;
        const double dt = now - g.at;
        // Where the client thinks it is, having integrated the last velocity it heard.
        const dv2 gp = g.q.pos();
        const v2  gv = g.q.vel();
        const dv2 guess(gp.x + gv.x * dt, gp.y + gv.y * dt);
        const double dx = b.pos.x - guess.x, dy = b.pos.y - guess.y;
        const double posErr = std::sqrt(dx * dx + dy * dy);
        const float  guessAng = g.q.angle() + g.q.spinRate() * (float)dt;
        const double angErr = std::fabs((double)wrapAngle(b.ang - guessAng));
        const double err = posErr / motionTolerance + angErr / angleTolerance + dt / refreshPeriod;
        if (err < 1.0) continue;                     // near enough: say nothing
        cands.push_back({ s, err });
    }
    if (cands.empty()) return;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.err > b.err; });
    const int n = std::min((int)cands.size(), motionsPerTick);

    Writer wr;
    wr.u8((uint8_t)Msg::RockMotion);
    wr.u8((uint8_t)n);
    for (int i = 0; i < n; ++i) {
        Body& b = w->bodies[cands[(size_t)i].slot];
        wr.u32(b.netId);
        const MotionQ q = MotionQ::from(b);
        q.write(wr);
        Believed g;
        g.q = q;  g.at = now;  g.known = true;
        believed[b.netId] = g;
        ++motionsSent;
    }
    motionBytes += wr.size();
    out = std::move(wr.b);
}

void HostReplicator::collectAudit(double now, std::vector<uint8_t>& out) {
    if (now < auditAt) return;
    auditAt = now + auditPeriod;

    // Every rock that has been shot, in a stable order. Each audit covers the next
    // slice after wherever the last one stopped, so that over a few periods every
    // rock is checked however many there are.
    static std::vector<std::pair<uint32_t, int>> shot;
    shot.clear();
    for (int s : w->active) {
        const Body& b = w->bodies[s];
        if (b.alive && b.netId && b.authored) shot.push_back({ b.netId, s });
    }
    if (shot.empty()) return;
    std::sort(shot.begin(), shot.end());
    size_t start = 0;
    while (start < shot.size() && shot[start].first <= auditCursor) ++start;
    if (start >= shot.size()) start = 0;                       // wrapped round: begin again

    Writer wr;
    wr.u8((uint8_t)Msg::RockAudit);
    const size_t countAt = wr.b.size();
    wr.u16(0);
    uint16_t n = 0;
    for (size_t i = start; i < shot.size() && n < auditPerMessage; ++i) {
        wr.u32(shot[i].first);
        const World::FieldSummary sm = w->summarise(shot[i].second);
        wr.u32(sm.solid);  wr.u16((uint16_t)sm.cx);  wr.u16((uint16_t)sm.cy);
        auditCursor = shot[i].first;
        ++n;
    }
    wr.b[countAt] = (uint8_t)n;
    wr.b[countAt + 1] = (uint8_t)(n >> 8);
    auditBytes += wr.size();
    out = std::move(wr.b);
}

void HostReplicator::writeField(uint32_t netId, std::vector<uint8_t>& out) {
    const int s = w->slotOfNetId(netId);
    if (s < 0) return;
    const Body& b = w->bodies[s];
    Writer wr;
    wr.u8((uint8_t)Msg::RockField);
    wr.u32(netId);
    wr.dvec(b.pos);
    wr.vec(b.vel);
    wr.f32(b.ang);
    wr.f32(b.angVel);
    wr.u32(b.seed);
    wr.u16((uint16_t)b.f.w);
    wr.u16((uint16_t)b.f.h);
    wr.f32(b.f.cell);
    wr.vec(b.f.origin);
    for (float v : b.f.d) wr.f32(v);
    fieldBytes += wr.size();
    ++repairsSent;
    out = std::move(wr.b);
}

// ---------------------------------------------------------------- client --
void ClientReplicator::begin(World& world) {
    w = &world;
    w->journal = false;          // a client never authors anything
    w->byNetId.clear();
}

bool ClientReplicator::applyReliable(const uint8_t* d, size_t n) {
    Reader r(d, n);
    const Msg m = (Msg)r.u8();
    switch (m) {
    case Msg::RockSpawn: {
        const uint32_t id = r.u32();
        const dv2 pos = r.dvec();
        const float radius = r.f32();
        const uint32_t seed = r.u32();
        if (r.bad) return false;
        if (w->slotOfNetId(id) >= 0) return true;              // already have it
        const int s = w->spawn(pos, radius, seed);
        if (s < 0) return false;
        w->bodies[s].netId = id;
        w->byNetId[id] = s;
        ++rocksKnown;
        return true;
    }
    case Msg::RockCarve: {
        const uint32_t id = r.u32();
        const v2 local = r.vec();
        const float radius = r.f32(), wobble = r.f32();
        const uint32_t seed = r.u32();
        if (r.bad) return false;
        const int s = w->slotOfNetId(id);
        if (s < 0) return false;
        v2 at = local;  float rad = radius;
        if (jitter > 0.0f) { at += jitterRng.disc() * jitter;  rad += jitterRng.sym(jitter); }
        w->damageLocal(s, at, rad, wobble, seed);
        w->bodies[s].dirty = false;                // only the host decides when a rock splits
        ++carvesApplied;
        return true;
    }
    case Msg::RockSplit: {
        const uint32_t id = r.u32();
        const uint32_t firstChild = r.u32();
        const uint8_t count = r.u8();
        if (r.bad) return false;
        const int s = w->slotOfNetId(id);
        if (s < 0) return false;
        // Run the same split, then name the pieces the way the host named them.
        // Both sides label the components in the same order, so going through
        // lastSplitPieces in order lines the two namings up. Slot numbers cannot
        // be used for this: the two machines allocate them independently.
        w->byNetId.erase(id);
        w->bodies[s].netId = 0;
        w->splitNow(s);
        int named = 0;
        for (int piece : w->lastSplitPieces) {
            if (named >= count) break;
            Body& b = w->bodies[piece];
            if (!b.alive) continue;
            b.netId = firstChild + (uint32_t)named;
            w->byNetId[b.netId] = piece;
            ++named;
        }
        ++splitsApplied;
        return named == count;
    }
    case Msg::RockSettle: {
        const uint32_t id = r.u32();
        if (r.bad) return false;
        const int s = w->slotOfNetId(id);
        if (s < 0) return true;                    // it was removed straight after
        w->bodies[s].dirty = false;
        w->refinalize(s, true);                    // the same re-centre the host did
        return true;
    }
    case Msg::RockRemove: {
        const uint32_t id = r.u32();
        if (r.bad) return false;
        const int s = w->slotOfNetId(id);
        if (s >= 0) w->destroy(s);
        return true;
    }
    case Msg::RockField: {
        const uint32_t id = r.u32();
        const dv2 pos = r.dvec();
        const v2 vel = r.vec();
        const float ang = r.f32(), angVel = r.f32();
        const uint32_t seed = r.u32();
        const int fw = r.u16(), fh = r.u16();
        const float cell = r.f32();
        const v2 origin = r.vec();
        if (r.bad || fw <= 0 || fh <= 0 || (size_t)fw * fh > 1u << 20) return false;
        int s = w->slotOfNetId(id);
        if (s < 0) {
            s = w->spawn(pos, 40.0f, seed);
            if (s < 0) return false;
            w->bodies[s].netId = id;
            w->byNetId[id] = s;
        }
        Body& b = w->bodies[s];
        b.pos = pos;  b.vel = vel;  b.ang = ang;  b.angVel = angVel;
        b.f.w = fw;  b.f.h = fh;  b.f.cell = cell;  b.f.origin = origin;
        b.f.d.resize((size_t)fw * fh);
        for (float& v : b.f.d) v = r.f32();
        if (r.bad) return false;
        b.dirty = b.geomDirty = b.authored = true;
        w->refinalize(s, false);       // the origin came with the field
        ++repairsApplied;
        repairAsked.erase(id);
        return true;
    }
    case Msg::RockAudit: {
        const uint16_t n = r.u16();
        diverged.clear();
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t id = r.u32();
            World::FieldSummary theirs;
            theirs.solid = r.u32();
            theirs.cx = (int16_t)r.u16();
            theirs.cy = (int16_t)r.u16();
            if (r.bad) return false;
            const int s = w->slotOfNetId(id);
            auto asked = repairAsked.find(id);
            if (asked != repairAsked.end() && now - asked->second < 3.0) continue;   // already on its way
            if (s < 0) { diverged.push_back(id); repairAsked[id] = now; continue; }   // they have a rock we lack
            if (!World::summariesClose(w->summarise(s), theirs)) { diverged.push_back(id); repairAsked[id] = now; }
        }
        return true;
    }
    default:
        return false;
    }
}

bool ClientReplicator::applyUnreliable(const uint8_t* d, size_t n) {
    Reader r(d, n);
    const Msg m = (Msg)r.u8();
    if (m != Msg::RockMotion) return false;
    const uint8_t count = r.u8();
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t id = r.u32();
        const MotionQ q = MotionQ::read(r);
        if (r.bad) return false;
        const int s = w->slotOfNetId(id);
        if (s < 0) continue;
        Body& b = w->bodies[s];
        b.pos = q.pos();  b.vel = q.vel();  b.ang = q.angle();  b.angVel = q.spinRate();
        b.refreshTrig();
        ++motionsApplied;
    }
    return true;
}

void ClientReplicator::deadReckon(float dt) {
    for (int s = 0; s < (int)w->bodies.size(); ++s) {
        Body& b = w->bodies[s];
        if (!b.alive) continue;
        b.pos.x += (double)b.vel.x * dt;
        b.pos.y += (double)b.vel.y * dt;
        b.ang = wrapAngle(b.ang + b.angVel * dt);
        b.refreshTrig();
    }
    now += dt;
}

}  // namespace net
