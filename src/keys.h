// keys.h -- the handful of Windows virtual-key codes the game indexes Input::down with,
// for builds where windows.h is not there to define them. The values are the real ones,
// so the key tables and the SDL mapping in main.cpp agree with the Windows build.
#pragma once
#ifndef _WIN32

#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_F1       0x70
#define VK_F4       0x73
#define VK_F11      0x7A
#define VK_F12      0x7B
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5

#endif  // !_WIN32
