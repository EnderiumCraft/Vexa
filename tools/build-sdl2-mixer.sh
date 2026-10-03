#!/bin/sh
# Builds SDL_mixer 2 for Vexa with the SDK (and its SDL), to be added to it:
#
#   tools/build-sdl2-mixer.sh SDL2_mixer-x.y.z.tar.gz WORK_DIR SDK_DIR SDL_PREFIX PREFIX
#
# With the formats that need nothing else: WAV, AIFF and VOC, and Ogg Vorbis,
# MP3 and FLAC through the single-file decoders SDL_mixer carries (stb_vorbis,
# minimp3, dr_flac). It becomes PREFIX/lib/libSDL2_mixer.a, with SDL_mixer.h
# next to SDL's headers, and a pkg-config file and a CMake package
# (SDL2_mixer::SDL2_mixer) to find it.
set -e
TARBALL=$(realpath "$1")
WORK=$2
SDK=$(realpath "$3")
SDL=$(realpath "$4")
mkdir -p "$5"
PREFIX=$(realpath "$5")
CC="$SDK/bin/vexa-cc"
JOBS=$(nproc 2>/dev/null || echo 4)

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "$TARBALL" -C "$WORK"
SRC=$(echo "$WORK"/SDL2_mixer-*)
VERSION=${SRC##*/SDL2_mixer-}

cd "$SRC"
mkdir -p obj
printf '%s\n' src/*.c src/codecs/*.c | xargs -P "$JOBS" -I{} sh -c '
    out=obj/$(echo "{}" | tr / _).o
    "$0" -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare -Werror=implicit-function-declaration \
        -Iinclude -Isrc -Isrc/codecs -I"$1/include/SDL2" -D_REENTRANT \
        -DMUSIC_WAV -DMUSIC_OGG -DOGG_USE_STB -DMUSIC_MP3_MINIMP3 -DMUSIC_FLAC_DRFLAC \
        -c "{}" -o "$out"' "$CC" "$SDL"
rm -f libSDL2_mixer.a
ar rcs libSDL2_mixer.a obj/*.o

rm -rf "$PREFIX"
mkdir -p "$PREFIX/include/SDL2" "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake/SDL2_mixer" \
    "$PREFIX/licenses"
cp include/SDL_mixer.h "$PREFIX/include/SDL2/"
cp libSDL2_mixer.a "$PREFIX/lib/"
cp LICENSE.txt "$PREFIX/licenses/SDL2_mixer.txt"
cat > "$PREFIX/lib/pkgconfig/SDL2_mixer.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SDL2_mixer
Description: Sound mixing and music for SDL (for Vexa)
Version: $VERSION
Requires: sdl2
Libs: -L\${libdir} -lSDL2_mixer
Cflags: -I\${includedir}/SDL2
PC
cat > "$PREFIX/lib/cmake/SDL2_mixer/SDL2_mixerConfig.cmake" <<'CMAKE'
# find_package(SDL2_mixer) for Vexa's SDK: SDL2_mixer::SDL2_mixer (static).
get_filename_component(_vexa_sdk "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_package(SDL2 REQUIRED CONFIG)
if(NOT TARGET SDL2_mixer::SDL2_mixer)
  add_library(SDL2_mixer::SDL2_mixer STATIC IMPORTED)
  set_target_properties(SDL2_mixer::SDL2_mixer PROPERTIES
    IMPORTED_LOCATION "${_vexa_sdk}/lib/libSDL2_mixer.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_vexa_sdk}/include/SDL2"
    INTERFACE_LINK_LIBRARIES SDL2::SDL2)
  add_library(SDL2_mixer::SDL2_mixer-static ALIAS SDL2_mixer::SDL2_mixer)
endif()
set(SDL2_mixer_FOUND TRUE)
CMAKE
sed "s/@VERSION@/$VERSION/" > "$PREFIX/lib/cmake/SDL2_mixer/SDL2_mixerConfigVersion.cmake" <<'CMAKE'
set(PACKAGE_VERSION "@VERSION@")
if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
CMAKE
echo "SDL_mixer $VERSION: $(ls obj | wc -l) files, in $PREFIX"
