#!/bin/sh
# Builds OpenGL for the Linux programs in /linux, from source, with musl:
#   1. LLVM's C++ runtime (libc++, libc++abi, libunwind), which musl lacks
#   2. LLVM itself (only the x86 back end, as one shared library)
#   3. Mesa, with llvmpipe (OpenGL compiled to machine code by LLVM) and
#      softpipe: libGL for X programs (GLX on Xlib, so any X server, Xvexa
#      included, can show it) and OSMesa (drawing into memory, no X needed)
# Each step is skipped if it's done already (stamps in the work directory).
#
# Usage: tools/build-mesa.sh <llvm-project tarball> <mesa tarball> <work dir>
#                            <X sysroot> <out root> <Linux headers>
set -eu
llvm_tarball=$(realpath "$1") mesa_tarball=$(realpath "$2")
mkdir -p "$3" "$5"
work=$(realpath "$3") x11=$(realpath "$4") root=$(realpath "$5") headers=$(realpath "$6")
here=$(dirname "$(realpath "$0")")
jobs=$(nproc)
llvm_src="$work/$(basename "$llvm_tarball" .tar.xz)"

export MUSL_CC="${MUSL_CC:-musl-gcc}"
CC="$here/musl-cc-wrapper.sh"
CXX="$here/musl-libcxx-wrapper.sh"
# LLVM's build runs programs it has just built (musl ones, which need libc++).
export LD_LIBRARY_PATH="$root/usr/lib"

log() {
    printf '[mesa] %s\n' "$*"
}

step() {
    if [ -f "$work/.done-$1" ]; then
        return
    fi
    log "$1"
    "build_$1"
    touch "$work/.done-$1"
}

build_runtimes() {
    rm -rf "$llvm_src" "$work/runtimes"
    tar -xJf "$llvm_tarball" -C "$work"
    cmake -G Ninja -S "$llvm_src/runtimes" -B "$work/runtimes" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="-isystem $headers" -DCMAKE_CXX_FLAGS="-isystem $headers" \
        -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_HAS_ATOMIC_LIB=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXX_ENABLE_STATIC=OFF -DLIBCXXABI_ENABLE_STATIC=OFF -DLIBUNWIND_ENABLE_STATIC=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
        > "$work/runtimes.log" 2>&1 || { tail -30 "$work/runtimes.log"; return 1; }
    ninja -C "$work/runtimes" -j"$jobs" cxx cxxabi unwind >> "$work/runtimes.log" 2>&1 ||
        { grep -A5 'error:' "$work/runtimes.log" | head -40; return 1; }
    DESTDIR="$root" ninja -C "$work/runtimes" install-cxx install-cxxabi install-unwind \
        >> "$work/runtimes.log" 2>&1
}

build_llvm() {
    export LIBCXX_ROOT="$root"
    rm -rf "$work/llvm"
    cmake -G Ninja -S "$llvm_src/llvm" -B "$work/llvm" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="-isystem $headers" -DCMAKE_CXX_FLAGS="-isystem $headers" \
        -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
        -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
        -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_BUILD_TOOLS=OFF \
        -DLLVM_INCLUDE_UTILS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DHAVE_UNW_ADD_DYNAMIC_FDE=1 \
        -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-musl \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-musl -DLLVM_PARALLEL_LINK_JOBS=1 \
        > "$work/llvm.log" 2>&1 || { tail -30 "$work/llvm.log"; return 1; }
    ninja -C "$work/llvm" -j"$jobs" LLVM llvm-config >> "$work/llvm.log" 2>&1 ||
        { grep -A5 'error:' "$work/llvm.log" | head -40; return 1; }
    DESTDIR="$root" ninja -C "$work/llvm" install-LLVM install-llvm-headers \
        >> "$work/llvm.log" 2>&1 || { tail -20 "$work/llvm.log"; return 1; }
    # (Mesa's build asks it how to use LLVM; with the tools off, it has no install target.)
    install -D "$work/llvm/bin/llvm-config" "$root/usr/bin/llvm-config"
}

build_mesa() {
    export LIBCXX_ROOT="$root"
    name=$(tar -tzf "$mesa_tarball" | head -1 | cut -d/ -f1)
    rm -rf "${work:?}/$name"
    tar -xzf "$mesa_tarball" -C "$work"
    cd "$work/$name"
    cat > native.ini <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
llvm-config = '$root/usr/bin/llvm-config'
EOF
    PKG_CONFIG_LIBDIR="$x11/usr/lib/pkgconfig:$x11/usr/share/pkgconfig" PKG_CONFIG_PATH= \
        PKG_CONFIG_SYSROOT_DIR="$x11" \
        CFLAGS="-O2 -isystem $headers" CXXFLAGS="-O2 -isystem $headers" \
        LDFLAGS="-L$x11/usr/lib -Wl,-rpath-link,$x11/usr/lib -L$root/usr/lib -Wl,-rpath-link,$root/usr/lib" \
        meson setup _build --native-file native.ini --prefix=/usr --libdir=lib \
        --buildtype=release -Dplatforms=x11 -Dglx=xlib -Dgallium-drivers=swrast \
        -Dvulkan-drivers= -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled \
        -Dosmesa=true -Dllvm=enabled -Dshared-llvm=enabled -Dshared-glapi=enabled \
        -Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dlmsensors=disabled -Dbuild-tests=false -Dshader-cache=disabled \
        > configure.log 2>&1 || { tail -40 configure.log; return 1; }
    ninja -C _build -j"$jobs" > build.log 2>&1 || { grep -A5 'error' build.log | head -40; return 1; }
    DESTDIR="$root" meson install -C _build --no-rebuild > install.log 2>&1 ||
        { tail -20 install.log; return 1; }
}

step runtimes
step llvm
step mesa
