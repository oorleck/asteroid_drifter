#include "gl.h"
#ifdef _WIN32
#include <cstdio>

#define GL_DEFINE(ret, name, params) PFN_##name name = nullptr;
GL_FUNCTION_LIST(GL_DEFINE)
#undef GL_DEFINE

PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB = nullptr;
PFN_wglChoosePixelFormatARB    wglChoosePixelFormatARB    = nullptr;
PFN_wglSwapIntervalEXT         wglSwapIntervalEXT         = nullptr;

static void* glGetAny(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    // wglGetProcAddress returns these sentinels for 1.1 entry points.
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 ||
        p == (void*)0x3 || p == (void*)-1) {
        static HMODULE lib = LoadLibraryA("opengl32.dll");
        p = lib ? (void*)GetProcAddress(lib, name) : nullptr;
    }
    return p;
}

bool glLoadCoreFunctions() {
    int missing = 0;
#define AST_GL_LOAD(ret, name, params)                                   \
    name = (PFN_##name)glGetAny(#name);                              \
    if (!name) { fprintf(stderr, "GL: missing %s\n", #name); ++missing; }
    GL_FUNCTION_LIST(AST_GL_LOAD)
#undef AST_GL_LOAD
    return missing == 0;
}

bool wglLoadExtensions() {
    wglCreateContextAttribsARB = (PFN_wglCreateContextAttribsARB)glGetAny("wglCreateContextAttribsARB");
    wglChoosePixelFormatARB    = (PFN_wglChoosePixelFormatARB)   glGetAny("wglChoosePixelFormatARB");
    wglSwapIntervalEXT         = (PFN_wglSwapIntervalEXT)        glGetAny("wglSwapIntervalEXT");
    return wglCreateContextAttribsARB != nullptr;
}
#endif  // _WIN32
