// render.h -- HDR wireframe renderer.
//
// Two pipelines feed one floating-point scene buffer that is then bloomed and
// tonemapped:
//
//   bodies : contours live in a persistent GPU arena and never move. Per-frame
//            we upload only a small transform table and issue a single
//            glMultiDrawArrays over GL_LINE_LOOP ranges.
//   dyn    : streamed lines and points for the player, bullets, sparks, stars
//            and HUD -- rebuilt every frame because they are all tiny.
#pragma once
#include "gl.h"
#include "core.h"
#include <vector>
#include <cmath>

struct Camera {
    dv2   pos;              // world-space centre
    float halfW = 900.0f;   // half the visible width, in world units
    float angle = 0.0f;     // rotation applied to world geometry (POV camera)
    float aspect = 16.0f / 9.0f;
    float halfH() const { return halfW / aspect; }
    // Radius of the circle enclosing the view, valid at any camera rotation.
    float viewRadius() const { return std::sqrt(halfW * halfW + halfH() * halfH()); }
};

// One texel pair in the transform buffer: camera-relative placement + colour.
struct BodyXform {
    float px, py, cs, sn;
    float r, g, b, flash;
};

// A slot in the vertex arena. cap is a power of two so slots are recyclable.
struct GeomSlot {
    int base = -1;
    int cap  = 0;
    bool valid() const { return base >= 0; }
};

struct Renderer {
    int   fbw = 0, fbh = 0;
    Camera cam;
    float lineWeight = 1.0f;     // >1 redraws with a sub-pixel offset
    float bloomAmount = 1.55f;
    float gain = 1.0f;           // brightness of everything drawn as lines (dimmed behind the shop)
    void  setGain(float g) { flush(); gain = g; }
    float exposure = 1.25f;

    bool init(int w, int h);
    void shutdown();
    void resize(int w, int h);

    // ---- body geometry arena ------------------------------------------
    // Uploads a contour; pass the previous slot to have it recycled.
    GeomSlot uploadBody(const v2* pts, int n, uint32_t bodyIndex,
                        const float* heat, GeomSlot old);
    void     freeBody(GeomSlot s);
    void     arenaReset();                 // caller must re-upload everything
    bool     arenaOverflowed() const { return overflow; }
    size_t   arenaBytes() const { return (size_t)arenaCap * 16; }

    // ---- frame --------------------------------------------------------
    void beginScene();
    void setBodyXforms(const BodyXform* x, int count);
    void drawBodies(const int* firsts, const int* counts, int n);
    void endScene();                       // bloom + tonemap to the backbuffer
    bool screenshot(const char* path);     // grabs the back buffer as a PNG

    // ---- streamed geometry --------------------------------------------
    void useWorldProjection();
    void useHudProjection();
    void line(v2 a, v2 b, Col c, float inten = 1.0f);
    void poly(const v2* p, int n, bool closed, Col c, float inten = 1.0f);
    void circle(v2 c, float r, int segs, Col col, float inten = 1.0f);
    void arc(v2 c, float r, float a0, float a1, int segs, Col col, float inten = 1.0f);
    void point(v2 p, float size, Col c, float inten = 1.0f);
    void flush();

    // ---- vector text ---------------------------------------------------
    void  text(v2 p, float height, const char* s, Col c, float inten = 1.0f);
    float textWidth(float height, const char* s) const;

    // ---- stats ---------------------------------------------------------
    int statLineVerts = 0, statPointVerts = 0, statBodyLoops = 0, statBodyVerts = 0;

private:
    struct RT { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
    RT scene, half[2], quarter[2], eighth[2];

    GLuint progBody = 0, progDyn = 0, progBg = 0;
    GLuint progBright = 0, progBlur = 0, progDown = 0, progComposite = 0;

    GLuint arenaVbo = 0, arenaVao = 0;
    int    arenaCap = 0, arenaUsed = 0;
    bool   overflow = false;
    static const int NCLASS = 13;          // 16 .. 65536 vertices
    std::vector<int> freeList[NCLASS];

    GLuint xformBuf = 0, xformTex = 0;
    int    xformCount = 0;

    GLuint dynVbo = 0, dynVao = 0;
    size_t dynVboCap = 0;
    GLuint emptyVao = 0;

    struct DynV { float x, y; uint32_t c; float i, s; };
    std::vector<DynV> lineVerts, pointVerts;
    float projX = 1, projY = 1, projOX = 0, projOY = 0;
    float rotC = 1, rotS = 0;
    float time = 0;

    void makeRT(RT& rt, int w, int h);
    void killRT(RT& rt);
    void fullscreen(GLuint prog, const RT& dst, GLuint srcTex, int srcW, int srcH);
    void setProj(float sx, float sy, float ox, float oy, float c, float s);
};
