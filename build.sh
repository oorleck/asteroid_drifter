#!/bin/sh
# MinGW-w64 build. Pass "debug" for an unoptimised build with symbols.
set -e
export PATH="/c/msys64/mingw64/bin:$PATH"
CXX=${CXX:-g++}
SRC="src/main.cpp src/game.cpp src/level.cpp src/level_hud.cpp src/enemies.cpp src/enemies_fx.cpp src/ships.cpp src/audio.cpp src/sounds.cpp src/audio_game.cpp src/audio_check.cpp src/net.cpp src/net_world.cpp src/weapons.cpp src/shop.cpp src/world.cpp src/render.cpp src/field.cpp src/gl.cpp"
OUT=build/asteroid.exe
mkdir -p build
if [ "$1" = "debug" ]; then
  FLAGS="-g -O0 -DDEBUG"
else
  FLAGS="-O3 -ffast-math -fno-math-errno -DNDEBUG"
fi
# Windows keeps the image locked for a moment after the game exits.
taskkill //F //IM asteroid.exe >/dev/null 2>&1 || true
for try in 1 2 3 4 5; do
  if $CXX -std=c++17 $FLAGS -Wall -Wno-unused-variable $SRC -o $OUT \
          -lopengl32 -lgdi32 -lwinmm -lws2_32 -static; then
    echo "built $OUT"
    exit 0
  fi
  sleep 1
done
echo "BUILD FAILED" >&2
exit 1
