#!/bin/sh
# Builds SpiderMonkey for x86_64-unknown-vexa: the js/src of mozjs_sys 153.4.0-0 with
# Vexa's edits (third_party/mozjs-vexa.patch), configured the way mozjs_sys configures
# it, with the encoding_c_mem headers on the include path.
#
#   build-mozjs-vexa.sh WORKDIR
#
# Needs: cargo with network access to crates.io (for mozjs_sys and encoding_c_mem),
# patch, python3, make, and the SDK built (build/sdk/vexa-sdk). The host compilers must
# be clang 19 or later: SpiderMonkey's configure rejects the older clang 18 that some
# systems have. Set HOST_CC and HOST_CXX to pick them; they default to clang-19 in
# /usr/lib/llvm-19.
#
# WORKDIR is recreated on each run. Its obj/ directory holds the build.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
sdk=$repo/build/sdk/vexa-sdk
host_cc=${HOST_CC:-/usr/lib/llvm-19/bin/clang}
host_cxx=${HOST_CXX:-/usr/lib/llvm-19/bin/clang++}

echo "== mozjs_sys 153.4.0-0 and encoding_c_mem 0.2.6 (the sources and their headers)"
rm -rf "$work/fetch"
mkdir -p "$work/fetch/src"
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
CPPFLAGS="-I$encoding" \
"$work/mozjs/js/src/configure" \
    --disable-jemalloc --disable-js-shell --disable-tests --disable-export-js \
    --disable-shared-js --build-backends=RecursiveMake --disable-jit --without-intl-api --disable-wasm-moz-intgemm \
    --target=x86_64-unknown-vexa CC=vexa-cc CXX=vexa-c++ AR=ar

echo "== build"
PATH="$sdk/bin:$PATH" \
PYTHONDONTWRITEBYTECODE=1 \
MOZBUILD_STATE_PATH="$work/state" \
HOST_CC="$host_cc" HOST_CXX="$host_cxx" \
VEXA_HOST_CC="$host_cc" VEXA_HOST_CXX="$host_cxx" \
make -j"$(nproc)"
