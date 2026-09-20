@echo off
REM Builds Asteroid Drifter. Tries MSVC first, then MinGW-w64 from MSYS2.
setlocal
set SRC=src\main.cpp src\game.cpp src\level.cpp src\level_hud.cpp src\enemies.cpp src\enemies_fx.cpp src\ships.cpp src\audio.cpp src\sounds.cpp src\audio_game.cpp src\audio_check.cpp src\versus.cpp src\net.cpp src\net_world.cpp src\net_game.cpp src\weapons.cpp src\shop.cpp src\world.cpp src\render.cpp src\field.cpp src\gl.cpp
if not exist build mkdir build

where cl >nul 2>nul
if %ERRORLEVEL%==0 goto msvc

for %%D in ("C:\msys64\mingw64\bin" "C:\msys64\ucrt64\bin" "C:\mingw64\bin") do (
    if exist %%~D\g++.exe (
        set "PATH=%%~D;%PATH%"
        goto gcc
    )
)
echo Could not find cl.exe or g++.exe.
echo Open a "Developer Command Prompt for VS", or install MSYS2 mingw-w64-x86_64-gcc.
exit /b 1

:msvc
echo Building with MSVC...
cl /nologo /std:c++17 /O2 /EHsc /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /W3 %SRC% /Fe:build\asteroid.exe /Fo:build\ ^
   /link opengl32.lib gdi32.lib user32.lib winmm.lib ws2_32.lib
goto done

:gcc
echo Building with MinGW-w64...
g++ -std=c++17 -O3 -ffast-math -fno-math-errno -DNDEBUG -Wall -Wno-unused-variable ^
    %SRC% -o build\asteroid.exe -lopengl32 -lgdi32 -lwinmm -lws2_32 -static

:done
if %ERRORLEVEL%==0 echo Built build\asteroid.exe
exit /b %ERRORLEVEL%
