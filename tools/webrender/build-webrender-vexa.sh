#!/bin/sh
# Builds a WebRender test for x86_64-unknown-vexa: WebRender 0.70 (the version Servo 0.7.0
# uses) draws a rectangle into a surfman 0.14 surface. surfman's context comes from OSMesa,
# which is Mesa's software OpenGL (softpipe). The crates come from the cargo registry, with
# Vexa's edits applied:
#   third_party/surfman-vexa.patch    surfman's OSMesa backend (selected on Vexa)
#   third_party/libc-vexa.patch       libc 0.2.190, with the Vexa module that
#                                     tools/rust/gen-vexa-libc.py writes
# The program is tools/webrender/webrender-test. It prints "webrender-test: passed" when the
# frame's pixels are right.
#
#   build-webrender-vexa.sh WORKDIR
#
# Output: WORKDIR/webrender-test, a stripped x86_64-unknown-vexa executable.
# Needs: the vexa-std toolchain (tools/rust/build-std-vexa.sh), the SDK (build/sdk/vexa-sdk),
# clang 19 as the host compiler (HOST_CC and HOST_CXX; they default to /usr/lib/llvm-19),
# the repo's zlib for Vexa (make build/zlib-native/libz.a: freetype-sys builds FreeType and
# libpng from source, and libpng needs zlib), cargo with network access to crates.io, and
# patch, python3 and strip. WORKDIR is recreated on each run.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
sdk=$repo/build/sdk/vexa-sdk
target=$repo/tools/rust/x86_64-unknown-vexa.json
host_cc=${HOST_CC:-/usr/lib/llvm-19/bin/clang}
host_cxx=${HOST_CXX:-/usr/lib/llvm-19/bin/clang++}
zlib=$repo/build/zlib-native/zlib-1.3.dfsg
if [ ! -f "$zlib/zlib.h" ] || [ ! -f "$repo/build/zlib-native/libz.a" ]; then
    echo "no zlib for Vexa: run make build/zlib-native/libz.a" >&2
    exit 1
fi
registry=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/ | head -1)

echo "== the crates: webrender 0.70.0, surfman 0.14.0 and libc 0.2.190 (sources)"
rm -rf "$work/fetch"
mkdir -p "$work/fetch/src"
cat > "$work/fetch/Cargo.toml" <<'EOF2'
[package]
name = "fetch"
version = "0.0.0"
edition = "2021"

[dependencies]
webrender = "=0.70.0"
surfman = "=0.14.0"
libc = "=0.2.190"
EOF2
echo 'fn main() {}' > "$work/fetch/src/main.rs"
(cd "$work/fetch" && cargo fetch -q)

echo "== surfman with Vexa's OSMesa backend"
rm -rf "$work/surfman"
cp -r "$registry/surfman-0.14.0" "$work/surfman"
chmod -R u+w "$work/surfman"
patch -p1 -s -d "$work/surfman" < "$repo/third_party/surfman-vexa.patch"

echo "== libc 0.2.190 with Vexa's module"
rm -rf "$work/libc"
cp -r "$registry/libc-0.2.190" "$work/libc"
chmod -R u+w "$work/libc"
patch -p2 -s -d "$work/libc" < "$repo/third_party/libc-vexa.patch"
python3 "$repo/tools/rust/gen-vexa-libc.py" "$repo/libvexa/include" \
    "$work/libc/src/unix/vexa" "$work/libc/src/unix/mod.rs"

echo "== the test program, next to those two crates"
rm -rf "$work/app"
mkdir -p "$work/app"
cp -r "$here/webrender-test/." "$work/app/"

echo "== build it for x86_64-unknown-vexa"
export PATH="$sdk/bin:$PATH"
export RUSTC_BOOTSTRAP=1
export CC_x86_64_unknown_vexa=vexa-cc
export CXX_x86_64_unknown_vexa=vexa-c++
export AR_x86_64_unknown_vexa=ar
export CFLAGS_x86_64_unknown_vexa="-I$zlib"
export VEXA_ZLIB_LIB_DIR="$repo/build/zlib-native"
export HOST_CC="$host_cc"
export HOST_CXX="$host_cxx"
export VEXA_HOST_CC="$host_cc"
export VEXA_HOST_CXX="$host_cxx"
llvm_lib=$(dirname "$host_cc")/../lib
export LIBCLANG_PATH="$llvm_lib"
export MOZBUILD_STATE_PATH="$work/state"
export BINDGEN_EXTRA_CLANG_ARGS="-nostdinc++ -isystem $sdk/include/c++/v1 -nostdinc -isystem $sdk/include -isystem $llvm_lib/clang/19/include"
(cd "$work/app" && cargo +vexa-std build --target "$target" -Zbuild-std=core,alloc,std,panic_abort)

strip -o "$work/webrender-test" "$work/app/target/x86_64-unknown-vexa/debug/webrender-test"
echo "built $work/webrender-test"
