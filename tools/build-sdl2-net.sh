#!/bin/sh
# Builds SDL_net 2 for Vexa with the SDK (and its SDL), to be added to it:
#
#   tools/build-sdl2-net.sh SDL2_net-x.y.z.tar.gz WORK_DIR SDK_DIR SDL_PREFIX PREFIX
#
# TCP and UDP over libvexa's BSD sockets. It becomes PREFIX/lib/libSDL2_net.a,
# with SDL_net.h next to SDL's headers, and a pkg-config file and a CMake
# package (SDL2_net::SDL2_net) to find it.
set -e
TARBALL=$(realpath "$1")
WORK=$2
SDK=$(realpath "$3")
SDL=$(realpath "$4")
mkdir -p "$5"
PREFIX=$(realpath "$5")
CC="$SDK/bin/vexa-cc"

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "$TARBALL" -C "$WORK"
SRC=$(echo "$WORK"/SDL2_net-*)
VERSION=${SRC##*/SDL2_net-}

cd "$SRC"
for f in SDLnet.c SDLnetTCP.c SDLnetUDP.c SDLnetselect.c; do
    "$CC" -O2 -g -Wall -Werror=implicit-function-declaration -I. -I"$SDL/include/SDL2" \
        -D_REENTRANT -c "$f" -o "$f.o"
done
rm -f libSDL2_net.a
ar rcs libSDL2_net.a SDLnet*.o

rm -rf "$PREFIX"
mkdir -p "$PREFIX/include/SDL2" "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake/SDL2_net" \
    "$PREFIX/licenses"
cp SDL_net.h "$PREFIX/include/SDL2/"
cp libSDL2_net.a "$PREFIX/lib/"
cp LICENSE.txt "$PREFIX/licenses/SDL2_net.txt"
cat > "$PREFIX/lib/pkgconfig/SDL2_net.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SDL2_net
Description: Networking for SDL (for Vexa)
Version: $VERSION
Requires: sdl2
Libs: -L\${libdir} -lSDL2_net
Cflags: -I\${includedir}/SDL2
PC
cat > "$PREFIX/lib/cmake/SDL2_net/SDL2_netConfig.cmake" <<'CMAKE'
# find_package(SDL2_net) for Vexa's SDK: SDL2_net::SDL2_net (static).
get_filename_component(_vexa_sdk "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
find_package(SDL2 REQUIRED CONFIG)
if(NOT TARGET SDL2_net::SDL2_net)
  add_library(SDL2_net::SDL2_net STATIC IMPORTED)
  set_target_properties(SDL2_net::SDL2_net PROPERTIES
    IMPORTED_LOCATION "${_vexa_sdk}/lib/libSDL2_net.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_vexa_sdk}/include/SDL2"
    INTERFACE_LINK_LIBRARIES SDL2::SDL2)
  add_library(SDL2_net::SDL2_net-static ALIAS SDL2_net::SDL2_net)
endif()
set(SDL2_net_FOUND TRUE)
CMAKE
sed "s/@VERSION@/$VERSION/" > "$PREFIX/lib/cmake/SDL2_net/SDL2_netConfigVersion.cmake" <<'CMAKE'
set(PACKAGE_VERSION "@VERSION@")
if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
CMAKE
echo "SDL_net $VERSION: in $PREFIX"
