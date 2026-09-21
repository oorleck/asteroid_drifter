#include "world.h"
#include <cstdio>

// --------------------------------------------------------------- lifetime --
void World::init(uint64_t worldSeed) {
    seed = worldSeed;
    bodies.clear();
    bodies.reserve(cfg::MAX_BODIES);       // slots never move once reserved
    freeSlots.clear();
    loaded.clear();
    saved.clear();
    active.clear();
    pendingFree.clear();
    events.clear();
    zones.clear();
    ops.clear();  byNetId.clear();  nextNetId = 1;  journal = false;
    liveCount = 0;
}

static Col rockColour(uint32_t s) {
    Rng rng(s ^ 0xA53Cu);
    // Cool steel by default, with the occasional warmer or icier rock.
    const float t = rng.f();
    Col c;
    if (t < 0.12f)      c = Col(0.55f, 0.42f, 0.26f);      // rusty
    else if (t < 0.24f) c = Col(0.30f, 0.46f, 0.55f);      // icy
    else if (t < 0.30f) c = Col(0.46f, 0.30f, 0.50f);      // violet
    else                c = Col(0.40f, 0.48f, 0.52f);      // steel
    const float b = rng.range(1.7f, 2.6f);     // pushed well over the bloom threshold
    return Col(c.r * b, c.g * b, c.b * b, 1.0f);
}

// ---------------------------------------------------------- journalling --
uint32_t World::assignNetId(int slot) {
    Body& b = bodies[slot];
    if (b.netId == 0) {
        b.netId = nextNetId++;
        byNetId[b.netId] = slot;
    }
    return b.netId;
}

int World::slotOfNetId(uint32_t id) const {
    auto it = byNetId.find(id);
    if (it == byNetId.end()) return -1;
    const int s = it->second;
    if (s < 0 || s >= (int)bodies.size()) return -1;
    return (bodies[s].alive && bodies[s].netId == id) ? s : -1;
}

// A rock's field is a plain grid of floats, so this is enough to notice that two
// machines have stopped agreeing about a rock. It is a check, not a signature.
uint32_t World::fieldHash(int slot) const {
    if (slot < 0 || slot >= (int)bodies.size()) return 0;
    const Body& b = bodies[slot];
    if (!b.alive) return 0;
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix((uint32_t)b.f.w);  mix((uint32_t)b.f.h);
    // Quantised, so the last bit of float noise does not read as a disagreement.
    for (size_t i = 0; i < b.f.d.size(); i += 3)
        mix((uint32_t)(int32_t)std::lround(b.f.d[i] * 16.0f));
    return h;
}

bool World::damageLocal(int slot, v2 localPos, float radius, float wobble, uint32_t rseed) {
    if (slot < 0 || slot >= (int)bodies.size()) return false;
    Body& b = bodies[slot];
    if (!b.alive || b.f.d.empty()) return false;
    if (!fieldCarveDisc(b.f, localPos, radius, wobble, rseed)) return false;
    b.dirty      = true;
    b.geomDirty  = true;
    b.authored   = true;
    b.flash      = 1.0f;
    b.lastImpact = localPos;
    if (journal) {
        WorldOp op;
        op.kind = WorldOp::Carve;
        op.id = assignNetId(slot);
        op.local = localPos;
        op.radius = radius;
        op.wobble = wobble;
        op.seed = rseed;
        ops.push_back(op);
    }
    return true;
}

int World::spawn(dv2 p, float radius, uint32_t rseed) {
    int slot;
    if (!freeSlots.empty()) {
        slot = freeSlots.back();
        freeSlots.pop_back();
    } else {
        if ((int)bodies.size() >= cfg::MAX_BODIES) return -1;
        bodies.push_back(Body());
        slot = (int)bodies.size() - 1;
    }
    Body& b = bodies[slot];
    const uint32_t keepGen = b.gen;
    b = Body();
    b.gen   = keepGen;
    b.pos   = p;
    b.seed  = rseed;
    b.color = rockColour(rseed);
    b.genRadius = radius;
    b.alive = true;
    fieldMakeRock(b.f, radius, rseed);
    finalizeBody(slot, true);
    if (!bodies[slot].alive) return -1;
    if (journal) {
        WorldOp op;
        op.kind = WorldOp::Spawn;
        op.id = assignNetId(slot);
        op.pos = p;
        op.radius = radius;          // the requested radius: what rebuilds the shape
        op.seed = rseed;
        ops.push_back(op);
    }
    return slot;
}

void World::clearZone(dv2 centre, double radius) {
    zones.push_back({ centre, radius });
    for (size_t i = 0; i < bodies.size(); ++i) {
        const Body& b = bodies[i];
        if (!b.alive) continue;
        const double dx = b.pos.x - centre.x, dy = b.pos.y - centre.y;
        const double need = radius + b.radius;
        if (dx * dx + dy * dy < need * need) destroy((int)i);
    }
}

bool World::zoneBlocks(dv2 p, double r) const {
    for (const Zone& z : zones) {
        const double dx = p.x - z.pos.x, dy = p.y - z.pos.y;
        const double need = z.r + r;
        if (dx * dx + dy * dy < need * need) return true;
    }
    return false;
}

bool World::spaceFree(dv2 p, double radius, double margin) const {
    for (const Body& b : bodies) {
        if (!b.alive) continue;
        const double dx = p.x - b.pos.x, dy = p.y - b.pos.y;
        const double need = (radius + b.radius) * margin;
        if (dx * dx + dy * dy < need * need) return false;
    }
    return true;
}

void World::destroy(int slot) {
    Body& b = bodies[slot];
    if (!b.alive) return;
    if (journal && b.netId) {
        byNetId.erase(b.netId);
        if (!splitting) {                  // a split reports the parent itself
            WorldOp op;
            op.kind = WorldOp::Remove;
            op.id = b.netId;
            ops.push_back(op);
        }
    }
    b.netId = 0;
    b.alive = false;
    b.gen++;
    if (b.geom.valid()) pendingFree.push_back(b.geom);
    b.geom = GeomSlot();
    b.f = Field();
    b.pts.clear();  b.pts.shrink_to_fit();
    b.loops.clear(); b.loops.shrink_to_fit();
    b.heat.clear();  b.heat.shrink_to_fit();
    freeSlots.push_back(slot);
}

// Recomputes mass properties and moves the local origin back onto the centre
// of mass, which keeps the rotational dynamics correct as rock is shot away.
void World::finalizeBody(int slot, bool recenter) {
    Body& b = bodies[slot];
    MassProps mp = fieldMass(b.f);
    if (mp.area < 1.0f) { destroy(slot); return; }
    if (recenter) {
        b.refreshTrig();
        const v2 wcom = b.dirToWorld(mp.com);
        b.pos += dv2(wcom.x, wcom.y);
        // The point that becomes the new origin was already moving with the spin.
        b.vel += perp(wcom) * b.angVel;
        b.lastImpact -= mp.com;
        if (!fieldCropAndCenter(b.f, mp.com)) { destroy(slot); return; }
        mp = fieldMass(b.f);
    }
    b.mass       = std::max(1.0f, mp.area * cfg::DENSITY);
    b.invMass    = 1.0f / b.mass;
    b.inertia    = std::max(1.0f, mp.inertia * cfg::DENSITY);
    b.invInertia = 1.0f / b.inertia;
    b.dirty      = false;
    b.geomDirty  = true;
    b.refreshTrig();

    // A usable bounding radius straight away. The contour pass refines it, but it
    // only rebuilds a few rocks per frame, and until a rock's turn came round its
    // radius was 1: every hit-test and collision treated it as a single point.
    float r2 = 0;
    for (int y = 0; y < b.f.h; ++y)
        for (int x = 0; x < b.f.w; ++x)
            if (b.f.at(x, y) > 0.0f) r2 = std::max(r2, len2(b.f.samplePos(x, y)));
    b.radius = std::sqrt(r2) + b.f.cell;
}

void World::rebuildGeometry(int slot, Renderer& r) {
    Body& b = bodies[slot];
    fieldContours(b.f, b.pts, b.loops);
    if (b.pts.empty() || b.loops.empty()) { destroy(slot); return; }

    float r2max = 0;
    for (const v2& p : b.pts) r2max = std::max(r2max, len2(p));
    b.radius = std::sqrt(r2max);

    // Per-vertex glow around the most recent impact point.
    b.heat.resize(b.pts.size());
    const float heatR = std::max(18.0f, b.f.cell * 6.0f);
    for (size_t i = 0; i < b.pts.size(); ++i)
        b.heat[i] = clampf(1.0f - len(b.pts[i] - b.lastImpact) / heatR, 0.0f, 1.0f);

    // Mirror every loop slightly inward. The dim second outline is what makes
    // a bare contour read as a solid rock instead of a soap bubble.
    static std::vector<v2>    vbuf;
    static std::vector<float> hbuf;
    vbuf.assign(b.pts.begin(), b.pts.end());
    hbuf.assign(b.heat.begin(), b.heat.end());
    const float inset = std::max(2.2f, b.f.cell * 0.85f);
    const size_t outerLoops = b.loops.size();
    for (size_t li = 0; li < outerLoops; ++li) {
        const Loop L = b.loops[li];
        if (L.count < 8) continue;
        v2 lo = b.pts[L.first], hi = lo;
        for (int i = 1; i < L.count; ++i) {
            const v2 p = b.pts[L.first + i];
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
        }
        if (len(hi - lo) < inset * 5.0f) continue;      // too tight to inset cleanly
        const int first = (int)vbuf.size();
        for (int i = 0; i < L.count; ++i) {
            const v2 p = b.pts[L.first + i];
            const v2 g = b.f.gradient(p);
            const float gl = len(g);
            vbuf.push_back(gl > 1e-6f ? p + g * (inset / gl) : p);
            hbuf.push_back(-1.0f);
        }
        b.loops.push_back({first, L.count});
    }

    b.geom = r.uploadBody(vbuf.data(), (int)vbuf.size(), (uint32_t)slot,
                          hbuf.data(), b.geom);
    b.geomDirty = false;
}

// Damage first: a carved field may have fallen apart into several rocks, or just
// moved its centre of mass. Budgeted, so a big blast is spread over a few frames.
void World::settleDirty() {
    rebuildsThisFrame = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (rebuildsThisFrame >= cfg::REBUILD_BUDGET) break;
        if (!bodies[i].alive || !bodies[i].dirty) continue;
        splitBody((int)i);
        ++rebuildsThisFrame;
    }
}

void World::syncGeometry(Renderer& r) {
    for (GeomSlot s : pendingFree) r.freeBody(s);
    pendingFree.clear();

    settleDirty();
    // Then re-contour whatever changed shape.
    int uploads = 0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (uploads >= cfg::REBUILD_BUDGET * 3) break;
        if (!bodies[i].alive || !bodies[i].geomDirty) continue;
        rebuildGeometry((int)i, r);
        ++uploads;
    }
    if (r.arenaOverflowed()) {
        // The arena filled up: grow it, then re-upload every live contour.
        r.arenaReset();
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (!bodies[i].alive) continue;
            bodies[i].geom = GeomSlot();
            bodies[i].geomDirty = true;
        }
        for (size_t i = 0; i < bodies.size(); ++i)
            if (bodies[i].alive) rebuildGeometry((int)i, r);
    }
}

void World::markAllDirty() {
    for (Body& b : bodies) if (b.alive) b.geomDirty = true;
}

size_t World::fieldBytes() const {
    size_t n = 0;
    for (const Body& b : bodies) if (b.alive) n += b.f.bytes();
    return n;
}

// ------------------------------------------------------- chunk streaming --
void World::loadChunk(i64 cx, i64 cy) {
    const i64 k = chunkKey(cx, cy);
    auto it = saved.find(k);
    if (it != saved.end()) {
        // This chunk has been shot at before: restore exactly what we stored.
        for (SavedBody& sb : it->second.bodies) {
            if (zoneBlocks(sb.pos, 160.0)) continue;     // the goal keeps its space
            int slot;
            if (!freeSlots.empty()) { slot = freeSlots.back(); freeSlots.pop_back(); }
            else if ((int)bodies.size() < cfg::MAX_BODIES) { bodies.push_back(Body()); slot = (int)bodies.size() - 1; }
            else break;
            Body& b = bodies[slot];
            const uint32_t keepGen = b.gen;
            b = Body();
            b.gen    = keepGen;
            b.pos    = sb.pos;
            b.vel    = sb.vel;
            b.ang    = sb.ang;
            b.angVel = sb.angVel;
            b.seed   = sb.seed;
            b.color  = sb.color;
            b.f      = std::move(sb.f);
            b.alive  = true;
            b.authored = true;
            finalizeBody(slot, false);
            if (journal && bodies[slot].alive) { assignNetId(slot); fullSync.push_back(bodies[slot].netId); }
        }
        saved.erase(it);
        return;
    }

    Rng rng(hashCombine(seed, hashCombine((uint64_t)cx * 0x9E3779B1ull,
                                          (uint64_t)cy * 0x85EBCA6Bull)));
    const int n = rng.i(2, 4);
    for (int i = 0; i < n; ++i) {
        const float radius = rng.f() < 0.08f ? rng.range(118.0f, 178.0f)
                                             : rng.range(26.0f, 96.0f);
        // Try a few spots; give up rather than pile rocks on top of each other.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const dv2 p(cx * cfg::CHUNK + rng.f() * cfg::CHUNK,
                        cy * cfg::CHUNK + rng.f() * cfg::CHUNK);
            // The lumpy silhouette can exceed the nominal radius by ~35%, so
            // reserve room for the real bounding circle, not the requested one.
            if (!spaceFree(p, radius * 1.42, 1.10)) continue;
            if (zoneBlocks(p, radius * 1.42)) continue;      // keep the goal clear
            const uint32_t rockSeed = rng.u32();
            const int slot = spawn(p, radius, rockSeed);
            if (slot >= 0) {
                bodies[slot].vel = rng.disc() * 6.0f;
                (void)rng.f();                                // (this used to be the spin: the draw stays, so a seed still lays out the same rocks)
                // Every rock is born spinning, at a speed drawn from a normal distribution. It has
                // its own generator, seeded by the rock, so nothing else about the world moves.
                Rng spin(hashCombine((uint64_t)rockSeed, 0x51D1ull));
                bodies[slot].angVel = clampf(spin.normal() * cfg::SPIN_SIGMA, -3.0f * cfg::SPIN_SIGMA, 3.0f * cfg::SPIN_SIGMA);
            }
            break;
        }
    }
}

void World::unloadChunk(i64 cx, i64 cy) {
    const i64 k = chunkKey(cx, cy);
    static std::vector<int> mine;
    mine.clear();
    bool anyAuthored = false;
    for (size_t i = 0; i < bodies.size(); ++i) {
        Body& b = bodies[i];
        if (!b.alive) continue;
        if ((i64)std::floor(b.pos.x / cfg::CHUNK) != cx) continue;
        if ((i64)std::floor(b.pos.y / cfg::CHUNK) != cy) continue;
        mine.push_back((int)i);
        anyAuthored |= b.authored;
    }
    if (anyAuthored) {
        SavedChunk& bucket = saved[k];
        bucket.stamp = ++saveStamp;
        bucket.bodies.clear();
        bucket.bodies.reserve(mine.size());
        for (int s : mine) {
            Body& b = bodies[s];
            SavedBody sb;
            sb.pos = b.pos; sb.vel = b.vel; sb.ang = b.ang; sb.angVel = b.angVel;
            sb.seed = b.seed; sb.color = b.color; sb.f = std::move(b.f);
            bucket.bodies.push_back(std::move(sb));
        }
        // Bound how much shot-up terrain we hold on to: drop the half that
        // has gone longest without being visited.
        if (saved.size() > 2048) {
            std::vector<uint64_t> stamps;
            stamps.reserve(saved.size());
            for (const auto& kv : saved) stamps.push_back(kv.second.stamp);
            std::nth_element(stamps.begin(), stamps.begin() + stamps.size() / 2, stamps.end());
            const uint64_t cut = stamps[stamps.size() / 2];
            for (auto it2 = saved.begin(); it2 != saved.end(); ) {
                if (it2->second.stamp < cut) it2 = saved.erase(it2);
                else                         ++it2;
            }
        }
    }
    for (int s : mine) destroy(s);
}

void World::streamChunks(dv2 centre, double viewRadius, int loadBudget) {
    liveCount = (int)bodies.size() - (int)freeSlots.size();
    const i64 ccx = (i64)std::floor(centre.x / cfg::CHUNK);
    const i64 ccy = (i64)std::floor(centre.y / cfg::CHUNK);
    int R = (int)std::ceil(viewRadius / cfg::CHUNK) + 1;
    if (R < 2)  R = 2;
    if (R > 26) R = 26;

    // Walk outward in rings so the budget is always spent on the chunks the
    // player is closest to; the far ones fill in over the next few frames.
    int budget = loadBudget;
    for (int ring = 0; ring <= R && budget > 0; ++ring) {
        for (i64 cy = ccy - ring; cy <= ccy + ring && budget > 0; ++cy) {
            const bool edgeRow = (cy == ccy - ring) || (cy == ccy + ring);
            for (i64 cx = ccx - ring; cx <= ccx + ring && budget > 0; ++cx) {
                if (!edgeRow && cx != ccx - ring && cx != ccx + ring) continue;
                const i64 k = chunkKey(cx, cy);
                if (loaded.find(k) != loaded.end()) continue;
                if (liveCount >= cfg::MAX_BODIES - 8) continue;
                loadChunk(cx, cy);
                loaded[k] = ChunkCoord{cx, cy};
                liveCount = (int)bodies.size() - (int)freeSlots.size();
                --budget;
            }
        }
    }

    pendingUnload.clear();
    const i64 keep = R + 2;
    for (const auto& kv : loaded) {
        const i64 dx = kv.second.cx - ccx, dy = kv.second.cy - ccy;
        if (dx > keep || dx < -keep || dy > keep || dy < -keep)
            pendingUnload.push_back(kv.first);
    }
    for (i64 k : pendingUnload) {
        const ChunkCoord c = loaded[k];
        unloadChunk(c.cx, c.cy);
        loaded.erase(k);
    }
}

// ------------------------------------------------------------ broadphase --
void World::Grid::build(const World& w, dv2 centre, double radius) {
    cell = 640.0;
    origin = dv2(centre.x - radius, centre.y - radius);
    nx = ny = (int)std::ceil(2.0 * radius / cell) + 1;
    if (nx > 512) nx = ny = 512;
    head.assign((size_t)nx * ny, -1);
    next.assign(w.bodies.size(), -1);
    for (int s : w.active) {
        const Body& b = w.bodies[s];
        const int x = (int)std::floor((b.pos.x - origin.x) / cell);
        const int y = (int)std::floor((b.pos.y - origin.y) / cell);
        if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
        const size_t c = (size_t)y * nx + x;
        next[s] = head[c];
        head[c] = s;
    }
}

int World::allocSlot() {
    if (!freeSlots.empty()) {
        const int s = freeSlots.back();
        freeSlots.pop_back();
        return s;
    }
    if ((int)bodies.size() >= cfg::MAX_BODIES) return -1;
    bodies.push_back(Body());
    return (int)bodies.size() - 1;
}

// --------------------------------------------------------------- collision --
// Samples one rock's contour against the other rock's field. Because both are
// distance fields, the sample value *is* the penetration depth and the gradient
// *is* the surface normal, so irregular shot-up shapes collide correctly.
void World::narrowphase(int ia, int ib) {
    Body& A = bodies[ia];
    Body& B = bodies[ib];
    // Probe with whichever outline has fewer points.
    int ti = ia, pi = ib;
    if (B.pts.size() < A.pts.size()) { ti = ib; pi = ia; }
    const Body& T = bodies[ti];
    const Body& P = bodies[pi];
    if (P.pts.empty() || T.f.d.empty()) return;

    const int stride = std::max(1, (int)P.pts.size() / 40);
    const float tr2 = T.radius * T.radius;
    const size_t before = contacts.size();
    for (size_t i = 0; i < P.pts.size(); i += stride) {
        const dv2 wp = P.toWorld(P.pts[i]);
        const v2  lp = T.toLocal(wp);
        if (len2(lp) > tr2) continue;
        const float d = T.f.sample(lp);
        if (d <= 0.0f) continue;
        const v2 g = T.f.gradient(lp);
        if (len2(g) < 1e-12f) continue;
        ContactPoint c;
        c.a = ti;
        c.b = pi;
        c.p = wp;
        c.n = T.dirToWorld(norm(g) * -1.0f);   // out of T, toward P
        c.depth = d;
        c.w = 1.0f;
        contacts.push_back(c);
        if (contacts.size() - before >= 12) break;
    }
    const float w = (contacts.size() > before) ? 1.0f / (float)(contacts.size() - before) : 1.0f;
    for (size_t i = before; i < contacts.size(); ++i) contacts[i].w = w;
}

void World::solveContacts(float dt) {
    const float restitution = 0.06f;
    const float friction    = 0.45f;
    for (int iter = 0; iter < 3; ++iter) {
        for (ContactPoint& c : contacts) {
            Body& A = bodies[c.a];
            Body& B = bodies[c.b];
            if (!A.alive || !B.alive) continue;
            const v2 rA = tov2(c.p - A.pos);
            const v2 rB = tov2(c.p - B.pos);
            const v2 rv = B.velAt(rB) - A.velAt(rA);
            const float rnA = cross(rA, c.n), rnB = cross(rB, c.n);
            const float k = A.invMass + B.invMass
                          + A.invInertia * rnA * rnA + B.invInertia * rnB * rnB;
            if (k < 1e-12f) continue;

            const float vn = dot(rv, c.n);
            // Baumgarte term pushes overlapping rock apart. The cap matters:
            // two rocks that spawn deeply merged would otherwise be launched
            // across the system on the first frame.
            const float bias = std::min(0.25f * std::max(0.0f, c.depth - 0.4f) / dt,
                                        cfg::MAX_SEPARATION_SPEED);
            float j = (-(1.0f + restitution) * vn + bias) / k * c.w;
            if (j < 0.0f) j = 0.0f;
            if (iter == 0 && j * k > 260.0f && c.depth > 1.0f) {
                WorldEvent e;
                e.pos = c.p; e.vel = rv * -0.25f; e.size = 1.0f;
                e.kind = WorldEvent::Impact; e.col = A.color;
                if (events.size() < 512) events.push_back(e);
            }
            A.vel    -= c.n * (j * A.invMass);
            A.angVel -= rnA * j * A.invInertia;
            B.vel    += c.n * (j * B.invMass);
            B.angVel += rnB * j * B.invInertia;

            const v2 t = perp(c.n);
            const float rtA = cross(rA, t), rtB = cross(rB, t);
            const float kt = A.invMass + B.invMass
                           + A.invInertia * rtA * rtA + B.invInertia * rtB * rtB;
            if (kt < 1e-12f) continue;
            float jt = -dot(rv, t) / kt * c.w;
            const float lim = friction * j;
            jt = clampf(jt, -lim, lim);
            A.vel    -= t * (jt * A.invMass);
            A.angVel -= rtA * jt * A.invInertia;
            B.vel    += t * (jt * B.invMass);
            B.angVel += rtB * jt * B.invInertia;
        }
    }
}

void World::step(float dt, dv2 focus) {
    liveCount = (int)bodies.size() - (int)freeSlots.size();
    active.clear();
    maxBodyRadius = 64.0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        Body& b = bodies[i];
        if (!b.alive) continue;
        active.push_back((int)i);
        if (b.radius > maxBodyRadius) maxBodyRadius = b.radius;
    }
    grid.build(*this, focus, cfg::SIM_RADIUS + 3000.0);

    const double simR2 = cfg::SIM_RADIUS * cfg::SIM_RADIUS;
    simCount = 0;
    for (int s : active) {
        Body& b = bodies[s];
        const double dx = b.pos.x - focus.x, dy = b.pos.y - focus.y;
        b.simulated = (dx * dx + dy * dy) < simR2;
        b.pos.x += (double)b.vel.x * dt;
        b.pos.y += (double)b.vel.y * dt;
        b.ang = wrapAngle(b.ang + b.angVel * dt);
        b.flash = approach(b.flash, 0.0f, 3.2f, dt);
        // A whisper of damping. Not physical in vacuum, but it bleeds off the
        // energy the positional contact solver injects and keeps the field calm.
        b.vel    *= std::exp(-0.05f * dt);
        b.angVel *= std::exp(-cfg::SPIN_DAMPING * dt);
        b.refreshTrig();
        if (b.simulated) ++simCount;
    }

    contacts.clear();
    for (int ia : active) {
        Body& A = bodies[ia];
        if (!A.simulated) continue;
        const double reach = A.radius + maxBodyRadius;
        grid.forEachNear(*this, A.pos, reach, [&](int ib) {
            if (ib <= ia) return;                       // each pair once
            Body& B = bodies[ib];
            if (!B.simulated) return;
            const double dx = B.pos.x - A.pos.x, dy = B.pos.y - A.pos.y;
            const double rr = (double)A.radius + B.radius;
            if (dx * dx + dy * dy > rr * rr) return;
            narrowphase(ia, ib);
        });
    }
    contactsThisFrame = (int)contacts.size();
    if (!contacts.empty()) {
        solveContacts(dt);
        // Destructible bodies change mass mid-collision, so belt-and-braces
        // clamps keep one bad contact from launching a rock out of the system.
        for (int s : active) {
            Body& b = bodies[s];
            if (!b.simulated) continue;
            const float sp = len(b.vel);
            if (sp > cfg::MAX_BODY_SPEED) b.vel = b.vel * (cfg::MAX_BODY_SPEED / sp);
            b.angVel = clampf(b.angVel, -cfg::MAX_BODY_SPIN, cfg::MAX_BODY_SPIN);
        }
    }
}

// ---------------------------------------------------------------- queries --
v2 World::gravityAt(dv2 p, double reach, int* dominant) const {
    v2 a(0, 0);
    double best = 0;
    int bestIdx = -1;
    grid.forEachNear(*this, p, reach, [&](int i) {
        const Body& b = bodies[i];
        if (!b.alive) return;
        const double dx = b.pos.x - p.x, dy = b.pos.y - p.y;
        const double r2 = dx * dx + dy * dy;
        // Softening keeps the pull finite when you are standing on the rock.
        const double soft = 0.30 * (double)b.radius * b.radius;
        const double denom = std::pow(r2 + soft, 1.5);
        const double m = (double)cfg::GRAV_CONST * b.mass / denom;
        a.x += (float)(dx * m);
        a.y += (float)(dy * m);
        const double strength = m * std::sqrt(r2 + soft);
        if (strength > best) { best = strength; bestIdx = i; }
    });
    if (dominant) *dominant = bestIdx;
    return a;
}

int World::probe(dv2 p, float r, v2* normal, float* depth, int ignore) const {
    int hit = -1;
    float bestDepth = -1e30f;
    grid.forEachNear(*this, p, (double)r + maxBodyRadius, [&](int i) {
        if (i == ignore) return;
        const Body& b = bodies[i];
        if (!b.alive || b.f.d.empty()) return;
        const double dx = p.x - b.pos.x, dy = p.y - b.pos.y;
        const double rr = (double)b.radius + r;
        if (dx * dx + dy * dy > rr * rr) return;
        const v2 lp = b.toLocal(p);
        const float d = b.f.sample(lp) + r;           // inflate by the probe radius
        if (d <= 0.0f || d <= bestDepth) return;
        const v2 g = b.f.gradient(lp);
        if (len2(g) < 1e-12f) return;
        bestDepth = d;
        hit = i;
        if (normal) *normal = b.dirToWorld(norm(g) * -1.0f);
        if (depth)  *depth  = d;
    });
    return hit;
}

int World::solidAt(dv2 p, v2* localOut) const {
    int hit = -1;
    grid.forEachNear(*this, p, maxBodyRadius, [&](int i) {
        if (hit >= 0) return;
        const Body& b = bodies[i];
        if (!b.alive || b.f.d.empty()) return;
        const double dx = p.x - b.pos.x, dy = p.y - b.pos.y;
        if (dx * dx + dy * dy > (double)b.radius * b.radius) return;
        const v2 lp = b.toLocal(p);
        if (b.f.sample(lp) > 0.0f) {
            hit = i;
            if (localOut) *localOut = lp;
        }
    });
    return hit;
}

// ------------------------------------------------------------ destruction --
bool World::damage(int slot, dv2 worldPos, float radius, float wobble, uint32_t rseed) {
    if (slot < 0 || slot >= (int)bodies.size()) return false;
    Body& b = bodies[slot];
    if (!b.alive || b.f.d.empty()) return false;
    // One carve routine, so everything that chews rock is journalled in one place.
    return damageLocal(slot, b.toLocal(worldPos), radius, wobble, rseed);
}

void World::explode(dv2 c, float carveR, float impulseR, float impulse, uint32_t seed) {
    for (int s : active) {
        Body& b = bodies[s];
        if (!b.alive) continue;
        const double dx = b.pos.x - c.x, dy = b.pos.y - c.y;
        const float  d  = (float)std::sqrt(dx * dx + dy * dy);
        if (d > impulseR + b.radius && d > carveR + b.radius) continue;

        // The crater is a plain disc, so a rock at the edge of the blast is only
        // bitten and one at the centre is simply gone.
        if (d < carveR + b.radius)
            damage(s, c, carveR, 0.22f, seed + (uint32_t)s * 2654435761u);

        if (d < impulseR) {
            Rng r(seed ^ ((uint32_t)s * 40503u));
            const v2 dir = d > 1e-3f ? v2((float)dx, (float)dy) / d : r.dir();
            const float f = 1.0f - d / impulseR;
            // Momentum, so light pieces fly and a mountain hardly stirs.
            b.vel    += dir * (impulse * f * f * b.invMass);
            b.angVel += r.sym(1.4f) * f;
        }
    }
}

// Re-labels the field; anything that came loose becomes its own body (or a
// puff of debris if it is too small to be worth simulating).
void World::splitBody(int slot) {
    Body& b = bodies[slot];
    static std::vector<int> labels;
    const int nc = fieldLabelComponents(b.f, labels);
    if (nc <= 0) { destroy(slot); return; }
    if (nc == 1) {
        if (journal && b.netId) {                 // it stayed one rock but its centre of mass moved
            WorldOp op;
            op.kind = WorldOp::Settle;
            op.id = b.netId;
            ops.push_back(op);
        }
        finalizeBody(slot, true);
        return;
    }

    const dv2   pos    = b.pos;
    const v2    vel    = b.vel;
    const float ang    = b.ang,  angVel = b.angVel;
    const Col   col    = b.color;
    const uint32_t sd  = b.seed;
    const v2    impact = b.lastImpact;
    const float ca = b.cosA, sa = b.sinA;
    Field parent = std::move(b.f);
    b.f = Field();
    const uint32_t parentId = journal ? b.netId : 0;
    splitting = true;
    destroy(slot);
    splitting = false;
    uint32_t firstChild = 0;
    uint8_t  childCount = 0;
    lastSplitPieces.clear();

    Rng rng(hashCombine(sd, (uint64_t)nc * 7919ull));
    for (int c = 0; c < nc; ++c) {
        Field piece;
        if (!fieldExtractComponent(parent, labels, c, piece)) continue;
        const MassProps mp = fieldMass(piece);
        const v2 wcom = rot(mp.com, ca, sa);

        if (mp.area < cfg::MIN_PIECE) {
            WorldEvent e;
            e.pos  = dv2(pos.x + wcom.x, pos.y + wcom.y);
            e.vel  = vel + perp(wcom) * angVel + rng.dir() * rng.range(25.0f, 80.0f);
            e.size = std::sqrt(std::max(1.0f, mp.area));
            e.kind = WorldEvent::Debris;
            e.col  = col;
            if (events.size() < 512) events.push_back(e);
            continue;
        }
        const int ns = allocSlot();
        if (ns < 0) continue;
        Body& nb = bodies[ns];
        const uint32_t keepGen = nb.gen;
        nb = Body();
        nb.gen    = keepGen;
        nb.pos    = pos;
        nb.vel    = vel;
        nb.ang    = ang;
        nb.angVel = angVel;
        nb.color  = col;
        nb.seed   = sd ^ ((uint32_t)c * 2654435761u);
        nb.f      = std::move(piece);
        nb.alive  = true;
        nb.authored = true;
        nb.flash  = 1.0f;
        nb.lastImpact = impact;
        nb.refreshTrig();
        finalizeBody(ns, true);                 // moves origin onto the new COM
        if (!bodies[ns].alive) continue;
        // Nudge the pieces apart so a fresh cut visibly opens up.
        bodies[ns].vel    += norm(wcom) * rng.range(6.0f, 22.0f);
        bodies[ns].angVel += rng.sym(0.25f);
        lastSplitPieces.push_back(ns);
        if (journal) {
            const uint32_t cid = assignNetId(ns);
            if (childCount == 0) firstChild = cid;
            ++childCount;
        }

        WorldEvent e;
        e.pos  = bodies[ns].pos;
        e.vel  = bodies[ns].vel;
        e.size = bodies[ns].radius;
        e.kind = WorldEvent::Split;
        e.col  = col;
        if (events.size() < 512) events.push_back(e);
    }
    if (journal && parentId) {
        WorldOp op;
        op.kind = WorldOp::Split;
        op.id = parentId;
        op.firstChild = firstChild;
        op.children = childCount;
        ops.push_back(op);
    }
}

// ------------------------------------------------------------- rendering --
void World::collectRenderData(const Camera& cam, Renderer& r,
                              std::vector<BodyXform>& xf,
                              std::vector<int>& firsts, std::vector<int>& counts) {
    xf.assign(bodies.size(), BodyXform{});
    firsts.clear();
    counts.clear();
    const double hw = cam.halfW * 1.06, hh = cam.halfH() * 1.06;
    const float pxPerUnit = (float)r.fbw / (2.0f * cam.halfW);
    const double cc = std::cos(cam.angle), ss = std::sin(cam.angle);

    for (int s : active) {
        Body& b = bodies[s];
        const double dx = b.pos.x - cam.pos.x, dy = b.pos.y - cam.pos.y;
        // Cull in camera space, so a rotated POV camera still culls exactly.
        // The transform itself stays unrotated; the vertex shader spins it.
        const double rx = dx * cc - dy * ss;
        const double ry = dx * ss + dy * cc;
        if (std::fabs(rx) > hw + b.radius || std::fabs(ry) > hh + b.radius) continue;

        BodyXform& x = xf[s];
        x.px = (float)dx; x.py = (float)dy;
        x.cs = b.cosA;    x.sn = b.sinA;
        x.r = b.color.r;  x.g = b.color.g; x.b = b.color.b;
        x.flash = b.flash;

        // Anything only a couple of pixels across is drawn as a single dot.
        const float screenR = b.radius * pxPerUnit;
        if (screenR < 2.0f) {
            r.point(v2((float)dx, (float)dy), clampf(screenR * 1.8f, 1.0f, 3.0f), b.color, 1.4f);
            continue;
        }
        if (!b.geom.valid()) continue;
        for (const Loop& L : b.loops) {
            firsts.push_back(b.geom.base + L.first);
            counts.push_back(L.count);
        }
    }
}

// ------------------------------------------------------- tolerant summary --
World::FieldSummary World::summarise(int slot) const {
    FieldSummary s;
    if (slot < 0 || slot >= (int)bodies.size() || !bodies[slot].alive) return s;
    const Field& f = bodies[slot].f;
    double sx = 0, sy = 0;
    uint32_t n = 0;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (f.at(x, y) > 0.0f) {
                const v2 p = f.samplePos(x, y);
                sx += p.x;  sy += p.y;  ++n;
            }
    s.solid = n;
    if (n) {
        auto q = [](double v) { return (int16_t)std::max(-32000.0, std::min(32000.0, v * 8.0)); };
        s.cx = q(sx / n);
        s.cy = q(sy / n);
    }
    return s;
}

bool World::summariesClose(const FieldSummary& a, const FieldSummary& b) {
    const uint32_t hi = std::max(a.solid, b.solid), lo = std::min(a.solid, b.solid);
    const uint32_t slack = std::max<uint32_t>(6, hi / 80);              // a few samples, or ~1.25%
    if (hi - lo > slack) return false;
    return std::abs((int)a.cx - (int)b.cx) <= 8 && std::abs((int)a.cy - (int)b.cy) <= 8;   // 1 unit
}

void World::refreshIndex(dv2 focus) {
    liveCount = (int)bodies.size() - (int)freeSlots.size();
    active.clear();
    maxBodyRadius = 64.0;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const Body& b = bodies[i];
        if (!b.alive) continue;
        active.push_back((int)i);
        if (b.radius > maxBodyRadius) maxBodyRadius = b.radius;
    }
    grid.build(*this, focus, cfg::SIM_RADIUS + 3000.0);
}
