// field.h -- the signed scalar field that every asteroid is made of.
//
// One structure does four jobs:
//   * shape        : d(p) > 0 means solid rock
//   * rendering    : marching squares extracts the contour loops we draw
//   * collision    : bilinear sample gives penetration depth, gradient gives normal
//   * destruction  : carving mins a disc into the field; connected-component
//                    labelling then tells us when a rock fell into pieces.
#pragma once
#include "core.h"
#include <vector>

struct Loop { int first, count; };          // range inside a contour point list

struct MassProps {
    float area = 0;          // world units^2
    v2    com;               // centre of mass, field-local
    float inertia = 0;       // second moment about com, per unit density
};

struct Field {
    int   w = 0, h = 0;      // sample counts
    float cell = 1;          // world units between samples
    v2    origin;            // local-space position of sample (0,0)
    std::vector<float> d;    // w*h, > 0 inside the rock

    void  alloc(int W, int H, float c, v2 o) { w = W; h = H; cell = c; origin = o; d.assign((size_t)W * H, -c); }
    float  at(int x, int y) const { return d[(size_t)y * w + x]; }
    float& at(int x, int y)       { return d[(size_t)y * w + x]; }
    v2     samplePos(int x, int y) const { return origin + v2(x * cell, y * cell); }
    size_t bytes() const { return d.capacity() * sizeof(float); }

    // Local-space bilinear lookup. Outside the grid returns a negative value
    // that still grows with distance, so gravity/collision stay well behaved.
    float sample(v2 p) const;
    // Analytic gradient of the bilinear patch; points *into* the rock.
    v2 gradient(v2 p) const;
};

// Builds a lumpy closed rock of the given radius, complete with a few craters.
void fieldMakeRock(Field& f, float radius, uint32_t seed);

// d = min(d, |p-c| - r): boolean subtraction of a (slightly irregular) disc.
// Returns true if any sample changed. `wobble` adds edge noise; 0 = clean circle.
bool fieldCarveDisc(Field& f, v2 c, float r, float wobble, uint32_t seed);

// Marching squares at the zero level set. Emits closed loops (outer boundary
// and every hole) as contiguous ranges into `pts`.
void fieldContours(const Field& f, std::vector<v2>& pts, std::vector<Loop>& loops);

MassProps fieldMass(const Field& f);

// Forces the one-sample border to be empty so contours are always closed.
void fieldSealBorder(Field& f);

// Trims the grid to the solid bbox (plus margin) and shifts `origin` so that
// local (0,0) lands on `newOrigin`. Returns false if nothing solid is left.
bool fieldCropAndCenter(Field& f, v2 newOrigin);

// 4-connected labelling of solid samples. labels[] is -1 where empty.
// Returns the number of components found.
int fieldLabelComponents(const Field& f, std::vector<int>& labels);

// Copies one labelled component into `dst`, cropped to its own bounds.
bool fieldExtractComponent(const Field& src, const std::vector<int>& labels, int id, Field& dst);
