#!/bin/sh
# Builds what NetSurf (the native web browser) needs, with the SDK, as static
# libraries in one prefix: zlib, libpng, libjpeg-turbo, FreeType, expat, and curl
# (HTTP and HTTPS, over the SDK's Mbed TLS). Each step is skipped if it's done
# already (stamps in the work directory).
#
# Usage: tools/build-netsurf-deps.sh <SDK dir> <work dir> <prefix> <tarball>...
# (the tarballs: zlib, libpng, libjpeg-turbo, FreeType, curl, expat)
set -eu
sdk=$(realpath "$1")
mkdir -p "$2" "$3"
work=$(realpath "$2") prefix=$(realpath "$3")
shift 3
zlib_tarball=$(realpath "$1") png_tarball=$(realpath "$2") jpeg_tarball=$(realpath "$3")
freetype_tarball=$(realpath "$4") curl_tarball=$(realpath "$5") expat_tarball=$(realpath "$6")
jobs=$(nproc)
export CC="$sdk/bin/vexa-cc"
toolchain="$sdk/cmake/vexa.cmake"

log() {
    printf '[netsurf-deps] %s\n' "$*"
}

step() {
    if [ -f "$work/.done-$1" ]; then
        return
    fi
    log "$1"
    (cd "$work" && "build_$1") > "$work/$1.log" 2>&1 || { tail -30 "$work/$1.log"; exit 1; }
    touch "$work/.done-$1"
}

# Unpacks a tarball into the work directory; prints the source directory.
unpack() {
    name=$(tar -tf "$1" | head -1 | cut -d/ -f1)
    rm -rf "${work:?}/$name"
    tar -xf "$1" -C "$work"
    echo "$work/$name"
}

cmake_build() { # <source dir> <build dir> <cmake options>...
    src=$1 dir=$2
    shift 2
    rm -rf "$dir"
    cmake -S "$src" -B "$dir" -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$dir" -j"$jobs"
    cmake --install "$dir"
}

build_zlib() {
    src=$(unpack "$zlib_tarball")
    cd "$src"
    CHOST=x86_64 ./configure --static --prefix="$prefix"
    make -j"$jobs" libz.a
    make install
}

build_png() {
    src=$(unpack "$png_tarball")
    cmake_build "$src" "$work/png-build" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_HARDWARE_OPTIMIZATIONS=OFF -DZLIB_ROOT="$prefix" \
        -DZLIB_LIBRARY="$prefix/lib/libz.a" -DZLIB_INCLUDE_DIR="$prefix/include"
    ln -sf libpng.a "$prefix/lib/libpng16.a" # (What libpng16.pc names.)
}

build_jpeg() {
    src=$(unpack "$jpeg_tarball")
    cmake_build "$src" "$work/jpeg-build" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF \
        -DWITH_TURBOJPEG=OFF -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_INSTALL_LIBDIR="$prefix/lib" \
        -DCMAKE_INSTALL_INCLUDEDIR="$prefix/include"
}

build_freetype() {
    src=$(unpack "$freetype_tarball")
    cmake_build "$src" "$work/freetype-build" -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_REQUIRE_ZLIB=ON \
        -DZLIB_LIBRARY="$prefix/lib/libz.a" -DZLIB_INCLUDE_DIR="$prefix/include"
}

build_expat() {
    src=$(unpack "$expat_tarball")
    cmake_build "$src" "$work/expat-build" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF \
        -DCMAKE_C_FLAGS=-DXML_DEV_URANDOM \
        -DCMAKE_INSTALL_LIBDIR="$prefix/lib" -DCMAKE_INSTALL_INCLUDEDIR="$prefix/include"
}

build_curl() {
    src=$(unpack "$curl_tarball")
    cd "$src"
    # (Cross-compiling for Vexa: "linux" to configure, but without Linux's extras.)
    CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib" \
        ./configure --host=x86_64-pc-linux-gnu --prefix="$prefix" --disable-shared --enable-static \
        --with-mbedtls="$sdk" --with-zlib="$prefix" --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
        --without-ca-path --without-libpsl --without-brotli --without-zstd --without-nghttp2 \
        --without-libidn2 --without-librtmp --without-libssh2 --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
        --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-manual \
        --disable-unix-sockets --disable-threaded-resolver --disable-ntlm --disable-docs \
        ac_cv_func_getpwuid_r=no
    make -j"$jobs" -C lib
    make -C lib install
    make -C include install
    make install-pkgconfigDATA install-binSCRIPTS
}

step zlib
step png
step jpeg
step freetype
step expat
step curl
log "done: $prefix"
