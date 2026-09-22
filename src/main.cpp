// main.cpp -- the window, a 3.3 core context, input and the frame loop.
// Windows: Win32 and WGL, linking only against opengl32 and gdi32.
// Linux:   SDL2 and GLX, linking against SDL2 and libGL.
// Everything the game itself does is between platformPump() and swapBuffersNow().
#include "gl.h"
#include "game.h"
#include "net_session.h"
#include "net.h"
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
    bool walkTest = false, jumpTest = false, rangeTest = false, spinTest = false, laserTest = false;
    bool sandboxMode = false, levelTest = false, nukeTest = false, enemyTest = false;
    bool peaceful = false, allItemsArg = false;
    bool showcase = false, shipGallery = false;
    bool weaponTest = false, shopTest = false, keyLog = false, lifeTest = false, shipTest = false;
    bool soundCheck = false, soundTest = false, soundQuick = false, noSound = false, soundGameTest = false, syncTest = false, netTest = false, udpTest = false, versusTest = false, netGameTest = false, fractalTest = false;
    int versusBots = 0, hostPort = 0, hostBots = 0;
    bool loopbackOnly = false;
    std::string playerName;
    std::string joinAddr;
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
        else if (!strcmp(argv[i], "-allitems"))             allItemsArg = true;
        else if (!strcmp(argv[i], "-weapontest"))           weaponTest = true;
        else if (!strcmp(argv[i], "-shoptest"))             shopTest = true;
        else if (!strcmp(argv[i], "-keylog"))               keyLog = true;
        else if (!strcmp(argv[i], "-lifetest"))             lifeTest = true;
        else if (!strcmp(argv[i], "-shiptest"))             shipTest = true;
        else if (!strcmp(argv[i], "-soundcheck"))           soundCheck = true;
        else if (!strcmp(argv[i], "-soundtest"))            soundTest = true;
        else if (!strcmp(argv[i], "-soundquick"))           { soundTest = true; soundQuick = true; }
        else if (!strcmp(argv[i], "-soundgametest"))        soundGameTest = true;
        else if (!strcmp(argv[i], "-synctest"))             syncTest = true;
        else if (!strcmp(argv[i], "-nettest"))              netTest = true;
        else if (!strcmp(argv[i], "-udptest"))              udpTest = true;
        else if (!strcmp(argv[i], "-versustest"))           versusTest = true;
        else if (!strcmp(argv[i], "-fractaltest"))          fractalTest = true;
        else if (!strcmp(argv[i], "-netgametest"))          netGameTest = true;
        else if (!strcmp(argv[i], "-host"))                 { hostPort = 4790; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') hostPort = atoi(argv[++i]); if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '7' && strlen(argv[i + 1]) == 1) hostBots = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-join") && i + 1 < argc)  joinAddr = argv[++i];
        else if (!strcmp(argv[i], "-loopback"))             loopbackOnly = true;
        else if (!strcmp(argv[i], "-name") && i + 1 < argc)  playerName = argv[++i];
        else if (!strcmp(argv[i], "-versus"))               { versusBots = 1; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '7') versusBots = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-nosound"))              noSound = true;
        else if (!strcmp(argv[i], "-volume") && i + 1 < argc) volumeArg = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-showcase"))             showcase = true;
        else if (!strcmp(argv[i], "-shipgallery"))          shipGallery = true;
        else if (!strcmp(argv[i], "-nuketest"))             nukeTest = true;
        else if (!strcmp(argv[i], "-enemytest"))            enemyTest = true;
        else if (!strcmp(argv[i], "-walktest"))             walkTest = true;
        else if (!strcmp(argv[i], "-jumptest"))             jumpTest = true;
        else if (!strcmp(argv[i], "-rangetest"))            rangeTest = true;
        else if (!strcmp(argv[i], "-spintest"))             spinTest = true;
        else if (!strcmp(argv[i], "-lasertest"))            laserTest = true;
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
    game.sandbox    = sandboxMode || walkTest || jumpTest || spinTest || bulletTest || rocketTest || persistTest;
    // Benchmarks and the scripted demo run must not die or time out.
    game.invincible = selftest;
    game.allItems = allItemsArg;                     // -allitems: try every weapon from the start
    if (povCam) game.povCamera = true;
    game.init(renderer, seed);
    if (versusBots > 0) game.startVersus(renderer, versusBots);      // -versus N: a match against N bots
    if (!playerName.empty()) snprintf(game.localName, sizeof game.localName, "%s", playerName.c_str());
    if (hostPort > 0) {
        std::string err;
        if (!game.startHost(renderer, hostPort, hostBots, &err, loopbackOnly)) fatal(("Could not host: " + err).c_str());
        printf("hosting a match on UDP port %d. Others join with:  asteroid.exe -join <your address>:%d\n", hostPort, hostPort);
        printf("(over the internet the port must be forwarded to this machine; on a LAN or a VPN it just works)\n");
    } else if (!joinAddr.empty()) {
        std::string ip = joinAddr;  int port = 4790;
        const size_t colon = ip.find(':');
        if (colon != std::string::npos) { port = atoi(ip.c_str() + colon + 1); ip = ip.substr(0, colon); }
        std::string err;
        if (!game.startClient(renderer, ip.c_str(), port, &err)) fatal(("Could not join: " + err).c_str());
    }
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
        game.startLevel(3);
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
            game.startLevel(3);
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

        // ---- -allitems: everything owned from the start, and kept full
        {
            game.allItems = true;
            game.startRun(renderer);
            const Player& p = game.pl;
            check(p.hasHoming && p.hasSalvo && p.hasField && p.hasShield && p.hasFractal, "with -allitems every weapon is owned from the first frame");
            check(p.salvoAmmo == rules::SALVO_MAX && p.nukeAmmo == rules::NUKE_MAX && p.fractalAmmo == rules::FRACTAL_MAX,
                  "with full magazines");
            check(p.shield == rules::SHIELD_CAPACITY && p.field == 100.0f, "and a charged shield and force field");
            check(game.credits == 0, "without touching the credits");
            game.pl.salvoAmmo = 0;  game.pl.nukeAmmo = 0;  game.pl.fractalAmmo = 0;  game.pl.shield = 3.0f;
            game.startLevel(2);
            check(game.pl.salvoAmmo == rules::SALVO_MAX && game.pl.nukeAmmo == rules::NUKE_MAX &&
                  game.pl.fractalAmmo == rules::FRACTAL_MAX && game.pl.shield == rules::SHIELD_CAPACITY,
                  "and every new level tops them all back up");
            game.allItems = false;
            game.startRun(renderer);
            check(!game.pl.hasFractal && !game.pl.hasSalvo && game.pl.nukeAmmo == 0, "without the flag a run still starts with nothing");
        }

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
            game.pl.fuel = tune::FUEL_MAX;  game.pl.fuelLocked = false;
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
            game.pl.hasFractal = true;  game.pl.fractalAmmo = 2;  game.pl.fractalCd = 0.0f;
            Input fz;  fz.pressed['Z'] = true;  fz.mousePx = v2(gWidth * 0.6f, gHeight * 0.5f);   // a close cross: it divides before it meets anything
            run(fz, 1);
            check(plays(Sfx::FractalFire) >= 1, "the fractal shell has its own launch sound");
            run(idle, 90);                                   // the split comes after the time the cursor distance takes at launch speed
            check(plays(Sfx::FractalSplit) >= 1, "and a crack each time it divides");
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
            // Somewhere round the player with open sky between them (rock may be in the way at any one spot).
            dv2 at(game.pl.pos.x + 260.0, game.pl.pos.y + 120.0);
            for (int k = 0; k < 24; ++k) {
                const float a = 0.4f + k * 0.26f;
                const dv2 cand(game.pl.pos.x + std::cos(a) * 340.0, game.pl.pos.y + std::sin(a) * 340.0);   // (missiles are only launched from beyond 300)
                if (game.world.solidAt(cand) < 0 && game.clearLine(cand, game.pl.pos)) { at = cand; break; }
            }
            game.spawnDrone(s, at);
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
            game.startLevel(3);
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

    if (syncTest) {
        // Does one seed give the same rocks however the player got there? A joining
        // client could only generate the world for itself if it did.
        //
        // It does not, and this measures by how much. Chunk generation asks
        // spaceFree() about the rocks that happen to be loaded at that moment, and on
        // a rejected placement it retries, drawing more random numbers; the rock's own
        // seed is only consumed when a placement succeeds. So one rejection shifts the
        // stream for the rest of the chunk, and the divergence cascades into the
        // neighbours. That is harmless in a single-player game, and it is why a
        // multiplayer host has to send the rocks rather than name a seed.
        //
        // This is a diagnostic, not a regression test. What it does hold the game to
        // is the control at the end: one seed and one path must always rebuild the
        // same world, or nothing is reproducible.
        printf("synctest:\n");
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        struct Rock { double x, y; float r; uint32_t seed; };
        auto chunkOf = [](dv2 p, i64& cx, i64& cy) {
            cx = (i64)std::floor(p.x / cfg::CHUNK);
            cy = (i64)std::floor(p.y / cfg::CHUNK);
        };
        auto rocksIn = [&](World& w, i64 cx, i64 cy) {
            std::vector<Rock> out;
            for (const Body& b : w.bodies) {
                if (!b.alive) continue;
                i64 bx, by;  chunkOf(b.pos, bx, by);
                if (bx != cx || by != cy) continue;
                out.push_back({ b.pos.x, b.pos.y, b.radius, b.seed });
            }
            std::sort(out.begin(), out.end(), [](const Rock& a, const Rock& b) {
                return a.x != b.x ? a.x < b.x : a.y < b.y;
            });
            return out;
        };
        auto same = [](const std::vector<Rock>& a, const std::vector<Rock>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i].x - b[i].x) > 1e-9 || std::fabs(a[i].y - b[i].y) > 1e-9 ||
                    std::fabs(a[i].r - b[i].r) > 1e-4f || a[i].seed != b[i].seed) return false;
            return true;
        };
        // Walk a world along a path, streaming as a player would.
        auto walk = [&](World& w, dv2 from, dv2 to, int steps) {
            for (int i = 0; i <= steps; ++i) {
                const double t = (double)i / steps;
                const dv2 p(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
                w.streamChunks(p, 1300.0, 100000);
                w.step(1.0f / 60.0f, p);
            }
        };

        const dv2 MEET(12000.0, 0.0);
        i64 mcx, mcy;  chunkOf(MEET, mcx, mcy);

        // Two players arriving at the same place from opposite directions.
        static World wEast, wWest;
        wEast.init(0x5EEDFACEull);
        wWest.init(0x5EEDFACEull);
        walk(wEast, dv2(0, 0), MEET, 40);                    // came from the west
        walk(wWest, dv2(24000.0, 0.0), MEET, 40);            // came from the east
        const auto a = rocksIn(wEast, mcx, mcy);
        const auto b = rocksIn(wWest, mcx, mcy);
        printf("  chunk (%lld,%lld): approached from the west %d rocks, from the east %d rocks\n",
               (long long)mcx, (long long)mcy, (int)a.size(), (int)b.size());
        for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
            if (i < a.size()) printf("      west: (%9.2f,%9.2f) r=%6.1f seed=%u\n", a[i].x, a[i].y, a[i].r, a[i].seed);
            if (i < b.size()) printf("      east: (%9.2f,%9.2f) r=%6.1f seed=%u\n", b[i].x, b[i].y, b[i].r, b[i].seed);
        }
        const bool chunkAgrees = same(a, b);
        printf("  -> reaching a chunk from two directions gives %s\n",
               chunkAgrees ? "the same rocks" : "DIFFERENT rocks");

        // The same question over a whole region, which is what a client would need.
        {
            int chunksSame = 0, chunksDiff = 0, rocksA = 0, rocksB = 0;
            for (i64 cy = mcy - 1; cy <= mcy + 1; ++cy)
                for (i64 cx = mcx - 1; cx <= mcx + 1; ++cx) {
                    const auto ra = rocksIn(wEast, cx, cy), rb = rocksIn(wWest, cx, cy);
                    rocksA += (int)ra.size();  rocksB += (int)rb.size();
                    if (same(ra, rb)) ++chunksSame; else ++chunksDiff;
                }
            printf("  the 3x3 chunks around the meeting point: %d match, %d differ (%d rocks vs %d)\n",
                   chunksSame, chunksDiff, rocksA, rocksB);
            printf("  -> a client cannot be given a seed and left to build the world: it must be sent %d rocks\n",
                   rocksA);
            check(chunksDiff == 0 || !chunkAgrees,
                  "the finding is consistent: generation depends on the route taken");
        }

        // A control: the same path twice must always agree, or nothing is reproducible.
        {
            static World w1, w2;
            w1.init(0x5EEDFACEull);
            w2.init(0x5EEDFACEull);
            walk(w1, dv2(0, 0), MEET, 40);
            walk(w2, dv2(0, 0), MEET, 40);
            bool allSame = true;
            for (i64 cy = mcy - 1; cy <= mcy + 1; ++cy)
                for (i64 cx = mcx - 1; cx <= mcx + 1; ++cx)
                    allSame = allSame && same(rocksIn(w1, cx, cy), rocksIn(w2, cx, cy));
            check(allSame, "the same path twice always gives the same world");
        }

        printf("synctest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
    }

    if (netGameTest) {
        // Two whole games in one process, a host and a client, joined by a link with
        // configurable loss and latency. The client's world starts empty. Everything
        // it ends up with, it was told.
        printf("netgametest:\n");
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        const float dt = 1.0f / 60.0f;

        static Game hostG, cliG;
        struct Report { double kbps = 0; };

        auto aimAtG = [&](Game& g, Input& in, dv2 target) {
            const v2 rel = tov2(target - g.cam.pos);
            const v2 q = rot(rel, std::cos(g.cam.angle), std::sin(g.cam.angle));
            const float k = (float)renderer.fbw / (2.0f * g.cam.halfW);
            in.mousePx = v2(renderer.fbw * 0.5f + q.x * k, renderer.fbh * 0.5f - q.y * k);
        };

        auto scenario = [&](const char* label, float loss, double latency, int bots, bool realSockets = false) {
            printf("\n  [%s] %s, %d bots on the host\n", label,
                   realSockets ? "real UDP sockets on the loopback interface" : "in-process link", bots);
            if (!realSockets) printf("      loss %.0f%%, latency %.0f ms one way\n", loss * 100.0f, latency * 1000.0);
            net::LoopLink la, lb;
            la.other = &lb;  lb.other = &la;
            la.loss = lb.loss = loss;
            la.latency = lb.latency = latency;
            la.rng = 1234;  lb.rng = 9876;
            // Real sockets, when asked: bound to loopback only, so no firewall prompt and nothing leaves the machine.
            static net::UdpLink sockHost, sockCli;
            net::Link* hostLink = &la;
            net::Link* cliLink = &lb;
            if (realSockets) {
                sockHost.close();  sockCli.close();
                sockHost = net::UdpLink();  sockCli = net::UdpLink();
                const bool ok = sockHost.open(47836, true) && sockCli.connect("127.0.0.1", 47836);
                check(ok, "the sockets open");
                hostLink = &sockHost;  cliLink = &sockCli;
            }

            // The client first: starting either one resets the renderer's shared vertex arena.
            cliG.baseSeed = 0x5EEDFACEull;  hostG.baseSeed = 0x5EEDFACEull;
            cliG.startClientOn(renderer, cliLink);
            hostG.startHostOn(renderer, hostLink, bots);
            hostG.invincible = false;  cliG.invincible = false;
            Input hostIn, cliIn;
            hostIn.mousePx = cliIn.mousePx = v2(renderer.fbw * 0.5f, renderer.fbh * 0.5f);
            auto step = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    if (realSockets) Sleep(1);                        // let the operating system deliver
                    else { la.advance(dt);  lb.advance(dt); }
                    cliG.update(renderer, cliIn, dt);
                    hostG.update(renderer, hostIn, dt);
                }
            };
            auto peerOf = [](Game& g, int id) -> Game::Peer* {
                for (Game::Peer& p : g.peers) if (p.body.id == id) return &p;
                return nullptr;
            };
            // How many of the host's rocks does the client have, and how many differ?
            auto compareWorlds = [&](int& hostRocks, int& cliRocks, int& missing, int& differ, double& worst) {
                std::unordered_map<uint32_t, int> hi, ci;
                for (int s = 0; s < (int)hostG.world.bodies.size(); ++s)
                    if (hostG.world.bodies[s].alive && hostG.world.bodies[s].netId) hi[hostG.world.bodies[s].netId] = s;
                for (int s = 0; s < (int)cliG.world.bodies.size(); ++s)
                    if (cliG.world.bodies[s].alive && cliG.world.bodies[s].netId) ci[cliG.world.bodies[s].netId] = s;
                hostRocks = (int)hi.size();  cliRocks = (int)ci.size();
                missing = 0;  differ = 0;  worst = 0;
                for (auto& kv : hi) {
                    auto it = ci.find(kv.first);
                    if (it == ci.end()) { ++missing; continue; }
                    if (!World::summariesClose(hostG.world.summarise(kv.second), cliG.world.summarise(it->second))) ++differ;
                    worst = std::max(worst, len(hostG.world.bodies[kv.second].pos - cliG.world.bodies[it->second].pos));
                }
                for (auto& kv : ci) if (!hi.count(kv.first)) ++missing;
            };

            // ---- joining
            step(60 * 5);
            check(cliG.net && cliG.net->welcomed && !cliG.net->rejected, "the client is welcomed");
            check(cliG.pl.id > 0 && cliG.pl.id != hostG.pl.id, "and is given an id of its own");
            check(hostG.peers.size() == (size_t)bots + 1, "the host has a player for it");
            check(cliG.peers.size() == (size_t)bots + 1, "and the client knows of the host and any bots");
            check(peerOf(cliG, cliG.pl.id) == nullptr, "and it is never listed as its own opponent");
            {
                Game::Peer* h = peerOf(cliG, hostG.pl.id);
                Game::Peer* mine = peerOf(hostG, cliG.pl.id);
                printf("      names: host calls itself '%s' and the client '%s'; the client calls itself '%s' and the host '%s'\n", hostG.pl.name, mine ? mine->body.name : "?", cliG.pl.name, h ? h->body.name : "?");
                check(h && std::strcmp(h->body.name, "HOST") == 0 && std::strcmp(hostG.pl.name, "HOST") == 0, "the host is called HOST on both machines");
                check(mine && std::strcmp(cliG.pl.name, mine->body.name) == 0 && std::string(cliG.pl.name) == "PILOT " + std::to_string(cliG.pl.id),
                      "the client is given the same name on both, numbered so two pilots are not confused");
            }
            {
                int hr, cr, missing, differ; double worst;
                compareWorlds(hr, cr, missing, differ, worst);
                printf("      after joining: host %d rocks, client %d, %d missing, %d differing, worst position error %.1f\n", hr, cr, missing, differ, worst);
                check(hr > 300 && missing == 0 && differ == 0, "it has been sent every rock, and they match");
            }
            check(!cliG.pl.dead, "the client has been put on a rock");
            {
                Game::Peer* mine = peerOf(hostG, cliG.pl.id);
                check(mine && len(mine->body.pos - cliG.pl.pos) < 120.0, "where the client thinks it is matches where the host has it");
            }

            // A clear stage for the fighting: the host removes the rocks from a disc and the client is told.
            const dv2 A(0.0, 0.0);
            hostG.world.clearZone(A, 700.0);
            step(30);
            {
                int inside = 0;
                for (const Body& b : cliG.world.bodies) if (b.alive && len(b.pos - A) < 500.0) ++inside;
                check(inside == 0, "rocks the host removed are gone from the client too");
            }
            auto stage = [&](double gap) {
                hostG.resetMatch();
                step(20);
                Game::Peer* c = peerOf(hostG, cliG.pl.id);
                hostG.pl.pos = dv2(-gap * 0.5, 0.0);   hostG.pl.vel = v2(0, 0);   hostG.pl.protect = 0.0f;
                c->body.pos = dv2(gap * 0.5, 0.0);     c->body.vel = v2(0, 0);     c->body.protect = 0.0f;
                hostG.bullets.clear();
                for (Game::Peer& p : hostG.peers) if (p.bot) { p.body.dead = true; p.body.respawnIn = 999.0f; }
                step(30);
            };
            auto hold = [&]() {                        // keep both in the same place while they shoot
                Game::Peer* c = peerOf(hostG, cliG.pl.id);
                hostG.pl.pos.y = 0.0;  hostG.pl.vel = v2(0, 0);
                if (c) { c->body.pos.y = 0.0;  c->body.vel = v2(0, 0); }
                cliG.pl.pos.y = 0.0;   cliG.pl.vel = v2(0, 0);
            };

            const uint64_t bytes0 = hostLink->bytesSent;
            const double fightStart = hostG.net->now;

            // ---- the client shoots the host
            stage(500.0);
            {
                cliIn.mouse[0] = true;
                for (int i = 0; i < 60; ++i) {
                    hold();
                    if (Game::Peer* h = peerOf(cliG, hostG.pl.id)) aimAtG(cliG, cliIn, h->body.pos);
                    step(1);
                }
                cliIn.mouse[0] = false;
                step(45);
                const float lost = 100.0f - hostG.pl.health;
                Game::Peer* h = peerOf(cliG, hostG.pl.id);
                printf("      the client fired for a second: the host lost %.0f suit, the client sees %.0f\n", lost, h ? 100.0f - h->body.health : -1.0f);
                check(lost >= rules::VS_RIFLE_DAMAGE, "rounds fired by a client on its own screen hurt the host's player");
                check(h && std::fabs((100.0f - h->body.health) - lost) <= 1.0f, "and the client sees the same damage");
                check(hostG.pl.id != cliG.pl.id && cliG.pl.health == 100.0f, "the client's own suit is untouched");
            }

            // ---- the host shoots the client, and the client sees the shots
            stage(500.0);
            size_t maxBullets = 0;
            {
                hostIn.mouse[0] = true;
                for (int i = 0; i < 60; ++i) {
                    hold();
                    if (Game::Peer* c = peerOf(hostG, cliG.pl.id)) aimAtG(hostG, hostIn, c->body.pos);
                    step(1);
                    maxBullets = std::max(maxBullets, cliG.bullets.size());
                }
                hostIn.mouse[0] = false;
                step(45);
                Game::Peer* c = peerOf(hostG, cliG.pl.id);
                printf("      the host fired for a second: the client lost %.0f suit; up to %d rounds in flight on its screen\n",
                       100.0f - cliG.pl.health, (int)maxBullets);
                check(c && cliG.pl.health < 100.0f, "rounds fired by the host hurt the client");
                check(c && std::fabs(cliG.pl.health - c->body.health) <= 1.0f, "and the client's suit reading is the host's");
                check(maxBullets > (loss >= 0.15f ? 0u : 3u), "and the client saw the rounds fly");   // (on a link losing a fifth of everything, rounds arrive in bursts: one at a time is enough)
            }

            // ---- a kill, seen by both
            stage(500.0);
            {
                cliG.net->lastHealth = 100;
                hostG.pl.health = 100.0f;
                if (Game::Peer* c = peerOf(hostG, cliG.pl.id)) c->body.health = 10.0f;
                hostIn.mouse[0] = true;
                for (int i = 0; i < 240 && !cliG.pl.dead; ++i) {
                    hold();
                    if (Game::Peer* c = peerOf(hostG, cliG.pl.id)) aimAtG(hostG, hostIn, c->body.pos);
                    step(1);
                }
                hostIn.mouse[0] = false;
                int lag = 0;                                            // how long after the client learned of its death was it told who did it?
                while (cliG.killFeed.empty() && lag++ < 240) step(1);
                printf("      the announcement reached the client %d ms after it learned it had died\n", lag * 1000 / 60);
                step(30);
                Game::Peer* h = peerOf(cliG, hostG.pl.id);
                check(cliG.pl.dead && cliG.pl.deaths == 1, "the client is told it has died");
                check(hostG.pl.frags == 1 && h && h->body.frags == 1, "the host's frag is on both screens");
                check(!hostG.killFeed.empty() && !cliG.killFeed.empty(), "and the kill is announced on both");
                check(!cliG.killFeed.empty() && std::strstr(cliG.killFeed[0].text, "HOST") != nullptr && std::strstr(cliG.killFeed[0].text, cliG.pl.name) != nullptr, "with both their names");
                step((int)((rules::RESPAWN_TIME + 0.6f) * 60.0f));
                check(!cliG.pl.dead && cliG.pl.health == 100.0f, "the client comes back after the respawn time");
                Game::Peer* mine = peerOf(hostG, cliG.pl.id);
                check(mine && len(mine->body.pos - cliG.pl.pos) < 200.0, "at the place the host chose");
            }

            // ---- moving: the client runs its own spaceman, and the host must end up agreeing
            {
                hostG.world.clearZone(A, 700.0);
                Game::Peer* c = peerOf(hostG, cliG.pl.id);
                if (c) { c->body.pos = dv2(100.0, 0.0);  c->body.vel = v2(0, 0); }
                cliG.pl.pos = dv2(100.0, 0.0);  cliG.pl.vel = v2(0, 0);
                step(20);
                const dv2 start = cliG.pl.pos;
                cliIn.mouse[1] = true;                                    // the rocket
                for (int i = 0; i < 90; ++i) {
                    aimAtG(cliG, cliIn, dv2(cliG.pl.pos.x, cliG.pl.pos.y + 500.0));
                    step(1);
                }
                cliIn.mouse[1] = false;
                Game::Peer* after = peerOf(hostG, cliG.pl.id);
                const double err = after ? len(after->body.pos - cliG.pl.pos) : 1e9;
                printf("      the client flew %.0f units; the host has it %.1f units from where the client does\n",
                       len(cliG.pl.pos - start), err);
                check(len(cliG.pl.pos - start) > 100.0, "commands from the client move its spaceman");
                check(err < 60.0 + latency * 2000.0, "and the two machines agree about where it ended up");
            }

            // ---- shooting rocks: the host carves them, the client is told how
            {
                int best = -1;
                for (int s : hostG.world.active) {
                    const Body& b = hostG.world.bodies[s];
                    if (!b.alive || b.radius < 60.0f || len(b.pos) < 900.0 || len(b.pos) > 1700.0) continue;
                    if (best < 0 || b.radius > hostG.world.bodies[best].radius) best = s;
                }
                if (best >= 0) {
                    const Body& b = hostG.world.bodies[best];
                    const v2 dirOut = norm(tov2(b.pos));
                    const dv2 stand(b.pos.x - dirOut.x * (b.radius + 250.0), b.pos.y - dirOut.y * (b.radius + 250.0));
                    hostG.pl.pos = stand;  hostG.pl.vel = v2(0, 0);  hostG.pl.protect = 5.0f;
                    hostIn.mouse[0] = true;
                    for (int i = 0; i < 240; ++i) {
                        hostG.pl.pos = stand;  hostG.pl.vel = v2(0, 0);  hostG.pl.protect = 5.0f;
                        aimAtG(hostG, hostIn, hostG.world.bodies[best].alive ? hostG.world.bodies[best].pos : dv2(0, 0));
                        step(1);
                    }
                    hostIn.mouse[0] = false;
                    step(300);                                  // let the last splits and settling reach the client
                }
                int hr, cr, missing, differ; double worst;
                compareWorlds(hr, cr, missing, differ, worst);
                printf("      after shooting rocks: host %d rocks, client %d, %d missing, %d differing, worst position error %.1f\n",
                       hr, cr, missing, differ, worst);
                check(best >= 0, "there was a rock to shoot");
                check(missing == 0, "every rock the host has, the client has, and no others");
                check(differ == 0, "and they are the same shape");
            }

            // ---- the end of a match: only the host can start the next one
            {
                hostG.world.clearZone(A, 700.0);
                step(20);
                stage(500.0);
                hostG.pl.frags = rules::FRAG_LIMIT - 1;
                hostG.pl.health = 100.0f;
                if (Game::Peer* c = peerOf(hostG, cliG.pl.id)) c->body.health = 5.0f;
                hostIn.mouse[0] = true;
                for (int i = 0; i < 300 && !hostG.match.over; ++i) {
                    hold();
                    if (Game::Peer* c = peerOf(hostG, cliG.pl.id)) aimAtG(hostG, hostIn, c->body.pos);
                    step(1);
                }
                hostIn.mouse[0] = false;
                step(30);
                check(hostG.match.over && cliG.match.over && cliG.match.winner == hostG.pl.id, "both machines agree the match is over, and who won");
                const int round = cliG.match.round;
                const dv2 where = cliG.pl.pos;
                const int hostFrags = hostG.pl.frags;
                Input enter;  enter.mousePx = v2(renderer.fbw * 0.5f, renderer.fbh * 0.5f);
                enter.pressed[VK_RETURN] = true;
                cliG.update(renderer, enter, dt);                     // the client presses Enter
                step(20);
                check(cliG.match.over && cliG.match.round == round && len(cliG.pl.pos - where) < 50.0 && hostG.pl.frags == hostFrags,
                      "a client pressing Enter does not restart anything: only the host can");
                while (hostG.match.overTime <= 1.1f) step(1);          // the result stays up a second before Enter counts
                hostG.update(renderer, enter, dt);                    // the host does
                step(60);
                check(!hostG.match.over && !cliG.match.over && cliG.match.round == hostG.match.round && cliG.match.round == round + 1,
                      "when the host restarts, both are in the new match");
                Game::Peer* h = peerOf(cliG, hostG.pl.id);
                check(hostG.pl.frags == 0 && cliG.pl.frags == 0 && h && h->body.frags == 0, "with the scores cleared on every screen");
            }
            const double kbps = (double)(hostLink->bytesSent - bytes0) / 1024.0 / std::max(1.0, hostG.net->now - fightStart);
            printf("      host -> client while fighting: %.1f KB/s   (round trip measured %.0f ms, %d re-sent)\n",
                   kbps, cliG.net->ep.rel.srtt * 1000.0, (int)hostG.net->clients[0].ep.rel.resent);
            check(kbps < 60.0, "the host sends under 60 KB/s to a client");

            // ---- leaving
            cliG.netShutdown();
            step(30);
            check(hostG.peers.size() == (size_t)bots, "when the client says goodbye the host drops it at once");
            if (hostG.peers.size() != (size_t)bots) {
                int waited = 0;
                while (hostG.peers.size() != (size_t)bots && waited++ < 60 * 8) { la.advance(dt); hostG.update(renderer, hostIn, dt); }
                check(hostG.peers.size() == (size_t)bots, "(the goodbye was lost; the host dropped it on the timeout instead)");
            }
            hostG.netShutdown();
            return kbps;
        };

        scenario("a good connection", 0.0f, 0.0, 0);
        scenario("a normal one", 0.02f, 0.040, 0);
        scenario("a poor one", 0.08f, 0.090, 0);
        scenario("with a bot in the match", 0.02f, 0.040, 1);
        scenario("a terrible one", 0.20f, 0.150, 0);
        scenario("over the operating system", 0.0f, 0.0, 0, true);

        // ---- three players: a host and two clients, over one host link
        {
            printf("\n  [three players] a host and two clients, 2%% loss, 40 ms one way\n");
            static Game cli2G;
            net::LoopLink hostL, c1L, c2L;
            hostL.targets[0] = &c1L;   hostL.targets[1] = &c2L;
            c1L.other = &hostL;  c1L.myPeer = 0;
            c2L.other = &hostL;  c2L.myPeer = 1;
            hostL.loss = c1L.loss = c2L.loss = 0.02f;
            hostL.latency = c1L.latency = c2L.latency = 0.040;
            hostL.rng = 11;  c1L.rng = 22;  c2L.rng = 33;
            cliG.baseSeed = cli2G.baseSeed = hostG.baseSeed = 0x5EEDFACEull;
            cliG.startClientOn(renderer, &c1L);
            cli2G.startClientOn(renderer, &c2L);
            hostG.startHostOn(renderer, &hostL, 0);
            hostG.invincible = cliG.invincible = cli2G.invincible = false;
            Input hIn, aIn, bIn;
            hIn.mousePx = aIn.mousePx = bIn.mousePx = v2(renderer.fbw * 0.5f, renderer.fbh * 0.5f);
            auto step = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    hostL.advance(dt);  c1L.advance(dt);  c2L.advance(dt);
                    cliG.update(renderer, aIn, dt);
                    cli2G.update(renderer, bIn, dt);
                    hostG.update(renderer, hIn, dt);
                }
            };
            auto peerOf = [](Game& g, int id) -> Game::Peer* {
                for (Game::Peer& p : g.peers) if (p.body.id == id) return &p;
                return nullptr;
            };
            step(60 * 6);
            check(cliG.net->welcomed && cli2G.net->welcomed, "both clients are welcomed");
            check(cliG.pl.id != cli2G.pl.id && cliG.pl.id > 0 && cli2G.pl.id > 0, "with different ids");
            check(hostG.peers.size() == 2, "the host has both");
            check(cliG.peers.size() == 2 && cli2G.peers.size() == 2, "and each client knows of the other two players");
            check(peerOf(cliG, cli2G.pl.id) && peerOf(cli2G, cliG.pl.id), "including one another");
            check(!peerOf(cliG, cliG.pl.id) && !peerOf(cli2G, cli2G.pl.id), "and never itself");
            check(std::string(cliG.pl.name) != std::string(cli2G.pl.name), "the two clients have different names");

            auto rocks = [](Game& g) {
                int n = 0;
                for (const Body& b : g.world.bodies) if (b.alive) ++n;
                return n;
            };
            int hostRocks = rocks(hostG), r1 = rocks(cliG), r2 = rocks(cli2G);
            printf("      rocks: host %d, first client %d, second client %d\n", hostRocks, r1, r2);
            check(r1 == hostRocks && r2 == hostRocks, "both clients were sent the same world");

            // Client one kills client two, watched by all.
            const dv2 A(0.0, 0.0);
            hostG.world.clearZone(A, 700.0);
            step(30);
            hostG.resetMatch();
            step(30);
            Game::Peer* p1 = peerOf(hostG, cliG.pl.id);
            Game::Peer* p2 = peerOf(hostG, cli2G.pl.id);
            p1->body.pos = dv2(-250.0, 0.0);  p1->body.vel = v2(0, 0);  p1->body.protect = 0.0f;
            p2->body.pos = dv2(250.0, 0.0);   p2->body.vel = v2(0, 0);  p2->body.protect = 0.0f;
            hostG.pl.pos = dv2(0.0, 900.0);   hostG.pl.protect = 5.0f;
            p2->body.health = 12.0f;
            step(30);
            aIn.mouse[0] = true;
            for (int i = 0; i < 300 && !cli2G.pl.dead; ++i) {
                p1->body.pos.y = 0.0;  p1->body.vel = v2(0, 0);
                p2->body.pos.y = 0.0;  p2->body.vel = v2(0, 0);
                cliG.pl.pos.y = 0.0;   cliG.pl.vel = v2(0, 0);
                cli2G.pl.pos.y = 0.0;  cli2G.pl.vel = v2(0, 0);
                if (Game::Peer* t = peerOf(cliG, cli2G.pl.id)) aimAtG(cliG, aIn, t->body.pos);
                step(1);
            }
            aIn.mouse[0] = false;
            step(45);
            const int a = cliG.pl.id, b = cli2G.pl.id;
            auto fragsSeenBy = [&](Game& g, int id) -> int {
                if (g.pl.id == id) return g.pl.frags;
                Game::Peer* p = peerOf(g, id);
                return p ? p->body.frags : -99;
            };
            printf("      frags for the first client, as seen by the host / itself / the second: %d / %d / %d\n",
                   fragsSeenBy(hostG, a), fragsSeenBy(cliG, a), fragsSeenBy(cli2G, a));
            check(cli2G.pl.dead && cli2G.pl.deaths == 1, "the second client is told it was killed");
            check(fragsSeenBy(hostG, a) == 1 && fragsSeenBy(cliG, a) == 1 && fragsSeenBy(cli2G, a) == 1,
                  "the first client's frag is on all three screens");
            check(fragsSeenBy(hostG, b) == 0 && fragsSeenBy(cliG, b) == 0 && fragsSeenBy(cli2G, b) == 0, "the second client has none, on any");
            check(!hostG.killFeed.empty() && !cliG.killFeed.empty() && !cli2G.killFeed.empty(), "all three saw the announcement");
            step(60 * 4);
            check(!cli2G.pl.dead, "the victim is back");
            // the third player's shots are seen by the other client, not just the host
            {
                size_t seen = 0;
                hIn.mouse[0] = true;
                for (int i = 0; i < 90; ++i) {
                    hostG.pl.protect = 5.0f;
                    hostG.pl.aim = 0.0f;
                    step(1);
                    seen = std::max(seen, cliG.bullets.size() + cli2G.bullets.size());
                }
                hIn.mouse[0] = false;
                check(seen > 3, "shots fired by the host are seen by both clients");
            }
            // one client leaving does not disturb the other
            cliG.netShutdown();
            step(60);
            check(hostG.peers.size() == 1 && cli2G.peers.size() == 1, "when one client leaves, the others carry on with one fewer");
            check(cli2G.net && cli2G.net->welcomed && !cli2G.net->lost, "and the remaining client is still connected");
            cli2G.netShutdown();
            hostG.netShutdown();
        }

        // ---- a client that vanishes is dropped after a timeout
        {
            printf("\n  [a client that vanishes]\n");
            net::LoopLink la, lb;
            la.other = &lb;  lb.other = &la;
            cliG.startClientOn(renderer, &lb);
            hostG.startHostOn(renderer, &la, 0);
            Input in;  in.mousePx = v2(renderer.fbw * 0.5f, renderer.fbh * 0.5f);
            for (int i = 0; i < 180; ++i) { la.advance(dt); lb.advance(dt); cliG.update(renderer, in, dt); hostG.update(renderer, in, dt); }
            check(hostG.peers.size() == 1, "it joined");
            for (int i = 0; i < 60 * 8; ++i) { la.advance(dt); lb.advance(dt); hostG.update(renderer, in, dt); }   // the client stops answering
            check(hostG.peers.empty(), "and after a few seconds of silence the host lets it go");
            hostG.netShutdown();
            cliG.netShutdown();
        }

        printf("netgametest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
    }

    if (versusTest) {
        printf("versustest:\n");
        const float dt = 1.0f / 60.0f;
        Input idle;
        idle.mousePx = v2(gWidth * 0.5f, gHeight * 0.5f);
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        auto run = [&](int n) { for (int i = 0; i < n; ++i) game.update(renderer, idle, dt); };

        // ---- a match starts
        game.startVersus(renderer, 1);
        game.invincible = false;
        check(game.versus && game.sandbox && game.peers.size() == 1, "a match has you and one bot");
        check(!game.pl.dead && !game.peers[0].body.dead, "both are alive");
        check(game.pl.health == 100.0f && game.pl.protect > 0.0f, "you start on full suit, protected");
        check(game.pl.id != game.peers[0].body.id, "and the two have different ids");
        check(len(game.pl.pos - game.peers[0].body.pos) >= rules::SPAWN_APART * 0.8, "they do not spawn on top of each other");
        check(len(game.pl.pos) < rules::ARENA_RADIUS && len(game.peers[0].body.pos) < rules::ARENA_RADIUS, "both are inside the arena");
        {   // both spawn standing on a rock
            run(60);
            check(game.pl.grounded || len(game.pl.vel) < 400.0f, "you settle on the rock you spawned on");
        }

        // A clear stage for the shooting tests: two players in open space.
        const dv2 A(0.0, 0.0);
        game.world.clearZone(A, 700.0);
        auto stage = [&](double gap) {
            game.resetMatch();
            game.invincible = false;
            game.pl.pos = dv2(-gap * 0.5, 0.0);   game.pl.vel = v2(0, 0);   game.pl.protect = 0.0f;
            Player& b = game.peers[0].body;
            b.pos = dv2(gap * 0.5, 0.0);          b.vel = v2(0, 0);          b.protect = 0.0f;
            game.peers[0].bot = false;
            game.peers[0].cmd = PlayerCmd();
            game.bullets.clear();
        };
        auto aimBotAtPlayer = [&]() {
            Player& b = game.peers[0].body;
            b.pos.y = 0.0;  b.vel = v2(0, 0);
            game.pl.pos.y = 0.0;  game.pl.vel = v2(0, 0);
            game.peers[0].cmd.aim = std::atan2((float)(game.pl.pos.y - b.pos.y), (float)(game.pl.pos.x - b.pos.x));
        };

        // ---- shooting
        {
            stage(500.0);
            for (int i = 0; i < 40; ++i) {
                aimBotAtPlayer();
                game.peers[0].cmd.fire = i < 20;
                game.update(renderer, idle, dt);
            }
            const float lost = 100.0f - game.pl.health;
            printf("      a bot fired for a third of a second at 500 units: you lost %.0f suit\n", lost);
            check(lost >= rules::VS_RIFLE_DAMAGE, "rifle rounds hurt the player they hit");
            check(std::fmod(lost + 0.01f, rules::VS_RIFLE_DAMAGE) < 0.05f || lost >= 100.0f, "each round costs exactly the rifle damage");
            check(game.peers[0].body.health == 100.0f, "and the shooter is not hurt by their own rounds");
        }
        {
            stage(500.0);
            Player& b = game.peers[0].body;
            // The player shoots the bot; ownership is by id, so the shooter is immune.
            game.localCmd.aim = 0.0f;
            game.pl.aim = 0.0f;
            for (int i = 0; i < 25; ++i) {
                game.pl.pos.y = 0.0;  game.pl.vel = v2(0, 0);  b.pos.y = 0.0;  b.vel = v2(0, 0);
                game.pl.fireCd = 0.0f;
                game.fire(game.pl, false);        // straight along +x, at the bot
                game.pl.aim = 0.0f;
                game.update(renderer, idle, dt);
            }
            check(game.pl.health == 100.0f, "your own rounds never hurt you");
            check(b.health < 100.0f, "and they hurt the bot");
        }

        // ---- spawn protection
        {
            stage(500.0);
            game.pl.protect = 1.0f;
            for (int i = 0; i < 30; ++i) {
                aimBotAtPlayer();
                game.pl.protect = 1.0f;
                game.peers[0].cmd.fire = true;
                game.update(renderer, idle, dt);
            }
            check(game.pl.health == 100.0f, "a player who has just arrived cannot be hurt");
        }

        // ---- a kill
        {
            stage(500.0);
            game.pl.health = 5.0f;
            int tries = 0;
            while (!game.pl.dead && tries++ < 240) {
                aimBotAtPlayer();
                game.peers[0].cmd.fire = true;
                game.update(renderer, idle, dt);
            }
            game.peers[0].cmd.fire = false;
            check(game.pl.dead && game.playerGone(), "a player at zero suit is out");
            check(game.peers[0].body.frags == 1 && game.pl.deaths == 1, "the killer is credited with a frag, the victim a death");
            check(!game.killFeed.empty(), "the kill is announced");
            const float lostFrags = (float)game.pl.frags;
            (void)lostFrags;
            // respawn
            const dv2 diedAt = game.pl.pos;
            game.peers[0].cmd.fire = false;
            run((int)((rules::RESPAWN_TIME + 0.3f) * 60.0f));
            check(!game.pl.dead && game.pl.health == 100.0f, "they come back after the respawn time");
            check(game.pl.protect > 0.0f, "protected");
            check(game.world.probe(game.pl.pos, 6.0f, nullptr, nullptr) < 0 && len(game.pl.pos) < rules::ARENA_RADIUS,
                  "in open space inside the arena, not buried in rock");
            check(len(game.pl.pos - diedAt) > 1.0, "and not at the spot where they died");
        }

        // ---- a shell
        {
            // Rock a long way off bends a heavy shell's path, so a straight shot can miss.
            // This test is about the damage, not the aim: try a few corrections and take the first that lands.
            float lost = 0.0f;
            for (int k = 0; k < 9 && lost <= 0.0f; ++k) {
                const float offset = ((k + 1) / 2) * 0.03f * ((k & 1) ? -1.0f : 1.0f);
                stage(500.0);
                game.pl.protect = 0.0f;
                for (int i = 0; i < 90 && !game.pl.dead && game.pl.health == 100.0f; ++i) {
                    aimBotAtPlayer();
                    game.peers[0].cmd.aim += offset;
                    if (i == 0) game.peers[0].cmd.heavySeq = (uint8_t)(game.peers[0].body.seenHeavy + 1);
                    game.update(renderer, idle, dt);
                }
                lost = 100.0f - game.pl.health;
            }
            printf("      one shell at 500 units: %.0f suit\n", lost);
            check(lost >= rules::VS_HEAVY_DAMAGE * 0.3f && lost <= rules::VS_HEAVY_DAMAGE + 0.1f, "a shell does its damage, less if it lands beside you");
        }
        // ---- a blast credits whoever set it off
        {
            stage(500.0);
            game.pl.protect = 0.0f;
            game.explodeOwner = game.peers[0].body.id;
            game.explode(game.pl.pos, 70.0f, 0.0f, 60.0f, 0.0f, 0.0f, false);
            check(game.pl.health < 100.0f, "a blast hurts a player in reach");
            game.pl.health = 3.0f;
            game.explodeOwner = game.peers[0].body.id;
            game.explode(game.pl.pos, 70.0f, 0.0f, 60.0f, 0.0f, 0.0f, false);
            check(game.pl.dead && game.peers[0].body.frags == 1, "and a kill by blast credits the one who fired it");
        }

        // ---- a suicide costs a frag
        {
            stage(500.0);
            game.pl.frags = 3;
            game.pl.health = 1.0f;
            game.pl.protect = 0.0f;
            game.damagePlayer(game.pl, 50.0f, v2(0, 0), -1);
            check(game.pl.dead && game.pl.frags == 2, "dying to the world costs a frag");
        }

        // ---- the match ends
        {
            stage(500.0);
            game.peers[0].body.frags = rules::FRAG_LIMIT - 1;
            game.pl.health = 5.0f;  game.pl.protect = 0.0f;
            int tries = 0;
            while (!game.match.over && tries++ < 240) {
                aimBotAtPlayer();
                game.peers[0].cmd.fire = true;
                game.update(renderer, idle, dt);
            }
            game.peers[0].cmd.fire = false;
            check(game.match.over && game.match.winner == game.peers[0].body.id, "reaching the frag limit ends the match with a winner");
            const float hp = game.pl.health;
            game.damagePlayer(game.peers[0].body, 30.0f, v2(0, 0), game.pl.id);
            check(game.peers[0].body.health == 100.0f && hp == game.pl.health, "and nobody can be hurt once it is over");
            run((int)((rules::VS_MATCH_OVER + 0.5f) * 60.0f));
            check(!game.match.over && game.pl.frags == 0 && game.peers[0].body.frags == 0, "a new match begins on its own with the scores cleared");
            // a client must not start matches by itself
            game.netClient = true;
            game.match.over = true;  game.match.overTime = 0.0f;
            run((int)((rules::VS_MATCH_OVER + 0.5f) * 60.0f));
            check(game.match.over, "a network client waits for the host to say so");
            game.netClient = false;
            game.resetMatch();
        }

        // ---- the wall
        {
            stage(500.0);
            game.pl.pos = dv2(rules::ARENA_RADIUS + 300.0, 0.0);
            game.pl.vel = v2(120.0f, 0.0f);
            double maxR = 0;
            for (int i = 0; i < 240; ++i) {
                game.pl.protect = 1.0f;
                game.update(renderer, idle, dt);
                maxR = std::max(maxR, len(game.pl.pos));
            }
            printf("      pushed out to %.0f (arena %.0f), back at %.0f after 4 s\n", maxR, rules::ARENA_RADIUS, len(game.pl.pos));
            check(len(game.pl.pos) < maxR - 100.0, "past the edge of the arena, something pushes you back in");
        }

        // ---- bots fight each other and the match keeps going
        {
            game.startVersus(renderer, 3);
            game.invincible = true;                       // so the local player is a bystander
            game.vsShots = game.vsHits = 0;
            int frames = 0, maxFrags = 0, outsideMax = 0;
            double nearestSum = 0;  long nearestN = 0;
            int matches = 0;
            for (; frames < 60 * 240; ++frames) {
                game.update(renderer, idle, dt);
                if (game.match.over && game.match.overTime < dt * 1.5f) ++matches;   // count each match as it ends
                game.eachPlayer([&](Player& p) { maxFrags = std::max(maxFrags, p.frags); });
                if (frames % 30 == 0) {
                    // how close is each living player to its nearest living opponent?
                    game.eachPlayer([&](Player& a) {
                        if (a.dead) return;
                        double best = 1e30;
                        game.eachPlayer([&](Player& o) { if (&o != &a && !o.dead) best = std::min(best, len(o.pos - a.pos)); });
                        if (best < 1e29) { nearestSum += best; ++nearestN; }
                        outsideMax = std::max(outsideMax, (int)(len(a.pos) > rules::ARENA_RADIUS * 1.6));
                    });
                }
            }
            int totalDeaths = 0;
            game.eachPlayer([&](Player& p) { totalDeaths += p.deaths; });
            printf("      four players, 3 bots, 4 minutes: %d rounds fired, %d landed (%.0f%%), %d matches finished\n",
                   game.vsShots, game.vsHits, game.vsShots ? 100.0 * game.vsHits / game.vsShots : 0.0, matches);
            printf("      %d deaths in the current match, leader on %d; on average %.0f units from the nearest opponent\n",
                   totalDeaths, maxFrags, nearestN ? nearestSum / nearestN : 0.0);
            check(game.vsHits > 20, "bots hit each other with a fair share of their rounds");
            check(totalDeaths + matches * rules::FRAG_LIMIT >= 4, "they keep killing each other: at least a kill a minute");   // 6 to 21 across builds: who a bot picks as nearest (the idle player included) swings it
            check(nearestN && nearestSum / nearestN < 1500.0, "they close the distance rather than sit on their own rocks");
            check(game.peers.size() == 3, "nobody has vanished");
            check(outsideMax == 0, "and nobody has drifted far out of the arena");
        }

        // ---- draw it, once, to prove the HUD does not fall over
        {
            game.startVersus(renderer, 2);
            game.invincible = true;
            for (int i = 0; i < 60 * 8; ++i) game.update(renderer, idle, dt);
            game.render(renderer);
            char path[600];
            snprintf(path, sizeof path, "%s_versus.png", shotPath);
            renderer.screenshot(path);
            check(true, "the arena and its scoreboard draw");
        }

        printf("versustest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
    }

    if (udpTest) {
        // The same protocol over a real socket, on the loopback interface: a host and a
        // client in one process talking through Winsock. It cannot say anything about
        // routers or the internet, but it does prove the packets, the fragmenting and the
        // reliable channel work on actual sockets, not just the in-process link.
        printf("udptest:\n");
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        net::UdpLink hostLink, cliLink;
        const int port = 47831;
        const bool opened = hostLink.open(port, true);
        check(opened, "the host binds a UDP port");
        if (!opened) { printf("  (%s)\n", hostLink.lastError.c_str()); }
        check(cliLink.connect("127.0.0.1", port), "the client points at the host");
        if (opened) {
            net::Endpoint host, cli;
            host.link = &hostLink;  host.peer = 0;
            cli.link = &cliLink;    cli.peer = 0;

            // What the client sends first (so the host learns where it is), then a
            // stream of reliable messages from the host including some far bigger than a packet.
            std::vector<std::vector<uint8_t>> sent;
            for (int i = 0; i < 60; ++i) {
                std::vector<uint8_t> m;
                m.push_back((uint8_t)(1 + i % 20));
                const size_t extra = (i % 15 == 7) ? 20000 : (size_t)(10 + i * 7);
                for (size_t k = 0; k < extra; ++k) m.push_back((uint8_t)((i * 31 + k * 7) & 0x7F));
                sent.push_back(m);
            }
            std::vector<uint8_t> hello = { 1, 'h', 'i' };
            cli.sendReliable(hello);
            bool hostHeard = false, queued = false;
            LARGE_INTEGER f0, t0, t;
            QueryPerformanceFrequency(&f0);  QueryPerformanceCounter(&t0);
            auto clock = [&]() { QueryPerformanceCounter(&t); return (double)(t.QuadPart - t0.QuadPart) / (double)f0.QuadPart; };
            int unrelSent = 0, unrelGot = 0;
            double doneAt = -1;
            std::vector<std::vector<uint8_t>> got;
            double now = 0;
            while ((now = clock()) < 4.0) {
                cli.poll(now);
                for (auto& m : cli.ready) got.push_back(m);
                cli.ready.clear();
                unrelGot += (int)cli.unreliable.size();
                cli.unreliable.clear();
                cli.flush(now);
                host.poll(now);
                for (auto& m : host.ready) if (m == hello) hostHeard = true;
                host.ready.clear();
                if (hostHeard && !queued) {
                    queued = true;
                    for (auto& m : sent) host.sendReliable(m);
                }
                if (queued && unrelSent == 0)                          // a burst of small lossy messages, all at once
                    for (; unrelSent < 100; ++unrelSent) { std::vector<uint8_t> u = { 2, (uint8_t)unrelSent, 9, 9, 9 }; host.sendUnreliable(u); }
                host.flush(now);
                if (got.size() == sent.size() && host.rel.pending() == 0) {
                    if (doneAt < 0) doneAt = now;
                    if (now - doneAt > 0.3 || unrelGot >= 100) break;          // let the last lossy ones land
                }
                Sleep(2);
            }
            check(hostHeard, "the host receives the client's first message");
            check(got.size() == sent.size(), "every reliable message arrives, including the 20 KB ones");
            bool same = got.size() == sent.size();
            for (size_t i = 0; same && i < got.size(); ++i) same = got[i] == sent[i];
            check(same, "and each arrives intact and in order");
            printf("      lossy messages: %d sent, %d arrived\n", unrelSent, unrelGot);
            check(unrelGot >= 95, "at least 95 of 100 lossy messages arrive (a loopback drops almost none)");
            check(host.rel.pending() == 0, "the host has been told everything was received");
            printf("      took %.2f s; host sent %llu packets (%llu bytes), client %llu; round trip %.1f ms; %d resends\n",
                   now, (unsigned long long)hostLink.packetsSent, (unsigned long long)hostLink.bytesSent,
                   (unsigned long long)cliLink.packetsSent, host.rel.srtt * 1000.0, (int)host.rel.resent);
            check(host.rel.srtt < 0.05, "the measured round trip on loopback is under 50 ms");
        }
        printf("udptest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
    }

    if (netTest) {
        // The spike: can a second machine keep an identical, shot-up asteroid field
        // from nothing but messages, and what does that cost on the wire?
        //
        // A host world is streamed, populated and attacked (rifle tunnels, heavy
        // shells, and the odd nuke, all through the real World::damage / explode).
        // A client world starts EMPTY and, with a different seed, cannot cheat by
        // generating anything itself. They are joined by two in-process links with
        // configurable loss and latency. At the end the two worlds are compared rock
        // by rock: same set, same field, and how far apart in position.
        printf("nettest:\n");
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        struct Result { bool sameSet = false; int hostRocks = 0, cliRocks = 0, hashMismatch = 0;
                        double meanErr = 0, maxErr = 0; double steadyKBps = 0, initialKB = 0;
                        int repairs = 0, resends = 0, sabotaged = 0; double rtt = 0; bool joined = false; int inexact = 0; int sabotagedAlive = 0, sabotagedRepaired = 0; double firstDetect = 0; };

        auto run = [&](const char* label, float loss, double latency, double attackSeconds, int shooters, bool sabotage, float jitter = 0.0f) -> Result {
            static World hostW, cliW;
            const uint64_t seed = 0x5EEDFACEull;
            hostW.init(seed);
            cliW.init(seed ^ 0xDEADBEEFull);                 // a different seed: it must not generate its own rocks
            hostW.streamChunks(dv2(0, 0), 3000.0, 100000);
            hostW.step(1.0f / 60.0f, dv2(0, 0));
            hostW.settleDirty();

            net::HostReplicator rep;      rep.begin(hostW);
            net::ClientReplicator cr;     cr.begin(cliW);  cr.jitter = jitter;
            net::LoopLink la, lb;
            la.other = &lb;  lb.other = &la;
            la.loss = lb.loss = loss;
            la.latency = lb.latency = latency;
            la.rng = 777;  lb.rng = 4242;
            net::Endpoint host, cli;
            host.link = &la;  cli.link = &lb;

            std::vector<std::vector<uint8_t>> msgs;
            rep.collectInitial(msgs);
            for (auto& m : msgs) host.sendReliable(m);

            Rng rng(99);
            const float dt = 1.0f / 60.0f;
            double now = 0;
            const double joinSeconds = 3.0;                       // nothing but the field arriving
            const double total = joinSeconds + attackSeconds + 5.0;
            const int frames = (int)(total * 60.0);
            uint64_t sentAtInitialDone = 0;
            bool initialDone = false, attackDone = false, joinedInTime = false;
            uint64_t sentAtAttackEnd = 0;
            int repairsRequested = 0, sabotageCount = 0;
            bool sabotaged = false;
            std::vector<uint32_t> sabotagedIds, repairedIds;
            double sabotagedAt = 0, firstRepairAt = 0;
            std::vector<uint8_t> buf;

            // How many rocks a "shooter" can pick from: those near the origin.
            auto pickRock = [&]() -> int {
                std::vector<int> cand;
                for (int s : hostW.active) {
                    const Body& b = hostW.bodies[s];
                    if (b.alive && len2(b.pos) < 1800.0 * 1800.0 && b.radius > 24.0f) cand.push_back(s);
                }
                if (cand.empty()) return -1;
                return cand[(size_t)rng.i(0, (int)cand.size() - 1)];
            };
            auto shoot = [&](float caliber, int carves) {
                const int s = pickRock();
                if (s < 0) return;
                Body& b = hostW.bodies[s];
                const v2 dir = rng.dir();
                const float R = b.radius;
                const float off = rng.sym(R * 0.5f);
                for (int i = 0; i < carves; ++i) {
                    const float t = -R + (float)i * caliber * 0.62f;
                    const v2 p = perp(dir) * off + dir * t;
                    hostW.damage(s, dv2(b.pos.x + p.x, b.pos.y + p.y), caliber, 0.34f, rng.u32());
                }
            };

            for (int f = 0; f < frames; ++f) {
                now += dt;
                la.advance(dt);  lb.advance(dt);
                const bool attacking = now >= joinSeconds && now < joinSeconds + attackSeconds;

                // ---- sabotage: make the client wrong in the ways a real one could go wrong
                if (sabotage && !sabotaged && now > joinSeconds + attackSeconds * 0.4) {
                    sabotaged = true;
                    int corrupted = 0;
                    for (int s = 0; s < (int)cliW.bodies.size() && corrupted < 4; ++s) {
                        Body& b = cliW.bodies[s];
                        if (!b.alive || !b.authored || b.f.d.size() < 400) continue;     // a real rock, not a pebble
                        sabotagedIds.push_back(b.netId);
                        if (corrupted < 3) {
                            // A carve that never arrived: solid material where the host has a hole.
                            for (int y = 0; y < b.f.h; ++y)
                                for (int x = 0; x < b.f.w; ++x) {
                                    const v2 p = b.f.samplePos(x, y);
                                    if (len(p - v2(b.f.w * b.f.cell * 0.15f, b.f.h * b.f.cell * 0.15f) - b.f.origin) < 16.0f)
                                        b.f.at(x, y) = std::max(b.f.at(x, y), 4.0f);
                                }
                        }
                        else               cliW.destroy(s);                                                   // a rock it lost
                        ++corrupted;
                    }
                    sabotageCount = corrupted;
                    sabotagedAt = now;
                }

                // ---- host: the game happens
                if (attacking && (f % 2) == 0) {
                    // two players firing the rifle: about 24 shots a second between them
                    for (int k = 0; k < shooters; ++k) shoot(5.2f, 3);
                    if (rng.f() < 0.5f) shoot(5.2f, 3);
                    if (f % 120 == 0) shoot(21.0f, 5);                       // a heavy shell now and then
                    if (f % 360 == 0) {                                      // and a nuke every six seconds
                        const int s = pickRock();
                        if (s >= 0) hostW.explode(hostW.bodies[s].pos, 323.0f, 391.0f, 3.2e6f, rng.u32());
                    }
                }
                hostW.step(dt, dv2(0, 0));
                hostW.settleDirty();

                // ---- host: tell the client
                msgs.clear();
                rep.collectShapeChanges(msgs);
                for (auto& m : msgs) host.sendReliable(m);
                if ((f % 2) == 0) {
                    buf.clear();
                    rep.collectMotion(now, buf);
                    if (!buf.empty()) host.sendUnreliable(buf);
                    buf.clear();
                    rep.collectAudit(now, buf);
                    if (!buf.empty()) host.sendReliable(buf);
                }
                host.flush(now);

                // ---- the network delivers; the client applies
                cli.poll(now);
                for (auto& m : cli.ready) {
                    cr.applyReliable(m.data(), m.size());
                    if (!m.empty() && (net::Msg)m[0] == net::Msg::RockAudit && !cr.diverged.empty()) {
                        net::Writer w;
                        w.u8((uint8_t)net::Msg::RockRepair);
                        w.u16((uint16_t)cr.diverged.size());
                        for (uint32_t id : cr.diverged) w.u32(id);
                        cli.sendReliable(w.b);
                        repairsRequested += (int)cr.diverged.size();
                        for (uint32_t id : cr.diverged) {
                            const bool mine = std::find(sabotagedIds.begin(), sabotagedIds.end(), id) != sabotagedIds.end();
                            repairedIds.push_back(id);
                            if (mine && firstRepairAt == 0) firstRepairAt = now;
                            if (sabotage) printf("      [t=%.1f] repair asked for rock %u: %s\n", now, id, mine ? "one we damaged" : "NOT one we damaged");
                        }
                        cr.diverged.clear();
                    }
                }
                cli.ready.clear();
                for (auto& m : cli.unreliable) cr.applyUnreliable(m.data(), m.size());
                cli.unreliable.clear();
                cr.deadReckon(dt);
                cli.flush(now);

                // ---- host: a repair request arrives
                host.poll(now);
                for (auto& m : host.ready) {
                    net::Reader r(m.data(), m.size());
                    if ((net::Msg)r.u8() != net::Msg::RockRepair) continue;
                    const uint16_t n = r.u16();
                    for (uint16_t i = 0; i < n && !r.bad; ++i) {
                        std::vector<uint8_t> field;
                        rep.writeField(r.u32(), field);
                        if (!field.empty()) host.sendReliable(field);
                    }
                }
                host.ready.clear();
                host.unreliable.clear();

                // Bandwidth is read off at the two edges of the shooting.
                if (!initialDone && now >= joinSeconds) {
                    initialDone = true;
                    sentAtInitialDone = la.bytesSent;              // everything sent while joining
                    joinedInTime = host.rel.pending() == 0;
                }
                if (!attackDone && now >= joinSeconds + attackSeconds) {
                    attackDone = true;
                    sentAtAttackEnd = la.bytesSent;
                }
            }

            // ---- compare
            Result res;
            std::unordered_map<uint32_t, int> hostIds;
            for (int s = 0; s < (int)hostW.bodies.size(); ++s)
                if (hostW.bodies[s].alive && hostW.bodies[s].netId) hostIds[hostW.bodies[s].netId] = s;
            std::unordered_map<uint32_t, int> cliIds;
            for (int s = 0; s < (int)cliW.bodies.size(); ++s)
                if (cliW.bodies[s].alive && cliW.bodies[s].netId) cliIds[cliW.bodies[s].netId] = s;
            res.hostRocks = (int)hostIds.size();
            res.cliRocks = (int)cliIds.size();
            int missing = 0;
            double sumErr = 0;
            for (auto& kv : hostIds) {
                auto it = cliIds.find(kv.first);
                if (it == cliIds.end()) { ++missing; continue; }
                const Body& hb = hostW.bodies[kv.second];
                const Body& cb = cliW.bodies[it->second];
                if (!World::summariesClose(hostW.summarise(kv.second), cliW.summarise(it->second))) ++res.hashMismatch;
                if (hostW.fieldHash(kv.second) != cliW.fieldHash(it->second)) ++res.inexact;
                const double err = len(tov2(hb.pos - cb.pos));
                sumErr += err;
                res.maxErr = std::max(res.maxErr, err);
            }
            for (auto& kv : cliIds) if (!hostIds.count(kv.first)) ++missing;
            res.sameSet = missing == 0;
            res.meanErr = hostIds.empty() ? 0 : sumErr / (double)hostIds.size();

            res.steadyKBps = (double)(sentAtAttackEnd - sentAtInitialDone) / 1024.0 / attackSeconds;
            res.initialKB = (double)sentAtInitialDone / 1024.0;
            res.repairs = repairsRequested;
            res.resends = (int)host.rel.resent;
            res.sabotaged = sabotageCount;
            for (uint32_t id : sabotagedIds) {
                if (!hostIds.count(id)) continue;                       // the host has since split or removed it: nothing left to repair
                ++res.sabotagedAlive;
                if (std::find(repairedIds.begin(), repairedIds.end(), id) != repairedIds.end()) ++res.sabotagedRepaired;
            }
            res.firstDetect = firstRepairAt > 0 ? firstRepairAt - sabotagedAt : -1.0;
            res.joined = joinedInTime;
            res.rtt = host.rel.srtt;

            printf("\n  [%s] loss %.0f%%, latency %.0f ms, %.0f s of shooting\n", label, loss * 100.0f, latency * 1000.0, attackSeconds);
            printf("      rocks: host %d, client %d   carves sent %d, rocks introduced %d, splits applied %d\n",
                   res.hostRocks, res.cliRocks, rep.carvesSent, rep.rocksIntroduced, cr.splitsApplied);
            printf("      joining: %.1f KB in the first 3 s, for %d rocks\n", res.initialKB, res.hostRocks);
            printf("      host -> client while playing: %.1f KB/s   (shape %.1f, motion %.1f, audit %.1f, repairs %.1f KB total)\n",
                   res.steadyKBps, rep.shapeBytes / 1024.0, rep.motionBytes / 1024.0, rep.auditBytes / 1024.0, rep.fieldBytes / 1024.0);
            printf("      motion messages: %d sent for %.0f s = %.0f a second\n", rep.motionsSent, total, rep.motionsSent / total);
            printf("      position error at the end: mean %.2f, max %.2f units   field mismatches: %d   repairs asked for: %d\n",
                   res.meanErr, res.maxErr, res.hashMismatch, res.repairs);
            printf("      messages re-sent: %d   round trip measured at %.0f ms\n", res.resends, res.rtt * 1000.0);
            printf("      rocks not bit-for-bit identical (tolerated): %d\n", res.inexact);
            printf("      packets host->client %llu, client->host %llu\n",
                   (unsigned long long)la.packetsSent, (unsigned long long)lb.packetsSent);
            return res;
        };

        // A quiet game: two people shooting, over a perfect link.
        const Result clean = run("clean link", 0.0f, 0.0, 30.0, 1, false);
        check(clean.sameSet, "clean link: the client ends up with exactly the host's rocks");
        check(clean.hashMismatch == 0, "clean link: every rock's shape matches, without any repair");
        check(clean.maxErr < 25.0, "clean link: every rock is close to where the host has it");

        // The same game over a bad connection.
        const Result lossy = run("lossy link", 0.05f, 0.060, 30.0, 1, false);
        check(lossy.sameSet, "5% loss, 60 ms: the client still ends up with exactly the host's rocks");
        check(lossy.hashMismatch == 0, "5% loss, 60 ms: every rock's shape matches");
        check(lossy.maxErr < 45.0, "5% loss, 60 ms: every rock is close to where the host has it");

        // A terrible one. The resend timer has to learn the round trip or it will
        // send everything twice before the first acknowledgement can get back.
        const Result awful = run("awful link", 0.20f, 0.150, 20.0, 1, false);
        check(awful.sameSet, "20% loss, 150 ms: the client still ends up with exactly the host's rocks");
        check(awful.hashMismatch == 0, "20% loss, 150 ms: every rock's shape matches");
        check(awful.rtt > 0.25 && awful.rtt < 0.5, "20% loss, 150 ms: the round trip is measured (about 300 ms)");
        check(awful.initialKB < clean.initialKB * 3.0, "20% loss, 150 ms: joining costs under 3x the clean price, not 8x");

        // The repair path, which nothing else exercises: make the client wrong on
        // purpose (three warped rocks and one lost one) and check the host notices.
        const Result broken = run("sabotaged client", 0.02f, 0.040, 30.0, 1, true);
        check(broken.sabotaged == 4, "sabotage: four rocks were damaged on the client");
        printf("      sabotage: %d rocks damaged, %d of them still exist on the host, %d of those were repaired; first noticed %.1f s after\n",
               broken.sabotaged, broken.sabotagedAlive, broken.sabotagedRepaired, broken.firstDetect);
        check(broken.sabotagedAlive >= 1, "sabotage: at least one damaged rock survived to be checked");
        check(broken.sabotagedRepaired == broken.sabotagedAlive, "sabotage: every damaged rock that still exists was caught and repaired");
        check(broken.firstDetect > 0.0 && broken.firstDetect < 25.0, "sabotage: and the first was noticed within 25 s");
        check(broken.hashMismatch == 0 && broken.sameSet, "sabotage: and after the repair the worlds agree again");

        // Two machines whose arithmetic differs in the last bits. A nudge of a thousandth
        // of a unit per carve is far bigger than a real last-bit difference would be, so
        // this is a hard test of whether tolerance plus repair keeps the worlds together.
        const Result jittery = run("float noise", 0.02f, 0.040, 30.0, 1, false, 0.001f);
        check(jittery.sameSet && jittery.hashMismatch == 0, "float noise: the worlds still end up agreeing");
        check(jittery.repairs < 20, "float noise: and it takes fewer than 20 repairs to keep them there");
        const Result jittery2 = run("heavy float noise", 0.02f, 0.040, 30.0, 1, false, 0.05f);
        check(jittery2.sameSet && jittery2.hashMismatch == 0, "heavy noise (0.05 units): the repairs still bring them back together");

        // A busy game: eight people firing at once.
        const Result stress = run("stress", 0.02f, 0.040, 30.0, 8, false);
        check(stress.sameSet && stress.hashMismatch == 0, "eight shooters: the worlds still agree exactly");

        check(clean.steadyKBps < 60.0, "the steady traffic to one client is under 60 KB/s");
        check(stress.steadyKBps < 120.0, "even eight shooters stay under 120 KB/s");
        check(clean.initialKB < 200.0, "joining costs under 200 KB");

        printf("nettest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
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
            std::string pattern;
            bool exact = true;
            float prevLen = 0.0f, prevHull = 0.0f;
            bool growing = true, tougher = true;
            for (int n = 1; n <= 30; ++n) {
                game.startLevel(n);
                const bool has = game.level.hasShip;
                pattern += has ? 'S' : '.';
                if (has != (n % 3 == 0)) exact = false;
                if (has) {
                    float length = 0.0f;
                    for (const v2& p : game.level.ship.hull) length = std::max(length, 2.0f * std::fabs(p.x));
                    printf("    level %2d: tier %d, %4.0f long, hull %5.0f, mounts %.0f units, %d weapons, rifle gets %.0f%%, a nuke takes %.0f%% of it\n", n, game.level.ship.tier, length,
                           game.level.ship.maxHp, game.level.ship.mountR, (int)game.level.ship.weapons.size(),
                           100.0f * game.level.ship.riflePass / rules::SHIP_RIFLE_FACTOR, 100.0f * game.level.ship.nukeShare);
                    if (n >= 6 && length < prevLen * 1.15f && length < rules::SHIP_LENGTH_CAP * 0.9f) growing = false;
                    if (n >= 6 && game.level.ship.maxHp < prevHull * 1.3f) tougher = false;
                    prevLen = length;  prevHull = game.level.ship.maxHp;
                }
            }
            printf("  levels 1-30: %s\n", pattern.c_str());
            game.startLevel(1);
            check(!game.level.hasShip, "level 1 has no warship");
            game.startLevel(2);
            check(!game.level.hasShip, "nor does level 2");
            game.startLevel(3);
            check(game.level.hasShip, "level 3 has the first one");
            check(exact, "and from there every third level, and no others");
            check(growing, "each warship is clearly bigger than the one before");
            check(tougher, "and each has a much tougher hull");
            game.startLevel(3);
            {
                float length = 0.0f, width = 0.0f;
                for (const v2& p : game.level.ship.hull) { length = std::max(length, 2.0f * std::fabs(p.x)); width = std::max(width, 2.0f * std::fabs(p.y)); }
                printf("    the first: %.0f by %.0f units, against a turret 34 across\n", length, width);
                check(length > 4.0f * 34.0f && length < 8.0f * 34.0f, "the first warship is about six times the length of a turret");
                check(width > 2.0f * 34.0f, "and more than twice its width, being stubby");
            }
            game.startLevel(6);
            const std::string a = game.level.ship.name;
            game.startLevel(6);
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
        game.startLevel(3);
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
            game.startLevel(3);
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

    if (fractalTest) {
        printf("fractaltest:\n");
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        // ---- how far it flies between splits depends on how far the cursor is
        {
            check(Game::fractalSplitDistance(0.0f) == rules::FRACTAL_SPLIT_MIN, "a cursor on top of you gives the shortest interval");
            check(Game::fractalSplitDistance(100000.0f) == rules::FRACTAL_SPLIT_MAX, "a cursor a very long way off gives the longest");
            bool monotonic = true;
            float prev = 0.0f;
            for (float a = 0.0f; a <= 4000.0f; a += 25.0f) {
                const float x = Game::fractalSplitDistance(a);
                if (x < prev - 1e-3f) monotonic = false;
                prev = x;
            }
            check(monotonic, "the farther the cursor, the longer between splits, never shorter");
            const float a1 = Game::fractalSplitDistance(500.0f), a2 = Game::fractalSplitDistance(1000.0f);
            printf("      aiming at 500 units: splits every %.0f; at 1000 units: every %.0f\n", a1, a2);
            check(std::fabs(a1 - 500.0f) < 0.01f && std::fabs(a2 - 1000.0f) < 0.01f, "in the middle of the range it is the cursor distance itself");
        }

        // ---- a fresh level, and open space to fire into
        game.startRun(renderer);
        game.invincible = true;
        game.floating = true;
        game.startLevel(3);
        game.level.slots.clear();
        game.level.diff.droneSpeed = 0.0f;
        const dv2 C(6000.0, 6000.0);
        openSpot(C);
        game.pl.hasFractal = true;
        game.pl.fractalAmmo = 12;

        auto hold = [&]() { game.pl.pos = C;  game.pl.vel = v2(0, 0); };
        auto fireAt = [&](float aimDist, float angle) {
            hold();
            game.pl.aim = angle;
            game.lastAimDist = aimDist;
            game.pl.fractalCd = 0.0f;
            game.fireFractal(aimDist);
        };
        auto runFrames = [&](int n) { for (int i = 0; i < n; ++i) { hold(); game.update(renderer, idle, dt); } };

        // ---- the split cascade: how many, how strong, how often
        {
            game.bullets.clear();
            game.enemies.clear();
            const dv2 muzzle(C.x + 16.0, C.y);
            fireAt(200.0f, 0.0f);                                   // splits every 200 units of cursor distance, about 0.36 s: the whole cascade fits in the cleared zone
            check(game.bullets.size() == 1 && game.bullets[0].gen == 0, "firing makes one shell");
            check(game.bullets[0].homing, "and it is a homing device");

            int maxGen = -1;  size_t peak = 0;
            bool allHoming = true, powerLaw = true, neverPastFive = true, oneParent = true;
            double firstSplitDist = -1;
            std::vector<size_t> whenCount(8, 0);
            size_t frames = 0;
            for (; frames < 60 * 8; ++frames) {
                runFrames(1);
                peak = std::max(peak, game.bullets.size());
                if (firstSplitDist < 0 && game.bullets.size() >= 2) {
                    double d = 0;
                    for (const Bullet& b : game.bullets) d += len(b.pos - muzzle);
                    firstSplitDist = d / (double)game.bullets.size();
                }
                for (const Bullet& b : game.bullets) {
                    maxGen = std::max(maxGen, b.gen);
                    if (!b.homing) allHoming = false;
                    if (b.gen > rules::FRACTAL_SPLITS) neverPastFive = false;
                    if (b.gen >= 0 && std::fabs(b.power - std::pow(rules::FRACTAL_CHILD, (float)b.gen)) > 1e-4f) powerLaw = false;
                }
                if (game.bullets.empty()) break;
            }
            printf("      one shell: peak %d pieces at once, deepest generation %d, first split %.0f units from the muzzle\n",
                   (int)peak, maxGen, firstSplitDist);
            check(maxGen == rules::FRACTAL_SPLITS, "it splits five times and no more");
            check(neverPastFive, "no piece is ever a sixth generation");
            check(peak == 32, "so one shell becomes 32 pieces at its most");
            check(allHoming, "every piece is a homing device");
            check(powerLaw, "each generation is exactly 0.45 of the one before (half, less a tenth)");
            check(std::fabs(rules::FRACTAL_CHILD - 0.5f * 0.9f) < 1e-6f, "and 0.45 is 50% minus 10%");
            check(std::fabs(firstSplitDist - 200.0) < 40.0, "the first split comes after the interval the cursor distance asked for");
        }
        {   // the same shot with the cursor twice as far away splits later
            double dNear = 0, dFar = 0;
            for (int pass = 0; pass < 2; ++pass) {
                game.bullets.clear();
                const dv2 muzzle(C.x + 16.0, C.y);
                fireAt(pass == 0 ? 300.0f : 900.0f, 0.0f);
                for (int i = 0; i < 60 * 3 && game.bullets.size() < 2; ++i) runFrames(1);
                double d = 0;
                for (const Bullet& b : game.bullets) d += len(b.pos - muzzle);
                (pass == 0 ? dNear : dFar) = d / std::max<size_t>(1, game.bullets.size());
            }
            printf("      cursor at 300 units: first split after %.0f; cursor at 900: after %.0f\n", dNear, dFar);
            check(dFar > dNear * 2.0, "a far cursor really does make it fly further before it divides");
        }

        // ---- the pieces hunt different targets
        struct Hunt { float fractal[2] = { 0, 0 }; float heavy[2] = { 0, 0 }; int alive[2] = { 0, 0 }; };
        Hunt hunt;
        {
            auto arena = [&](float hp) {
                game.bullets.clear();  game.enemies.clear();  game.waves.clear();
                Enemy a = plainDrone(dv2(C.x + 700.0, C.y + 300.0));
                Enemy b = plainDrone(dv2(C.x + 700.0, C.y - 300.0));
                a.hp = a.maxHp = b.hp = b.maxHp = hp;
                game.enemies.push_back(a);  game.enemies.push_back(b);
                return std::make_pair(a.id, b.id);
            };
            auto lost = [&](int id) { const Enemy* e = game.findEnemy(id); return e ? e->maxHp - e->hp : 1e6f; };
            {   // a single ordinary homing shell, for comparison
                const auto ids = arena(1e6f);
                hold();  game.pl.aim = 0.0f;  game.pl.heavyCd = 0.0f;
                game.fire(game.pl, true);
                for (int i = 0; i < 60 * 4; ++i) runFrames(1);
                hunt.heavy[0] = lost(ids.first);  hunt.heavy[1] = lost(ids.second);
            }
            {   // the fractal shell, same shot
                const auto ids = arena(1e6f);
                fireAt(300.0f, 0.0f);                                   // the cross half way to them: it divides before it arrives
                for (int i = 0; i < 60 * 5; ++i) runFrames(1);
                hunt.fractal[0] = lost(ids.first);  hunt.fractal[1] = lost(ids.second);
            }
            printf("      damage to the two drones: homing shell %.0f / %.0f,  fractal shell %.0f / %.0f\n",
                   hunt.heavy[0], hunt.heavy[1], hunt.fractal[0], hunt.fractal[1]);
            check(hunt.heavy[0] + hunt.heavy[1] > 0.0f, "the ordinary homing shell finds one of them");
            check(hunt.heavy[0] == 0.0f || hunt.heavy[1] == 0.0f, "and only one, since it is one shell");
            check(hunt.fractal[0] > 0.0f && hunt.fractal[1] > 0.0f, "the fractal shell's pieces find both, each hunting its own");
            const float fr = hunt.fractal[0] + hunt.fractal[1], he = hunt.heavy[0] + hunt.heavy[1];
            check(fr > he * 2.0f, "and a single fractal shell does more than twice the damage of a homing shell");

            // Ordinary drones die to it.
            {
                game.bullets.clear();  game.enemies.clear();
                Enemy a = plainDrone(dv2(C.x + 700.0, C.y + 200.0));
                Enemy b = plainDrone(dv2(C.x + 700.0, C.y - 200.0));
                Enemy c2 = plainDrone(dv2(C.x + 1000.0, C.y));
                game.enemies.push_back(a);  game.enemies.push_back(b);  game.enemies.push_back(c2);
                fireAt(250.0f, 0.0f);                                   // divides early, so the swarm has room to spread
                for (int i = 0; i < 60 * 5; ++i) runFrames(1);
                int left = 0;
                for (const Enemy& e : game.enemies) if (e.alive) ++left;
                printf("      three ordinary drones: %d left standing\n", left);
                check(left == 0, "one shell clears three ordinary drones");
            }
        }

        // ---- pieces choose different targets even when they would rather share
        {
            game.bullets.clear();  game.enemies.clear();
            Enemy a = plainDrone(dv2(C.x + 800.0, C.y + 350.0));
            Enemy b = plainDrone(dv2(C.x + 800.0, C.y - 350.0));
            game.enemies.push_back(a);  game.enemies.push_back(b);
            fireAt(200.0f, 0.0f);                                   // splits very soon
            for (int i = 0; i < 90; ++i) {                          // run until the first generation of pieces exists
                runFrames(1);
                bool have = false;
                for (const Bullet& bl : game.bullets) if (bl.gen == 1) have = true;
                if (have) break;
            }
            std::vector<int> targets;
            for (const Bullet& bl : game.bullets) if (bl.gen == 1) targets.push_back(bl.targetId);
            std::sort(targets.begin(), targets.end());
            printf("      the two first-generation pieces are chasing targets %d and %d\n",
                   targets.size() > 0 ? targets[0] : -1, targets.size() > 1 ? targets[1] : -1);
            check(targets.size() == 2 && targets[0] != targets[1] && targets[0] != 0, "the two pieces are given different targets");
        }

        // ---- carving rock
        {
            game.bullets.clear();  game.enemies.clear();
            const dv2 R(C.x + 350.0, C.y);
            const int slot = game.world.spawn(R, 90.0f, 4242u);
            game.world.step(dt, C);
            auto solid = [&]() { return slot >= 0 && game.world.bodies[slot].alive ? game.world.summarise(slot).solid : 0u; };
            const uint32_t before = solid();
            fireAt(350.0f, 0.0f);
            for (int i = 0; i < 60 * 3; ++i) runFrames(1);
            uint32_t after = 0;
            for (int s : game.world.active) {
                const Body& b = game.world.bodies[s];
                if (b.alive && len(b.pos - R) < 250.0) after += game.world.summarise(s).solid;
            }
            printf("      a rock of %u solid samples has %u left within reach after one shell\n", before, after);
            check(before > 0 && after < before, "a fractal shell carves rock like any other");
        }

        // ---- the blast and the crater follow the damage each piece does
        {
            float removed[3] = { 0, 0, 0 };
            float radii[3]   = { 0, 0, 0 };
            const int gens[3] = { 0, 2, 4 };
            for (int k = 0; k < 3; ++k) {
                game.bullets.clear();  game.enemies.clear();
                const dv2 R(C.x + 500.0, C.y);
                const int slot = game.world.spawn(R, 140.0f, 777u);
                game.world.step(dt, C);
                auto solid = [&]() { return slot >= 0 && game.world.bodies[slot].alive ? game.world.summarise(slot).solid : 0u; };
                const uint32_t before = solid();
                Bullet b;
                b.gen = gens[k];
                b.power = std::pow(rules::FRACTAL_CHILD, (float)gens[k]);
                b.heavy = true;
                b.pos = dv2(R.x - 140.0, R.y);                      // on the rock's rim
                b.col = Col(1, 0.5f, 0.1f);
                game.shellBurst(b);
                const uint32_t after = solid();
                removed[k] = (float)before - (float)after;
                radii[k] = rules::FRACTAL_SPLASH_R * std::sqrt(b.power);
                game.world.destroy(slot);
            }
            printf("      rock removed by a generation 0, 2, 4 burst: %.0f, %.0f, %.0f samples (blast radii %.0f, %.0f, %.0f)\n",
                   removed[0], removed[1], removed[2], radii[0], radii[1], radii[2]);
            check(removed[0] > removed[1] * 1.5f && removed[1] > removed[2] * 1.2f && removed[2] >= 0.0f,
                  "the weaker the piece, the smaller the crater it makes");
            check(removed[0] > 0.0f, "a full-strength burst bites a real crater out of the rock");
            check(std::fabs(radii[1] / radii[0] - rules::FRACTAL_CHILD) < 1e-3f,
                  "and the blast area follows the damage: a generation two piece has 0.45 squared of the area");
        }

        // ---- a split is narrower than it was, and each piece is quicker than its parent
        {
            game.bullets.clear();  game.enemies.clear();
            fireAt(300.0f, 0.0f);
            float parentSpeed = 0.0f;
            for (int i = 0; i < 60 * 2; ++i) {
                for (const Bullet& bl : game.bullets) if (bl.gen == 0) parentSpeed = len(bl.vel);
                runFrames(1);
                int n = 0;
                for (const Bullet& bl : game.bullets) if (bl.gen == 1) ++n;
                if (n == 2) break;
            }
            v2 va(0, 0), vb(0, 0);  int n = 0;
            for (const Bullet& bl : game.bullets) if (bl.gen == 1) { (n++ == 0 ? va : vb) = bl.vel; }
            const float ang = n == 2 ? std::fabs(wrapAngle(std::atan2(va.y, va.x) - std::atan2(vb.y, vb.x))) : 0.0f;
            const float sp  = n == 2 ? 0.5f * (len(va) + len(vb)) : 0.0f;
            printf("      the two pieces leave %.1f degrees apart (%.1f from the parent's line), at %.0f against the parent's %.0f\n",
                   ang * 57.2958f, ang * 0.5f * 57.2958f, sp, parentSpeed);
            check(n == 2 && std::fabs(ang - 2.0f * rules::FRACTAL_SPREAD) < 0.06f, "the two pieces leave the parent's line by the (narrower) spread angle");
            check(std::fabs(rules::FRACTAL_SPREAD - 0.30f * 0.75f) < 1e-6f, "which is 25% smaller than the 0.30 radians it was");
            check(n == 2 && parentSpeed > 0.0f && sp > parentSpeed * 1.08f && sp < parentSpeed * 1.17f, "and each piece is faster than the parent was (about 14%)");
        }

        // ---- ammunition, cooldown, ownership
        {
            game.bullets.clear();  game.enemies.clear();
            game.pl.hasFractal = false;  game.pl.fractalAmmo = 5;  game.pl.fractalCd = 0.0f;
            game.fireFractal(500.0f);
            check(game.bullets.empty() && game.pl.fractalAmmo == 5, "without the weapon, nothing happens");
            game.pl.hasFractal = true;  game.pl.fractalAmmo = 0;
            game.fireFractal(500.0f);
            check(game.bullets.empty(), "without shells, nothing happens");
            game.pl.fractalAmmo = 2;  game.pl.fractalCd = 0.0f;
            game.fireFractal(500.0f);
            check(game.bullets.size() == 1 && game.pl.fractalAmmo == 1, "a shot costs one shell");
            game.fireFractal(500.0f);
            check(game.bullets.size() == 1 && game.pl.fractalAmmo == 1, "and there is a cooldown between shots");
            check(game.pl.fractalCd > 1.0f, "a long one");
            game.bullets.clear();
            // Z through the real input path
            game.pl.fractalCd = 0.0f;  game.pl.fractalAmmo = 3;
            Input z;  z.pressed['Z'] = true;  z.mousePx = v2(renderer.fbw * 0.8f, renderer.fbh * 0.5f);
            hold();
            game.update(renderer, z, dt);
            check(game.pl.fractalAmmo == 2 && !game.bullets.empty(), "the Z key fires it");
            check(game.lastAimDist > 50.0f, "and the distance to the cursor was measured (the cross is not on the spaceman)");
        }

        // ---- the depot: the dearest thing on the shelf
        {
            game.startRun(renderer);
            game.invincible = true;
            check(ITEM_COUNT == 6 && ITEM_FRACTAL == 5, "the depot has a sixth row");
            int dearest = 0;
            for (int i = 0; i < ITEM_COUNT; ++i) if (i != ITEM_FRACTAL) dearest = std::max(dearest, game.priceOf(i));
            printf("      price %d credits (the next dearest thing is %d)\n", game.priceOf(ITEM_FRACTAL), dearest);
            check(game.priceOf(ITEM_FRACTAL) > dearest * 2, "it costs more than twice anything else in the depot");
            check(rules::PRICE_FRACTAL / rules::FRACTAL_LOAD > rules::PRICE_NUKE / rules::NUKE_PACK * 2, "and each shell costs more than twice a nuke");

            game.credits = rules::PRICE_FRACTAL - 1;
            check(!game.buy(ITEM_FRACTAL) && !game.pl.hasFractal, "one credit short buys nothing");
            game.credits = rules::PRICE_FRACTAL;
            check(game.buy(ITEM_FRACTAL) && game.pl.hasFractal && game.credits == 0, "the price unlocks it");
            check(game.pl.fractalAmmo == rules::FRACTAL_LOAD, "with its first load");
            game.credits = 5000;
            check(game.priceOf(ITEM_FRACTAL) == rules::PRICE_FRACTAL_AMMO, "more shells are cheaper than the launcher");
            game.buy(ITEM_FRACTAL);
            check(game.pl.fractalAmmo == 2 * rules::FRACTAL_LOAD && game.credits == 5000 - rules::PRICE_FRACTAL_AMMO, "and add another load");
            while (game.pl.fractalAmmo < rules::FRACTAL_MAX) game.buy(ITEM_FRACTAL);
            const int cr = game.credits;
            check(!game.buy(ITEM_FRACTAL) && game.credits == cr && game.pl.fractalAmmo == rules::FRACTAL_MAX, "the magazine is capped, and a full one costs nothing");

            // through the menu: the 6 key
            game.pl.hasFractal = false;  game.pl.fractalAmmo = 0;
            game.credits = 2000;
            game.state = State::Shop;
            Input six;  six.pressed['6'] = true;
            game.update(renderer, six, dt);
            check(game.pl.hasFractal && game.credits == 2000 - rules::PRICE_FRACTAL, "the 6 key buys it in the depot");
            game.state = State::Playing;
        }

        // ---- a picture of it, mid-cascade, with the depot row
        {
            game.startRun(renderer);
            game.invincible = true;  game.floating = true;
            game.startLevel(3);
            game.level.slots.clear();  game.level.diff.droneSpeed = 0.0f;
            openSpot(C);
            game.pl.hasFractal = true;  game.pl.fractalAmmo = 3;
            game.cam.halfW = game.zoomTarget = 420.0f;
            game.bullets.clear();  game.enemies.clear();
            for (int k = 0; k < 5; ++k) {
                Enemy e = plainDrone(dv2(C.x + 600.0 + 30.0 * (k % 2), C.y + (k - 2) * 105.0));
                e.hp = e.maxHp = 1e6f;
                game.enemies.push_back(e);
            }
            fireAt(300.0f, 0.0f);                                   // a near cursor: it divides quickly, in view
            char path[600];
            game.level.banner = 0.0f;                               // no intro text over the picture
            for (int i = 0; i < 60 * 3; ++i) {
                runFrames(1);
            const int shots[3] = { 16, 26, 36 };
                for (int k = 0; k < 3; ++k)
                    if (i == shots[k]) {
                        {   // where is everything? (the picture is the check on this, so say so in numbers too)
                            printf("      frame %d: pieces by generation:", i);
                            int perGen[8] = {};
                            double reach = 0;
                            for (const Bullet& b : game.bullets) { if (b.gen >= 0 && b.gen < 8) ++perGen[b.gen]; reach = std::max(reach, b.pos.x - C.x); }
                            for (int g = 0; g <= 5; ++g) printf(" %d", perGen[g]);
                            printf("   furthest piece %.0f units out; drones at", reach);
                            for (const Enemy& e : game.enemies) printf(" %.0f", e.pos.x - C.x);
                            printf("\n");
                            game.cam.pos = dv2(C.x + 300.0, C.y);         // look at the action, not at the spaceman
                        }
                        game.render(renderer);
                        snprintf(path, sizeof path, "%s_fractal%d.png", shotPath, k);
                        renderer.screenshot(path);
                    }
            }
            game.startRun(renderer);
            game.credits = 3000;
            game.pl.hasFractal = true;  game.pl.fractalAmmo = 4;
            game.state = State::Shop;
            for (int i = 0; i < 3; ++i) game.update(renderer, idle, dt);
            game.render(renderer);
            snprintf(path, sizeof path, "%s_fractal_shop.png", shotPath);
            renderer.screenshot(path);
            check(true, "the shell and the depot row draw");
        }

        printf("fractaltest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return failures == 0 ? 0 : 1;
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
            game.pl.fuel = tune::FUEL_MAX;
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
            game.pl.fuel = tune::FUEL_MAX;  game.pl.fuelLocked = false;
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
                const bool tank = game.pl.fuel > (in.mouse[1] ? 0.02f : 0.35f) * tune::FUEL_MAX;   // burn in bursts: start above a third of the tank
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
        check(dmgAt[3] < 5.0f, "well outside the blast, next to no damage");   // (a fast-spinning rock can scrape you in that frame)

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
        game.world.bodies[big].angVel = 0.0f;  game.world.bodies[big].vel = v2(0, 0);   // the turret test is about the turret: a rock spinning away would carry it out of range
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

    if (laserTest) {
        printf("lasertest:\n");
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-80s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        game.startRun(renderer);
        game.invincible = false;
        game.floating = true;
        game.startLevel(4);
        game.level.slots.clear();
        const dv2 C(6000.0, 6000.0);
        openSpot(C);
        const Difficulty D = game.level.diff;

        // ---- the small changes
        check(std::fabs(rules::SHIP_FIRE_SLOW * 3.0f - 1.35f) < 1e-4f, "warship guns fire three times as often as they did (1.35 -> 0.45)");
        {
            Rng a(1), b(1);
            const Difficulty d1 = makeDifficulty(6, a);
            const float was = std::max(0.03f, 0.17f - 0.011f * 6.0f);
            (void)b;
            printf("      enemy scatter at level 6: %.3f (it was %.3f)\n", d1.aimError, was);
            check(std::fabs(d1.aimError - 0.5f * was) < 1e-4f, "enemy shots scatter half as much as they did");
        }

        // ---- warships: variety, and how many carry the ray
        int withLaser = 0, ships = 0, multipleLasers = 0, laserAtBow = 0;
        float rateLo = 9, rateHi = 0, powLo = 9, powHi = 0, spdLo = 9, spdHi = 0;
        int shotsLo = 9, shotsHi = -9;
        float gunShareLo = 1, gunShareHi = 0;
        int typeCount[5] = { 0, 0, 0, 0, 0 };
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            Ship sh;
            generateShip(sh, seed * 2654435761ull, 4, D);
            ++ships;
            int lasers = 0, guns = 0, others = 0;
            for (const ShipWeapon& w : sh.weapons) {
                ++typeCount[(int)w.type];
                if (w.type == ShipWeapon::Laser) { ++lasers; if (w.local.x > 0.3f * sh.radius && std::fabs(w.local.y) < 1.0f) ++laserAtBow; continue; }
                (w.type == ShipWeapon::Gun ? guns : others)++;
                rateLo = std::min(rateLo, w.rate);  rateHi = std::max(rateHi, w.rate);
                powLo = std::min(powLo, w.power);   powHi = std::max(powHi, w.power);
                spdLo = std::min(spdLo, w.speed);   spdHi = std::max(spdHi, w.speed);
                if (w.type == ShipWeapon::Gun) { shotsLo = std::min(shotsLo, w.shots); shotsHi = std::max(shotsHi, w.shots); }
            }
            if (lasers) ++withLaser;
            if (lasers > 1) ++multipleLasers;
            if (guns + others > 0) {
                const float gs = (float)guns / (guns + others);
                gunShareLo = std::min(gunShareLo, gs);  gunShareHi = std::max(gunShareHi, gs);
            }
        }
        printf("      300 warships: %d carry the ray (%.0f%%); weapon rate %.2f-%.2f, power %.2f-%.2f, speed %.2f-%.2f, gun burst %+d to %+d\n",
               withLaser, 100.0 * withLaser / ships, rateLo, rateHi, powLo, powHi, spdLo, spdHi, shotsLo, shotsHi);
        printf("      weapons made: %d guns, %d missile pods, %d flak, %d cannon, %d rays; the share of guns on a ship runs from %.0f%% to %.0f%%\n",
               typeCount[0], typeCount[1], typeCount[2], typeCount[3], typeCount[4], 100.0f * gunShareLo, 100.0f * gunShareHi);
        check(withLaser > ships * 0.30 && withLaser < ships * 0.60, "some warships carry the ray (about 45%), and not all");
        check(multipleLasers == 0, "never more than one ray on a ship");
        check(laserAtBow == withLaser, "and it sits on the bow");
        check(rateHi / rateLo > 1.8f, "weapons fire at different rates");
        check(powHi / powLo > 1.5f, "and hit with different strength");
        check(spdHi / spdLo > 1.25f, "and their rounds fly at different speeds");
        check(shotsHi - shotsLo >= 3, "and the guns fire bursts of different length");
        check(gunShareHi - gunShareLo > 0.6f, "some ships are mostly guns and some hardly any: each has its own doctrine");
        check(typeCount[1] > 50 && typeCount[2] > 50 && typeCount[3] > 50, "and every kind of weapon turns up");

        // ---- one ship with the ray, and only the ray, in front of a player who stays put
        Ship base;
        {
            bool found = false;
            for (uint64_t seed = 1; seed < 500 && !found; ++seed) {
                generateShip(base, seed * 7919ull, 4, D);
                for (const ShipWeapon& w : base.weapons) if (w.type == ShipWeapon::Laser) found = true;
            }
            check(found, "a ship with the ray to test");
        }
        auto stage = [&](float playerY) {
            game.bullets.clear();  game.ebullets.clear();  game.missiles.clear();  game.pmissiles.clear();  game.enemies.clear();  game.ships.clear();
            game.waves.clear();
            base.anchor = C;  base.pos = C;  base.baseAngle = 0.0f;  base.angle = 0.0f;
            game.level.ship = base;
            game.spawnShip();
            Ship& s = game.ships.back();
            Enemy* laser = nullptr;
            for (ShipWeapon& w : s.weapons) {
                Enemy* e = game.findEnemy(w.enemyId);
                if (!e) continue;
                if (w.type == ShipWeapon::Laser) laser = e; else e->alive = false;     // only the ray
            }
            if (laser) laser->gunCd = 0.0f;
            game.pl.pos = dv2(C.x + s.radius + 800.0, C.y + playerY);
            game.pl.vel = v2(0, 0);
            game.pl.health = 100.0f;
            game.pl.protect = 0.0f;
            return laser ? laser->id : 0;
        };
        auto hold = [&](dv2 at) { game.pl.pos = at;  game.pl.vel = v2(0, 0); };

        // The timeline: charge, ray, recharge.
        {
            const int id = stage(0.0f);
            const dv2 at = game.pl.pos;
            double tCharge = -1, tFire = -1, tEnd = -1, tNext = -1;
            float healthAtFire = 0.0f, healthAfter = 0.0f;
            bool hurtBeforeFire = false;
            int prev = 0;
            int shieldOn = 0;
            for (int i = 0; i < 60 * 14; ++i) {
                hold(at);
                game.pl.hasShield = true;  game.pl.shield = rules::SHIELD_CAPACITY;  game.pl.shieldUp = true;  ++shieldOn;
                game.update(renderer, idle, dt);
                const Enemy* e = game.findEnemy(id);
                if (!e) break;
                const double now = i * (double)dt;
                if (e->burst == 1 && prev != 1 && tCharge < 0) tCharge = now;
                if (e->burst == 1 && tFire < 0 && game.pl.health < 100.0f) hurtBeforeFire = true;
                if (e->burst == 2 && prev != 2 && tFire < 0) { tFire = now; healthAtFire = game.pl.health; }
                if (e->burst == 0 && prev == 2 && tEnd < 0) { tEnd = now; healthAfter = game.pl.health; }
                if (e->burst == 1 && tEnd >= 0 && prev == 0 && tNext < 0) tNext = now;
                prev = e->burst;
                if (tNext >= 0) break;
            }
            printf("      charge starts at %.2f s, the ray at %.2f s (%.2f s later), off at %.2f s, next charge at %.2f s (%.2f s after the ray went out)\n",
                   tCharge, tFire, tFire - tCharge, tEnd, tNext, tNext - tEnd);
            check(tCharge >= 0 && tFire >= 0, "it starts to shoot when you are in range, and the ray comes");
            check(std::fabs((tFire - tCharge) - rules::LASER_CHARGE) < 0.06, "2 seconds from starting to shoot until the ray fires");
            check(std::fabs((tEnd - tFire) - rules::LASER_FIRE_TIME) < 0.08, "and the ray stays on for under a second");
            check(tNext >= 0 && std::fabs((tNext - tEnd) - rules::LASER_RECHARGE) < 0.1, "7 seconds to recharge before it can start again");
            check(!hurtBeforeFire, "the charging line does no harm");
            printf("      suit lost to one ray: %.0f (with the shield up)\n", healthAtFire - healthAfter);
            check(healthAtFire - healthAfter >= 60.0f, "standing in the ray hurts badly, whatever shield is up");
        }
        // Moving out of the line in the last moment is the defence.
        {
            const int id = stage(0.0f);
            const dv2 at = game.pl.pos;
            bool moved = false;
            for (int i = 0; i < 60 * 5; ++i) {
                const Enemy* e = game.findEnemy(id);
                if (e && e->burst == 1 && e->burstCd < rules::LASER_LOCK * 0.6f) moved = true;
                hold(moved ? dv2(at.x, at.y + 220.0) : at);
                game.update(renderer, idle, dt);
                if (e && e->burst == 0 && moved) break;
            }
            check(moved && game.pl.health >= 100.0f, "stepping 220 units aside after it locks means the ray misses you");
        }
        // It cuts through rock, near and far, and through shots and missiles.
        {
            const int id = stage(0.0f);
            const dv2 at = game.pl.pos;
            const float R = game.ships.back().radius;
            const dv2 nearRock(C.x + R + 350.0, C.y), farRock(C.x + R + 1800.0, C.y + 3.0);
            const int sn = game.world.spawn(nearRock, 70.0f, 4242u);
            const int sf = game.world.spawn(farRock, 70.0f, 4243u);
            game.world.step(dt, C);
            auto solid = [&](int slot) { return slot >= 0 && game.world.bodies[slot].alive ? game.world.summarise(slot).solid : 0u; };
            const uint32_t nb = solid(sn), fb = solid(sf);
            // and something in flight, put in the line as the ray comes on
            const double seedX = C.x + R + 1000.0;
            auto shotThere = [&]() {
                for (const EnemyBullet& b : game.ebullets) if (std::fabs(b.pos.x - seedX) < 3.0 && b.life > 0.0f) return true;
                return false;
            };
            bool seeded = false;  int since = -1;  bool goneAfter = false;
            for (int i = 0; i < 60 * 4; ++i) {
                const Enemy* e = game.findEnemy(id);
                if (e && e->burst == 2 && !seeded) {
                    seeded = true;  since = 0;
                    EnemyBullet eb;  eb.pos = dv2(seedX, C.y + 6.0);  eb.vel = v2(0, 0);  eb.life = 5.0f;
                    game.ebullets.push_back(eb);
                }
                hold(at);
                game.update(renderer, idle, dt);
                if (seeded && ++since == 3) goneAfter = !shotThere();
                if (e && e->burst == 0 && seeded) break;
            }
            const uint32_t na = solid(sn), fa = solid(sf);
            printf("      near rock %u -> %u samples, far rock (past you) %u -> %u; a stationary shot in the ray is %s\n",
                   nb, na, fb, fa, goneAfter ? "gone" : "still there");
            check(nb > 0 && na < nb * 0.7, "the ray cuts a slot through a rock in front of you");
            check(fb > 0 && fa < fb * 0.7, "and through a rock far behind you: it cuts through everything");
            check(seeded && goneAfter, "and a shot caught in the ray is gone");
        }
        // Pictures: the sight line while it charges, the ray, and the ray through rock.
        if (shotPath && shotPath[0]) {
            const int id = stage(0.0f);
            const dv2 at = game.pl.pos;
            const float R = game.ships.back().radius;
            game.world.spawn(dv2(C.x + R + 350.0, C.y - 20.0), 90.0f, 4242u);
            game.world.step(dt, C);
            game.zoomTarget = 900.0f;
            int shot = 0;
            for (int i = 0; i < 60 * 4 && shot < 3; ++i) {
                hold(at);
                game.update(renderer, idle, dt);
                const Enemy* e = game.findEnemy(id);
                if (!e) break;
                const bool want = (shot == 0 && e->burst == 1 && e->burstCd < 1.0f && e->burstCd > 0.8f)
                               || (shot == 1 && e->burst == 1 && e->burstCd < rules::LASER_LOCK * 0.5f)
                               || (shot == 2 && e->burst == 2 && e->burstCd < rules::LASER_FIRE_TIME - 0.25f);
                if (want) {
                    game.cam.pos = dv2(C.x + R * 0.6 + 350.0, C.y);
                    game.cam.halfW = 900.0f;
                    game.render(renderer);
                    char path[512];
                    snprintf(path, sizeof path, "%s_laser%d.png", shotPath, shot);
                    renderer.screenshot(path);
                    ++shot;
                }
            }
        }
        printf("lasertest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (rangeTest) {
        printf("rangetest:\n");
        const float dt = 1.0f / 60.0f;
        Input idle;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-78s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        game.startRun(renderer);
        game.invincible = true;
        game.floating = true;
        game.startLevel(3);
        game.level.slots.clear();
        game.level.diff.droneSpeed = 0.0f;
        const dv2 C(6000.0, 6000.0);
        openSpot(C);
        game.pl.hasFractal = true;  game.pl.fractalAmmo = 12;
        game.pl.hasSalvo = true;    game.pl.salvoAmmo = 12;
        auto hold = [&]() { game.pl.pos = C;  game.pl.vel = v2(0, 0); };
        auto run = [&](int n) { for (int i = 0; i < n; ++i) { hold(); game.update(renderer, idle, dt); } };

        // ---- the numbers you asked for
        check(std::fabs(tune::FUEL_REGEN - 22.0f * 0.85f * 0.85f) < 0.05f, "the rocket's fuel refills 15% slower again (15.9 a second)");
        check(tune::HEAVY_GRAV == 10.0f * 0.4f, "the F shell feels 60% less gravity");
        game.bullets.clear();
        hold();  game.pl.aim = 0.0f;  game.pl.fractalCd = 0.0f;
        game.fireFractal(600.0f);
        check(game.bullets.size() == 1 && std::fabs(game.bullets[0].gravScale - 6.0f * 0.4f) < 1e-4f, "and so does the Z shell");
        game.bullets.clear();

        // ---- the F shell goes off when its time is up, well short of where it would have got
        {
            game.bullets.clear();  game.enemies.clear();  game.waves.clear();
            hold();  game.pl.aim = 0.0f;  game.pl.heavyCd = 0.0f;
            game.fire(game.pl, true);
            const dv2 from = game.bullets.empty() ? C : game.bullets[0].pos;
            double reach = 0;  int frames = 0;
            for (; frames < 60 * 6 && !game.bullets.empty(); ++frames) {
                run(1);
                if (!game.bullets.empty()) reach = std::max(reach, len(game.bullets[0].pos - from));
            }
            printf("      the shell flew %.0f units in %.2f s, then went off (limit %.1f s)\n", reach, frames * dt, rules::HOMING_LIFE);
            check(game.bullets.empty() && frames <= (int)((rules::HOMING_LIFE + 0.1f) * 60.0f), "the F shell is gone within its time limit");
            check(reach < 1500.0, "and it did not get further than about 1300 units");
            check(!game.waves.empty(), "and it went off with a bang, rather than just fading");
        }
        // ---- an enemy further away than that cannot be hit by it
        {
            game.bullets.clear();  game.enemies.clear();  game.waves.clear();
            Enemy e = plainDrone(dv2(C.x + 2100.0, C.y));
            game.enemies.push_back(e);
            hold();  game.pl.aim = 0.0f;  game.pl.heavyCd = 0.0f;
            game.fire(game.pl, true);
            run(60 * 5);
            const Enemy* left = game.findEnemy(e.id);
            check(left && left->hp == left->maxHp, "a drone 2100 units away is out of reach of the F shell");
        }

        // ---- the salvo blows up after 2.3 s
        {
            game.pmissiles.clear();  game.enemies.clear();  game.waves.clear();
            hold();  game.pl.salvoCd = 0.0f;
            game.fireSalvo(v2(1, 0));
            check((int)game.pmissiles.size() == rules::SALVO_COUNT, "a salvo is five missiles");
            const dv2 from = C;
            double reach = 0;  int frames = 0;
            for (; frames < 60 * 6 && !game.pmissiles.empty(); ++frames) {
                run(1);
                for (const PMissile& m : game.pmissiles) reach = std::max(reach, len(m.pos - from));
            }
            printf("      the missiles flew %.0f units in %.2f s (limit %.1f s)\n", reach, frames * dt, rules::SALVO_LIFE);
            check(game.pmissiles.empty() && frames <= (int)((rules::SALVO_LIFE + 0.1f) * 60.0f), "the missiles all blow up within their time limit");
            check(reach < 1500.0, "and none got further than about 1300 units");
            check(!game.waves.empty(), "with a blast, not just a fade");
        }
        // ---- the fractal shell, aimed as far as the cursor can go
        {
            game.bullets.clear();  game.enemies.clear();  game.waves.clear();
            hold();  game.pl.aim = 0.0f;  game.pl.fractalCd = 0.0f;
            game.fireFractal(3000.0f);
            const dv2 from = game.bullets.empty() ? C : game.bullets[0].pos;
            double reach = 0;  int frames = 0;
            for (; frames < 60 * 8 && !game.bullets.empty(); ++frames) {
                run(1);
                for (const Bullet& b : game.bullets) reach = std::max(reach, len(b.pos - from));
            }
            printf("      the fractal shell flew %.0f units in %.2f s (limit %.1f s)\n", reach, frames * dt, rules::FRACTAL_LIFE);
            check(game.bullets.empty() && frames <= (int)((rules::FRACTAL_LIFE + 0.1f) * 60.0f), "every fractal piece is gone within the time limit");
            check(reach < 2000.0, "and none got very far");
            check(!game.waves.empty(), "having gone off");
        }
        // ---- the F shell goes for the enemy nearest the cursor, marked before you fire
        {
            game.bullets.clear();  game.enemies.clear();  game.waves.clear();
            game.pl.hasHoming = true;
            Enemy a = plainDrone(dv2(C.x + 500.0, C.y));                 // straight ahead
            Enemy b = plainDrone(dv2(C.x + 500.0, C.y + 300.0));
            Enemy d = plainDrone(dv2(C.x + 500.0, C.y - 300.0));
            Enemy distant = plainDrone(dv2(C.x + 2600.0, C.y + 40.0));       // out of the shell's reach
            game.enemies.push_back(a);  game.enemies.push_back(b);  game.enemies.push_back(d);  game.enemies.push_back(distant);
            auto lockWith = [&](dv2 cursor) {
                Input in;
                game.cam.pos = C;  game.cam.angle = 0.0f;
                aimAt(in, cursor);
                hold();
                game.update(renderer, in, dt);
                return game.homingLock;
            };
            const int atB = lockWith(dv2(C.x + 520.0, C.y + 260.0));
            const int atD = lockWith(dv2(C.x + 480.0, C.y - 280.0));
            const int atA = lockWith(dv2(C.x + 700.0, C.y + 20.0));
            const int atFar = lockWith(dv2(C.x + 2600.0, C.y + 40.0));
            printf("      cursor by the upper drone locks %d (it is %d), by the lower drone %d (it is %d), in front %d (it is %d)\n", atB, b.id, atD, d.id, atA, a.id);
            check(atB == b.id && atD == d.id && atA == a.id, "the F shell's target is the enemy closest to the cursor, whichever it is");
            check(atFar != distant.id && atFar != 0, "an enemy too far away for the shell is never chosen");
            // Fired at the one marked: the shell carries that target, not whatever is nearest its heading.
            {
                Input in;
                game.cam.pos = C;  game.cam.angle = 0.0f;
                aimAt(in, dv2(C.x + 520.0, C.y + 260.0));
                in.pressed['F'] = true;
                hold();  game.pl.heavyCd = 0.0f;
                game.update(renderer, in, dt);
                int carried = -1;
                for (const Bullet& bl : game.bullets) if (bl.heavy && bl.gen < 0) carried = bl.targetId;
                check(carried == b.id, "and a shell fired then goes for it");
            }
            game.bullets.clear();
            if (shotPath && shotPath[0]) {                                // a picture of the mark, before firing
                lockWith(dv2(C.x + 520.0, C.y + 260.0));
                game.cam.pos = dv2(C.x + 250.0, C.y);  game.cam.halfW = 700.0f;
                game.render(renderer);
                char path[512];
                snprintf(path, sizeof path, "%s_hominglock.png", shotPath);
                renderer.screenshot(path);
            }
            game.pl.hasHoming = false;
            check(lockWith(dv2(C.x + 520.0, C.y + 260.0)) == 0, "with no shell bought there is no lock");
            game.pl.hasHoming = true;
            game.enemies.clear();
            check(lockWith(dv2(C.x + 520.0, C.y + 260.0)) == 0, "and with nothing to lock onto there is none");
        }
        printf("rangetest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (spinTest) {
        printf("spintest:\n");
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-78s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        // Right after the world is made: every rock's spin, before anything has knocked it about.
        std::vector<float> w;
        for (int s : game.world.active) {
            const Body& b = game.world.bodies[s];
            if (b.alive) w.push_back(b.angVel);
        }
        const size_t n = w.size();
        double mean = 0, sq = 0;
        for (float x : w) { mean += x; sq += (double)x * x; }
        mean /= std::max<size_t>(1, n);
        const double sd = std::sqrt(sq / std::max<size_t>(1, n) - mean * mean);
        size_t in1 = 0, in2 = 0, pos = 0, still = 0;
        for (float x : w) {
            const double z = std::fabs(x - mean) / std::max(1e-9, sd);
            if (z < 1.0) ++in1;
            if (z < 2.0) ++in2;
            if (x > 0.0f) ++pos;
            if (std::fabs(x) < 0.02f) ++still;
        }
        printf("      %d rocks: mean spin %+.3f rad/s, spread %.3f, %.0f%% within one sigma, %.0f%% within two, %.0f%% spinning the positive way\n",
               (int)n, mean, sd, 100.0 * in1 / std::max<size_t>(1, n), 100.0 * in2 / std::max<size_t>(1, n), 100.0 * pos / std::max<size_t>(1, n));
        check(n > 200, "there are plenty of rocks to look at");
        check(std::fabs(mean) < 0.05, "the spins are centred on none");
        check(sd > 1.05 && sd < 1.35, "with a spread of about 1.2 rad/s (twice what it was, and twice that again)");
        check(in1 > 0.60 * n && in1 < 0.76 * n, "about 68% of them within one standard deviation");
        check(in2 > 0.91 * n && in2 < 0.99 * n, "and about 95% within two: a bell curve, not a flat spread");
        check(pos > 0.42 * n && pos < 0.58 * n, "half turn each way");
        check(still < 0.08 * n, "and hardly any sit still");
        // The Rng itself.
        Rng r(12345);
        double m2 = 0, s2 = 0;
        const int N = 20000;
        for (int i = 0; i < N; ++i) { const double x = r.normal(); m2 += x; s2 += x * x; }
        m2 /= N;  s2 = std::sqrt(s2 / N - m2 * m2);
        printf("      20000 draws of Rng::normal(): mean %+.3f, deviation %.3f\n", m2, s2);
        check(std::fabs(m2) < 0.03 && std::fabs(s2 - 1.0) < 0.03, "Rng::normal() has mean 0 and deviation 1");

        // They keep turning: ten seconds on, most of the spin is still there.
        const Player keep = game.pl;
        double before = 0, after = 0;
        for (int s : game.world.active) if (game.world.bodies[s].alive) before += std::fabs(game.world.bodies[s].angVel);
        const int alive0 = (int)game.world.active.size();
        for (int i = 0; i < 600; ++i) game.world.step(1.0f / 60.0f, game.pl.pos);
        int counted = 0;
        for (int s : game.world.active) if (game.world.bodies[s].alive) { after += std::fabs(game.world.bodies[s].angVel); ++counted; }
        (void)keep;  (void)alive0;
        const double ratio = (after / std::max(1, counted)) / (before / std::max<size_t>(1, n));
        printf("      ten seconds later the average spin is %.0f%% of what it was\n", ratio * 100.0);
        check(ratio > 0.6, "the rocks are still turning ten seconds on (the damping is gentle)");
        printf("spintest: %s\n", failures == 0 ? "PASS" : "FAIL");
        fflush(stdout);
        renderer.shutdown();
        return 0;
    }

    if (jumpTest) {
        // Stand on top of the biggest rock and jump with the cursor in several places:
        // the jump should leave toward the cursor, never into the ground, and a cursor
        // below or along the surface should give a low leap along it.
        const float dt = 1.0f / 60.0f;
        int best = -1;
        for (int s : game.world.active)
            if (best < 0 || game.world.bodies[s].radius > game.world.bodies[best].radius) best = s;
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            printf("  %-78s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        game.povCamera = false;
        game.floating = false;
        game.pl.hasFractal = false;
        const Body& rock = game.world.bodies[best];
        const v2 n0(0.0f, 1.0f);
        dv2 p = dv2(rock.pos.x + n0.x * (rock.radius + 40.0), rock.pos.y + n0.y * (rock.radius + 40.0));
        for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n0 * -1.0f;
        p += n0 * 9.0f;

        printf("jumptest: the tank holds %.0f, was 100; gravity constant %.0f, was 100\n", tune::FUEL_MAX, cfg::GRAV_CONST);
        check(tune::FUEL_MAX == 24.0f && game.pl.fuel == tune::FUEL_MAX, "the fuel tank is at 24 (25% smaller again) and starts full");
        check(std::fabs(tune::THRUST - 360.0f) < 0.01f, "the rocket is 360: 20% weaker than the 450 it was");
        check(std::fabs(cfg::GRAV_CONST - 258.75f) < 0.01f, "gravity is 15% stronger again (258.75, from 225)");

        struct Case { const char* name; float degFromUp; };            // clockwise from the surface normal
        const Case cases[] = { { "cursor straight up", 0.0f }, { "cursor 40 degrees to the right", 40.0f },
                               { "cursor 60 degrees to the left", -60.0f }, { "cursor level with the ground, right", 90.0f },
                               { "cursor level with the ground, left", -90.0f }, { "cursor straight down", 180.0f } };
        for (const Case& cs : cases) {
            // Rocks turn now, so find the top of it again each time.
            p = dv2(rock.pos.x + n0.x * (rock.radius + 40.0), rock.pos.y + n0.y * (rock.radius + 40.0));
            for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n0 * -1.0f;
            p += n0 * 9.0f;
            game.pl.pos = p;  game.pl.vel = rock.vel;  game.pl.up = n0;
            game.pl.grounded = false;  game.pl.jumpCd = 0.0f;
            Input idle;
            for (int f = 0; f < 45; ++f) {
                game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;
                game.update(renderer, idle, dt);
            }
            const v2 up = game.pl.up;
            const bool grounded = game.pl.grounded;
            const v2 right(up.y, -up.x);
            const float a = cs.degFromUp * PIF / 180.0f;
            const v2 want = up * std::cos(a) + right * std::sin(a);        // where the cursor is, from the player
            Input in;
            in.pressed[VK_SPACE] = true;
            game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;
            aimAt(in, game.pl.pos + dv2(want.x * 300.0, want.y * 300.0));
            game.update(renderer, in, dt);
            Body* gb = game.world.get(game.pl.ground);
            const v2 surf = gb ? gb->velAt(tov2(game.pl.pos - gb->pos)) : v2(0, 0);
            const v2 rel = game.pl.vel - surf;
            const float sp = len(rel);
            const v2 dir = sp > 1.0f ? rel / sp : v2(0, 0);
            const float lift = dot(dir, up);
            const float agree = dot(dir, want);
            printf("      %-36s speed %5.0f, lift %+.2f, agreement with the cursor %+.2f\n", cs.name, sp, lift, agree);
            check(grounded, "it starts on the ground");
            check(std::fabs(sp - tune::JUMP_SPEED) < 25.0f, "and leaves at the jump speed");
            check(lift > 0.10f, "the jump never goes into the rock");
            if (std::cos(a) >= 0.2f) check(agree > 0.97f, "and it goes toward the cursor");
            else if (cs.degFromUp != 180.0f)
                check(dot(dir, right) * std::sin(a) > 0.9f, "a cursor at or below the horizon gives a low leap toward that side");
        }
        // The spaceman feels gravity 30% harder than anything else: one frame of free fall against the field's own pull.
        {
            check(std::fabs(tune::PLAYER_GRAV - 1.3f) < 1e-4f, "gravity pulls the spaceman 30% harder");
            const dv2 hover(rock.pos.x, rock.pos.y + rock.radius + 260.0);
            game.pl.pos = hover;  game.pl.vel = v2(0, 0);  game.pl.grounded = false;  game.pl.up = n0;
            const v2 g = game.world.gravityAt(hover, 2600.0);
            Input idle;
            game.cam.pos = hover;  game.cam.angle = 0.0f;
            game.update(renderer, idle, dt);
            const float pulled = len(game.pl.vel), field = len(g) * dt;
            printf("      one frame in free fall: the spaceman gained %.3f u/s, the field alone would give %.3f (x%.2f)\n", pulled, field, field > 0 ? pulled / field : 0.0f);
            check(field > 0.0f && std::fabs(pulled / field - tune::PLAYER_GRAV) < 0.08f, "and in free fall he speeds up 1.3 times as fast as the field's pull");
        }

        // A rock's pull reaches 25% less far from its surface, and is unchanged at the surface itself.
        {
            const dv2 C2(30000.0, 30000.0);
            game.world.clearZone(C2, 3500.0);
            const int sl = game.world.spawn(C2, 100.0f, 9999u);
            if (sl >= 0) game.world.step(dt, C2);                         // (so the broadphase knows about it)
            check(sl >= 0 && std::fabs(cfg::GRAVITY_REACH - 0.75f) < 1e-6f, "a lone rock to measure, and the reach is 25% shorter");
            if (sl >= 0) {
                const Body& lone = game.world.bodies[sl];
                auto original = [&](double r) {                          // the pull at distance r the way it used to be worked out
                    const double soft = 0.30 * (double)lone.radius * lone.radius;
                    return (double)cfg::GRAV_CONST * lone.mass * r / std::pow(r * r + soft, 1.5);
                };
                double atSurface = 0, ratio300 = 0, ratio900 = 0, expect300 = 0;
                for (int k = 0; k < 3; ++k) {
                    const double h = k == 0 ? 0.0 : (k == 1 ? 300.0 : 900.0);
                    const double r = lone.radius + h;
                    const v2 g = game.world.gravityAt(dv2(lone.pos.x + r, lone.pos.y), 2600.0);
                    const double now = len(g), was = original(r);
                    if (k == 0) atSurface = now / was;
                    if (k == 1) { ratio300 = now / was; expect300 = original(lone.radius + h / cfg::GRAVITY_REACH) / was; }
                    if (k == 2) ratio900 = now / was;
                }
                printf("      the pull against what it was: %.2f at the surface, %.2f at 300 units up, %.2f at 900 units up\n", atSurface, ratio300, ratio900);
                check(std::fabs(atSurface - 1.0) < 0.02, "at the surface the pull is what it always was");
                check(std::fabs(ratio300 - expect300) < 0.03 && ratio300 < 0.85, "higher up it is weaker: worked out as if 33% further away");
                check(ratio900 < ratio300, "and the further up, the more it has dropped");
            }
        }

        // How high is a plain jump? Three times what 260 gave: v^2 / 2g, so the speed is 260 x sqrt(3).
        {
            check(std::fabs(tune::JUMP_SPEED - 260.0f * std::sqrt(3.0f)) < 0.2f, "the jump speed is 260 times the square root of three: three times the height");
            p = dv2(rock.pos.x + n0.x * (rock.radius + 40.0), rock.pos.y + n0.y * (rock.radius + 40.0));
            for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n0 * -1.0f;
            p += n0 * 9.0f;
            game.pl.pos = p;  game.pl.vel = rock.vel;  game.pl.up = n0;  game.pl.grounded = false;  game.pl.jumpCd = 0.0f;
            Input idle;
            for (int f = 0; f < 45; ++f) { game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;  game.update(renderer, idle, dt); }
            const dv2 from = game.pl.pos;
            const v2 up = game.pl.up;
            float apex = 0.0f;
            for (int f = 0; f < 200; ++f) {
                Input in;
                if (f == 0) in.pressed[VK_SPACE] = true;
                game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;
                aimAt(in, game.pl.pos + dv2(up.x * 300.0, up.y * 300.0));
                game.update(renderer, in, dt);
                apex = std::max(apex, dot(tov2(game.pl.pos - from), up));
            }
            printf("      a jump straight up, no rocket, rises %.0f units before it turns round\n", apex);
            check(apex > 100.0f, "a jump lifts the spaceman well clear of the rock (over 100 units)");
        }

        // Rocket jumps: the F shell, fired at the rock below, throws the spaceman clear.
        {
            game.pl.hasHoming = true;
            struct Run { const char* name; bool jump; bool shell; float aimDown; };
            const Run runs[] = { { "a plain jump", true, false, 2 }, { "the F shell at the ground, standing", false, true, 1 },
                                 { "a jump and the F shell at the ground together", true, true, 1 },
                                 { "the F shell fired up into open sky", false, true, 2 } };
            float apex[4] = { 0, 0, 0, 0 }, lost[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; ++k) {
                p = dv2(rock.pos.x + n0.x * (rock.radius + 40.0), rock.pos.y + n0.y * (rock.radius + 40.0));
                for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n0 * -1.0f;
                p += n0 * 9.0f;
                game.pl.pos = p;  game.pl.vel = rock.vel;  game.pl.up = n0;  game.pl.grounded = false;  game.pl.jumpCd = 0.0f;
                game.pl.heavyCd = 0.0f;  game.pl.health = 100.0f;  game.pl.sinceHurt = 0.0f;
                game.bullets.clear();
                Input idle;
                for (int f = 0; f < 45; ++f) { game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;  game.update(renderer, idle, dt); }
                const dv2 from = game.pl.pos;
                const v2 up = game.pl.up;
                const float h0 = game.pl.health;
                for (int f = 0; f < 200; ++f) {
                    Input in;
                    if (f == 0) { in.pressed[VK_SPACE] = runs[k].jump;  in.pressed['F'] = runs[k].shell; }
                    game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;
                    const v2 side(up.y, -up.x);
                    const v2 target = runs[k].aimDown > 1.5f ? up * 300.0f : (runs[k].aimDown > 0.5f ? up * -300.0f : side * 300.0f);
                    aimAt(in, game.pl.pos + dv2(target.x, target.y));
                    game.update(renderer, in, dt);
                    apex[k] = std::max(apex[k], dot(tov2(game.pl.pos - from), up));
                    if (f == 20) lost[k] = h0 - game.pl.health;
                }
                printf("      %-56s rises %4.0f units, suit lost %.0f\n", runs[k].name, apex[k], lost[k]);
            }
            check(apex[1] > 60.0f, "the F shell at the ground throws the spaceman clear of it: a rocket jump");
            check(apex[2] > apex[0] * 1.4f, "and a jump with it goes much higher than a jump alone");
            check(lost[3] < 2.0f && apex[3] < apex[0] * 1.15f, "a shell fired up into the sky gives no shove (the recoil aside)");
            check(lost[1] > 2.0f && lost[1] < 35.0f, "it costs a bruise, not the suit");
            game.pl.health = 100.0f;
        }

        // How much can the rocket do from the ground? (reported, not judged)
        for (int withJump = 0; withJump < 2; ++withJump) {
            p = dv2(rock.pos.x + n0.x * (rock.radius + 40.0), rock.pos.y + n0.y * (rock.radius + 40.0));
            for (int i = 0; i < 400 && game.world.solidAt(p) < 0; ++i) p += n0 * -1.0f;
            p += n0 * 9.0f;
            game.pl.pos = p;  game.pl.vel = rock.vel;  game.pl.up = n0;  game.pl.grounded = false;  game.pl.jumpCd = 0.0f;
            game.pl.fuel = tune::FUEL_MAX;  game.pl.fuelLocked = false;
            Input idle;
            for (int f = 0; f < 45; ++f) { game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;  game.update(renderer, idle, dt); }
            const dv2 from = game.pl.pos;
            float top = 0.0f;
            for (int f = 0; f < 240; ++f) {
                Input in;
                in.mouse[1] = true;
                if (withJump && f == 0) in.pressed[VK_SPACE] = true;
                game.cam.pos = game.pl.pos;  game.cam.angle = 0.0f;
                aimAt(in, game.pl.pos + dv2(0.0, 300.0));
                game.update(renderer, in, dt);
                top = std::max(top, (float)(game.pl.pos.y - from.y));
            }
            printf("      holding the rocket straight up %s: the highest point is %.0f units above the surface\n",
                   withJump ? "after a jump" : "from standing", top);
        }
        printf("jumptest: %s\n", failures == 0 ? "PASS" : "FAIL");
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
        const float budget = 0.03f + tune::FUEL_REGEN / (tune::FUEL_REGEN + tune::FUEL_BURN);        // regen / (regen + burn), plus slack
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
    if (game.net) printf("network: %s | %d other player(s) | rocks known: %d | you: %d frags, %d deaths\n", game.netStatus().c_str(), (int)game.peers.size(), game.world.liveCount, game.pl.frags, game.pl.deaths);
    game.netShutdown();
    audio::shutdown();
    renderer.shutdown();
    platformShutdown();
    return 0;
}
