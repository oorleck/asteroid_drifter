#!/bin/sh
# Linux build (SDL2 + libGL). Pass "debug" for an unoptimised build with symbols.
# Needs: SDL2 and OpenGL headers.  Fedora: sudo dnf install SDL2-devel mesa-libGL-devel
#                                  Debian/Ubuntu: sudo apt install libsdl2-dev libgl1-mesa-dev
set -e
CXX=${CXX:-g++}
SRC="src/main.cpp src/game.cpp src/level.cpp src/level_hud.cpp src/enemies.cpp src/enemies_fx.cpp src/ships.cpp src/audio.cpp src/sounds.cpp src/audio_game.cpp src/audio_check.cpp src/weapons.cpp src/shop.cpp src/world.cpp src/render.cpp src/field.cpp src/gl.cpp"
OUT=build/asteroid
mkdir -p build

if ! pkg-config --exists sdl2; then
  echo "SDL2 development files not found." >&2
  echo "  Fedora: sudo dnf install SDL2-devel mesa-libGL-devel" >&2
  exit 1
fi

if [ "$1" = "debug" ]; then
  FLAGS="-g -O0 -DDEBUG"
else
  FLAGS="-O3 -ffast-math -fno-math-errno -DNDEBUG"
fi

$CXX -std=c++17 $FLAGS -Wall -Wno-unused-variable $SRC -o $OUT \
     $(pkg-config --cflags sdl2) $(pkg-config --libs sdl2) -lGL -lpthread
echo "built $OUT"
