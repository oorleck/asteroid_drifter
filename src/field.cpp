#include "field.h"
#include <cstring>

// ---------------------------------------------------------------- sampling --
float Field::sample(v2 p) const {
    if (d.empty()) return -1.0f;
    const float gx = (p.x - origin.x) / cell;
    const float gy = (p.y - origin.y) / cell;
    const float mx = (float)(w - 1), my = (float)(h - 1);
    if (gx < 0.0f || gy < 0.0f || gx > mx || gy > my) {
        // Outside the grid: keep falling off so gradients stay sane.
        const float cx = clampf(gx, 0, mx), cy = clampf(gy, 0, my);
        const float dx = (gx - cx) * cell, dy = (gy - cy) * cell;
        return -std::sqrt(dx * dx + dy * dy) - cell;
    }
    const int x0 = (int)gx, y0 = (int)gy;
    const int x1 = x0 + 1 < w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float tx = gx - x0, ty = gy - y0;
    const float v00 = at(x0, y0), v10 = at(x1, y0), v01 = at(x0, y1), v11 = at(x1, y1);
    return lerpf(lerpf(v00, v10, tx), lerpf(v01, v11, tx), ty);
}

v2 Field::gradient(v2 p) const {
    if (d.empty()) return v2(0, 0);
    const float gx = (p.x - origin.x) / cell;
    const float gy = (p.y - origin.y) / cell;
    const float mx = (float)(w - 1), my = (float)(h - 1);
    if (gx < 0.0f || gy < 0.0f || gx > mx || gy > my) {
        const float cx = clampf(gx, 0, mx), cy = clampf(gy, 0, my);
        return norm(v2(cx - gx, cy - gy));     // points back toward the rock
    }
    const int x0 = (int)gx, y0 = (int)gy;
    const int x1 = x0 + 1 < w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float tx = gx - x0, ty = gy - y0;
    const float v00 = at(x0, y0), v10 = at(x1, y0), v01 = at(x0, y1), v11 = at(x1, y1);
    const float ddx = ((v10 - v00) * (1 - ty) + (v11 - v01) * ty) / cell;
    const float ddy = ((v01 - v00) * (1 - tx) + (v11 - v10) * tx) / cell;
    return v2(ddx, ddy);
}

// -------------------------------------------------------------- generation --
void fieldSealBorder(Field& f) {
    if (f.w < 2 || f.h < 2) return;
    const float e = -f.cell;
    for (int x = 0; x < f.w; ++x) {
        f.at(x, 0)       = std::min(f.at(x, 0), e);
        f.at(x, f.h - 1) = std::min(f.at(x, f.h - 1), e);
    }
    for (int y = 0; y < f.h; ++y) {
        f.at(0, y)       = std::min(f.at(0, y), e);
        f.at(f.w - 1, y) = std::min(f.at(f.w - 1, y), e);
    }
}

void fieldMakeRock(Field& f, float radius, uint32_t seed) {
    Rng rng(seed);
    // Radial harmonics keep the silhouette closed and smooth by construction.
    const int NH = 14;                      // harmonics k = 2..15
    float amp[NH], ph[NH];
    float total = 0;
    for (int k = 0; k < NH; ++k) {
        // Low orders give the silhouette, high orders the rocky crinkle.
        amp[k] = rng.range(0.03f, 0.15f) / (1.0f + (float)k * 0.45f);
        ph[k]  = rng.angle();
        total += amp[k];
    }
    const float maxR = radius * (1.0f + total);
    float cell   = clampf(radius / 31.0f, 1.5f, 6.0f);
    float extent = maxR * 1.10f + 3.0f * cell;
    int   n      = (int)(2.0f * extent / cell) + 3;
    if (n > 320) { n = 320; cell = 2.0f * extent / (float)(n - 3); }
    if (n < 12)  { n = 12;  cell = 2.0f * extent / (float)(n - 3); }

    // Evaluating nine harmonics per sample is the single hottest loop in world
    // generation, so bake the radial profile once and interpolate it instead.
    const int LUT = 1024;
    static std::vector<float> prof;
    prof.resize(LUT + 1);
    for (int i = 0; i <= LUT; ++i) {
        const float th = -PIF + (float)i * (TAUF / LUT);
        float R = 1.0f;
        for (int k = 0; k < NH; ++k) R += amp[k] * std::sin((k + 2) * th + ph[k]);
        prof[i] = radius * R;
    }
    prof[LUT] = prof[0];

    f.alloc(n, n, cell, v2(-extent, -extent));
    const float toIdx = (float)LUT / TAUF;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const v2 p = f.samplePos(x, y);
            const float r = len(p);
            const float t = (std::atan2(p.y, p.x) + PIF) * toIdx;
            int i0 = (int)t;
            if (i0 < 0) i0 = 0;
            if (i0 >= LUT) i0 = LUT - 1;
            f.at(x, y) = lerpf(prof[i0], prof[i0 + 1], t - (float)i0) - r;
        }
    }
    // A few old craters so fresh rocks do not look machined.
    const int craters = rng.i(2, 5);
    for (int i = 0; i < craters; ++i) {
        const float rr = radius * rng.range(0.14f, 0.34f);
        const v2    c  = rng.dir() * (radius * rng.range(0.92f, 1.16f));   // always bites the rim
        fieldCarveDisc(f, c, rr, 0.22f, rng.u32());
    }
    fieldSealBorder(f);
}

// ------------------------------------------------------------------ carving --
bool fieldCarveDisc(Field& f, v2 c, float r, float wobble, uint32_t seed) {
    if (f.d.empty() || r <= 0) return false;
    const float rmax = r * (1.0f + wobble) + f.cell;
    const float gx0 = (c.x - rmax - f.origin.x) / f.cell;
    const float gy0 = (c.y - rmax - f.origin.y) / f.cell;
    const float gx1 = (c.x + rmax - f.origin.x) / f.cell;
    const float gy1 = (c.y + rmax - f.origin.y) / f.cell;
    // Stay one sample inside so the sealed border is never re-opened.
    int x0 = std::max(1, (int)std::floor(gx0)), x1 = std::min(f.w - 2, (int)std::ceil(gx1));
    int y0 = std::max(1, (int)std::floor(gy0)), y1 = std::min(f.h - 2, (int)std::ceil(gy1));
    if (x0 > x1 || y0 > y1) return false;

    Rng rng(seed);
    const float p1 = rng.angle(), p2 = rng.angle();
    bool changed = false;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const v2 p = f.samplePos(x, y) - c;
            float rr = r;
            if (wobble > 0.0f) {
                const float th = std::atan2(p.y, p.x);
                rr *= 1.0f + wobble * (0.62f * std::sin(3 * th + p1) + 0.38f * std::sin(5 * th + p2));
            }
            const float nd = len(p) - rr;
            float& cur = f.at(x, y);
            if (nd < cur) { cur = nd; changed = true; }
        }
    }
    return changed;
}

// --------------------------------------------------------- marching squares --
void fieldContours(const Field& f, std::vector<v2>& pts, std::vector<Loop>& loops) {
    pts.clear();
    loops.clear();
    if (f.w < 2 || f.h < 2) return;

    // Scratch reused between calls; contouring happens on one thread.
    static std::vector<int>  edgeVert;
    static std::vector<v2>   vp;
    static std::vector<int>  adjA, adjB;
    static std::vector<char> used;

    edgeVert.assign((size_t)f.w * f.h * 2, -1);
    vp.clear(); adjA.clear(); adjB.clear();

    const float cell = f.cell;
    // Each crossed grid edge contributes exactly one contour vertex, which is
    // what makes the loops watertight without any point welding.
    auto vertOnEdge = [&](int x, int y, int t) -> int {
        const size_t id = (((size_t)y * f.w) + x) * 2 + t;
        int vi = edgeVert[id];
        if (vi >= 0) return vi;
        const float va = f.at(x, y);
        const float vb = (t == 0) ? f.at(x + 1, y) : f.at(x, y + 1);
        const float den = va - vb;
        float tt = (std::fabs(den) < 1e-20f) ? 0.5f : va / den;
        tt = clampf(tt, 0.0f, 1.0f);
        const v2 a = f.samplePos(x, y);
        const v2 p = (t == 0) ? v2(a.x + tt * cell, a.y) : v2(a.x, a.y + tt * cell);
        vi = (int)vp.size();
        vp.push_back(p);
        adjA.push_back(-1);
        adjB.push_back(-1);
        edgeVert[id] = vi;
        return vi;
    };
    auto link = [&](int i, int j) {
        if (i == j || i < 0 || j < 0) return;
        if (adjA[i] < 0) adjA[i] = j; else if (adjB[i] < 0) adjB[i] = j;
        if (adjA[j] < 0) adjA[j] = i; else if (adjB[j] < 0) adjB[j] = i;
    };

    for (int y = 0; y < f.h - 1; ++y) {
        for (int x = 0; x < f.w - 1; ++x) {
            const float v00 = f.at(x, y),         v10 = f.at(x + 1, y);
            const float v11 = f.at(x + 1, y + 1), v01 = f.at(x, y + 1);
            int code = 0;
            if (v00 > 0) code |= 1;
            if (v10 > 0) code |= 2;
            if (v11 > 0) code |= 4;
            if (v01 > 0) code |= 8;
            if (code == 0 || code == 15) continue;

            const bool centreSolid = (v00 + v10 + v11 + v01) > 0.0f;
            switch (code) {
                case 1:  link(vertOnEdge(x, y, 1),     vertOnEdge(x, y, 0));     break;
                case 2:  link(vertOnEdge(x, y, 0),     vertOnEdge(x + 1, y, 1)); break;
                case 3:  link(vertOnEdge(x, y, 1),     vertOnEdge(x + 1, y, 1)); break;
                case 4:  link(vertOnEdge(x + 1, y, 1), vertOnEdge(x, y + 1, 0)); break;
                case 6:  link(vertOnEdge(x, y, 0),     vertOnEdge(x, y + 1, 0)); break;
                case 7:  link(vertOnEdge(x, y, 1),     vertOnEdge(x, y + 1, 0)); break;
                case 8:  link(vertOnEdge(x, y + 1, 0), vertOnEdge(x, y, 1));     break;
                case 9:  link(vertOnEdge(x, y + 1, 0), vertOnEdge(x, y, 0));     break;
                case 11: link(vertOnEdge(x, y + 1, 0), vertOnEdge(x + 1, y, 1)); break;
                case 12: link(vertOnEdge(x + 1, y, 1), vertOnEdge(x, y, 1));     break;
                case 13: link(vertOnEdge(x + 1, y, 1), vertOnEdge(x, y, 0));     break;
                case 14: link(vertOnEdge(x, y, 0),     vertOnEdge(x, y, 1));     break;
                // Saddles: the cell centre decides which corners stay joined.
                case 5:
                    if (centreSolid) { link(vertOnEdge(x, y, 1),     vertOnEdge(x, y + 1, 0));
                                       link(vertOnEdge(x, y, 0),     vertOnEdge(x + 1, y, 1)); }
                    else             { link(vertOnEdge(x, y, 1),     vertOnEdge(x, y, 0));
                                       link(vertOnEdge(x + 1, y, 1), vertOnEdge(x, y + 1, 0)); }
                    break;
                case 10:
                    if (centreSolid) { link(vertOnEdge(x, y, 1),     vertOnEdge(x, y, 0));
                                       link(vertOnEdge(x + 1, y, 1), vertOnEdge(x, y + 1, 0)); }
                    else             { link(vertOnEdge(x, y, 1),     vertOnEdge(x, y + 1, 0));
                                       link(vertOnEdge(x, y, 0),     vertOnEdge(x + 1, y, 1)); }
                    break;
                default: break;
            }
        }
    }

    // Walk the adjacency into contiguous, closed loops ready for LINE_LOOP.
    const int n = (int)vp.size();
    used.assign(n, 0);
    pts.reserve(n);
    for (int s = 0; s < n; ++s) {
        if (used[s]) continue;
        const int first = (int)pts.size();
        int prev = -1, cur = s;
        while (cur >= 0 && !used[cur]) {
            used[cur] = 1;
            pts.push_back(vp[cur]);
            const int a = adjA[cur], b = adjB[cur];
            int nxt = -1;
            if      (a >= 0 && a != prev && !used[a]) nxt = a;
            else if (b >= 0 && b != prev && !used[b]) nxt = b;
            prev = cur;
            cur  = nxt;
        }
        const int count = (int)pts.size() - first;
        if (count >= 3) loops.push_back({first, count});
        else            pts.resize(first);
    }
}

// ------------------------------------------------------------ mass & split --
MassProps fieldMass(const Field& f) {
    MassProps mp;
    if (f.d.empty()) return mp;
    const float cellArea = f.cell * f.cell;
    double wsum = 0, cx = 0, cy = 0;
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            // Sub-cell coverage keeps mass continuous as rock is shaved away.
            const float cov = clampf(0.5f + f.at(x, y) / f.cell, 0.0f, 1.0f);
            if (cov <= 0) continue;
            const v2 p = f.samplePos(x, y);
            wsum += cov;
            cx   += (double)cov * p.x;
            cy   += (double)cov * p.y;
        }
    }
    if (wsum <= 1e-9) return mp;
    mp.com  = v2((float)(cx / wsum), (float)(cy / wsum));
    mp.area = (float)wsum * cellArea;

    double I = 0;
    const double selfTerm = (double)f.cell * f.cell / 6.0;   // square cell about its own centre
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            const float cov = clampf(0.5f + f.at(x, y) / f.cell, 0.0f, 1.0f);
            if (cov <= 0) continue;
            const v2 r = f.samplePos(x, y) - mp.com;
            I += (double)cov * (len2(r) + selfTerm);
        }
    }
    mp.inertia = (float)(I * cellArea);
    return mp;
}

// Bounding box of every sample passing `keep`, padded by `margin`.
template <typename Pred>
static bool solidBounds(const Field& f, int margin, Pred keep,
                        int& x0, int& y0, int& x1, int& y1) {
    x0 = f.w; y0 = f.h; x1 = -1; y1 = -1;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (keep(x, y)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return false;
    x0 = std::max(0, x0 - margin);      y0 = std::max(0, y0 - margin);
    x1 = std::min(f.w - 1, x1 + margin); y1 = std::min(f.h - 1, y1 + margin);
    return true;
}

bool fieldCropAndCenter(Field& f, v2 newOrigin) {
    int x0, y0, x1, y1;
    if (!solidBounds(f, 3, [&](int x, int y) { return f.at(x, y) > 0.0f; }, x0, y0, x1, y1))
        return false;
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;

    Field nf;
    nf.alloc(nw, nh, f.cell, f.origin + v2(x0 * f.cell, y0 * f.cell) - newOrigin);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
            nf.at(x, y) = f.at(x0 + x, y0 + y);
    fieldSealBorder(nf);
    f = std::move(nf);
    return true;
}

int fieldLabelComponents(const Field& f, std::vector<int>& labels) {
    const size_t n = (size_t)f.w * f.h;
    labels.assign(n, -1);
    static std::vector<int> stack;
    int next = 0;
    for (int sy = 0; sy < f.h; ++sy) {
        for (int sx = 0; sx < f.w; ++sx) {
            const size_t si = (size_t)sy * f.w + sx;
            if (labels[si] >= 0 || f.d[si] <= 0.0f) continue;
            const int id = next++;
            stack.clear();
            stack.push_back((int)si);
            labels[si] = id;
            while (!stack.empty()) {
                const int ci = stack.back(); stack.pop_back();
                const int cx = ci % f.w, cy = ci / f.w;
                const int nb[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
                for (int k = 0; k < 4; ++k) {
                    const int nx = nb[k][0], ny = nb[k][1];
                    if (nx < 0 || ny < 0 || nx >= f.w || ny >= f.h) continue;
                    const size_t ni = (size_t)ny * f.w + nx;
                    if (labels[ni] >= 0 || f.d[ni] <= 0.0f) continue;
                    labels[ni] = id;
                    stack.push_back((int)ni);
                }
            }
        }
    }
    return next;
}

bool fieldExtractComponent(const Field& src, const std::vector<int>& labels, int id, Field& dst) {
    int x0, y0, x1, y1;
    if (!solidBounds(src, 3, [&](int x, int y) { return labels[(size_t)y * src.w + x] == id; },
                     x0, y0, x1, y1))
        return false;
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    dst.alloc(nw, nh, src.cell, src.origin + v2(x0 * src.cell, y0 * src.cell));
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            const size_t si = (size_t)(y0 + y) * src.w + (x0 + x);
            const int lab = labels[si];
            // Rock belonging to a sibling piece becomes empty space here but
            // keeps its magnitude, so the shared boundary lands in the same place.
            dst.at(x, y) = (lab >= 0 && lab != id) ? -std::fabs(src.d[si]) : src.d[si];
        }
    }
    fieldSealBorder(dst);
    return true;
}
