#!/bin/sh
# Builds SDL 2 for Vexa with the SDK, to be added to the SDK:
#
#   tools/build-sdl2.sh SDL2-x.y.z.tar.gz WORK_DIR SDK_DIR PREFIX
#
# SDL's own sources, configured by sdk/sdl2/SDL_config.h, with Vexa's video
# and audio drivers (sdk/sdl2/video, sdk/sdl2/audio) added to its lists. It
# becomes PREFIX/lib/libSDL2.a (linked into each program: Vexa's loader
# loads only libvexa.so), with its headers in PREFIX/include/SDL2, and
# sdl2-config, a pkg-config file and a CMake package to find it; PREFIX is
# laid out like the SDK, which it's copied into.
set -e
TARBALL=$(realpath "$1")
WORK=$2
SDK=$(realpath "$3")
mkdir -p "$4"
PREFIX=$(realpath "$4")
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC="$SDK/bin/vexa-cc"
JOBS=$(nproc 2>/dev/null || echo 4)

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xzf "$TARBALL" -C "$WORK"
SRC=$(echo "$WORK"/SDL2-*)
VERSION=${SRC##*/SDL2-}

# Vexa's configuration and drivers.
cp "$HERE/sdk/sdl2/SDL_config.h" "$SRC/include/SDL_config.h"
mkdir -p "$SRC/src/video/vexa" "$SRC/src/audio/vexa"
cp "$HERE/sdk/sdl2/video/"*.c "$SRC/src/video/vexa/"
cp "$HERE/sdk/sdl2/audio/"*.c "$SRC/src/audio/vexa/"
# Into the drivers' lists, first (so they're the ones used).
sed -i 's|^extern VideoBootStrap COCOA_bootstrap;|extern VideoBootStrap VEXA_bootstrap;\n&|' \
    "$SRC/src/video/SDL_sysvideo.h"
sed -i '0,/^static VideoBootStrap \*bootstrap\[\] = {/s||&\n#ifdef SDL_VIDEO_DRIVER_VEXA\n    \&VEXA_bootstrap,\n#endif|' \
    "$SRC/src/video/SDL_video.c"
sed -i 's|^extern AudioBootStrap PIPEWIRE_bootstrap;|extern AudioBootStrap VEXAAUDIO_bootstrap;\n&|' \
    "$SRC/src/audio/SDL_sysaudio.h"
sed -i '0,/^static const AudioBootStrap \*const bootstrap\[\] = {/s||&\n#ifdef SDL_AUDIO_DRIVER_VEXA\n    \&VEXAAUDIO_bootstrap,\n#endif|' \
    "$SRC/src/audio/SDL_audio.c"
grep -q '&VEXA_bootstrap' "$SRC/src/video/SDL_video.c"
grep -q '&VEXAAUDIO_bootstrap' "$SRC/src/audio/SDL_audio.c"

cd "$SRC"
SOURCES=$(ls src/*.c src/atomic/*.c src/audio/*.c src/audio/dummy/*.c src/audio/vexa/*.c \
    src/cpuinfo/*.c src/dynapi/*.c src/events/*.c src/file/*.c src/filesystem/unix/*.c \
    src/haptic/*.c src/haptic/dummy/*.c src/hidapi/*.c src/joystick/*.c src/joystick/dummy/*.c \
    src/libm/*.c src/loadso/dummy/*.c src/locale/*.c src/locale/dummy/*.c src/misc/*.c \
    src/misc/dummy/*.c src/power/*.c src/render/*.c src/render/software/*.c src/sensor/*.c \
    src/sensor/dummy/*.c src/stdlib/*.c src/thread/*.c src/thread/pthread/*.c src/timer/*.c \
    src/timer/unix/*.c src/video/*.c src/video/yuv2rgb/*.c src/video/dummy/*.c \
    src/video/vexa/*.c)
mkdir -p obj
# (-include: what SDL's CMake build gives every file.)
printf '%s\n' $SOURCES | xargs -P "$JOBS" -I{} sh -c '
    out=obj/$(echo "{}" | tr / _).o
    "$0" -O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare -Werror=implicit-function-declaration -Werror=int-conversion -Iinclude -Isrc \
        -D_REENTRANT -DSDL_BUILDING_LIBRARY=1 -DDYNAPI_NEEDS_DLOPEN=1 \
        -fvisibility=hidden -c "{}" -o "$out"' "$CC"
rm -f libSDL2.a
ar rcs libSDL2.a obj/*.o

# Installing, laid out as in the SDK.
rm -rf "$PREFIX"
mkdir -p "$PREFIX/include/SDL2" "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake/SDL2" "$PREFIX/bin" \
    "$PREFIX/licenses"
cp include/*.h "$PREFIX/include/SDL2/"
rm -f "$PREFIX/include/SDL2/SDL_config_"*.h "$PREFIX/include/SDL2/"*.cmake
cp libSDL2.a "$PREFIX/lib/"
ar rc "$PREFIX/lib/libSDL2main.a" # (SDL_main isn't needed on Vexa.)
cp LICENSE.txt "$PREFIX/licenses/SDL2.txt"
cat > "$PREFIX/bin/sdl2-config" <<CONFIG
#!/bin/sh
# sdl2-config for Vexa's SDK: SDL $VERSION, linked statically.
SDK=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
for arg; do
    case \$arg in
    --version) echo $VERSION ;;
    --prefix | --exec-prefix) echo "\$SDK" ;;
    --cflags) echo "-I\$SDK/include/SDL2 -D_REENTRANT" ;;
    --libs | --static-libs) echo "-L\$SDK/lib -lSDL2" ;;
    *) echo "usage: sdl2-config [--version] [--cflags] [--libs]" >&2; exit 1 ;;
    esac
done
CONFIG
chmod +x "$PREFIX/bin/sdl2-config"
cat > "$PREFIX/lib/pkgconfig/sdl2.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: sdl2
Description: Simple DirectMedia Layer (for Vexa)
Version: $VERSION
Libs: -L\${libdir} -lSDL2
Cflags: -I\${includedir}/SDL2 -D_REENTRANT
PC
cat > "$PREFIX/lib/cmake/SDL2/SDL2Config.cmake" <<'CMAKE'
# find_package(SDL2) for Vexa's SDK: SDL2::SDL2 (static) and SDL2::SDL2main.
get_filename_component(_vexa_sdk "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SDL2_INCLUDE_DIRS "${_vexa_sdk}/include/SDL2")
set(SDL2_LIBRARIES SDL2::SDL2)
if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 STATIC IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
    IMPORTED_LOCATION "${_vexa_sdk}/lib/libSDL2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
    INTERFACE_COMPILE_DEFINITIONS "_REENTRANT")
  add_library(SDL2::SDL2-static ALIAS SDL2::SDL2)
  add_library(SDL2::SDL2main STATIC IMPORTED)
  set_target_properties(SDL2::SDL2main PROPERTIES
    IMPORTED_LOCATION "${_vexa_sdk}/lib/libSDL2main.a")
endif()
set(SDL2_FOUND TRUE)
CMAKE
sed "s/@VERSION@/$VERSION/" > "$PREFIX/lib/cmake/SDL2/SDL2ConfigVersion.cmake" <<'CMAKE'
set(PACKAGE_VERSION "@VERSION@")
if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
CMAKE
echo "SDL $VERSION: $(ls obj | wc -l) files, in $PREFIX"
