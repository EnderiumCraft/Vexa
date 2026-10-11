#!/bin/sh
# Builds SpiderMonkey for x86_64-unknown-vexa: the js/src of mozjs_sys 153.4.0-0 with
# Vexa's edits (third_party/mozjs-vexa.patch), configured the way mozjs_sys configures
# it. The headers mozjs_sys puts on the include path come from the same places here:
# encoding_c_mem; mozjs_normalizer_glue's header (cbindgen makes it when that crate
# builds); and zlib, from the repo's native build (ZLIB_DIR).
#
#   build-mozjs-vexa.sh WORKDIR
#
# Needs: cargo with network access to crates.io, patch, python3, make, the SDK built
# (build/sdk/vexa-sdk), and zlib's headers in build/zlib-native. The host compilers must
# be clang 19 or later: SpiderMonkey's configure rejects the older clang 18 that some
# systems have. HOST_CC and HOST_CXX pick them; they default to clang-19 in /usr/lib/llvm-19.
#
# WORKDIR is recreated on each run. Its obj/ directory holds the build.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
sdk=$repo/build/sdk/vexa-sdk
host_cc=${HOST_CC:-/usr/lib/llvm-19/bin/clang}
host_cxx=${HOST_CXX:-/usr/lib/llvm-19/bin/clang++}
zlib=${ZLIB_DIR:-$repo/build/zlib-native/zlib-1.3.dfsg}
if [ ! -f "$zlib/zlib.h" ]; then
    echo "no zlib.h in $zlib: run make build/zlib-native/libz.a, or set ZLIB_DIR" >&2
    exit 1
fi

echo "== the crates: mozjs_sys 153.4.0-0 and encoding_c_mem 0.2.6 (sources and headers)"
rm -rf "$work/fetch" "$work/normhdr"
mkdir -p "$work/fetch/src" "$work/normhdr/src"
cat > "$work/fetch/Cargo.toml" <<'EOF2'
[package]
name = "fetch"
version = "0.0.0"
edition = "2021"

[dependencies]
mozjs_sys = "=153.4.0-0"
encoding_c_mem = "=0.2.6"
EOF2
echo 'fn main() {}' > "$work/fetch/src/main.rs"
(cd "$work/fetch" && cargo fetch -q)

echo "== the normalizer glue header, made by its crate's build script"
cat > "$work/normhdr/Cargo.toml" <<'EOF2'
[package]
name = "normhdr"
version = "0.0.0"
edition = "2021"

[dependencies]
mozjs_normalizer_glue = "=153.4.0"
EOF2
echo 'fn main() {}' > "$work/normhdr/src/main.rs"
(cd "$work/normhdr" && cargo build -q)
normalizer_out=$(ls -d "$work"/normhdr/target/debug/build/mozjs_normalizer_glue-*/out/include | head -1)
# cbindgen writes #include <ostream> into the header. Its declarations need only <iosfwd>,
# and <ostream> is not available on Vexa, so the copy we put on the include path has that include changed.
normalizer="$work/normalizer-include"
mkdir -p "$normalizer"
sed "s|#include <ostream>|#include <iosfwd>|" "$normalizer_out/normalizer_glue.h" > "$normalizer/normalizer_glue.h"

registry=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/ | head -1)
mozjs_sys=$registry/mozjs_sys-153.4.0-0
encoding=$registry/encoding_c_mem-0.2.6/include

echo "== the js/src tree with the Vexa patch"
rm -rf "$work/mozjs"
cp -r "$mozjs_sys/mozjs" "$work/mozjs"
chmod -R u+w "$work/mozjs"
patch -p2 -s -d "$work/mozjs" < "$repo/third_party/mozjs-vexa.patch"

echo "== configure for x86_64-unknown-vexa"
mkdir -p "$work/obj"
cd "$work/obj"
PATH="$sdk/bin:$PATH" \
PYTHONDONTWRITEBYTECODE=1 \
MOZBUILD_STATE_PATH="$work/state" \
HOST_CC="$host_cc" HOST_CXX="$host_cxx" \
VEXA_HOST_CC="$host_cc" VEXA_HOST_CXX="$host_cxx" \
CPPFLAGS="-I$encoding -I$normalizer -I$zlib" \
"$work/mozjs/js/src/configure" \
    --disable-jemalloc --disable-js-shell --disable-tests --disable-export-js \
    --disable-shared-js --build-backends=RecursiveMake --disable-jit --without-intl-api \
    --disable-wasm-moz-intgemm \
    --target=x86_64-unknown-vexa CC=vexa-cc CXX=vexa-c++ AR=ar

echo "== build"
PATH="$sdk/bin:$PATH" \
PYTHONDONTWRITEBYTECODE=1 \
MOZBUILD_STATE_PATH="$work/state" \
HOST_CC="$host_cc" HOST_CXX="$host_cxx" \
VEXA_HOST_CC="$host_cc" VEXA_HOST_CXX="$host_cxx" \
make -j"$(nproc)"
