// main.cpp -- the window, a 3.3 core context, input and the frame loop.
// Windows: Win32 and WGL, linking only against opengl32 and gdi32.
// Linux:   SDL2 and GLX, linking against SDL2 and libGL.
// Everything the game itself does is between platformPump() and swapBuffersNow().
#include "gl.h"
#include "game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#ifndef _WIN32
#include <SDL2/SDL.h>
#endif

static Input   gInput;
static bool    gRunning = true;
static bool    gResized = false;
static int     gWidth = 1600, gHeight = 900;
static bool    gFullscreen = false;

#ifdef _WIN32
static WINDOWPLACEMENT gPrevPlacement = { sizeof(WINDOWPLACEMENT) };
static HWND    gHwnd  = nullptr;      // set once the window is up; the frame loop
static HDC     gDC    = nullptr;      // reaches the platform through these, so the
static HGLRC   gGLRC  = nullptr;      // loop itself has no #ifdefs in it.

static void fatal(const char* msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    MessageBoxA(nullptr, msg, "Asteroid Drifter", MB_OK | MB_ICONERROR);
    exit(1);
}

// Windows reports Left and Right Alt as one key (VK_MENU); the "extended key" bit in
// lParam tells them apart. The game wants Left Alt specifically, so split them.
static int keyFromMessage(WPARAM wp, LPARAM lp) {
    int vk = (int)wp;
    if (vk == VK_MENU) vk = (lp & (1 << 24)) ? VK_RMENU : VK_LMENU;
    return vk;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            gRunning = false;
            return 0;
        case WM_SIZE:
            gWidth  = LOWORD(lp);
            gHeight = HIWORD(lp);
            gResized = true;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = keyFromMessage(wp, lp);
            if (vk >= 0 && vk < 256) {
                if (!gInput.down[vk]) gInput.pressed[vk] = true;
                gInput.down[vk] = true;
            }
            if (vk == VK_ESCAPE) gRunning = false;
            // Alt is a game key now, so Windows' own Alt+F4 has to be done by hand.
            if (vk == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) gRunning = false;
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int vk = keyFromMessage(wp, lp);
            if (vk >= 0 && vk < 256) gInput.down[vk] = false;
            return 0;
        }
        // Tapping Alt would otherwise open the window menu, freeze the game and beep.
        case WM_SYSCHAR:
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        // Losing focus while a key is held would leave it "stuck down" (a raised shield
        // that never drops), because the key-up goes to the other window.
        case WM_KILLFOCUS:
            memset(gInput.down, 0, sizeof gInput.down);
            gInput.mouse[0] = gInput.mouse[1] = gInput.mouse[2] = false;
            return 0;
        case WM_MOUSEMOVE:
            gInput.mousePx = v2((float)(short)LOWORD(lp), (float)(short)HIWORD(lp));
            return 0;
        case WM_LBUTTONDOWN: if (!gInput.mouse[0]) gInput.mousePressed[0] = true; gInput.mouse[0] = true; SetCapture(hwnd); return 0;
        case WM_LBUTTONUP:   gInput.mouse[0] = false; ReleaseCapture(); return 0;
        case WM_RBUTTONDOWN: if (!gInput.mouse[1]) gInput.mousePressed[1] = true; gInput.mouse[1] = true; return 0;
        case WM_RBUTTONUP:   gInput.mouse[1] = false; return 0;
        case WM_MBUTTONDOWN: if (!gInput.mouse[2]) gInput.mousePressed[2] = true; gInput.mouse[2] = true; return 0;
        case WM_MBUTTONUP:   gInput.mouse[2] = false; return 0;
        case WM_MOUSEWHEEL:
            gInput.wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            // The game draws its own target where the cursor is, so hide the system
            // one over the window. (Over the title bar and borders it stays normal.)
            if (LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void toggleFullscreen(HWND hwnd) {
    const DWORD style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!gFullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &gPrevPlacement) &&
            GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            gFullscreen = true;
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &gPrevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        gFullscreen = false;
    }
}

// A throwaway context is the only way to reach wglChoosePixelFormatARB and
// wglCreateContextAttribsARB, which we need for a real core profile.
static void bootstrapWgl(HINSTANCE hinst) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = hinst;
    wc.lpszClassName = "AstBootstrap";
    wc.style         = CS_OWNDC;
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 1, 1,
                                nullptr, nullptr, hinst, nullptr);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    const int pf = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, pf, &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);

    if (!wglLoadExtensions()) fatal("This GPU driver does not expose WGL_ARB_create_context.");

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hinst);
}

// ---- what the frame loop calls, so the loop is the same on both platforms --

static void platformInit(bool novsync) {
    HINSTANCE hinst = GetModuleHandleA(nullptr);
    SetProcessDPIAware();
    bootstrapWgl(hinst);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = "AsteroidDrifter";
    wc.style         = CS_OWNDC;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);   // for the frame; the client area hides it (WM_SETCURSOR)
    RegisterClassA(&wc);

    RECT rc = { 0, 0, gWidth, gHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    gHwnd = CreateWindowExA(0, wc.lpszClassName, "Asteroid Drifter",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, hinst, nullptr);
    if (!gHwnd) fatal("CreateWindow failed.");
    gDC = GetDC(gHwnd);

    const int pfAttribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB,  1,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB,   WGL_FULL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB,     32,
        WGL_DEPTH_BITS_ARB,     0,
        WGL_STENCIL_BITS_ARB,   0,
        0
    };
    int  pixelFormat = 0;
    UINT numFormats  = 0;
    if (!wglChoosePixelFormatARB(gDC, pfAttribs, nullptr, 1, &pixelFormat, &numFormats) || !numFormats)
        fatal("No suitable pixel format available.");
    PIXELFORMATDESCRIPTOR pfd = {};
    DescribePixelFormat(gDC, pixelFormat, sizeof(pfd), &pfd);
    SetPixelFormat(gDC, pixelFormat, &pfd);

    const int ctxAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    gGLRC = wglCreateContextAttribsARB(gDC, nullptr, ctxAttribs);
    if (!gGLRC) fatal("Could not create an OpenGL 3.3 core context.");
    wglMakeCurrent(gDC, gGLRC);
    if (!glLoadCoreFunctions()) fatal("Missing required OpenGL 3.3 entry points.");
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(novsync ? 0 : 1);

    RECT cr;
    GetClientRect(gHwnd, &cr);
    gWidth  = cr.right - cr.left;
    gHeight = cr.bottom - cr.top;
}

static void platformPump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void swapBuffersNow()        { SwapBuffers(gDC); }
static void toggleFullscreenNow()   { toggleFullscreen(gHwnd); }
static void sleepMs(int ms)         { Sleep((DWORD)ms); }
static bool setVsync(bool on)       { if (!wglSwapIntervalEXT) return false; wglSwapIntervalEXT(on ? 1 : 0); return true; }

static double nowSeconds() {
    LARGE_INTEGER freq, c;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static void platformShutdown() {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gGLRC);
    ReleaseDC(gHwnd, gDC);
    DestroyWindow(gHwnd);
}

#else   // ---------------------------------------------------------- SDL2 ---

static SDL_Window*   gWin = nullptr;
static SDL_GLContext gCtx = nullptr;

static void fatal(const char* msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Asteroid Drifter", msg, gWin);
    exit(1);
}

// The game indexes its key tables with Windows virtual-key codes, so the SDL keys it
// cares about are translated back into those. Letters and digits already agree.
static int vkFromKey(const SDL_Keysym& k) {
    switch (k.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
        case SDLK_ESCAPE:                     return VK_ESCAPE;
        case SDLK_SPACE:                      return VK_SPACE;
        case SDLK_LEFT:                       return VK_LEFT;
        case SDLK_UP:                         return VK_UP;
        case SDLK_RIGHT:                      return VK_RIGHT;
        case SDLK_DOWN:                       return VK_DOWN;
        case SDLK_LALT:                       return VK_LMENU;
        case SDLK_RALT:                       return VK_RMENU;
        case SDLK_LSHIFT:                     return VK_LSHIFT;
        case SDLK_RSHIFT:                     return VK_RSHIFT;
        case SDLK_F1:                         return VK_F1;
        case SDLK_F4:                         return VK_F4;
        case SDLK_F11:                        return VK_F11;
        case SDLK_F12:                        return VK_F12;
        default: break;
    }
    if (k.sym >= SDLK_a && k.sym <= SDLK_z) return 'A' + (k.sym - SDLK_a);   // 'A'..'Z', as Windows reports them
    if (k.sym >= SDLK_0 && k.sym <= SDLK_9) return '0' + (k.sym - SDLK_0);
    return -1;
}

// Windows keeps VK_SHIFT and VK_MENU set whenever either side is down; the game reads
// both the sided and the unsided codes, so they are kept in step here.
static void syncModifiers() {
    gInput.down[VK_SHIFT] = gInput.down[VK_LSHIFT] || gInput.down[VK_RSHIFT];
    gInput.down[VK_MENU]  = gInput.down[VK_LMENU]  || gInput.down[VK_RMENU];
}

static void platformInit(bool novsync) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal(SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    gWin = SDL_CreateWindow("Asteroid Drifter",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            gWidth, gHeight,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!gWin) fatal(SDL_GetError());
    gCtx = SDL_GL_CreateContext(gWin);
    if (!gCtx) fatal("Could not create an OpenGL 3.3 core context.");
    SDL_GL_MakeCurrent(gWin, gCtx);
    if (!glLoadCoreFunctions()) fatal("Missing required OpenGL 3.3 entry points.");
    SDL_GL_SetSwapInterval(novsync ? 0 : 1);
    SDL_ShowCursor(SDL_DISABLE);          // the game draws its own target

    // The drawable can be larger than the window on a HiDPI screen; the renderer works
    // in drawable pixels, and so does Input::mousePx, so mouse positions are scaled.
    SDL_GL_GetDrawableSize(gWin, &gWidth, &gHeight);
}

// Mouse coordinates arrive in window points; the renderer thinks in drawable pixels.
static void mouseTo(int x, int y) {
    int ww = 1, wh = 1, dw = 1, dh = 1;
    SDL_GetWindowSize(gWin, &ww, &wh);
    SDL_GL_GetDrawableSize(gWin, &dw, &dh);
    gInput.mousePx = v2((float)x * (float)dw / (float)(ww ? ww : 1),
                        (float)y * (float)dh / (float)(wh ? wh : 1));
}

static void platformPump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                gRunning = false;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GL_GetDrawableSize(gWin, &gWidth, &gHeight);
                    gResized = true;
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    // A key held as focus goes elsewhere would stick down for ever
                    // (a raised shield that never drops), since the key-up is delivered
                    // to the other window.
                    memset(gInput.down, 0, sizeof gInput.down);
                    gInput.mouse[0] = gInput.mouse[1] = gInput.mouse[2] = false;
                }
                break;
            case SDL_KEYDOWN: {
                if (e.key.repeat) break;
                const int vk = vkFromKey(e.key.keysym);
                if (vk >= 0 && vk < 256) {
                    if (!gInput.down[vk]) gInput.pressed[vk] = true;
                    gInput.down[vk] = true;
                    syncModifiers();
                }
                if (vk == VK_ESCAPE) gRunning = false;
                if (vk == VK_F4 && (e.key.keysym.mod & KMOD_ALT)) gRunning = false;
                break;
            }
            case SDL_KEYUP: {
                const int vk = vkFromKey(e.key.keysym);
                if (vk >= 0 && vk < 256) { gInput.down[vk] = false; syncModifiers(); }
                break;
            }
            case SDL_MOUSEMOTION:
                mouseTo(e.motion.x, e.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int b = -1;
                if      (e.button.button == SDL_BUTTON_LEFT)   b = 0;
                else if (e.button.button == SDL_BUTTON_RIGHT)  b = 1;
                else if (e.button.button == SDL_BUTTON_MIDDLE) b = 2;
                if (b < 0) break;
                if (e.type == SDL_MOUSEBUTTONDOWN) {
                    if (!gInput.mouse[b]) gInput.mousePressed[b] = true;
                    gInput.mouse[b] = true;
                } else {
                    gInput.mouse[b] = false;
                }
                break;
            }
            case SDL_MOUSEWHEEL:
                gInput.wheel += (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f) * (float)e.wheel.y;
                break;
            default: break;
        }
    }
}

static void swapBuffersNow()      { SDL_GL_SwapWindow(gWin); }
static void sleepMs(int ms)       { SDL_Delay((Uint32)ms); }
static bool setVsync(bool on)     { return SDL_GL_SetSwapInterval(on ? 1 : 0) == 0; }
static double nowSeconds()        { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static void toggleFullscreenNow() {
    gFullscreen = !gFullscreen;
    SDL_SetWindowFullscreen(gWin, gFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_GL_GetDrawableSize(gWin, &gWidth, &gHeight);
    gResized = true;
}

static void platformShutdown() {
    SDL_GL_DeleteContext(gCtx);
    SDL_DestroyWindow(gWin);
    SDL_Quit();
}

#endif  // _WIN32

int main(int argc, char** argv) {
    uint64_t seed = 0x5EEDFACEull;
    bool wantFullscreen = false;
    int  frameLimit = 0;
    int  shotFrame  = 0;
    const char* shotPath = "shot.png";
    float zoomArg = 0;
    bool selftest = false;
    bool novsync = false;
    bool dbgTrace = false;
    bool povCam = false;
    bool persistTest = false;
    bool rocketTest = false;
    bool bulletTest = false;
    bool walkTest = false;
    bool sandboxMode = false, levelTest = false, nukeTest = false, enemyTest = false;
    bool peaceful = false;
    bool showcase = false, shipGallery = false;
    bool weaponTest = false, shopTest = false, keyLog = false, lifeTest = false, shipTest = false;
    bool soundCheck = false, soundTest = false, soundQuick = false, noSound = false, soundGameTest = false;
    float volumeArg = -1.0f;
    float aimX = -1, aimY = -1;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "-seed") && i + 1 < argc) seed    = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "-w")    && i + 1 < argc) gWidth  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")    && i + 1 < argc) gHeight = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-fs"))                   wantFullscreen = true;
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc) frameLimit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-shot")     && i + 1 < argc) shotFrame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-shotfile") && i + 1 < argc) shotPath  = argv[++i];
        else if (!strcmp(argv[i], "-zoom")     && i + 1 < argc) zoomArg   = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-aim") && i + 2 < argc) { aimX = (float)atof(argv[i+1]); aimY = (float)atof(argv[i+2]); i += 2; }
        else if (!strcmp(argv[i], "-sandbox"))              sandboxMode = true;
        else if (!strcmp(argv[i], "-leveltest"))            levelTest = true;
        else if (!strcmp(argv[i], "-peaceful"))             peaceful = true;
        else if (!strcmp(argv[i], "-weapontest"))           weaponTest = true;
        else if (!strcmp(argv[i], "-shoptest"))             shopTest = true;
        else if (!strcmp(argv[i], "-keylog"))               keyLog = true;
        else if (!strcmp(argv[i], "-lifetest"))             lifeTest = true;
        else if (!strcmp(argv[i], "-shiptest"))             shipTest = true;
        else if (!strcmp(argv[i], "-soundcheck"))           soundCheck = true;
        else if (!strcmp(argv[i], "-soundtest"))            soundTest = true;
        else if (!strcmp(argv[i], "-soundquick"))           { soundTest = true; soundQuick = true; }
        else if (!strcmp(argv[i], "-soundgametest"))        soundGameTest = true;
        else if (!strcmp(argv[i], "-nosound"))              noSound = true;
        else if (!strcmp(argv[i], "-volume") && i + 1 < argc) volumeArg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-showcase"))             showcase = true;
        else if (!strcmp(argv[i], "-shipgallery"))          shipGallery = true;
        else if (!strcmp(argv[i], "-nuketest"))             nukeTest = true;
        else if (!strcmp(argv[i], "-enemytest"))            enemyTest = true;
        else if (!strcmp(argv[i], "-walktest"))             walkTest = true;
        else if (!strcmp(argv[i], "-bullettest"))           bulletTest = true;
        else if (!strcmp(argv[i], "-rockettest"))           rocketTest = true;
        else if (!strcmp(argv[i], "-persisttest"))          persistTest = true;
        else if (!strcmp(argv[i], "-povcam"))               povCam = true;
        else if (!strcmp(argv[i], "-dbg"))                  dbgTrace = true;
        else if (!strcmp(argv[i], "-novsync"))              novsync = true;
        else if (!strcmp(argv[i], "-selftest")) { selftest = true; if (frameLimit == 0) frameLimit = 900; }
    }

    platformInit(novsync);

    printf("GL %s | %s\n", (const char*)glGetString(GL_VERSION),
                           (const char*)glGetString(GL_RENDERER));
    fflush(stdout);

    static Renderer renderer;
    if (!renderer.init(gWidth, gHeight))
        fatal("Shader compilation failed - run from a console to see the log.");
    renderer.lineWeight = 1.7f;

    static Game game;
    game.debugTrace = dbgTrace;
    // The older unit tests measure the plain toy, without enemies shooting at it.
    game.sandbox    = sandboxMode || walkTest || bulletTest || rocketTest || persistTest;
    // Benchmarks and the scripted demo run must not die or time out.
    game.invincible = selftest;
    if (povCam) game.povCamera = true;
    game.init(renderer, seed);
    if (zoomArg > 0) { game.zoomTarget = zoomArg; game.cam.halfW = zoomArg; }
    if (wantFullscreen) toggleFullscreenNow();

    // ---- helpers shared by the gameplay tests -----------------------------
    // The cursor position that makes the player aim at a world point.
    auto aimAt = [&](Input& in, dv2 target) {
        const v2 rel = tov2(target - game.cam.pos);
        const v2 q = rot(rel, std::cos(game.cam.angle), std::sin(game.cam.angle));
        const float k = (float)renderer.fbw / (2.0f * game.cam.halfW);
        in.mousePx = v2(renderer.fbw * 0.5f + q.x * k, renderer.fbh * 0.5f - q.y * k);
    };
    auto biggestRock = [&]() {
        int best = -1;
        for (int s : game.world.active)
            if (best < 0 || game.world.bodies[s].radius > game.world.bodies[best].radius) best = s;
        return best;
    };
    auto rockMassNear = [&](dv2 c, float R, int* count) {
        double m = 0;
        int n = 0;
        for (int s : game.world.active) {
            const Body& b = game.world.bodies[s];
            if (!b.alive) continue;
            if (len(b.pos - c) < R) { m += b.mass; ++n; }
        }
        if (count) *count = n;
        return m;
    };
    // Teleports the player somewhere and empties a large disc of rocks around it,
    // so a test can watch enemies behave without a rock getting in the way.
    auto openSpot = [&](dv2 p) {
        game.pl.pos = p;
        game.pl.vel = v2(0, 0);
        game.cam.pos = p;
        game.world.streamChunks(p, 2600.0, 100000);
        game.world.clearZone(p, 1800.0);
        game.enemies.clear();  game.missiles.clear();  game.ebullets.clear();
        game.pmissiles.clear();  game.bullets.clear();  game.nukes.clear();
        Input none;
        game.update(renderer, none, 1.0f / 60.0f);      // let the broadphase catch up
        game.pl.pos = p;
        game.pl.vel = v2(0, 0);
    };
    auto plainDrone = [&](dv2 p) {
        Enemy e;
        e.kind = Enemy::Drone;  e.alive = true;  e.pos = p;
        e.id = game.nextId++;
        e.hp = e.maxHp = 30.0f;  e.radius = rules::DRONE_RADIUS;
        e.hasGun = false;  e.hasMissile = false;         // just something to be hit
        return e;
    };

    if (shipGallery) {
        // Eight generated warships, one screenshot each, to look at the variety.
        const float dt = 1.0f / 60.0f;
        game.invincible = true;
        game.floating = true;
        game.startLevel(2);
        game.level.slots.clear();
        game.level.diff.aggroRange = 1.0f;                   // keep them calm for the portrait
        for (int i = 0; i < 8; ++i) {
            Ship s;
            generateShip(s, 1234u + 977u * i, 2 + 2 * i, game.level.diff);
            const dv2 A(40000.0 + 4000.0 * i, 0.0);
            s.anchor = s.pos = A;
            s.baseAngle = s.angle = 0.35f;
            game.ships.clear();  game.enemies.clear();
            game.world.clearZone(A, s.radius + 400.0f);
            game.pl.pos = dv2(A.x, A.y - s.radius * 1.15f);
            game.pl.vel = v2(0, 0);
            game.cam.pos = game.pl.pos;
            game.world.streamChunks(A, 3000.0, 100000);
            game.level.ship = s;  game.level.hasShip = true;  game.level.shipSpawned = false;
            game.spawnShip();
            game.zoomTarget = game.cam.halfW = s.radius * 1.9f;
            Input in;
            in.mousePx = v2(gWidth * 0.5f, gHeight * 0.5f);
            for (int k = 0; k < 20; ++k) game.update(renderer, in, dt);
            game.cam.pos = A;
            game.render(renderer);
            char path[600];
            snprintf(path, sizeof path, "%s_ship%d.png", shotPath, i);
            renderer.screenshot(path);
            printf("ship %d: %-20s style %d, length %.0f, %d weapons, hull %.0f\n", i, s.name, s.style,
                   s.radius * 2.0f, (int)s.weapons.size(), s.maxHp);
        }
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (showcase) {
        // Stage each of the new things and save a screenshot of it. The -shotfile
        // argument is used as the filename prefix.
        const float dt = 1.0f / 60.0f;
        game.invincible = true;
        Input in;
        auto snap = [&](const char* name) {
            game.render(renderer);
            char path[600];
            snprintf(path, sizeof path, "%s_%s.png", shotPath, name);
            renderer.screenshot(path);
        };
        auto run = [&](int n, Input& i) { for (int k = 0; k < n; ++k) game.update(renderer, i, dt); };
        auto zoom = [&](float z) { game.zoomTarget = z; game.cam.halfW = z; };
        in.mousePx = v2(gWidth * 0.62f, gHeight * 0.40f);

        // 1. the level intro, 2. the beacon pointer
        zoom(620.0f);
        run(40, in);
        snap("1_banner");
        run(200, in);
        snap("2_pointer");

        // 3. the beacon, close up
        {
            const dv2 g = game.level.goal;
            openSpot(dv2(g.x - 40.0, g.y - 330.0));
            zoom(560.0f);
            game.floating = true;
            run(20, in);
            snap("3_beacon");
        }

        // 4. a firefight with everything bought: rifle, homing shell, salvo
        game.startLevel(7);
        game.level.slots.clear();
        game.pl.hasHoming = game.pl.hasSalvo = game.pl.hasField = true;
        game.pl.hasShield = true;  game.pl.shield = 100.0f;
        game.pl.salvoAmmo = 12;  game.pl.nukeAmmo = 3;
        {
            game.pl.pos = dv2(0.0, 0.0);
            game.cam.pos = game.pl.pos;
            game.world.streamChunks(game.pl.pos, 3200.0, 100000);
            Input none;
            run(4, none);
            const int big = biggestRock();
            const Body& rock = game.world.bodies[big];
            dv2 sp;  v2 sn;  int sb = -1;
            bool ok = false;
            for (int a = 0; a < 30 && !ok; ++a) {
                const float ang = a * 0.7f;
                const dv2 hint(rock.pos.x + std::cos(ang) * (rock.radius + 30.0),
                               rock.pos.y + std::sin(ang) * (rock.radius + 30.0));
                if (!game.findSurfacePoint(hint, 400.0f, 30.0f, sp, sn, sb)) continue;
                const dv2 stand(sp.x + sn.x * 380.0, sp.y + sn.y * 380.0);
                if (game.world.solidAt(stand) >= 0 || !game.clearLine(dv2(sp.x + sn.x * 30.0, sp.y + sn.y * 30.0), stand)) continue;
                ok = true;
            }
            if (ok) {
                Slot s;  s.hasGun = true;  s.hasMissile = true;
                const v2 side = perp(sn);
                for (int k = 0; k < 3; ++k) {                       // a row of drones to fight
                    Slot d;  d.hasGun = true;  d.hasMissile = (k == 1);
                    game.spawnDrone(d, dv2(sp.x + sn.x * (430.0 + 70.0 * k) + side.x * (-260.0 + 260.0 * k),
                                           sp.y + sn.y * (430.0 + 70.0 * k) + side.y * (-260.0 + 260.0 * k)));
                    game.enemies.back().aggro = true;
                }
                game.spawnTurret(s, sp, sn, sb);
                game.enemies.back().aggro = true;
                game.pl.pos = dv2(sp.x + sn.x * 380.0, sp.y + sn.y * 380.0);
                game.pl.vel = v2(0, 0);
                game.floating = true;
                zoom(560.0f);
                for (int i = 0; i < 260; ++i) {
                    Input act;
                    act.mousePx = v2(gWidth * 0.5f, gHeight * 0.5f);
                    if (game.enemies.size() > 1) aimAt(act, game.enemies[1].pos);
                    act.mouse[0] = (i > 5 && i < 200);
                    if (i == 30)  act.pressed['G'] = true;         // the salvo
                    if (i == 100) act.pressed['F'] = true;         // the homing shell
                    game.update(renderer, act, dt);
                    if (i == 36)  snap("4_salvo");
                    if (i == 112) snap("5_homing");
                }
            }
        }

        // 5. the force field, with a stream of bullets breaking on it
        {
            const dv2 C(6000.0, 6000.0);
            openSpot(C);
            game.floating = true;
            game.pl.fieldOn = false;  game.pl.field = 100.0f;
            Input on;  on.pressed['X'] = true;
            game.update(renderer, on, dt);
            zoom(430.0f);
            Input idle;
            for (int i = 0; i < 70; ++i) {
                if (i % 3 == 0) {
                    for (int k = 0; k < 3; ++k) {
                        EnemyBullet b;
                        b.pos = dv2(C.x - 430.0, C.y + (k - 1) * 40.0 + (i % 7));
                        b.vel = v2(720.0f, (float)(k - 1) * -20.0f);
                        b.life = 2.0f;  b.damage = 8.0f;
                        game.ebullets.push_back(b);
                    }
                }
                game.update(renderer, idle, dt);
                game.pl.pos = C;  game.pl.vel = v2(0, 0);
            }
            snap("6_field");
        }

        // 5b. the blast shield, with fire coming in on the side it covers
        {
            const dv2 C(6000.0, 6000.0);
            openSpot(C);
            game.floating = true;
            game.pl.fieldOn = false;
            game.pl.hasShield = true;  game.pl.shield = 70.0f;
            zoom(300.0f);
            Input hold;
            hold.down[VK_LMENU] = true;
            for (int i = 0; i < 50; ++i) {
                aimAt(hold, dv2(C.x + 500.0, C.y));
                if (i % 3 == 0) {
                    EnemyBullet b;
                    b.pos = dv2(C.x + 330.0, C.y + ((i / 3) % 3 - 1) * 5.0);
                    b.vel = v2(-700.0f, 0.0f);
                    b.life = 2.0f;  b.damage = 4.0f;
                    game.ebullets.push_back(b);
                }
                game.update(renderer, hold, dt);
                game.pl.pos = C;  game.pl.vel = v2(0, 0);
                if (i == 34) snap("6b_shield");
            }
        }

        // 6. the nuke: in flight with its countdown, then the blast
        game.enemies.clear();  game.missiles.clear();  game.ebullets.clear();  game.pmissiles.clear();
        {
            game.pl.fieldOn = false;
            game.pl.pos = dv2(0.0, 0.0);  game.cam.pos = game.pl.pos;
            game.world.streamChunks(game.pl.pos, 3200.0, 100000);
            Input none;
            run(4, none);
            const int big = biggestRock();
            const Body& rock = game.world.bodies[big];
            game.pl.pos = dv2(rock.pos.x + rock.radius + 70.0, rock.pos.y + 10.0);
            game.pl.vel = rock.vel;
            game.cam.pos = game.pl.pos;
            game.floating = true;
            zoom(700.0f);
            in.mousePx = v2(gWidth * 0.5f, gHeight * 0.30f);
            run(5, in);
            game.pl.nukeCd = 0;
            game.throwNuke(v2(0.0f, 1.0f));
            for (int i = 0; i < 260 && !game.nukes.empty(); ++i) {
                game.update(renderer, in, dt);
                if (game.nukes.empty()) break;
                game.cam.pos = dv2((game.nukes[0].pos.x + game.pl.pos.x) * 0.5, (game.nukes[0].pos.y + game.pl.pos.y) * 0.5);
                if (game.nukes[0].fuse < 2.6f && game.nukes[0].fuse > 2.55f) snap("7_nuke_thrown");
                if (game.nukes[0].fuse < 0.9f && game.nukes[0].fuse > 0.85f) snap("8_nuke_fuse");
            }
            zoom(900.0f);
            run(9, in);
            snap("9_nuke_blast");
        }

        // 6c. a warship
        {
            game.startLevel(2);
            game.level.slots.clear();
            game.pl.hasHoming = game.pl.hasSalvo = true;  game.pl.salvoAmmo = 12;
            const dv2 A = game.level.ship.anchor;
            game.pl.pos = dv2(A.x - 350.0, A.y - 130.0 - game.level.ship.radius);
            game.pl.vel = v2(0, 0);
            game.cam.pos = game.pl.pos;
            game.world.streamChunks(A, 3200.0, 100000);
            game.floating = true;
            zoom(820.0f);
            Input act;
            for (int i = 0; i < 300; ++i) {
                game.pl.pos = dv2(A.x - 350.0, A.y - 130.0 - game.level.ship.radius);  game.pl.vel = v2(0, 0);
                act.mousePx = v2(gWidth * 0.5f, gHeight * 0.25f);
                if (!game.ships.empty()) aimAt(act, game.ships[0].pos);
                act.mouse[0] = i > 40 && i < 250;
                if (i == 90) act.pressed['G'] = true;
                game.update(renderer, act, dt);
                act.pressed['G'] = false;
                if (i == 20)  snap("C_ship_wakes");
                if (i == 200) snap("C_ship_fight");
            }
        }

        // 7. the depot, and 8. game over
        game.waves.clear();  game.parts.clear();  game.flash = 0.0f;   // a clean backdrop for the menu
        game.state = State::LevelComplete;
        game.stateTime = rules::COMPLETE_TIME + 1.0f;
        game.credits = 640;
        run(2, in);
        game.pl.hasHoming = true;  game.pl.hasField = false;  game.pl.salvoAmmo = 12;  game.pl.nukeAmmo = 3;
        {
            float x, y, w, h;
            game.shopRect(ITEM_SALVO, (float)renderer.fbw, (float)renderer.fbh, x, y, w, h);
            in.mousePx = v2(x + w * 0.5f, y + h * 0.5f);
            game.buy(ITEM_NUKE);                                  // so the depot shows a purchase note
        }
        run(3, in);
        snap("A_shop");
        game.state = State::Playing;
        game.lives = 2;
        game.killPlayer("SUIT BREACH");                          // a life lost, one still in hand
        run(70, in);
        snap("B_lifelost");
        game.state = State::Playing;
        game.lives = 1;
        game.killPlayer("SUIT BREACH");                          // the last one
        run(80, in);
        snap("B_gameover");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (shopTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        game.invincible = true;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        printf("shoptest:\n");

        // ---- a run starts with nothing but the rifle
        game.startRun(renderer);
        game.invincible = true;
        check(!game.pl.hasHoming && !game.pl.hasSalvo && !game.pl.hasField &&
              game.pl.nukeAmmo == 0 && game.credits == 0, "a fresh run owns no weapons and has no credits");
        {
            Input f;  f.pressed['F'] = true;  f.pressed['G'] = true;  f.pressed['N'] = true;  f.pressed['X'] = true;
            game.update(renderer, f, dt);
            check(game.bullets.empty() && game.pmissiles.empty() && game.nukes.empty() && !game.pl.fieldOn,
                  "F, G, N and X do nothing until the items are bought");
        }

        // ---- finishing a level pays out, then opens the depot
        {
            const int credits0 = game.credits;
            const float left = game.level.timeLeft;
            game.pl.pos = game.level.goal;
            game.pl.vel = v2(0, 0);
            game.update(renderer, idle, dt);
            const int expected = rules::CREDIT_LEVEL * 1 + (int)(left * rules::CREDIT_PER_SEC);
            printf("  finished level 1 with %.1f s left: paid %d credits (expected about %d)\n",
                   left, game.credits - credits0, expected);
            check(game.state == State::LevelComplete, "reaching the beacon completes the level");
            check(std::abs(game.credits - credits0 - expected) <= 12, "and pays 100 per level plus 10 per second left");
            for (int i = 0; i < 60 * 3 && game.state != State::Shop; ++i) game.update(renderer, idle, dt);
            check(game.state == State::Shop, "the depot opens after the celebration");
            check(game.enemies.empty() && game.ebullets.empty() && game.missiles.empty(),
                  "everything hostile stands down while you shop");
        }

        // ---- the world holds still in the depot
        {
            const int big = biggestRock();
            const dv2 p0 = game.world.bodies[big].pos;
            for (int i = 0; i < 40; ++i) game.update(renderer, idle, dt);
            check(len(game.world.bodies[big].pos - p0) < 1e-6, "the world is frozen while the depot is open");
        }

        // ---- buying: prices, ownership, magazine limits
        const int startCredits = game.credits;
        game.credits = 0;
        int refused = 0;
        for (int i = 0; i < ITEM_COUNT; ++i) if (!game.buy(i)) ++refused;
        check(refused == ITEM_COUNT && game.credits == 0 && !game.pl.hasHoming, "with no credits, nothing can be bought");

        game.credits = 5000;
        check(game.buy(ITEM_HOMING) && game.pl.hasHoming && game.credits == 5000 - rules::PRICE_HOMING,
              "the homing shell unlocks for its price");
        check(!game.buy(ITEM_HOMING) && game.credits == 5000 - rules::PRICE_HOMING,
              "and cannot be bought twice or charged twice");
        check(game.buy(ITEM_SALVO) && game.pl.hasSalvo && game.pl.salvoAmmo == rules::SALVO_LOAD,
              "the missile salvo unlocks with its first load");
        int before = game.credits;
        check(game.buy(ITEM_SALVO) && game.pl.salvoAmmo == 2 * rules::SALVO_LOAD &&
              before - game.credits == rules::PRICE_SALVO_AMMO, "more salvos cost the cheaper refill price");
        while (game.buy(ITEM_SALVO)) {}
        check(game.pl.salvoAmmo == rules::SALVO_MAX, "the salvo magazine stops at its limit");
        before = game.credits;
        check(!game.buy(ITEM_SALVO) && game.credits == before, "and a full magazine is not charged for");
        check(game.buy(ITEM_NUKE) && game.pl.nukeAmmo == rules::NUKE_PACK && !game.canBuy(ITEM_NUKE) == false,
              "a nuke pack adds three warheads");
        game.credits = 5000;
        while (game.buy(ITEM_NUKE)) {}
        check(game.pl.nukeAmmo == rules::NUKE_MAX, "nukes cap at nine");
        game.credits = 5000;
        check(game.buy(ITEM_FIELD) && game.pl.hasField, "the force field unlocks");
        check(game.buy(ITEM_SHIELD) && game.pl.hasShield && game.pl.shield == rules::SHIELD_CAPACITY,
              "the blast shield is fitted fully charged");
        before = game.credits;
        check(!game.buy(ITEM_SHIELD) && game.credits == before, "a full shield cannot be bought again");
        game.pl.shield = 30.0f;
        check(game.buy(ITEM_SHIELD) && game.pl.shield == rules::SHIELD_CAPACITY &&
              before - game.credits == rules::PRICE_SHIELD_REFILL, "a spent shield can be refilled for the refill price");

        // ---- clicking a row buys it, clicking continue leaves
        game.pl.hasHoming = false;
        game.credits = 1000;
        {
            float x, y, w, h;
            game.shopRect(ITEM_HOMING, (float)renderer.fbw, (float)renderer.fbh, x, y, w, h);
            Input click;
            click.mousePx = v2(x + w * 0.5f, y + h * 0.5f);
            click.mousePressed[0] = true;
            game.update(renderer, click, dt);
            check(game.pl.hasHoming && game.credits == 1000 - rules::PRICE_HOMING, "clicking a row buys that item");
        }
        {
            const int lv = game.level.number;
            Input go;
            go.pressed[VK_RETURN] = true;
            game.update(renderer, go, dt);
            check(game.state == State::Playing && game.level.number == lv + 1, "Enter leaves the depot for the next level");
            check(game.pl.hasHoming && game.pl.hasField && game.pl.hasSalvo, "purchases carry over into the next level");
        }

        // ---- a new run forgets everything
        game.startRun(renderer);
        check(!game.pl.hasHoming && !game.pl.hasField && game.credits == 0 && game.pl.nukeAmmo == 0,
              "starting a new run resets purchases and credits");
        (void)startCredits;

        printf("shoptest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (soundGameTest) {
        // Plays the game against a mixer with no device behind it and checks that the
        // things that happen make the sounds they should.
        audio::startOffline();
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        auto plays = [&](Sfx s) { return audio::stats().plays[(int)s]; };
        auto run = [&](Input& in, int n) { for (int i = 0; i < n; ++i) game.update(renderer, in, dt); };
        auto mixSome = [&](int frames) {
            std::vector<int16_t> buf((size_t)frames * 2);
            for (int d = 0; d < frames; d += 512) audio::mixInto(buf.data() + (size_t)d * 2, std::min(512, frames - d));
        };
        auto loopGain = [&](Sustain l) { return audio::stats().loops[(int)l]; };
        printf("soundgametest:\n");

        game.startRun(renderer);
        game.invincible = true;
        game.level.slots.clear();
        check(plays(Sfx::LevelStart) >= 1, "a run starts with the level sting");
        run(idle, 5);
        check(plays(Sfx::BeaconPing) >= 1, "the beacon pings");

        // ---- the spaceman
        game.respawn();
        run(idle, 30);
        {
            Input in;  in.mousePx = v2(gWidth * 0.6f, gHeight * 0.3f);  in.mouse[0] = true;
            run(in, 12);
            check(plays(Sfx::Rifle) >= 1, "firing the rifle");
        }
        {
            Input in;  in.pressed[VK_SPACE] = true;
            game.pl.grounded = true;  game.pl.coyote = 0.1f;  game.pl.jumpCd = 0.0f;
            run(in, 1);
            check(plays(Sfx::Jump) >= 1, "jumping");
        }
        {
            game.floating = true;
            Input in;  in.mouse[1] = true;
            run(in, 30);
            mixSome(audio::RATE / 2);
            check(loopGain(Sustain::Thruster) > 0.6f, "the rocket loop swells in while it burns");
            Input off;
            run(off, 3);
            mixSome(audio::RATE);
            check(loopGain(Sustain::Thruster) < 0.05f, "and fades out when it stops");
            // out of fuel: the dry click
            game.pl.fuel = 0.0f;  game.pl.fuelLocked = true;
            run(in, 3);
            check(plays(Sfx::FuelEmpty) >= 1, "an empty tank clicks");
            game.pl.fuel = 100.0f;  game.pl.fuelLocked = false;
            game.floating = false;
        }
        {
            game.pl.hasField = true;  game.pl.field = 100.0f;
            Input in;  in.pressed['X'] = true;
            run(in, 1);
            check(plays(Sfx::FieldOn) >= 1, "the force field switches on with a sound");
            Input keep;
            run(keep, 20);
            mixSome(audio::RATE / 2);
            check(loopGain(Sustain::FieldHum) > 0.3f, "and hums while it is on");
            run(in, 1);
            check(plays(Sfx::FieldOff) >= 1, "switching it off has a sound too");
        }
        {
            game.pl.hasShield = true;  game.pl.shield = 100.0f;
            Input in;  in.down[VK_LMENU] = true;  in.mousePx = v2(gWidth * 0.9f, gHeight * 0.5f);
            run(in, 30);
            mixSome(audio::RATE / 2);
            check(plays(Sfx::ShieldUp) >= 1 && loopGain(Sustain::ShieldHum) > 0.3f, "raising the shield rings and hums");
            const v2 aim = fromAngle(game.pl.aim);
            game.shieldAbsorb(dv2(game.pl.pos.x + aim.x * 46.0, game.pl.pos.y + aim.y * 46.0), 4.0f);
            check(plays(Sfx::ShieldBlock) >= 1, "stopping something clangs");
            game.shieldAbsorb(dv2(game.pl.pos.x + aim.x * 46.0, game.pl.pos.y + aim.y * 46.0), 500.0f);
            check(plays(Sfx::ShieldBreak) >= 1, "overloading it breaks it");
            game.pl.hasShield = false;
        }
        {
            game.pl.hasSalvo = true;  game.pl.salvoAmmo = 6;
            Input in;  in.pressed['G'] = true;
            run(in, 1);
            check(plays(Sfx::Salvo) >= 1, "the missile salvo");
            game.pl.nukeAmmo = 1;  game.pl.nukeCd = 0.0f;
            Input n;  n.pressed['N'] = true;
            run(n, 1);
            check(plays(Sfx::NukeThrow) >= 1, "throwing a nuke");
            run(idle, 60 * 4);
            check(plays(Sfx::NukeBeep) >= 8, "the fuse beeps");
            check(plays(Sfx::NukeBoom) >= 1, "and it goes off");
        }

        // ---- the enemy
        {
            game.pl.hasSalvo = false;
            Slot s;  s.hasGun = true;  s.hasMissile = true;
            game.spawnDrone(s, dv2(game.pl.pos.x + 260.0, game.pl.pos.y + 120.0));
            Enemy& d = game.enemies.back();
            d.aggro = true;  d.gunCd = 0.0f;  d.missileCd = 0.0f;
            run(idle, 60 * 3);
            check(plays(Sfx::EnemyShot) >= 1, "an enemy's bullets");
            check(plays(Sfx::MissileLaunch) >= 1, "a missile launch");
            Missile m;  m.id = 999;  m.pos = dv2(game.pl.pos.x + 260.0, game.pl.pos.y);  m.vel = v2(-100, 0);
            m.life = 8.0f;  m.maxSpeed = 350.0f;  m.turn = 1.0f;
            game.missiles.push_back(m);
            run(idle, 10);
            check(plays(Sfx::MissileWarn) >= 1, "a missile close by sets off the warning");
            for (Enemy& e : game.enemies) if (e.alive) game.damageEnemy(e, 5.0f, e.pos);
            check(plays(Sfx::EnemyHit) >= 1, "hitting an enemy");
            for (Enemy& e : game.enemies) if (e.alive) game.damageEnemy(e, 5000.0f, e.pos);
            check(plays(Sfx::ExplodeS) >= 1, "and killing one");
        }
        {
            const uint32_t before = plays(Sfx::ExplodeM);
            game.boom(game.pl.pos, 46.0f, pal::PLAYER);
            check(plays(Sfx::ExplodeM) > before, "a bigger blast makes the bigger sound");
        }
        {
            sleepMs(120);                                    // the sounds have minimum gaps, measured in real time
            const uint32_t s0 = plays(Sfx::RockSplit), h0 = plays(Sfx::RockHit);
            WorldEvent e;  e.kind = WorldEvent::Split;  e.pos = game.pl.pos;
            game.world.events.push_back(e);
            e.kind = WorldEvent::Impact;
            game.world.events.push_back(e);
            run(idle, 1);
            check(plays(Sfx::RockSplit) > s0 && plays(Sfx::RockHit) > h0, "rocks breaking and being hit");
        }

        // ---- the clock
        {
            game.startLevel(1);
            game.level.slots.clear();
            game.level.timeLeft = 6.5f;
            run(idle, 60 * 4);
            check(plays(Sfx::ClockTick) >= 1 && plays(Sfx::ClockTickHi) >= 1, "the clock ticks in the last seconds, faster at the very end");
        }

        // ---- the depot
        {
            game.startRun(renderer);
            game.invincible = true;
            game.level.slots.clear();
            game.pl.pos = game.level.goal;  game.pl.vel = v2(0, 0);
            run(idle, 1);
            check(plays(Sfx::LevelComplete) >= 1, "reaching the beacon");
            for (int i = 0; i < 60 * 3 && game.state != State::Shop; ++i) game.update(renderer, idle, dt);
            run(idle, 2);
            check(plays(Sfx::ShopOpen) >= 1, "the depot opens with a sound");
            float x, y, w, h;
            game.shopRect(ITEM_HOMING, (float)renderer.fbw, (float)renderer.fbh, x, y, w, h);
            Input hover;  hover.mousePx = v2(x + w * 0.5f, y + h * 0.5f);
            run(hover, 2);
            check(plays(Sfx::ShopHover) >= 1, "hovering over an item ticks");
            game.credits = 0;
            Input click = hover;  click.mousePressed[0] = true;
            run(click, 1);
            check(plays(Sfx::ShopDeny) >= 1, "trying to buy without the credits buzzes");
            game.credits = 1000;
            run(click, 1);
            check(plays(Sfx::ShopBuy) >= 1, "buying rings");
            const uint32_t ls = plays(Sfx::LevelStart);
            Input go;  go.pressed[VK_RETURN] = true;
            run(go, 1);
            check(plays(Sfx::ShopContinue) >= 1 && plays(Sfx::LevelStart) > ls, "leaving the depot starts the next level");
        }

        // ---- dying
        {
            game.startRun(renderer);
            game.invincible = false;
            game.level.slots.clear();
            game.hurtPlayer(30.0f);
            check(plays(Sfx::Hurt) >= 1, "being hit");
            game.hurtPlayer(1000.0f);
            check(plays(Sfx::Death) >= 1, "losing a life");
            for (int i = 0; i < 60 * 4 && game.state == State::Dead; ++i) game.update(renderer, idle, dt);
            check(plays(Sfx::Respawn) >= 1, "getting another go");
            game.lives = 1;
            game.hurtPlayer(1000.0f);
            check(plays(Sfx::GameOver) >= 1, "losing the last one");
            game.startRun(renderer);
            game.lives = 1;
            game.startLevel(5);
            game.startLevel(6);
            check(plays(Sfx::LivesRestored) >= 1, "and the top-up on level 6");
        }

        // ---- a warship
        {
            game.startRun(renderer);
            game.invincible = true;
            game.startLevel(2);
            game.level.slots.clear();
            const dv2 A = game.level.ship.anchor;
            game.pl.pos = dv2(A.x, A.y - 1500.0);
            game.world.streamChunks(A, 3200.0, 100000);
            run(idle, 2);
            check(plays(Sfx::ShipAlert) >= 1, "a warship coming into range");
            Ship& S = game.ships[0];
            // arm two of its weapons with a cannon and a flak gun, wake it, and stand in front of it
            game.floating = true;
            game.level.diff.aggroRange = 5000.0f;
            game.level.diff.gunRange = 3000.0f;
            int given = 0;
            for (ShipWeapon& w : S.weapons) {
                Enemy* e = game.findEnemy(w.enemyId);
                if (!e) continue;
                e->weapon = given == 0 ? (int)ShipWeapon::Cannon : (int)ShipWeapon::Flak;
                e->gunCd = 0.0f;
                if (++given == 2) break;
            }
            const dv2 stand(S.pos.x, S.pos.y - S.radius - 300.0);
            for (int i = 0; i < 60 * 8; ++i) {
                game.pl.pos = stand;  game.pl.vel = v2(0, 0);
                game.update(renderer, idle, dt);
            }
            check(plays(Sfx::CannonCharge) >= 1 && plays(Sfx::CannonFire) >= 1, "its cannon winds up and fires");
            check(plays(Sfx::FlakFire) >= 1, "its flak guns thump");
            Bullet b;  b.pos = S.pos;  b.vel = v2(1, 0);  b.caliber = 4.0f;  b.life = 1.0f;
            game.bulletHitsTargets(b);
            check(plays(Sfx::ShipHit) >= 1, "bullets ring off its hull");
            const uint32_t m0 = plays(Sfx::ExplodeM) + plays(Sfx::ExplodeS) + plays(Sfx::ExplodeL);
            game.damageShip(S, 1e9f, S.pos);
            check(plays(Sfx::ShipDeath) >= 1, "and when it dies it dies loudly");
            check(plays(Sfx::ExplodeM) + plays(Sfx::ExplodeS) + plays(Sfx::ExplodeL) == m0, "with its own sound rather than a pile of small ones");
        }

        // ---- the controls
        {
            Input p;  p.pressed['P'] = true;
            run(p, 1);
            check(plays(Sfx::Pause) >= 1, "pausing");
            run(p, 1);
            Input m;  m.pressed['M'] = true;
            run(m, 1);
            check(audio::muted(), "M mutes");
            run(m, 1);
            check(!audio::muted(), "and unmutes");
        }

        audio::shutdown();
        printf("soundgametest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (soundCheck) {
        const int rc = audio::check(shotPath);
        renderer.shutdown();
        return rc;
    }
    if (soundTest) {
        const int rc = audio::audition(soundQuick);
        renderer.shutdown();
        return rc;
    }

    if (shipTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        printf("shiptest:\n");

        // ---- which levels get a warship
        game.startRun(renderer);
        game.invincible = true;
        game.floating = true;
        {
            int with = 0, evenWith = 0, oddWith = 0;
            std::string pattern;
            for (int n = 1; n <= 30; ++n) {
                game.startLevel(n);
                const bool has = game.level.hasShip;
                pattern += has ? 'S' : '.';
                if (n >= 2) { with += has; (n % 2 == 0 ? evenWith : oddWith) += has; }
            }
            printf("  levels 1-30: %s\n", pattern.c_str());
            game.startLevel(1);
            check(!game.level.hasShip, "level 1 has no warship");
            game.startLevel(2);
            check(game.level.hasShip, "level 2 has the first one");
            check(with >= 10 && with <= 20, "roughly every other level from 2 on has one");
            check(evenWith > oddWith * 2, "mostly the even levels");
            game.startLevel(4);
            const std::string a = game.level.ship.name;
            game.startLevel(4);
            check(a == game.level.ship.name && game.level.hasShip, "a level always brings the same ship back");
        }

        // ---- generation
        {
            int bad = 0, weaponsBad = 0, same = 0;
            std::vector<std::string> names;
            for (int i = 0; i < 200; ++i) {
                Ship s, s2;
                generateShip(s, 555u + 31u * i, 2 + i % 20, game.level.diff);
                generateShip(s2, 555u + 31u * i, 2 + i % 20, game.level.diff);
                if (s.hull.size() != s2.hull.size() || s.weapons.size() != s2.weapons.size() ||
                    std::strcmp(s.name, s2.name) != 0) ++same;
                // The outline must not cross itself.
                const size_t n = s.hull.size();
                bool crossed = false;
                for (size_t a = 0; a < n && !crossed; ++a)
                    for (size_t b = a + 2; b < n && !crossed; ++b) {
                        if (a == 0 && b == n - 1) continue;
                        const v2 p1 = s.hull[a], p2 = s.hull[(a + 1) % n], p3 = s.hull[b], p4 = s.hull[(b + 1) % n];
                        auto side = [](v2 o, v2 p, v2 q) { return cross(p - o, q - o); };
                        const float d1 = side(p3, p4, p1), d2 = side(p3, p4, p2), d3 = side(p1, p2, p3), d4 = side(p1, p2, p4);
                        if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) &&
                            std::fabs(d1) > 1e-3f && std::fabs(d2) > 1e-3f && std::fabs(d3) > 1e-3f && std::fabs(d4) > 1e-3f)
                            crossed = true;
                    }
                if (crossed) ++bad;
                for (const ShipWeapon& w : s.weapons)
                    if (len(w.local) > s.radius) ++weaponsBad;
                names.push_back(s.name);
            }
            check(bad == 0, "200 generated outlines, none crosses itself");
            check(weaponsBad == 0, "every weapon is on the ship");
            check(same == 0, "the same seed always gives the same ship");
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            check(names.size() > 60, "the names vary");
        }

        // ---- a level with a ship: berth, clock, spawn
        game.startRun(renderer);
        game.invincible = true;
        game.startLevel(2);
        game.level.slots.clear();
        game.level.diff.aggroRange = 1.0f;                   // asleep until a test wakes it
        const dv2 A = game.level.ship.anchor;
        const float berth = game.level.ship.berth;
        const int nWeapons = (int)game.level.ship.weapons.size();
        {
            check(game.level.timeLeft == game.level.diff.timeLimit && game.level.diff.timeLimit >= rules::TIME_MIN + rules::SHIP_TIME_BONUS - 0.01f,
                  "the clock has the detour bonus on top");
            const float along = (float)(len(A - game.level.start) / game.level.diff.distance);
            check(along > 0.3f && along < 0.7f, "the berth is on the way, between start and beacon");
            game.pl.pos = dv2(A.x, A.y - 1500.0);
            game.pl.vel = v2(0, 0);
            game.cam.pos = game.pl.pos;
            game.world.streamChunks(A, 3200.0, 100000);
            game.world.step(dt, game.pl.pos);
            game.world.syncGeometry(renderer);
            int inside = 0;
            for (const Body& b : game.world.bodies)
                if (b.alive && len(b.pos - A) < berth) ++inside;
            check(inside == 0, "no rock inside the berth");
            check(game.ships.empty(), "the ship is not in play until the player is near");
            game.pl.pos = dv2(A.x, A.y - 1400.0);
            game.update(renderer, idle, dt);
            check(game.ships.size() == 1 && game.level.shipSpawned, "it wakes up as the player approaches");
            int hp = 0;
            for (const Enemy& e : game.enemies) if (e.kind == Enemy::Hardpoint) ++hp;
            check(hp == nWeapons && nWeapons >= 3, "one hardpoint enemy per weapon");
        }
        Ship& sh = game.ships[0];

        // ---- the hull stops the player
        {
            v2 out;
            check(game.hullDistance(sh, sh.pos, &out) < 0.0f, "the middle of the ship is inside the hull");
            check(game.hullDistance(sh, dv2(sh.pos.x + sh.radius * 3.0, sh.pos.y)) > 0.0f, "far outside is outside");
            game.pl.pos = sh.pos;
            game.pl.vel = v2(90.0f, 0.0f);
            game.update(renderer, idle, dt);
            check(game.hullDistance(sh, game.pl.pos) >= rules::PLAYER_HIT_R - 0.6f, "a player put inside is pushed back out");
            const dv2 from(sh.pos.x, sh.pos.y - sh.radius * 1.2);
            game.pl.pos = from;
            game.pl.vel = norm(tov2(sh.pos - from)) * 200.0f;
            float minD = 1e9f;
            for (int i = 0; i < 200; ++i) {
                game.update(renderer, idle, dt);
                minD = std::min(minD, game.hullDistance(sh, game.pl.pos));
            }
            check(minD > 0.0f, "flying at the hull, the player never gets inside it");
        }

        // ---- its weapons
        {
            Enemy* e = game.findEnemy(sh.weapons[0].enemyId);
            check(e && e->kind == Enemy::Hardpoint && e->alive, "the weapons are enemies you can hit");
            if (e) {
                const int cr0 = game.credits;
                const float hp0 = sh.hp;
                game.damageEnemy(*e, 99999.0f, e->pos);
                check(game.credits - cr0 == rules::CREDIT_WEAPON, "destroying a weapon pays");
                check(std::fabs((hp0 - sh.hp) - sh.maxHp * sh.hullShare) < 0.5f, "and costs the hull a slice");
            }
        }

        // ---- shooting it
        {
            game.pl.pos = dv2(sh.pos.x, sh.pos.y - sh.radius * 3.0);
            game.pl.vel = v2(0, 0);
            const float hp0 = sh.hp;
            Bullet b;
            b.pos = sh.pos;  b.vel = v2(1, 0);  b.life = 1;  b.caliber = 4.0f;  b.budget = 100;
            const bool used = game.bulletHitsTargets(b);
            check(used, "a rifle bullet inside the hull is stopped by it");
            check(std::fabs((hp0 - sh.hp) - rules::RIFLE_DAMAGE * rules::SHIP_RIFLE_FACTOR) < 0.01f,
                  "and does only a fraction of its damage to the hull");
            const float hp1 = sh.hp;
            game.explode(sh.pos, rules::HEAVY_SPLASH_R, 55.0f, 0, 0, 0, false);
            check(hp1 - sh.hp > 40.0f, "a blast goes straight through the armour");
            const float hp2 = sh.hp;
            game.explode(dv2(sh.pos.x + sh.radius + 500.0, sh.pos.y), 100.0f, 55.0f, 0, 0, 0, false);
            check(sh.hp == hp2, "a blast well outside does nothing to it");
            game.explode(sh.pos, rules::NUKE_RADIUS, 99999.0f, 0, 0, 0, true);
            check(std::fabs((hp2 - sh.hp) - sh.maxHp * rules::SHIP_NUKE_SHARE) < 1.0f || !sh.alive,
                  "a nuke on the hull takes a fixed share, not the lot");
            sh.hp = sh.maxHp;
            sh.alive = true;
        }

        // ---- every kind of weapon fires, and what it costs the player
        {
            const int levels[3] = { 2, 6, 12 };
            for (int li = 0; li < 3; ++li) {
                game.startRun(renderer);
                game.invincible = true;
                game.startLevel(levels[li]);
                game.level.slots.clear();
                Ship s2;
                generateShip(s2, 9001u + li, levels[li], game.level.diff);
                s2.anchor = s2.pos = dv2(60000.0, 60000.0);
                s2.baseAngle = s2.angle = 0.0f;
                game.level.ship = s2;
                game.world.clearZone(s2.anchor, s2.radius + 400.0f);
                game.world.streamChunks(s2.anchor, 3200.0, 100000);
                game.enemies.clear();  game.ships.clear();
                game.spawnShip();
                Ship& S = game.ships[0];
                const dv2 stand(S.pos.x, S.pos.y - S.radius * 0.6 - 450.0);   // 450 units off the beam, standing still
                game.pl.pos = stand;
                game.pl.vel = v2(0, 0);
                game.floating = true;
                game.level.diff.aggroRange = 4000.0f;
                game.level.timeLeft = 9999.0f;
                int types[4] = { 0, 0, 0, 0 };
                for (const ShipWeapon& w : S.weapons) ++types[w.type];
                const float dmg0 = game.damageTaken;
                const int shots0 = game.enemyShots, miss0 = game.missilesLaunched;
                bool charged = false;
                for (int i = 0; i < 60 * 12; ++i) {
                    game.pl.pos = stand;
                    game.pl.vel = v2(0, 0);
                    game.update(renderer, idle, dt);
                    for (const Enemy& e : game.enemies)
                        if (e.kind == Enemy::Hardpoint && e.weapon == ShipWeapon::Cannon && e.burst == 1) charged = true;
                }
                printf("  level %2d '%s': %d guns %d flak %d missile %d cannon | 12 s: %d bullets, %d missiles, %.0f damage (%.1f/s)\n",
                       levels[li], S.name, types[0], types[1], types[2], types[3],
                       game.enemyShots - shots0, game.missilesLaunched - miss0,
                       game.damageTaken - dmg0, (game.damageTaken - dmg0) / 12.0f);
                check(game.enemyShots - shots0 + game.missilesLaunched - miss0 > 5, "an awake ship shoots at the player");
                if (types[3] > 0) check(charged, "its cannons show a charge-up before firing");
                if (li == 0) {
                    const int cr0 = game.credits;
                    game.damageShip(S, 1e9f, S.pos);
                    check(!game.ships[0].alive, "a ship at zero hull is destroyed");
                    int liveWeapons = 0;
                    for (const Enemy& e : game.enemies) if (e.kind == Enemy::Hardpoint && e.alive) ++liveWeapons;
                    check(liveWeapons == 0, "and takes all its weapons with it");
                    check(game.credits - cr0 == rules::CREDIT_SHIP_BASE + rules::CREDIT_SHIP_PER_LEVEL * levels[li],
                          "and pays the bounty");
                    game.update(renderer, idle, dt);
                    check(game.ships.empty(), "the wreck is removed");
                }
            }
        }

        // ---- dying and retrying brings the whole ship back
        {
            game.startRun(renderer);
            game.invincible = false;
            game.startLevel(2);
            game.level.slots.clear();
            const std::string name = game.level.ship.name;
            const dv2 anchor = game.level.ship.anchor;
            game.pl.pos = dv2(anchor.x, anchor.y - 1500.0);
            game.world.streamChunks(anchor, 3200.0, 100000);
            game.update(renderer, idle, dt);
            check(game.ships.size() == 1, "warship in play");
            if (!game.ships.empty()) game.damageShip(game.ships[0], 1e9f, anchor);
            game.hurtPlayer(1000.0f);
            for (int i = 0; i < 60 * 4 && game.state == State::Dead; ++i) game.update(renderer, idle, dt);
            game.pl.pos = dv2(anchor.x, anchor.y - 1500.0);
            game.update(renderer, idle, dt);
            check(game.state == State::Playing && game.ships.size() == 1 && game.ships[0].alive &&
                  name == game.ships[0].name && game.ships[0].hp == game.ships[0].maxHp,
                  "after a lost life the same ship is back at full health");
        }

        printf("shiptest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (lifeTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        auto settle = [&](float seconds) {
            for (int i = 0; i < (int)(seconds * 60.0f) && !game.playerGone(); ++i) game.update(renderer, idle, dt);
        };
        printf("lifetest:\n");

        // ---- a run starts with three
        game.startRun(renderer);
        game.invincible = false;
        check(game.lives == rules::LIVES_START && rules::LIVES_START == 3, "a run starts with 3 lives");

        // ---- losing one restarts the same level
        game.pl.hasHoming = true;  game.pl.salvoAmmo = 7;
        game.earn(100);  game.levelCredits = game.credits;  game.levelEarned = game.totalEarned;
        const dv2 goal = game.level.goal, start = game.level.start;
        const int slotCount = (int)game.level.slots.size();
        const float limit = game.level.diff.timeLimit;
        settle(0.5f);
        game.earn(60);                                        // credits picked up during the doomed attempt
        game.pl.pos = dv2(start.x + 500.0, start.y + 300.0);
        game.hurtPlayer(1000.0f);
        check(game.state == State::Dead && game.lives == 2, "a fatal hit costs a life and enters the Dead state");
        game.killPlayer("SUIT BREACH");
        check(game.lives == 2, "dying twice in the same instant only costs one life");
        const dv2 deathPos = game.pl.pos;
        for (int i = 0; i < (int)((rules::DEATH_TIME + 0.5f) * 60.0f) && game.state == State::Dead; ++i)
            game.update(renderer, idle, dt);
        check(game.state == State::Playing, "after a short beat the game carries on");
        check(game.level.number == 1 && game.lives == 2, "same level, one life down");
        check(len(game.level.goal - goal) < 1e-6 && len(game.level.start - start) < 1e-6 &&
              (int)game.level.slots.size() == slotCount, "the beacon and the gauntlet are exactly as they were");
        check(game.level.timeLeft > limit - 0.5f, "the clock is full again");
        printf("    health %.1f, enemies %d, enemy bullets %d, missiles %d\n", game.pl.health, (int)game.enemies.size(), (int)game.ebullets.size(), (int)game.missiles.size());
        check(game.pl.health == 100.0f && game.ebullets.empty() && game.missiles.empty(), "a fresh suit and no fire in the air");
        check(len(game.pl.pos - deathPos) > 300.0 && len(game.pl.pos - start) < 1500.0, "back at the start of the level");
        check(game.pl.hasHoming && game.pl.salvoAmmo == 7, "purchases and ammunition stay as they were");
        check(game.credits == game.levelCredits && game.totalEarned == game.levelEarned && game.credits == 100,
              "credits are back to what they were when the level began");

        // ---- running out of time costs a life too
        game.level.timeLeft = 0.02f;
        settle(0.3f);
        check(game.state == State::Dead && game.lives == 1, "time up costs a life");
        for (int i = 0; i < (int)((rules::DEATH_TIME + 0.5f) * 60.0f) && game.state == State::Dead; ++i)
            game.update(renderer, idle, dt);
        check(game.state == State::Playing && game.lives == 1, "and the level restarts");

        // ---- the last life ends the run
        game.hurtPlayer(1000.0f);
        check(game.state == State::GameOver && game.lives == 0, "losing the last life is game over");
        for (int i = 0; i < 60 * 4; ++i) game.update(renderer, idle, dt);
        check(game.state == State::GameOver, "and the level does not restart on its own");
        {
            Input enter;  enter.pressed[VK_RETURN] = true;
            game.update(renderer, enter, dt);
            check(game.state == State::Playing && game.lives == 3 && game.level.number == 1, "Enter starts a new run with 3 lives");
        }

        // ---- every fifth level tops the lives back up
        game.startLevel(2);
        game.lives = 1;
        game.startLevel(5);
        check(game.lives == 1, "level 5 does not top them up");
        game.startLevel(6);
        check(game.lives == 3, "level 6 does");
        game.lives = 2;  game.startLevel(7);
        check(game.lives == 2, "level 7 does not");
        game.lives = 1;  game.startLevel(11);
        check(game.lives == 3, "level 11 does");
        game.lives = 3;  game.startLevel(16);
        check(game.lives == 3, "and never past 3");
        game.lives = 1;  game.startLevel(6, true);
        check(game.lives == 1, "retrying level 6 after a death does not refill them");

        // ---- and it happens through the real flow: beat level 5, leave the depot
        game.startRun(renderer);
        game.invincible = true;
        game.startLevel(5);
        game.lives = 1;
        game.pl.pos = game.level.goal;  game.pl.vel = v2(0, 0);
        game.update(renderer, idle, dt);
        for (int i = 0; i < 60 * 3 && game.state != State::Shop; ++i) game.update(renderer, idle, dt);
        check(game.state == State::Shop && game.lives == 1, "the depot after level 5 still shows the lives left");
        {
            Input go;  go.pressed[VK_RETURN] = true;
            game.update(renderer, go, dt);
            check(game.state == State::Playing && game.level.number == 6 && game.lives == 3,
                  "leaving it for level 6 restores 3 lives");
        }

        printf("lifetest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (weaponTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        game.invincible = true;
        game.floating = true;                             // hold still for the measurements
        game.startLevel(3);
        game.level.slots.clear();
        game.level.diff.droneSpeed = 0.0f;                 // targets that stay where they are put
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        auto target = [&](dv2 p, float hp) {
            Enemy e = plainDrone(p);
            e.hp = e.maxHp = hp;
            game.enemies.push_back(e);
        };
        printf("weapontest:\n");

        // ---- the homing shell curves onto a target a straight shot would miss
        float hpAfter[2] = { 0, 0 };
        for (int mode = 0; mode < 2; ++mode) {              // 0 = homing, 1 = same shell with homing off
            const dv2 C(6000.0, 6000.0);
            openSpot(C);
            game.pl.hasHoming = true;
            game.pl.heavyCd = 0.0f;
            target(dv2(C.x + 480.0, C.y + 260.0), 200.0f);      // 28 degrees off the line of aim
            Input fire;
            aimAt(fire, dv2(C.x + 900.0, C.y));
            fire.pressed['F'] = true;
            game.update(renderer, fire, dt);
            bool found = false;
            for (Bullet& b : game.bullets) if (b.heavy) { found = true; if (mode == 1) b.homing = false; }
            if (mode == 0) check(found && game.bullets[0].homing, "the charge shot is a homing shell");
            for (int i = 0; i < 100; ++i) game.update(renderer, idle, dt);
            hpAfter[mode] = game.enemies.empty() ? 0.0f : game.enemies[0].hp;
        }
        printf("  target health after the shot: %.0f with homing, %.0f without\n", hpAfter[0], hpAfter[1]);
        check(hpAfter[0] < 200.0f, "the homing shell hits a target well off its line");
        check(hpAfter[1] >= 200.0f, "the identical shell without homing misses it");

        // ---- the salvo: five missiles, five different targets
        {
            const dv2 C(9000.0, 9000.0);
            openSpot(C);
            game.pl.hasSalvo = true;
            game.pl.salvoAmmo = 4;
            game.pl.salvoCd = 0.0f;
            for (int i = 0; i < 5; ++i) {
                const float a = (i - 2) * 0.5f;
                target(dv2(C.x + std::cos(a) * (600.0 + 60.0 * i), C.y + std::sin(a) * (600.0 + 60.0 * i)), 22.0f);
            }
            const int kills0 = game.kills;
            const int credits0 = game.credits;
            Input fire;
            aimAt(fire, dv2(C.x + 900.0, C.y));
            fire.pressed['G'] = true;
            game.update(renderer, fire, dt);
            std::vector<int> ids;
            for (const PMissile& m : game.pmissiles) ids.push_back(m.targetId);
            std::sort(ids.begin(), ids.end());
            const bool distinct = std::unique(ids.begin(), ids.end()) == ids.end() && ids[0] != 0;
            check(game.pmissiles.size() == 5, "one press launches five small missiles");
            check(distinct, "each missile is assigned a different target");
            check(game.pl.salvoAmmo == 3, "and it costs one load of ammunition");
            game.update(renderer, fire, dt);
            check(game.pl.salvoAmmo == 3 && game.pmissiles.size() == 5, "the launcher has a cooldown between salvos");
            for (int i = 0; i < 200; ++i) game.update(renderer, idle, dt);
            int alive = 0;
            for (const Enemy& e : game.enemies) if (e.alive) ++alive;
            printf("  after the salvo: %d of 5 targets left, %d kills, %d credits earned\n",
                   alive, game.kills - kills0, game.credits - credits0);
            check(alive == 0 && game.kills - kills0 == 5, "five missiles destroy five separate targets");
            check(game.credits - credits0 == 5 * rules::CREDIT_DRONE, "and the kills pay out in credits");
        }
        // With fewer targets than missiles the spares double up; with none they fly straight.
        {
            const dv2 C(12000.0, 12000.0);
            openSpot(C);
            game.pl.salvoCd = 0.0f;
            target(dv2(C.x + 600.0, C.y + 100.0), 500.0f);
            target(dv2(C.x + 700.0, C.y - 120.0), 500.0f);
            Input fire;
            aimAt(fire, dv2(C.x + 900.0, C.y));
            fire.pressed['G'] = true;
            game.update(renderer, fire, dt);
            std::vector<int> ids;
            for (const PMissile& m : game.pmissiles) ids.push_back(m.targetId);
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            check(game.pmissiles.size() == 5 && ids.size() == 2, "with two targets, the five missiles share them out");

            openSpot(dv2(15000.0, 15000.0));
            game.pl.salvoCd = 0.0f;
            game.pl.salvoAmmo = 2;
            Input fire2;
            aimAt(fire2, dv2(15900.0, 15000.0));
            fire2.pressed['G'] = true;
            game.update(renderer, fire2, dt);
            bool allFree = game.pmissiles.size() == 5;
            for (const PMissile& m : game.pmissiles) if (m.targetId != 0) allFree = false;
            check(allFree, "with no targets, the missiles simply fly out on the aim line");
        }
        // Enemy missiles are valid targets too.
        {
            const dv2 C(18000.0, 18000.0);
            openSpot(C);
            game.pl.salvoCd = 0.0f;
            game.pl.salvoAmmo = 2;
            Missile em;
            em.id = game.nextId++;  em.pos = dv2(C.x + 500.0, C.y + 60.0);  em.vel = v2(-30.0f, 0.0f);
            em.life = 9.0f;  em.maxSpeed = 60.0f;  em.turn = 0.0f;  em.damage = 30.0f;
            game.missiles.push_back(em);
            Input fire;
            aimAt(fire, dv2(C.x + 900.0, C.y));
            fire.pressed['G'] = true;
            const int credits0 = game.credits;
            game.update(renderer, fire, dt);
            for (int i = 0; i < 160 && !game.missiles.empty(); ++i) game.update(renderer, idle, dt);
            check(game.missiles.empty(), "an incoming enemy missile can be shot down with the salvo");
            check(game.credits == credits0, "and that pays nothing");
        }

        // ---- the force field
        {
            const dv2 C(21000.0, 21000.0);
            // Bullets: a stream aimed at the player, once with the field and once without.
            float dmg[2] = { 0, 0 };
            for (int mode = 0; mode < 2; ++mode) {
                openSpot(C);
                game.pl.hasField = true;
                game.pl.field = 100.0f;
                game.pl.fieldOn = false;
                game.pl.fieldLocked = false;
                if (mode == 0) {
                    Input on;  on.pressed['X'] = true;
                    game.update(renderer, on, dt);
                    check(game.pl.fieldOn, "X switches the force field on");
                }
                const float before = game.damageTaken;
                for (int i = 0; i < 90; ++i) {
                    if (i % 5 == 0) {
                        EnemyBullet b;
                        b.pos = dv2(C.x - 450.0, C.y + ((i / 5) % 3 - 1) * 4.0);
                        b.vel = v2(700.0f, 0.0f);
                        b.life = 2.0f;  b.damage = 10.0f;
                        game.ebullets.push_back(b);
                    }
                    game.update(renderer, idle, dt);
                    game.pl.pos = C;  game.pl.vel = v2(0, 0);
                }
                dmg[mode] = game.damageTaken - before;
            }
            printf("  damage from a stream of bullets: %.0f with the field, %.0f without\n", dmg[0], dmg[1]);
            check(dmg[0] < 1.0f, "the field turns every bullet away");
            check(dmg[1] > 30.0f, "the same stream hurts without it");

            // Missiles, drones, rocks.
            openSpot(C);
            game.pl.hasField = true;  game.pl.field = 100.0f;  game.pl.fieldOn = false;  game.pl.fieldLocked = false;
            Missile m;
            m.id = game.nextId++;  m.pos = dv2(C.x - 120.0, C.y);  m.vel = v2(60.0f, 0.0f);
            m.life = 6.0f;  m.maxSpeed = 200.0f;  m.turn = 1.0f;  m.damage = 30.0f;
            game.missiles.push_back(m);
            target(dv2(C.x, C.y + 110.0), 500.0f);
            const int rock = game.world.spawn(dv2(C.x + 210.0, C.y - 20.0), 55.0f, 4242u);
            Input none;
            game.update(renderer, none, dt);                   // the rock joins the simulation
            game.pl.pos = C;
            const dv2 rockStart = game.world.bodies[rock].pos;
            Input on;  on.pressed['X'] = true;
            game.update(renderer, on, dt);
            for (int i = 0; i < 40; ++i) { game.update(renderer, idle, dt); game.pl.pos = C; game.pl.vel = v2(0, 0); }
            const double missileD = game.missiles.empty() ? 999.0 : len(game.missiles[0].pos - C);
            const double droneD = game.enemies.empty() ? 0.0 : len(game.enemies.back().pos - C);
            const double rockMoved = len(game.world.bodies[rock].pos - rockStart);
            printf("  after 0.7 s: missile %.0f from the player (was 120), drone %.0f (was 110), rock moved %.0f\n",
                   missileD, droneD, rockMoved);
            check(missileD > 200.0, "a homing missile is pushed back out of the bubble");
            check(droneD > 200.0, "a drone is shoved away");
            check(rockMoved > 15.0, "a nearby rock is flung outward");

            // Energy: drains while on, then it cuts out and stays off until recharged.
            game.pl.field = 100.0f;  game.pl.fieldOn = true;  game.pl.fieldLocked = false;
            for (int i = 0; i < 60; ++i) game.update(renderer, idle, dt);
            printf("  field energy after 1 s: %.0f (drain is %.0f per second)\n", game.pl.field, rules::FIELD_DRAIN);
            check(std::fabs(game.pl.field - (100.0f - rules::FIELD_DRAIN)) < 3.0f, "it drains at the set rate while on");
            for (int i = 0; i < 60 * 5; ++i) game.update(renderer, idle, dt);
            check(!game.pl.fieldOn && game.pl.fieldLocked, "it switches itself off when the energy runs out");
            game.pl.field = 2.0f;
            Input again;  again.pressed['X'] = true;
            game.update(renderer, again, dt);
            check(!game.pl.fieldOn, "and cannot be relit until it has recharged");
        }

        // ---- the blast shield
        {
            const dv2 C(24000.0, 24000.0);
            openSpot(C);
            game.pl.hasShield = false;
            game.pl.fuel = 100.0f;
            Input rm;
            rm.mouse[1] = true;
            aimAt(rm, dv2(C.x + 500.0, C.y));
            game.update(renderer, rm, dt);
            check(game.pl.thrusting && !game.pl.shieldUp, "right mouse is the rocket");
            Input alt;
            alt.down[VK_LMENU] = true;
            aimAt(alt, dv2(C.x + 500.0, C.y));
            game.update(renderer, alt, dt);
            check(!game.pl.shieldUp, "Left Alt does nothing before the shield is bought");
            Input ralt;
            ralt.down[VK_RMENU] = true;
            aimAt(ralt, dv2(C.x + 500.0, C.y));
            game.pl.hasShield = true;  game.pl.shield = rules::SHIELD_CAPACITY;
            game.update(renderer, ralt, dt);
            check(!game.pl.shieldUp, "and Right Alt never raises it: only Left Alt does");
            game.pl.fuel = 100.0f;  game.pl.fuelLocked = false;
            game.update(renderer, alt, dt);
            check(game.pl.shieldUp && !game.pl.thrusting, "once owned, holding Left Alt raises the shield");
            game.update(renderer, rm, dt);
            check(game.pl.thrusting && !game.pl.shieldUp, "and right mouse is still the rocket, shield or no shield");
            Input both;
            both.down[VK_LMENU] = true;  both.mouse[1] = true;
            aimAt(both, dv2(C.x + 500.0, C.y));
            game.update(renderer, both, dt);
            check(game.pl.thrusting && game.pl.shieldUp, "the two can be used together");
            Input sh;
            sh.down[VK_SHIFT] = true;
            aimAt(sh, dv2(C.x + 500.0, C.y));
            game.update(renderer, sh, dt);
            check(game.pl.thrusting && !game.pl.shieldUp, "Shift also fires the rocket");

            // Space and W jump whether or not the shield is owned; Alt never does.
            {
                auto jumpSpeed = [&](int key) {
                    game.pl.vel = v2(0, 0);  game.pl.up = v2(0, 1);
                    game.pl.coyote = 0.1f;   game.pl.jumpCd = 0.0f;
                    Input j;
                    j.pressed[key] = true;
                    game.update(renderer, j, dt);
                    return game.pl.vel.y;
                };
                game.pl.hasShield = false;
                const float spaceBefore = jumpSpeed(VK_SPACE);
                game.pl.hasShield = true;
                const float spaceAfter = jumpSpeed(VK_SPACE);
                const float wAfter = jumpSpeed('W');
                const float altAfter = jumpSpeed(VK_LMENU);
                printf("  jump speed: Space before the shield %.0f, Space after %.0f, W after %.0f, Alt %.0f\n",
                       spaceBefore, spaceAfter, wAfter, altAfter);
                check(spaceBefore > 100.0f && spaceAfter > 100.0f, "Space jumps, with or without the shield");
                check(wAfter > 100.0f, "and so does W");
                check(altAfter < 30.0f, "Alt does not jump");
            }

            // Fire a few bullets at the player from `fromDeg` (0 = the side the cursor is on).
            auto volley = [&](float fromDeg, int count, float damage, float* shieldSpent) {
                openSpot(C);
                game.floating = true;
                game.pl.hasShield = true;  game.pl.shield = rules::SHIELD_CAPACITY;
                const float before = game.damageTaken;
                const v2 dirFrom = fromAngle(fromDeg * PIF / 180.0f);
                Input hold;
                hold.down[VK_LMENU] = true;
                for (int i = 0; i < 90; ++i) {
                    aimAt(hold, dv2(C.x + 500.0, C.y));
                    if (i % 4 == 0 && i < 4 * count) {
                        EnemyBullet b;
                        b.pos = dv2(C.x + dirFrom.x * 380.0, C.y + dirFrom.y * 380.0);
                        b.vel = dirFrom * -700.0f;
                        b.life = 2.0f;  b.damage = damage;
                        game.ebullets.push_back(b);
                    }
                    game.update(renderer, hold, dt);
                    game.pl.pos = C;  game.pl.vel = v2(0, 0);
                }
                if (shieldSpent) *shieldSpent = rules::SHIELD_CAPACITY - game.pl.shield;
                return game.damageTaken - before;
            };
            float spent = 0;
            float dmg = volley(0.0f, 4, 10.0f, &spent);
            printf("  4 bullets from the cursor side: %.0f damage through, %.0f charge spent\n", dmg, spent);
            check(dmg < 0.5f && std::fabs(spent - 40.0f) < 1.0f, "bullets from the cursor side are stopped and cost their damage in charge");
            dmg = volley(180.0f, 4, 10.0f, &spent);
            printf("  4 bullets from behind: %.0f damage through, %.0f charge spent\n", dmg, spent);
            check(dmg > 35.0f && spent < 0.5f, "bullets from behind ignore it entirely");
            dmg = volley(30.0f, 4, 10.0f, &spent);
            printf("  4 bullets from 30 degrees off: %.0f damage through, %.0f charge spent\n", dmg, spent);
            check(dmg > 35.0f && spent < 0.5f, "the plate is only 30 degrees wide: a shot from further round gets through");
            dmg = volley(11.0f, 4, 10.0f, &spent);
            check(dmg < 0.5f, "but a shot from just inside its edge is still stopped");
            dmg = volley(0.0f, 3, 40.0f, &spent);
            printf("  3 heavy hits (120 damage) against 100 charge: %.0f through, %.0f charge spent\n", dmg, spent);
            check(std::fabs(dmg - 20.0f) < 1.5f && spent > 99.0f, "an overwhelmed shield empties and the rest gets through");

            // Blasts: a missile detonating in the arc is soaked up; from behind it is not.
            auto missileFrom = [&](float fromDeg, float* shieldSpent) {
                openSpot(C);
                game.floating = true;
                game.pl.hasShield = true;  game.pl.shield = rules::SHIELD_CAPACITY;
                const float before = game.damageTaken;
                const v2 dirFrom = fromAngle(fromDeg * PIF / 180.0f);
                Missile m;
                m.id = game.nextId++;
                m.pos = dv2(C.x + dirFrom.x * 300.0, C.y + dirFrom.y * 300.0);
                m.vel = dirFrom * -220.0f;
                m.life = 6.0f;  m.maxSpeed = 260.0f;  m.turn = 2.0f;  m.damage = 30.0f;
                game.missiles.push_back(m);
                Input hold;
                hold.down[VK_LMENU] = true;
                for (int i = 0; i < 100 && !game.missiles.empty(); ++i) {
                    aimAt(hold, dv2(C.x + 500.0, C.y));
                    game.update(renderer, hold, dt);
                    game.pl.pos = C;  game.pl.vel = v2(0, 0);
                }
                if (shieldSpent) *shieldSpent = rules::SHIELD_CAPACITY - game.pl.shield;
                return game.damageTaken - before;
            };
            dmg = missileFrom(0.0f, &spent);
            printf("  a missile from the cursor side: %.1f damage through, %.1f charge spent\n", dmg, spent);
            check(dmg < 0.5f && spent > 5.0f, "a homing missile blows up on the plate and the blast is soaked up");
            const float behind = missileFrom(180.0f, &spent);
            printf("  a missile from behind: %.1f damage through\n", behind);
            check(behind > 10.0f, "the same missile from behind hurts");

            // A nuke: in the arc it is mostly absorbed, behind you it is not.
            auto nukeAt = [&](float fromDeg, float* shieldSpent) {
                openSpot(C);
                game.floating = true;
                game.pl.hasShield = true;  game.pl.shield = rules::SHIELD_CAPACITY;
                const float before = game.damageTaken;
                const v2 dirFrom = fromAngle(fromDeg * PIF / 180.0f);
                Nuke n;
                n.pos = dv2(C.x + dirFrom.x * 170.0, C.y + dirFrom.y * 170.0);
                n.fuse = 0.001f;
                game.nukes.push_back(n);
                Input hold;
                hold.down[VK_LMENU] = true;
                aimAt(hold, dv2(C.x + 500.0, C.y));
                game.update(renderer, hold, dt);
                if (shieldSpent) *shieldSpent = rules::SHIELD_CAPACITY - game.pl.shield;
                return game.damageTaken - before;
            };
            const float nukeFront = nukeAt(0.0f, &spent);
            const float frontSpent = spent;
            const float nukeBack = nukeAt(180.0f, &spent);
            printf("  nuke 170 units away: %.0f damage with the shield toward it (%.0f charge), %.0f from behind\n",
                   nukeFront, frontSpent, nukeBack);
            check(nukeFront < nukeBack * 0.5f && frontSpent > 30.0f, "a nuke going off in front of the shield does far less harm");

            // An empty shield cannot be raised.
            game.pl.shield = 0.0f;
            Input hold;
            hold.down[VK_LMENU] = true;
            game.update(renderer, hold, dt);
            check(!game.pl.shieldUp, "an empty shield stays down until it is refilled");
        }

        printf("weapontest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (levelTest) {
        // A deliberately naive pilot: it rockets straight at the beacon, ignores
        // enemies and does not steer round rocks. That makes it a pessimistic
        // stand-in for a player, good for checking the time limit is fair.
        game.invincible = true;
        const float dt = 1.0f / 60.0f;
        Input in;
        printf("leveltest: bot flies straight at the beacon, ignoring rocks and enemies\n");
        int passed = 0;
        for (int lv = 1; lv <= 6; ++lv) {
            if (peaceful) game.level.slots.clear();       // no enemies: pure navigation time
            const float limit = game.level.diff.timeLimit;
            const double dist0 = len(game.level.goal - game.pl.pos);
            int tur = 0, dro = 0;
            for (const Slot& s : game.level.slots) {
                if (s.type == Slot::TurretSlot) ++tur;
                if (s.type == Slot::DroneSlot)  ++dro;
            }
            const float dmg0 = game.damageTaken;
            const int shots0 = game.enemyShots, miss0 = game.missilesLaunched;
            int frames = 0;
            bool reached = false;
            while (frames < 60 * 90) {
                // A competent pilot steers its *velocity* along the best lane it can
                // see: score each heading by how far it runs clear, weighted toward
                // the beacon, and slow down when the way is tight. Thrust then closes
                // the gap between the wanted and actual velocity, which cancels
                // gravity drift instead of fighting it every frame.
                const v2 toGoal = norm(tov2(game.level.goal - game.pl.pos));
                const v2 vel = game.pl.vel;
                const float look = clampf(len(vel) * 1.5f, 450.0f, 1200.0f);
                v2 heading = toGoal;
                float bestScore = -1.0f, bestClear = 0.0f;
                for (int k = -9; k <= 9; ++k) {
                    const float off = k * 0.19f;
                    const v2 d = rot(toGoal, off);
                    float clear = look;
                    for (float s = 24.0f; s < look; s += 14.0f) {
                        const dv2 p(game.pl.pos.x + d.x * s, game.pl.pos.y + d.y * s);
                        if (game.world.solidAt(p) >= 0) { clear = s; break; }
                    }
                    const float score = (clear / look) * 0.75f + 0.25f * std::cos(off);
                    if (score > bestScore) { bestScore = score; heading = d; bestClear = clear; }
                }
                const float cruise = clampf(bestClear * 0.9f, 160.0f, 900.0f);
                v2 want = heading * cruise - vel;
                const bool needBurn = len(want) > 140.0f;
                v2 thrustDir = norm(want);
                // On a rock the rocket only works pointed well above the surface
                // (gravity nearly matches its thrust), so hop and tilt up.
                in.pressed[VK_SPACE] = game.pl.grounded;
                if (game.pl.grounded) { thrustDir = norm(thrustDir + game.pl.up * 1.6f); }
                aimAt(in, dv2(game.pl.pos.x + thrustDir.x * 1000.0, game.pl.pos.y + thrustDir.y * 1000.0));
                const bool tank = game.pl.fuel > (in.mouse[1] ? 2.0f : 30.0f);
                in.mouse[1] = tank && (needBurn || game.pl.grounded);
                game.update(renderer, in, dt);
                if (lv == 1 && frames % 120 == 0 && frames <= 60 * 50)
                    printf("    t=%4.1f pos=(%.0f,%.0f) vel=%.0f fuel=%.0f locked=%d thrusting=%d grounded=%d "
                           "dist=%.0f left=%.1f state=%d\n", frames * dt, game.pl.pos.x, game.pl.pos.y,
                           len(game.pl.vel), game.pl.fuel, (int)game.pl.fuelLocked, (int)game.pl.thrusting,
                           (int)game.pl.grounded, (float)len(game.level.goal - game.pl.pos),
                           game.level.timeLeft, (int)game.state);
                ++frames;
                if (game.state == State::LevelComplete) { reached = true; break; }
            }
            const float used = frames * dt;
            printf("  level %d: %5.0f m in %4.1f s limit -> bot %s %4.1f s | %2d turrets %2d drones | "
                   "took %4.0f dmg, %3d shots, %2d missiles\n",
                   lv, dist0, limit, reached ? (used <= limit ? "made it in" : "LATE at   ") : "FAILED after",
                   used, tur, dro, game.damageTaken - dmg0, game.enemyShots - shots0,
                   game.missilesLaunched - miss0);
            if (reached && used <= limit) ++passed;
            in.mouse[1] = false;
            // Past the celebration, through the depot (the bot just skips it), and on.
            for (int i = 0; i < 60 * 12 && game.state != State::Playing; ++i) {
                in.pressed[VK_RETURN] = (game.state == State::Shop);
                game.update(renderer, in, dt);
            }
            in.pressed[VK_RETURN] = false;
        }
        printf("leveltest: bot beat the clock on %d of 6 levels\n", passed);
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (nukeTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        game.invincible = false;
        game.level.slots.clear();                        // no spawns muddying the counts
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        const int big = biggestRock();
        const Body& rock = game.world.bodies[big];
        const dv2 B(rock.pos.x + rock.radius + 260.0, rock.pos.y);     // ground zero, in open space
        const float R = rules::NUKE_RADIUS;
        printf("nuketest: blast radius %.0f, ground zero beside a rock of radius %.0f\n", R, rock.radius);

        // ---- a thrown nuke is slow, and gravity bends it hard
        {
            game.pl.pos = dv2(rock.pos.x + rock.radius + 60.0, rock.pos.y);
            game.pl.vel = rock.vel;
            game.pl.nukeAmmo = 3;
            game.pl.nukeCd = 0;
            const v2 aim(0.0f, 1.0f);                   // tangent to the rock
            game.throwNuke(aim);
            check(game.nukes.size() == 1 && game.pl.nukeAmmo == 2, "throwing uses one of the three warheads");
            const dv2 start = game.nukes[0].pos;
            const v2 v0 = game.nukes[0].vel;
            for (int i = 0; i < 60; ++i) game.update(renderer, idle, dt);
            const Nuke& n = game.nukes[0];
            const double straight = len(dv2(start.x + v0.x, start.y + v0.y) - dv2(start.x, start.y));
            const double bent = len(n.pos - dv2(start.x + v0.x, start.y + v0.y));
            printf("  after 1 s: speed %.0f, dragged %.0f units off a straight line (a %.0f-unit throw), fuse %.2f\n",
                   len(n.vel), bent, straight, n.fuse);
            check(bent > 40.0, "gravity drags the grenade far off a straight line");
            check(n.fuse > rules::NUKE_FUSE - 1.2f && n.fuse < rules::NUKE_FUSE - 0.8f, "the fuse ticks down with the clock");
            game.nukes.clear();
        }

        // ---- the blast itself
        game.pl.nukeAmmo = 0;
        game.enemies.clear();
        game.enemies.push_back(plainDrone(dv2(B.x, B.y + 100.0)));            // inside
        game.enemies.push_back(plainDrone(dv2(B.x - 200.0, B.y + 200.0)));     // inside
        game.enemies.push_back(plainDrone(dv2(B.x, B.y + R * 1.35f)));         // outside
        int rocksBefore = 0;
        const double massBefore = rockMassNear(B, R * 0.8f, &rocksBefore);
        game.pl.pos = dv2(B.x, B.y - R * 1.5);           // safe distance for this part
        game.pl.vel = v2(0, 0);
        Nuke nk;
        nk.pos = B;  nk.fuse = 0.02f;
        game.nukes.push_back(nk);
        for (int i = 0; i < 4; ++i) game.update(renderer, idle, dt);
        const bool flashed = game.flash > 0.3f;
        const size_t rings = game.waves.size();
        for (int i = 0; i < 40; ++i) game.update(renderer, idle, dt);       // let the splits settle
        int rocksAfter = 0;
        const double massAfter = rockMassNear(B, R * 0.8f, &rocksAfter);
        int alive = 0;
        for (const Enemy& e : game.enemies) if (e.alive) ++alive;
        printf("  rock inside the crater: %d rocks / mass %.0f  ->  %d rocks / mass %.0f\n",
               rocksBefore, massBefore, rocksAfter, massAfter);
        check(rocksBefore > 0, "there was rock to destroy");
        check(massAfter < massBefore * 0.35, "the blast removes most of the rock in its radius");
        check(alive == 1, "both drones in the blast die, the one outside survives");
        check(flashed && rings >= 3, "white-out flash and several shockwave rings");

        // ---- how it hurts the player, by distance
        const float dists[4] = { 60.0f, R * 0.5f, R * 0.9f, R * 1.4f };
        float dmgAt[4];
        for (int i = 0; i < 4; ++i) {
            game.pl.health = 100.0f;
            game.pl.sinceHurt = 0;
            game.pl.pos = dv2(B.x, B.y - dists[i]);
            game.pl.vel = v2(0, 0);
            const float before = game.damageTaken;
            Nuke k2;  k2.pos = B;  k2.fuse = 0.001f;   // goes off this frame
            game.nukes.push_back(k2);
            // One frame only: the blast's own damage lands on the detonation frame.
            // Later frames add debris hitting the player, which is real but not
            // what is being measured here.
            game.update(renderer, idle, dt);
            dmgAt[i] = game.damageTaken - before;
            game.state = State::Playing;
            game.pl.health = 100.0f;
        }
        printf("  damage to the player at %.0f / %.0f / %.0f / %.0f units: %.0f / %.0f / %.0f / %.0f\n",
               dists[0], dists[1], dists[2], dists[3], dmgAt[0], dmgAt[1], dmgAt[2], dmgAt[3]);
        check(dmgAt[0] > 90.0f, "standing at the centre is lethal");
        check(dmgAt[0] > dmgAt[1] && dmgAt[1] > dmgAt[2], "damage falls off with distance");
        check(dmgAt[3] < 1.0f, "well outside the blast, no damage");

        printf("nuketest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (enemyTest) {
        const float dt = 1.0f / 60.0f;
        Input idle;
        game.invincible = true;                          // keep going, but count the damage
        game.startLevel(7);                              // missiles and fast bullets are in play
        game.level.slots.clear();
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        printf("enemytest: level 7 rules (bullets %.0f u/s, missiles %.0f u/s, turn %.1f rad/s)\n",
               game.level.diff.bulletSpeed, game.level.diff.missileSpeed, game.level.diff.missileTurn);

        // ---- a turret bolted to a rock shoots at you and fires missiles
        const int big = biggestRock();
        const Body& rock = game.world.bodies[big];
        dv2 sp;  v2 sn;  int sb = -1;
        bool found = false;
        // Try points round the rock until one has open sky and a clear shot.
        for (int attempt = 0; attempt < 30 && !found; ++attempt) {
            const float a = attempt * 0.7f;
            const dv2 hint(rock.pos.x + std::cos(a) * (rock.radius + 30.0),
                           rock.pos.y + std::sin(a) * (rock.radius + 30.0));
            if (!game.findSurfacePoint(hint, 400.0f, 30.0f, sp, sn, sb)) continue;
            const dv2 stand(sp.x + sn.x * 450.0, sp.y + sn.y * 450.0);
            const dv2 muzzle(sp.x + sn.x * 20.0, sp.y + sn.y * 20.0);
            if (game.world.solidAt(stand) >= 0 || !game.clearLine(muzzle, stand)) continue;
            found = true;
        }
        check(found, "a turret can be mounted on the surface of a rock");
        if (found) {
            const Body& sbody = game.world.bodies[sb];
            printf("    surface point: field there %.2f (should be ~0), 3 units in %.2f, 3 units out %.2f | "
                   "solidAt(point) %d, solidAt(3 in) %d\n",
                   sbody.f.sample(sbody.toLocal(sp)),
                   sbody.f.sample(sbody.toLocal(dv2(sp.x - sn.x * 3.0, sp.y - sn.y * 3.0))),
                   sbody.f.sample(sbody.toLocal(dv2(sp.x + sn.x * 3.0, sp.y + sn.y * 3.0))),
                   game.world.solidAt(sp), game.world.solidAt(dv2(sp.x - sn.x * 3.0, sp.y - sn.y * 3.0)));
            Slot s;  s.hasGun = true;  s.hasMissile = true;
            game.spawnTurret(s, sp, sn, sb);
            Enemy& t = game.enemies.back();
            // The player stands in clear space 500 units out along the turret's normal.
            game.pl.pos = dv2(sp.x + sn.x * 500.0, sp.y + sn.y * 500.0);
            game.pl.vel = v2(0, 0);
            game.floating = true;
            t.aggro = true;
            printf("    setup: turret at (%.1f,%.1f) normal (%.2f,%.2f), player at (%.1f,%.1f), "
                   "dist %.1f, solid at player %d, clear from muzzle %d\n",
                   t.pos.x, t.pos.y, sn.x, sn.y, game.pl.pos.x, game.pl.pos.y,
                   (float)len(game.pl.pos - t.pos), game.world.solidAt(game.pl.pos),
                   (int)game.clearLine(dv2(sp.x + sn.x * 30.0, sp.y + sn.y * 30.0), game.pl.pos));
            const float dmg0 = game.damageTaken;
            for (int i = 0; i < 60 * 8; ++i) {
                game.update(renderer, idle, dt);
                if (i == 1 || i == 30 || i == 90 || i == 180) {
                    const Enemy& e0 = game.enemies[0];
                    const dv2 muz(e0.pos.x + std::cos(e0.aim) * 24.0 + e0.normal.x * 6.0,
                                  e0.pos.y + std::sin(e0.aim) * 24.0 + e0.normal.y * 6.0);
                    printf("    t=%d: dist %.0f, player v=(%.0f,%.0f) grounded=%d, clear=%d, gunCd %.2f, "
                           "aim %.2f, shots %d\n", i, (float)len(game.pl.pos - e0.pos), game.pl.vel.x,
                           game.pl.vel.y, (int)game.pl.grounded, (int)game.clearLine(muz, game.pl.pos),
                           e0.gunCd, e0.aim, game.enemyShots);
                    if (i == 1) {
                        const Body& mb = game.world.bodies[e0.mount.slot];
                        const v2 lp = mb.toLocal(muz);
                        const v2 lps = mb.toLocal(e0.pos);
                        printf("      turret pos (%.1f,%.1f) muzzle (%.1f,%.1f) normal (%.2f,%.2f) | rock pos (%.1f,%.1f) ang %.3f angVel %.3f | "
                               "field at pos %.2f, at muzzle %.2f, 8 units out %.2f\n",
                               e0.pos.x, e0.pos.y, muz.x, muz.y, e0.normal.x, e0.normal.y, mb.pos.x, mb.pos.y,
                               mb.ang, mb.angVel, mb.f.sample(lps), mb.f.sample(lp),
                               mb.f.sample(lps + mb.dirToWorld(v2(0,0)) + rot(e0.normal, mb.cosA, -mb.sinA) * 8.0f));
                        // What exactly is in the way?
                        const v2 d = tov2(game.pl.pos - muz);
                        const float L = len(d);
                        for (float s = 0; s < L; s += 9.0f) {
                            const dv2 p(muz.x + d.x / L * s, muz.y + d.y / L * s);
                            const int h = game.world.solidAt(p);
                            if (h >= 0) {
                                printf("      blocked at s=%.0f by body %d (radius %.0f, is turret's rock %d, alive %d)\n",
                                       s, h, game.world.bodies[h].radius, (int)(game.world.ref(h).slot == e0.mount.slot),
                                       (int)game.world.bodies[h].alive);
                                break;
                            }
                        }
                    }
                }
            }
            printf("  in 8 s: %d bullets, %d missiles fired, %.0f damage dealt, %d missiles alive\n",
                   game.enemyShots, game.missilesLaunched, game.damageTaken - dmg0, (int)game.missiles.size());
            for (const Enemy& e : game.enemies) {
                const dv2 muz(e.pos.x + std::cos(e.aim) * 24.0 + e.normal.x * 6.0,
                              e.pos.y + std::sin(e.aim) * 24.0 + e.normal.y * 6.0);
                printf("  turret: alive=%d aggro=%d dist=%.0f gunCd=%.2f burst=%d hasGun=%d aim=%.2f nAng=%.2f "
                       "clear=%d gunRange=%.0f state=%d\n", (int)e.alive, (int)e.aggro,
                       (float)len(game.pl.pos - e.pos), e.gunCd, e.burst, (int)e.hasGun, e.aim,
                       std::atan2(e.normal.y, e.normal.x), (int)game.clearLine(muz, game.pl.pos),
                       game.level.diff.gunRange, (int)game.state);
            }
            check(game.enemyShots > 5, "the turret fires bursts of bullets");
            check(game.missilesLaunched >= 1, "it lobs homing missiles");
            check(game.damageTaken - dmg0 > 0.0f, "and they actually hit the player");

            // Shoot the ground out from under it: it has nowhere to stand.
            game.enemies.clear();  game.missiles.clear();  game.ebullets.clear();
            game.spawnTurret(s, sp, sn, sb);
            game.enemies.back().aggro = false;
            const int kills0 = game.kills;
            game.world.damage(sb, dv2(sp.x - sn.x * 5.0, sp.y - sn.y * 5.0), 26.0f, 0.1f, 5u);
            for (int i = 0; i < 30; ++i) game.update(renderer, idle, dt);
            int turretsLeft = 0;
            for (const Enemy& e : game.enemies) if (e.alive && e.kind == Enemy::Turret) ++turretsLeft;
            check(turretsLeft == 0 && game.kills > kills0,
                  "a turret whose rock is shot away from under it is destroyed");
        }

        // ---- missiles home in on a moving target
        game.enemies.clear();  game.missiles.clear();  game.ebullets.clear();
        {
            openSpot(dv2(5000.0, 5000.0));
            Missile m;
            m.pos = dv2(5000.0 - 700.0, 5000.0);
            m.vel = v2(0, 200.0f);                       // launched off to the side
            m.life = 8.0f;  m.maxSpeed = game.level.diff.missileSpeed;
            m.turn = game.level.diff.missileTurn;  m.damage = game.level.diff.missileDamage;
            game.missiles.push_back(m);
            const float dmg0 = game.damageTaken;
            for (int i = 0; i < 60 * 6 && !game.missiles.empty(); ++i)
                game.update(renderer, idle, dt);
            check(game.missiles.empty() && game.damageTaken - dmg0 > 10.0f,
                  "a missile launched sideways turns round and hits the player");
        }

        // ---- shooting a missile down
        game.missiles.clear();
        {
            openSpot(dv2(5000.0, 5000.0));
            Missile m;
            m.pos = dv2(5000.0 + 400.0, 5000.0);
            m.vel = v2(-100.0f, 0.0f);  m.life = 8.0f;  m.maxSpeed = 300.0f;
            m.turn = 0.1f;  m.damage = 30.0f;
            game.missiles.push_back(m);
            Input fire;
            fire.mouse[0] = true;
            const int credits0 = game.credits;
            for (int i = 0; i < 90 && !game.missiles.empty(); ++i) {
                aimAt(fire, game.missiles[0].pos);
                game.update(renderer, fire, dt);
            }
            check(game.missiles.empty(),
                  "the player can shoot an incoming missile out of the sky");
            check(game.credits == credits0, "and shooting a missile down pays no credits");
        }

        // ---- shooting a drone dead
        game.enemies.clear();
        {
            openSpot(dv2(5000.0, 5000.0));
            Enemy d = plainDrone(dv2(5000.0 + 350.0, 5000.0));
            d.hp = d.maxHp = 60.0f;
            game.enemies.push_back(d);
            Input fire;
            fire.mouse[0] = true;
            const int kills0 = game.kills;
            int frames = 0;
            for (; frames < 60 * 4 && game.kills == kills0; ++frames) {
                if (!game.enemies.empty()) aimAt(fire, game.enemies[0].pos);
                game.floating = true;
                game.update(renderer, fire, dt);
            }
            printf("  the rifle killed a 60 hp drone in %.2f s\n", frames * dt);
            check(game.kills == kills0 + 1, "rifle fire kills a drone");
        }

        // ---- a drone closes in and fights
        game.enemies.clear();  game.ebullets.clear();
        {
            openSpot(dv2(9000.0, 9000.0));
            game.floating = true;
            Slot s;  s.hasGun = true;  s.hasMissile = false;
            game.spawnDrone(s, dv2(9000.0 + 1200.0, 9000.0));
            const float d0 = (float)len(game.enemies[0].pos - game.pl.pos);
            const int shots0 = game.enemyShots;
            for (int i = 0; i < 60 * 8; ++i) {
                game.update(renderer, idle, dt);
            }
            const float d1 = game.enemies.empty() ? -1.0f : (float)len(game.enemies[0].pos - game.pl.pos);
            printf("  drone range %.0f -> %.0f, %d shots fired\n", d0, d1, game.enemyShots - shots0);
            check(d1 > 150.0f && d1 < 450.0f, "a drone closes to fighting range and holds it");
            check(game.enemyShots - shots0 > 3, "and shoots while doing it");
        }

        printf("enemytest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (walkTest) {
        // Stand on the biggest rock at four points around it, hold D then A, and
        // look at which way the player really moves *on screen*. Screen space is
        // the world rolled by the camera angle, so rotate the ground-relative
        // velocity by cam.angle and read off x: positive means screen-right.
        const float dt = 1.0f / 60.0f;
        int best = -1;
        for (int s : game.world.active)
            if (best < 0 || game.world.bodies[s].radius > game.world.bodies[best].radius) best = s;
        float fastest = 0.0f;
        int failures = 0;
        for (int fixed = 0; fixed < 2; ++fixed) {
            game.povCamera = (fixed == 0);
            printf("walktest: %s camera\n", fixed ? "FIXED" : "POV");
            const char* names[4] = { "top", "right side", "bottom", "left side" };
            for (int k = 0; k < 4; ++k) {
                const Body& rock = game.world.bodies[best];
                const v2 n = fromAngle(PIF * 0.5f - k * PIF * 0.5f);   // top, right, bottom, left
                // Find the surface along that direction by marching inward.
                dv2 p = dv2(rock.pos.x + n.x * (rock.radius + 40.0), rock.pos.y + n.y * (rock.radius + 40.0));
                for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n * -1.0f;
                p += n * 9.0f;
                for (int key = 0; key < 2; ++key) {
                    Input in;
                    in.down[key == 0 ? 'D' : 'A'] = true;
                    game.pl.pos = p; game.pl.vel = rock.vel; game.pl.up = n;
                    game.pl.grounded = false; game.pl.jumpCd = 0;
                    game.cam.angle = game.povCamera ? PIF * 0.5f - std::atan2(n.y, n.x) : 0.0f;
                    game.cam.pos = p;
                    float sx = 0;
                    for (int f = 0; f < 20; ++f) {
                        game.update(renderer, in, dt);
                        game.cam.pos = game.pl.pos;   // no camera lag confounding the reading
                        if (f == 19) {
                            Body* gb = game.world.get(game.pl.ground);
                            const v2 rel = game.pl.vel - (gb ? gb->velAt(tov2(game.pl.pos - gb->pos)) : v2(0, 0));
                            sx = rot(rel, std::cos(game.cam.angle), std::sin(game.cam.angle)).x;
                        }
                    }
                    const bool wantRight = (key == 0);
                    // In the fixed camera the player can be upside-down, where the
                    // local right is screen-left, so only the POV mode must be exact.
                    fastest = std::max(fastest, std::fabs(sx));
                    const bool ok = fixed ? true : (wantRight ? sx > 5.0f : sx < -5.0f);
                    if (!ok) ++failures;
                    printf("  %-10s hold %c: screen-x speed %+7.1f  %s\n", names[k],
                           key == 0 ? 'D' : 'A', sx,
                           fixed ? "" : (ok ? "ok" : "WRONG WAY"));
                }
            }
        }
        if (fastest < 2.0f) {
            // Nothing moved at all: walking is switched off in the tuning, so there is
            // no direction to check. Say so rather than report eight false failures.
            printf("walktest: SKIPPED - A and D do not move the player (tune::WALK_SPEED and AIR_ACCEL are 0)\n");
        } else {
            printf("walktest: %s\n", failures == 0 ? "PASS" : "FAIL");
        }
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (bulletTest) {
        // Fire each bullet type tangentially past the biggest nearby rock, 60
        // units off its surface, and report how far gravity bends the path.
        Input idle;
        const float dt = 1.0f / 60.0f;
        int best = -1;
        for (int s : game.world.active)
            if (best < 0 || game.world.bodies[s].radius > game.world.bodies[best].radius) best = s;
        const Body& rock = game.world.bodies[best];
        const v2 radial(1.0f, 0.0f), tang(0.0f, 1.0f);
        const dv2 start(rock.pos.x + (rock.radius + 60.0f) * radial.x,
                        rock.pos.y + (rock.radius + 60.0f) * radial.y);
        printf("bullettest: rock radius %.0f, bullets fired tangentially 60 units above it\n",
               rock.radius);
        for (int heavy = 0; heavy < 2; ++heavy) {
            game.bullets.clear();
            Bullet b;
            b.pos = start;
            b.vel = tang * (heavy ? 700.0f : 1650.0f);
            b.caliber = 0.5f; b.budget = 1.0f; b.life = 5.0f; b.heavy = heavy != 0;
            b.gravScale = heavy ? tune::HEAVY_GRAV : tune::BULLET_GRAV;
            game.bullets.push_back(b);
            const v2 v0 = b.vel;
            int frames = 0;
            // A short window keeps the shot clear of anything else in the way.
            while (!game.bullets.empty() && frames < 12) { game.update(renderer, idle, dt); ++frames; }
            if (game.bullets.empty()) { printf("  %s: hit rock after %d frames\n", heavy ? "heavy " : "rifle ", frames); continue; }
            const v2 v1 = game.bullets[0].vel;
            printf("  %s: turned %.2f deg in %.2f s (speed %.0f -> %.0f)\n",
                   heavy ? "heavy " : "rifle ",
                   std::fabs(wrapAngle(std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x))) * 180.0f / PIF,
                   frames * dt, len(v0), len(v1));
        }
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (rocketTest) {
        // Hold the rocket down until the tank is empty, then keep holding it. However
        // the refuelling rules are set, thrust can never exceed what the fuel budget
        // pays for: over a long hold the duty cycle is at most regen / (regen + burn).
        Input held;
        held.mouse[1] = true;
        Input idle;
        const float dt = 1.0f / 60.0f;
        int emptyAt = -1, thrustAfterEmpty = 0, frames = 600;
        for (int i = 0; i < frames; ++i) {
            game.update(renderer, held, dt);
            if (emptyAt < 0 && game.pl.fuel <= 0.0f) emptyAt = i;
            if (emptyAt >= 0 && i > emptyAt && game.pl.thrusting) ++thrustAfterEmpty;
        }
        printf("rocket: empty at frame %d, thrusting frames while held empty: %d of %d\n",
               emptyAt, thrustAfterEmpty, frames - emptyAt - 1);
        // Release, let the tank refill a little, and make sure it works again.
        for (int i = 0; i < 180; ++i) game.update(renderer, idle, dt);
        const float refilled = game.pl.fuel;
        int thrustAfterRest = 0;
        for (int i = 0; i < 30; ++i) {
            game.update(renderer, held, dt);
            if (game.pl.thrusting) ++thrustAfterRest;
        }
        printf("rocket: after 3 s rest fuel=%.1f, thrusting frames on re-press: %d of 30\n",
               refilled, thrustAfterRest);
        const int heldFrames = frames - emptyAt - 1;
        const float budget = 0.03f + 22.0f / (22.0f + 25.0f);        // regen / (regen + burn), plus slack
        const float duty = heldFrames > 0 ? (float)thrustAfterEmpty / heldFrames : 0.0f;
        printf("rocket: duty cycle while held empty %.0f%% (the fuel budget allows about %.0f%%)\n",
               duty * 100.0f, budget * 100.0f);
        printf("rocket: %s\n", (duty <= budget && thrustAfterRest > 0) ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (persistTest) {
        // Blast the rocks around the origin, fly far enough away that their
        // chunks unload, come back, and check the damage came back with them.
        Input none;
        auto massNearOrigin = [&]() {
            double m = 0;
            for (int s : game.world.active) {
                const Body& b = game.world.bodies[s];
                if (b.pos.x * b.pos.x + b.pos.y * b.pos.y < 1500.0 * 1500.0) m += b.mass;
            }
            return m;
        };
        auto run = [&](int n) { for (int i = 0; i < n; ++i) game.update(renderer, none, 1.0f / 60.0f); };

        run(30);
        const double intact = massNearOrigin();
        for (int s : game.world.active) {
            Body& b = game.world.bodies[s];
            if (b.pos.x * b.pos.x + b.pos.y * b.pos.y < 900.0 * 900.0)
                game.world.damage(s, b.pos, b.radius * 0.5f, 0.2f, 4242u);
        }
        run(20);
        const double damaged = massNearOrigin();

        game.pl.pos = dv2(80000, 80000); game.pl.vel = v2(0, 0); game.cam.pos = game.pl.pos;
        run(150);
        const double awayCount = (double)game.world.liveCount;

        game.pl.pos = dv2(0, 0); game.pl.vel = v2(0, 0); game.cam.pos = game.pl.pos;
        run(300);
        const double reloaded = massNearOrigin();

        printf("persist: intact=%.0f  damaged=%.0f  (rocks while away=%.0f)  reloaded=%.0f\n",
               intact, damaged, awayCount, reloaded);
        printf("persist: %s\n", reloaded < intact * 0.95 ? "PASS - damage survived the round trip"
                                                         : "FAIL - rocks regenerated intact");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    double prev = nowSeconds();
    long long frameNo = 0;
    double    totalMs = 0;
    double    worstMs = 0;
    int       over16 = 0, over33 = 0;
    float fpsAccum = 0;
    int   fpsFrames = 0;
    bool  vsync = true;

    // Sound, for real play only: not for the benchmark or the screenshot runs.
    if (volumeArg >= 0.0f) audio::setMaster(volumeArg);
    audio::init(!noSound && !selftest && !shotFrame && frameLimit == 0);

    while (gRunning) {
        gInput.newFrame();
        platformPump();
        if (!gRunning) break;
        if (keyLog) {
            // Prints every change of the keys that matter, and a heartbeat, so a real
            // keyboard can be checked against what the game actually receives.
            static bool prevL = false, prevR = false, prevS = false;
            static long long lastBeat = 0;
            const bool l = gInput.down[VK_LMENU], rr = gInput.down[VK_RMENU], sp = gInput.down[VK_SPACE];
            if (l != prevL)  { printf("KEY left-alt  %s (frame %lld)\n", l ? "DOWN" : "UP", frameNo);  prevL = l; }
            if (rr != prevR) { printf("KEY right-alt %s (frame %lld)\n", rr ? "DOWN" : "UP", frameNo); prevR = rr; }
            if (sp != prevS) { printf("KEY space     %s (frame %lld)\n", sp ? "DOWN" : "UP", frameNo);  prevS = sp; }
            if (frameNo - lastBeat >= 60) { printf("beat frame %lld\n", frameNo); lastBeat = frameNo; }
            fflush(stdout);
        }

        if (gInput.pressed[VK_F11]) toggleFullscreenNow();
        if (gInput.pressed['V']) { vsync = !vsync; setVsync(vsync); }
        if (gInput.pressed['B']) renderer.bloomAmount = renderer.bloomAmount > 0.5f ? 0.0f : 1.0f;
        if (gInput.pressed['T']) renderer.lineWeight  = renderer.lineWeight  > 1.3f ? 1.0f : 1.7f;

        if (gResized) { renderer.resize(gWidth, gHeight); gResized = false; }

        const double now = nowSeconds();
        float dt = (float)(now - prev);
        prev = now;
        game.frameMs = dt * 1000.0f;
        dt = clampf(dt, 1.0f / 1000.0f, 1.0f / 30.0f);

        fpsAccum += dt;
        ++fpsFrames;
        if (fpsAccum > 0.33f) {
            game.fps = fpsFrames / fpsAccum;
            fpsAccum = 0;
            fpsFrames = 0;
        }

        if (aimX >= 0) gInput.mousePx = v2(gWidth * aimX, gHeight * aimY);
        if (selftest) {
            // Drive the game so carving, splitting, streaming and the HUD all run.
            const float t = (float)frameNo / 60.0f;
            gInput.mousePx = v2(gWidth  * (0.5f + 0.34f * std::cos(t * 0.9f)),
                                gHeight * (0.5f + 0.34f * std::sin(t * 0.9f)));
            gInput.mouse[0]  = true;
            gInput.mouse[1]  = (frameNo % 300) > 250;
            gInput.down[0x44] = ((frameNo / 70) & 1) != 0;   // D
            gInput.down[0x41] = !gInput.down[0x44];           // A
            if (frameNo %  90 ==  30) gInput.pressed[VK_SPACE] = true;
            if (frameNo % 240 == 120) gInput.pressed[0x46]     = true;   // F
            if (frameNo % 600 == 400) gInput.pressed[0x47]     = true;   // G
        }

        game.update(renderer, gInput, dt);
        game.render(renderer);
        if (gInput.pressed[VK_F12] || (shotFrame && frameNo + 1 == shotFrame))
            renderer.screenshot(shotPath);
        swapBuffersNow();
        ++frameNo;
        totalMs += game.frameMs;
        if (frameNo > 8 && game.frameMs > worstMs) worstMs = game.frameMs;
        if (frameNo > 8) { if (game.frameMs > 16.0) ++over16; if (game.frameMs > 33.0) ++over33; }
        if (frameLimit && frameNo >= frameLimit) gRunning = false;
    }

    printf("frames=%lld  avg=%.2f ms  worst=%.2f ms  rocks=%d  split=%d  "
           "shots=%d  parts=%d  fieldmem=%d KB  arena=%d KB\n",
           frameNo, frameNo ? totalMs / frameNo : 0.0, worstMs,
           game.world.liveCount, game.rocksSplit, game.shotsFired,
           (int)game.parts.size(), (int)(game.world.fieldBytes() / 1024),
           (int)(renderer.arenaBytes() / 1024));
    printf("frames over 16ms: %d   over 33ms: %d\n", over16, over33);
    fflush(stdout);
    audio::shutdown();
    renderer.shutdown();
    platformShutdown();
    return 0;
}
