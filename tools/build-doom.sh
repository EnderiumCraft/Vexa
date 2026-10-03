#!/bin/sh
# Builds Chocolate Doom for Vexa with the SDK (SDL 2 and SDL_mixer):
#
#   tools/build-doom.sh SOURCE_DIR PATCH WORK_DIR SDK_DIR OUTPUT
#
# Chocolate Doom's own sources and CMake build, with Vexa's patch
# (ports/chocolate-doom/vexa.patch: where its files are in an app bundle, a
# window by default) and without SDL_net (no network games yet). OUTPUT is
# the chocolate-doom program, stripped.
set -e
SRC=$(realpath "$1")
PATCH=$(realpath "$2")
WORK=$3
SDK=$(realpath "$4")
OUT=$5
JOBS=$(nproc 2>/dev/null || echo 4)

rm -rf "$WORK"
mkdir -p "$WORK"
cp -R "$SRC" "$WORK/src"
rm -rf "$WORK/src/.git"
patch -d "$WORK/src" -p1 --batch --forward --quiet < "$PATCH"
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_TOOLCHAIN_FILE="$SDK/cmake/vexa.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_SDL2_NET=OFF -DENABLE_SDL2_MIXER=ON > "$WORK/cmake.log"
cmake --build "$WORK/build" --target chocolate-doom -j "$JOBS" > "$WORK/build.log" 2>&1
mkdir -p "$(dirname "$OUT")"
strip -o "$OUT" "$WORK/build/src/chocolate-doom"
echo "Chocolate Doom: $(du -k "$OUT" | cut -f1) KiB, in $OUT"
