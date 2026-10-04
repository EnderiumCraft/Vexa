#!/bin/sh
# Builds OpenGL for native Vexa programs, from source: Mesa's OSMesa
# (OpenGL drawn into memory) with softpipe (gallium's software renderer, no
# LLVM) and virgl (the host's GPU, through the virtio GPU, when there's one),
# built against libvexa with the SDK's vexa-cc and vexa-c++, as one shared
# library, libOSMesa.so, with the C++ runtime and a small libdrm
# (third_party/libdrm-vexa) linked in. SDL's Vexa driver loads it for
# OpenGL windows; programs can also link to it.
#
# Usage: tools/build-mesa-vexa.sh <mesa tarball> <SDK dir> <work dir> <out dir>
# (<out dir> gets lib/libOSMesa.so, include/GL and include/KHR, and Mesa's
# license.)
set -eu
mesa_tarball=$(realpath "$1") sdk=$(realpath "$2")
mkdir -p "$3" "$4"
work=$(realpath "$3") out=$(realpath "$4")
here=$(dirname "$(realpath "$0")")
jobs=$(nproc)

name=$(tar -tzf "$mesa_tarball" | head -1 | cut -d/ -f1)
rm -rf "${work:?}/$name"
tar -xzf "$mesa_tarball" -C "$work"
cd "$work/$name"
patch -p1 -s < "$here/../third_party/mesa-vexa.patch"

# The small libdrm, for virgl's winsys (with a pkg-config file Mesa finds).
drm="$work/libdrm"
rm -rf "$drm"
mkdir -p "$drm/pkgconfig"
"$sdk/bin/vexa-cc" -O2 -fvisibility=hidden -I"$work/$name/include" \
    -c "$here/../third_party/libdrm-vexa/xf86drm.c" -o "$drm/xf86drm.o"
ar rcs "$drm/libdrm.a" "$drm/xf86drm.o"
cat > "$drm/pkgconfig/libdrm.pc" <<PC
Name: libdrm
Description: A small libdrm for Vexa
Version: 2.4.122
Cflags: -I$here/../third_party/libdrm-vexa
Libs: -L$drm -ldrm
PC

cat > cross.ini <<INI
[binaries]
c = '$sdk/bin/vexa-cc'
cpp = '$sdk/bin/vexa-c++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['$drm/pkgconfig']

[host_machine]
system = 'vexa'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
INI
PKG_CONFIG_PATH= meson setup _build --cross-file cross.ini --prefix=/ --libdir=lib \
    --buildtype=release -Dplatforms= -Dglx=disabled -Dgallium-drivers=swrast,virgl \
    -Dvulkan-drivers= -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dopengl=true -Dosmesa=true -Dllvm=disabled -Dshared-glapi=disabled \
    -Dxmlconfig=disabled -Dzstd=disabled -Dzlib=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dlmsensors=disabled -Dbuild-tests=false -Dshader-cache=disabled -Dexpat=disabled \
    -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
    -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-omx=disabled \
    -Dgallium-d3d12-video=disabled -Dandroid-libbacktrace=disabled \
    -Dcpp_rtti=false \
    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
ninja -C _build -j"$jobs" > build.log 2>&1 || { grep -B2 -A6 'error' build.log | head -60; exit 1; }
rm -rf "$out"
mkdir -p "$out/lib" "$out/include/GL" "$out/include/KHR"
# (Its soname is libOSMesa.so.8; Vexa's loader finds it as libOSMesa.so.)
strip --strip-unneeded -o "$out/lib/libOSMesa.so" _build/src/gallium/targets/osmesa/libOSMesa.so.8.0.0
cp include/GL/gl.h include/GL/glext.h include/GL/osmesa.h "$out/include/GL/"
cp include/KHR/khrplatform.h "$out/include/KHR/"
cp docs/license.rst "$out/license.rst"
