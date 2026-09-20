#include "render.h"
#include <cstdio>
#include <cstring>

// ------------------------------------------------------------------ shaders --
static const char* VS_BODY = R"(#version 330 core
layout(location = 0) in vec2  aPos;
layout(location = 1) in uint  aBody;
layout(location = 2) in float aHeat;
uniform samplerBuffer uXform;
uniform vec4 uProj;
uniform vec2 uOffset;
uniform vec2 uRot;                         // cos/sin of the camera rotation
out vec3 vCol;
void main() {
    int i = int(aBody) * 2;
    vec4 t = texelFetch(uXform, i);        // camera-relative pos + cos/sin
    vec4 c = texelFetch(uXform, i + 1);    // colour + impact flash
    vec2 p = vec2(aPos.x * t.z - aPos.y * t.w,
                  aPos.x * t.w + aPos.y * t.z) + t.xy;
    vec2 q = vec2(p.x * uRot.x - p.y * uRot.y,
                  p.x * uRot.y + p.y * uRot.x);
    gl_Position = vec4(q * uProj.xy + uProj.zw + uOffset, 0.0, 1.0);
    // A negative heat marks the dim inset rim that gives rocks some thickness.
    float dim = aHeat < 0.0 ? 0.34 : 1.0;
    float hot = max(aHeat, 0.0) * c.a;
    vCol = c.rgb * dim + vec3(2.4, 0.85, 0.20) * hot;
}
)";

static const char* FS_FLAT = R"(#version 330 core
in vec3 vCol;
uniform float uGain;
out vec4 oCol;
void main() { oCol = vec4(vCol * uGain, 1.0); }
)";

static const char* VS_DYN = R"(#version 330 core
layout(location = 0) in vec2  aPos;
layout(location = 1) in vec4  aCol;
layout(location = 2) in float aInt;
layout(location = 3) in float aSize;
uniform vec4 uProj;
uniform vec2 uOffset;
uniform vec2 uRot;                         // identity for the HUD projection
out vec4 vCol;
void main() {
    vec2 q = vec2(aPos.x * uRot.x - aPos.y * uRot.y,
                  aPos.x * uRot.y + aPos.y * uRot.x);
    gl_Position  = vec4(q * uProj.xy + uProj.zw + uOffset, 0.0, 1.0);
    gl_PointSize = aSize;
    vCol = vec4(aCol.rgb * aInt, aCol.a);
}
)";

static const char* FS_DYN = R"(#version 330 core
in vec4 vCol;
uniform float uGain;
out vec4 oCol;
void main() { oCol = vec4(vCol.rgb * uGain, vCol.a); }
)";

// Fullscreen triangle; no vertex buffer required.
static const char* VS_FULL = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* FS_BG = R"(#version 330 core
in vec2 vUv;
uniform vec2  uDrift;
uniform float uAspect;
out vec4 oCol;
float h21(vec2 p) { p = fract(p * vec2(127.13, 311.7)); p += dot(p, p + 41.7); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i), h21(i + vec2(1,0)), f.x),
               mix(h21(i + vec2(0,1)), h21(i + vec2(1,1)), f.x), f.y);
}
void main() {
    vec2 uv = vUv - 0.5;
    uv.x *= uAspect;
    vec2 q = uv * 2.2 + uDrift;
    float n = vnoise(q * 1.3) * 0.55 + vnoise(q * 3.1) * 0.29 + vnoise(q * 7.3) * 0.16;
    n = pow(clamp(n, 0.0, 1.0), 2.6);
    vec3 neb = mix(vec3(0.012, 0.016, 0.034), vec3(0.075, 0.030, 0.125), n);
    neb += vec3(0.008, 0.026, 0.044) * vnoise(q * 0.55 + vec2(3.1, 1.7));
    oCol = vec4(neb * (1.0 - 0.30 * dot(uv, uv)), 1.0);
}
)";

static const char* FS_BRIGHT = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uThreshold;
out vec4 oCol;
void main() {
    vec3 c = texture(uTex, vUv + uTexel * vec2(-1,-1)).rgb
           + texture(uTex, vUv + uTexel * vec2( 1,-1)).rgb
           + texture(uTex, vUv + uTexel * vec2(-1, 1)).rgb
           + texture(uTex, vUv + uTexel * vec2( 1, 1)).rgb;
    c *= 0.25;
    float l = max(max(c.r, c.g), c.b);
    oCol = vec4(c * (max(l - uThreshold, 0.0) / max(l, 1e-4)), 1.0);
}
)";

static const char* FS_DOWN = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uTexel;
out vec4 oCol;
void main() {
    vec3 c = texture(uTex, vUv + uTexel * vec2(-1,-1)).rgb
           + texture(uTex, vUv + uTexel * vec2( 1,-1)).rgb
           + texture(uTex, vUv + uTexel * vec2(-1, 1)).rgb
           + texture(uTex, vUv + uTexel * vec2( 1, 1)).rgb;
    oCol = vec4(c * 0.25, 1.0);
}
)";

static const char* FS_BLUR = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uDir;              // texel-sized step along one axis
out vec4 oCol;
void main() {
    const float w0 = 0.2270270270, w1 = 0.3162162162, w2 = 0.0702702703;
    vec3 c = texture(uTex, vUv).rgb * w0;
    c += texture(uTex, vUv + uDir * 1.3846153846).rgb * w1;
    c += texture(uTex, vUv - uDir * 1.3846153846).rgb * w1;
    c += texture(uTex, vUv + uDir * 3.2307692308).rgb * w2;
    c += texture(uTex, vUv - uDir * 3.2307692308).rgb * w2;
    oCol = vec4(c, 1.0);
}
)";

static const char* FS_COMPOSITE = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uScene, uB0, uB1, uB2;
uniform float uExposure, uBloom, uTime;
uniform vec2  uRes;
out vec4 oCol;
void main() {
    vec2 d = vUv - 0.5;
    float r2 = dot(d, d);
    float ca = 0.0022 * r2;                       // radial chromatic split
    vec3 s;
    s.r = texture(uScene, vUv + d * ca).r;
    s.g = texture(uScene, vUv).g;
    s.b = texture(uScene, vUv - d * ca).b;
    vec3 b = texture(uB0, vUv).rgb * 0.50
           + texture(uB1, vUv).rgb * 0.34
           + texture(uB2, vUv).rgb * 0.30;
    vec3 c = s + b * uBloom;
    c = vec3(1.0) - exp(-c * uExposure);
    c *= 1.0 - 0.42 * r2;                         // vignette
    c *= 0.975 + 0.025 * sin(vUv.y * uRes.y * 3.14159265);
    float g = fract(sin(dot(vUv * uRes, vec2(12.9898, 78.233)) + uTime) * 43758.5453);
    oCol = vec4(c + (g - 0.5) * 0.005, 1.0);
}
)";

// ------------------------------------------------------------ gl utilities --
static GLuint compile(GLenum type, const char* src, const char* tag) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[shader %s]\n%s\n", tag, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char* vs, const char* fs, const char* tag) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, tag);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, tag);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindFragDataLocation(p, 0, "oCol");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "[link %s]\n%s\n", tag, log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static void setU1i(GLuint p, const char* n, int v)          { glUniform1i(glGetUniformLocation(p, n), v); }
static void setU1f(GLuint p, const char* n, float v)        { glUniform1f(glGetUniformLocation(p, n), v); }
static void setU2f(GLuint p, const char* n, float a, float b){ glUniform2f(glGetUniformLocation(p, n), a, b); }
static void setU4f(GLuint p, const char* n, float a, float b, float c, float d) {
    glUniform4f(glGetUniformLocation(p, n), a, b, c, d);
}

void Renderer::makeRT(RT& rt, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (rt.tex && rt.w == w && rt.h == h) return;
    killRT(rt);
    rt.w = w; rt.h = h;
    glGenTextures(1, &rt.tex);
    glBindTexture(GL_TEXTURE_2D, rt.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[rt] incomplete framebuffer %dx%d\n", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::killRT(RT& rt) {
    if (rt.fbo) glDeleteFramebuffers(1, &rt.fbo);
    if (rt.tex) glDeleteTextures(1, &rt.tex);
    rt.fbo = rt.tex = 0;
    rt.w = rt.h = 0;
}

// ---------------------------------------------------------------- lifetime --
bool Renderer::init(int w, int h) {
    progBody      = link(VS_BODY, FS_FLAT,      "body");
    progDyn       = link(VS_DYN,  FS_DYN,       "dyn");
    progBg        = link(VS_FULL, FS_BG,        "bg");
    progBright    = link(VS_FULL, FS_BRIGHT,    "bright");
    progDown      = link(VS_FULL, FS_DOWN,      "down");
    progBlur      = link(VS_FULL, FS_BLUR,      "blur");
    progComposite = link(VS_FULL, FS_COMPOSITE, "composite");
    if (!progBody || !progDyn || !progBg || !progBright || !progDown || !progBlur || !progComposite)
        return false;

    glGenVertexArrays(1, &emptyVao);

    // Body vertex arena.
    arenaCap = 1 << 22;                       // 4M vertices = 64 MB, enough for a full world
    glGenBuffers(1, &arenaVbo);
    glGenVertexArrays(1, &arenaVao);
    glBindVertexArray(arenaVao);
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)arenaCap * 16, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 16, (void*)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 16, (void*)12);
    glBindVertexArray(0);

    // Per-body transform table, read in the vertex shader as a texture buffer.
    glGenBuffers(1, &xformBuf);
    glGenTextures(1, &xformTex);
    glBindTexture(GL_TEXTURE_BUFFER, xformTex);
    glBindBuffer(GL_TEXTURE_BUFFER, xformBuf);
    glBufferData(GL_TEXTURE_BUFFER, 1024, nullptr, GL_STREAM_DRAW);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, xformBuf);

    // Streamed line/point buffer.
    glGenBuffers(1, &dynVbo);
    glGenVertexArrays(1, &dynVao);
    glBindVertexArray(dynVao);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo);
    dynVboCap = 1 << 20;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)dynVboCap, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, (void*)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 20, (void*)12);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 20, (void*)16);
    glBindVertexArray(0);

    resize(w, h);
    return true;
}

void Renderer::shutdown() {
    killRT(scene);
    for (int i = 0; i < 2; ++i) { killRT(half[i]); killRT(quarter[i]); killRT(eighth[i]); }
    GLuint progs[] = {progBody, progDyn, progBg, progBright, progDown, progBlur, progComposite};
    for (GLuint p : progs) if (p) glDeleteProgram(p);
    if (arenaVbo)  glDeleteBuffers(1, &arenaVbo);
    if (dynVbo)    glDeleteBuffers(1, &dynVbo);
    if (xformBuf)  glDeleteBuffers(1, &xformBuf);
    if (xformTex)  glDeleteTextures(1, &xformTex);
    if (arenaVao)  glDeleteVertexArrays(1, &arenaVao);
    if (dynVao)    glDeleteVertexArrays(1, &dynVao);
    if (emptyVao)  glDeleteVertexArrays(1, &emptyVao);
}

void Renderer::resize(int w, int h) {
    fbw = w < 1 ? 1 : w;
    fbh = h < 1 ? 1 : h;
    cam.aspect = (float)fbw / (float)fbh;
    makeRT(scene, fbw, fbh);
    for (int i = 0; i < 2; ++i) {
        makeRT(half[i],    fbw / 2, fbh / 2);
        makeRT(quarter[i], fbw / 4, fbh / 4);
        makeRT(eighth[i],  fbw / 8, fbh / 8);
    }
}

// ------------------------------------------------------------- geom arena --
// Allocations are rounded to powers of two (16 .. 65536 vertices) so freed
// slots can always be handed straight back out. Worst case waste is 2x, and a
// full rebuild only happens when the arena genuinely runs out.
static const int GEOM_NCLASS = 13;          // 16 .. 65536 vertices

static int classFor(int n) {
    int cls = 0, cap = 16;
    while (cap < n && cls < GEOM_NCLASS - 1) { cap <<= 1; ++cls; }
    return cls;
}

GeomSlot Renderer::uploadBody(const v2* pts, int n, uint32_t bodyIndex,
                              const float* heat, GeomSlot old) {
    GeomSlot s;
    if (n <= 0) { freeBody(old); return s; }
    const int cls = classFor(n);
    const int cap = 16 << cls;
    if (cap < n) { freeBody(old); overflow = true; return s; }   // absurdly long contour

    if (old.valid() && old.cap == cap) {
        s = old;                                  // same bucket: reuse in place
    } else {
        freeBody(old);
        if (!freeList[cls].empty()) {
            s.base = freeList[cls].back();
            freeList[cls].pop_back();
            s.cap  = cap;
        } else if (arenaUsed + cap <= arenaCap) {
            s.base = arenaUsed;
            s.cap  = cap;
            arenaUsed += cap;
        } else {
            overflow = true;
            return GeomSlot();
        }
    }

    static std::vector<float> tmp;
    tmp.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        float* d = &tmp[(size_t)i * 4];
        d[0] = pts[i].x;
        d[1] = pts[i].y;
        memcpy(&d[2], &bodyIndex, 4);
        d[3] = heat ? heat[i] : 0.0f;
    }
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)s.base * 16, (GLsizeiptr)n * 16, tmp.data());
    return s;
}

void Renderer::freeBody(GeomSlot s) {
    if (!s.valid() || s.cap <= 0) return;
    int cls = 0, cap = 16;
    while (cap < s.cap && cls < NCLASS - 1) { cap <<= 1; ++cls; }
    freeList[cls].push_back(s.base);
}

void Renderer::arenaReset() {
    // Grow on overflow, then hand a clean arena back to the caller.
    if (overflow && arenaCap < (1 << 24)) arenaCap <<= 2;
    overflow  = false;
    arenaUsed = 0;
    for (int i = 0; i < NCLASS; ++i) freeList[i].clear();
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)arenaCap * 16, nullptr, GL_DYNAMIC_DRAW);
}

// ------------------------------------------------------------------ frame --
void Renderer::setProj(float sx, float sy, float ox, float oy, float c, float s) {
    if (sx != projX || sy != projY || ox != projOX || oy != projOY ||
        c != rotC || s != rotS) flush();
    projX = sx; projY = sy; projOX = ox; projOY = oy;
    rotC = c; rotS = s;
}

void Renderer::useWorldProjection() {
    setProj(1.0f / cam.halfW, 1.0f / cam.halfH(), 0.0f, 0.0f,
            std::cos(cam.angle), std::sin(cam.angle));
}

void Renderer::useHudProjection() {
    // The HUD never rotates with the world.
    setProj(2.0f / fbw, -2.0f / fbh, -1.0f, 1.0f, 1.0f, 0.0f);
}

void Renderer::beginScene() {
    time += 1.0f / 60.0f;
    statLineVerts = statPointVerts = statBodyLoops = statBodyVerts = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    glViewport(0, 0, scene.w, scene.h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Nebula backdrop, drifting very slowly with the camera for depth.
    glDisable(GL_BLEND);
    glUseProgram(progBg);
    setU2f(progBg, "uDrift", (float)std::fmod(cam.pos.x * 2.0e-5, 512.0),
                             (float)std::fmod(cam.pos.y * 2.0e-5, 512.0));
    setU1f(progBg, "uAspect", cam.aspect);
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Everything else is additive light on a dark field.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    useWorldProjection();
}

void Renderer::setBodyXforms(const BodyXform* x, int count) {
    xformCount = count;
    if (count <= 0) return;
    glBindBuffer(GL_TEXTURE_BUFFER, xformBuf);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)count * (GLsizeiptr)sizeof(BodyXform),
                 x, GL_STREAM_DRAW);
}

void Renderer::drawBodies(const int* firsts, const int* counts, int n) {
    if (n <= 0 || xformCount <= 0) return;
    statBodyLoops += n;
    for (int i = 0; i < n; ++i) statBodyVerts += counts[i];

    glUseProgram(progBody);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, xformTex);
    setU1i(progBody, "uXform", 0);
    setU4f(progBody, "uProj", projX, projY, projOX, projOY);
    setU2f(progBody, "uRot", rotC, rotS);
    glBindVertexArray(arenaVao);

    const int passes = lineWeight > 1.05f ? 2 : 1;
    for (int p = 0; p < passes; ++p) {
        const float k = (p == 0) ? 1.0f : clampf(lineWeight - 1.0f, 0.0f, 1.0f);
        setU1f(progBody, "uGain", gain * (p == 0 ? 1.0f : k * 0.75f));
        setU2f(progBody, "uOffset", p == 0 ? 0.0f : 1.0f / fbw,
                                    p == 0 ? 0.0f : 1.0f / fbh);
        glMultiDrawArrays(GL_LINE_LOOP, firsts, (const GLsizei*)counts, n);
    }
    glBindVertexArray(0);
}

// ------------------------------------------------------ streamed geometry --
void Renderer::line(v2 a, v2 b, Col c, float inten) {
    const uint32_t p = packCol(c);
    lineVerts.push_back({a.x, a.y, p, inten, 1.0f});
    lineVerts.push_back({b.x, b.y, p, inten, 1.0f});
}

void Renderer::poly(const v2* p, int n, bool closed, Col c, float inten) {
    if (n < 2) return;
    for (int i = 0; i + 1 < n; ++i) line(p[i], p[i + 1], c, inten);
    if (closed) line(p[n - 1], p[0], c, inten);
}

void Renderer::arc(v2 c, float r, float a0, float a1, int segs, Col col, float inten) {
    if (segs < 2) segs = 2;
    v2 prev = c + fromAngle(a0) * r;
    for (int i = 1; i <= segs; ++i) {
        const float a = lerpf(a0, a1, (float)i / segs);
        const v2 cur = c + fromAngle(a) * r;
        line(prev, cur, col, inten);
        prev = cur;
    }
}

void Renderer::circle(v2 c, float r, int segs, Col col, float inten) {
    arc(c, r, 0.0f, TAUF, segs, col, inten);
}

void Renderer::point(v2 p, float size, Col c, float inten) {
    pointVerts.push_back({p.x, p.y, packCol(c), inten, size});
}

void Renderer::flush() {
    if (lineVerts.empty() && pointVerts.empty()) return;
    statLineVerts  += (int)lineVerts.size();
    statPointVerts += (int)pointVerts.size();

    glUseProgram(progDyn);
    setU4f(progDyn, "uProj", projX, projY, projOX, projOY);
    setU2f(progDyn, "uRot", rotC, rotS);
    glBindVertexArray(dynVao);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo);

    // A weight above 1 redraws the batch shifted by half a pixel, which reads
    // as a thicker stroke without giving up the cheap GL_LINES path.
    const int passes = lineWeight > 1.05f ? 2 : 1;
    auto drawPasses = [&](GLenum mode, GLsizei count) {
        for (int pass = 0; pass < passes; ++pass) {
            setU1f(progDyn, "uGain", gain * (pass == 0 ? 1.0f
                                               : clampf(lineWeight - 1.0f, 0.0f, 1.0f) * 0.75f));
            setU2f(progDyn, "uOffset", pass == 0 ? 0.0f : 1.0f / fbw,
                                       pass == 0 ? 0.0f : 1.0f / fbh);
            glDrawArrays(mode, 0, count);
        }
    };
    auto upload = [&](const std::vector<DynV>& v) {
        const size_t bytes = v.size() * sizeof(DynV);
        if (bytes > dynVboCap) dynVboCap = bytes * 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)dynVboCap, nullptr, GL_STREAM_DRAW);  // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, v.data());
    };

    if (!lineVerts.empty())  { upload(lineVerts);  drawPasses(GL_LINES,  (GLsizei)lineVerts.size()); }
    if (!pointVerts.empty()) { upload(pointVerts); drawPasses(GL_POINTS, (GLsizei)pointVerts.size()); }

    lineVerts.clear();
    pointVerts.clear();
    glBindVertexArray(0);
}


// --------------------------------------------------------- post-processing --
void Renderer::fullscreen(GLuint prog, const RT& dst, GLuint srcTex, int srcW, int srcH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    setU1i(prog, "uTex", 0);
    setU2f(prog, "uTexel", 1.0f / (float)srcW, 1.0f / (float)srcH);
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::endScene() {
    flush();
    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);

    // Bright pass into half res, then a blurred pyramid down to an eighth.
    glBindFramebuffer(GL_FRAMEBUFFER, half[0].fbo);
    glViewport(0, 0, half[0].w, half[0].h);
    glUseProgram(progBright);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene.tex);
    setU1i(progBright, "uTex", 0);
    setU2f(progBright, "uTexel", 1.0f / scene.w, 1.0f / scene.h);
    setU1f(progBright, "uThreshold", 0.42f);
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    RT* levels[3] = {half, quarter, eighth};
    for (int l = 0; l < 3; ++l) {
        RT* L = levels[l];
        if (l > 0) fullscreen(progDown, L[0], levels[l - 1][0].tex, levels[l - 1][0].w, levels[l - 1][0].h);
        // separable gaussian, ping-ponging between the two buffers
        for (int axis = 0; axis < 2; ++axis) {
            const RT& src = L[axis];
            const RT& dst = L[axis ^ 1];
            glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
            glViewport(0, 0, dst.w, dst.h);
            glUseProgram(progBlur);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, src.tex);
            setU1i(progBlur, "uTex", 0);
            setU2f(progBlur, "uDir", axis == 0 ? 1.0f / src.w : 0.0f,
                                     axis == 0 ? 0.0f : 1.0f / src.h);
            glBindVertexArray(emptyVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        // after two passes the result is back in L[0]
    }

    // Composite to the default framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbw, fbh);
    glUseProgram(progComposite);
    const GLuint texes[4] = {scene.tex, half[0].tex, quarter[0].tex, eighth[0].tex};
    const char*  names[4] = {"uScene", "uB0", "uB1", "uB2"};
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, texes[i]);
        setU1i(progComposite, names[i], i);
    }
    setU1f(progComposite, "uExposure", exposure);
    setU1f(progComposite, "uBloom", bloomAmount);
    setU1f(progComposite, "uTime", time);
    setU2f(progComposite, "uRes", (float)fbw, (float)fbh);
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}

// ----------------------------------------------------------- vector font --
// Each glyph is a set of polylines on a 4 x 6 integer grid. Coordinate pairs
// are two digits (x then y); a bar starts a new stroke.
static const char* const GLYPHS[64] = {
/* 32   */ "",
/* !    */ "2226|2021",
/* "    */ "1416|3436",
/* #    */ "1026|3046|0242|0444",
/* $    */ "453616050413334241301001|2026",
/* %    */ "0046|0515|3141",
/* &    */ "4013040516263534001020|2042",
/* '    */ "2426",
/* (    */ "36141230",
/* )    */ "16343210",
/* *    */ "2325|2321|2314|2332|2334|2312",
/* +    */ "1333|2224",
/* ,    */ "2110",
/* -    */ "1333",
/* .    */ "1020",
/* /    */ "0046",
/* 0    */ "103041453616050110|0145",
/* 1    */ "042620|0040",
/* 2    */ "05163645440040",
/* 3    */ "0646234241301001",
/* 4    */ "30360242",
/* 5    */ "460603334241301001",
/* 6    */ "4536160501103041423303",
/* 7    */ "064610",
/* 8    */ "13334445361605041302011030414233",
/* 9    */ "0110304145361605041343",
/* :    */ "2122|2425",
/* ;    */ "2425|2110",
/* <    */ "361330",
/* =    */ "1232|1434",
/* >    */ "163310",
/* ?    */ "05163645442322|2021",
/* @    */ "4130100105163645444232222334",
/* A    */ "0004264440|0242",
/* B    */ "000636454433033342413000",
/* C    */ "4536160501103041",
/* D    */ "00062644422000",
/* E    */ "46060040|0333",
/* F    */ "460600|0333",
/* G    */ "45361605011030414323",
/* H    */ "0006|4046|0343",
/* I    */ "1030|2026|1636",
/* J    */ "3631201001",
/* K    */ "0006|460340",
/* L    */ "060040",
/* M    */ "0006234640",
/* N    */ "00064046",
/* O    */ "103041453616050110",
/* P    */ "00063645443303",
/* Q    */ "103041453616050110|2240",
/* R    */ "00063645443303|2340",
/* S    */ "453616050413334241301001",
/* T    */ "0646|2620",
/* U    */ "060110304146",
/* V    */ "062046",
/* W    */ "0610233046",
/* X    */ "0046|0640",
/* Y    */ "062346|2320",
/* Z    */ "06460040",
/* [    */ "36161030",
/* \    */ "0640",
/* ]    */ "16363010",
/* ^    */ "042624",
/* _    */ "0040",
};

static const char* glyphFor(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 95) return "";
    return GLYPHS[c - 32];
}

float Renderer::textWidth(float height, const char* s) const {
    const float adv = height * (5.0f / 6.0f);
    int n = 0;
    for (const char* p = s; *p; ++p) ++n;
    return n > 0 ? n * adv - height * (1.0f / 6.0f) : 0.0f;
}

void Renderer::text(v2 p, float height, const char* s, Col c, float inten) {
    const float sc = height / 6.0f;
    // HUD space has y growing downward; keep glyphs upright either way.
    const float sy = (projY < 0.0f) ? -sc : sc;
    const float adv = height * (5.0f / 6.0f);
    float x = p.x;
    for (const char* ch = s; *ch; ++ch, x += adv) {
        const char* g = glyphFor(*ch);
        bool pen = false;
        v2 prev;
        for (const char* q = g; *q; ) {
            if (*q == '|') { pen = false; ++q; continue; }
            if (!q[1]) break;
            const v2 cur(x + (q[0] - '0') * sc, p.y + (q[1] - '0') * sy);
            if (pen) line(prev, cur, c, inten);
            prev = cur;
            pen  = true;
            q += 2;
        }
    }
}

// ------------------------------------------------------------- screenshot --
// A tiny PNG writer. Deflate "stored" blocks mean no compression library is
// needed; the files are big but this is a debug/share feature, not an asset.
static uint32_t crc32of(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void pngChunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32of(&out[start], out.size() - start, 0));
}

bool Renderer::screenshot(const char* path) {
    const int w = fbw, h = fbh;
    std::vector<uint8_t> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    // PNG scanlines run top to bottom and each carries a filter byte.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 3));
    for (int y = h - 1; y >= 0; --y) {
        raw.push_back(0);
        const uint8_t* row = &px[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 0]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 2]);
        }
    }

    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = (off + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));       z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(2);           // 8-bit RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    pngChunk(out, "IHDR", ihdr);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", std::vector<uint8_t>());

    FILE* fp = fopen(path, "wb");
    if (!fp) return false;
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    fprintf(stderr, "screenshot: %s (%dx%d)\n", path, w, h);
    return true;
}
