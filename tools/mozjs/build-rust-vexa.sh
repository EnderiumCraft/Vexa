#!/bin/sh
# Builds a Rust program that runs a script on SpiderMonkey on Vexa, through the mozjs
# crate (0.26.8) on mozjs_sys 153.4.0-0. The crates come from the cargo registry, with
# Vexa's edits applied:
#   third_party/mozjs-sys-vexa.patch  the mozjs_sys crate (build script, configure flags, glue)
#   third_party/mozjs-vexa.patch      the SpiderMonkey tree inside it
#   third_party/libc-vexa.patch       libc 0.2.190, with the Vexa module that
#                                     tools/rust/gen-vexa-libc.py writes
# The program is tools/mozjs/rust-test; it prints the same check as vexa-js-test.cpp.
#
#   build-rust-vexa.sh WORKDIR
#
# Output: WORKDIR/vexa-js-rust, a stripped x86_64-unknown-vexa executable.
# Needs: the vexa-std toolchain (tools/rust/build-std-vexa.sh), the SDK
# (build/sdk/vexa-sdk), clang 19 as the host compiler (HOST_CC and HOST_CXX; they default
# to /usr/lib/llvm-19), cargo with network access to crates.io, and patch, python3 and
# strip. The C zlib that mozjs_sys uses comes from the libz-sys crate.
# WORKDIR is recreated on each run.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
sdk=$repo/build/sdk/vexa-sdk
target=$repo/tools/rust/x86_64-unknown-vexa.json
host_cc=${HOST_CC:-/usr/lib/llvm-19/bin/clang}
host_cxx=${HOST_CXX:-/usr/lib/llvm-19/bin/clang++}
registry=$(ls -d "${CARGO_HOME:-$HOME/.cargo}"/registry/src/*/ | head -1)

echo "== the crates: mozjs 0.26.8's mozjs_sys 153.4.0-0 and libc 0.2.190 (sources)"
rm -rf "$work/fetch"
mkdir -p "$work/fetch/src"
cat > "$work/fetch/Cargo.toml" <<'EOF2'
[package]
name = "fetch"
version = "0.0.0"
edition = "2021"

[dependencies]
mozjs = "=0.26.8"
mozjs_sys = "=153.4.0-0"
libc = "=0.2.190"
EOF2
echo 'fn main() {}' > "$work/fetch/src/main.rs"
(cd "$work/fetch" && cargo fetch -q)

echo "== mozjs_sys with Vexa's edits: the crate, then the SpiderMonkey tree inside it"
rm -rf "$work/mozjs_sys"
cp -r "$registry/mozjs_sys-153.4.0-0" "$work/mozjs_sys"
chmod -R u+w "$work/mozjs_sys"
patch -p1 -s -d "$work/mozjs_sys" < "$repo/third_party/mozjs-sys-vexa.patch"
patch -p2 -s -d "$work/mozjs_sys/mozjs" < "$repo/third_party/mozjs-vexa.patch"

echo "== libc 0.2.190 with Vexa's module (the sysroot's libc is 0.2.178; this one is for the crates)"
rm -rf "$work/libc"
cp -r "$registry/libc-0.2.190" "$work/libc"
chmod -R u+w "$work/libc"
patch -p2 -s -d "$work/libc" < "$repo/third_party/libc-vexa.patch"
python3 "$repo/tools/rust/gen-vexa-libc.py" "$repo/libvexa/include" \
    "$work/libc/src/unix/vexa" "$work/libc/src/unix/mod.rs"

echo "== the test program, next to those two crates"
rm -rf "$work/app"
mkdir -p "$work/app"
cp -r "$here/rust-test/." "$work/app/"

echo "== build it for x86_64-unknown-vexa"
export PATH="$sdk/bin:$PATH"
export RUSTC_BOOTSTRAP=1
export CC_x86_64_unknown_vexa=vexa-cc
export CXX_x86_64_unknown_vexa=vexa-c++
export AR_x86_64_unknown_vexa=ar
export HOST_CC="$host_cc"
export HOST_CXX="$host_cxx"
export VEXA_HOST_CC="$host_cc"
export VEXA_HOST_CXX="$host_cxx"
llvm_lib=$(dirname "$host_cc")/../lib
export LIBCLANG_PATH="$llvm_lib"
export MOZBUILD_STATE_PATH="$work/state"
export BINDGEN_EXTRA_CLANG_ARGS="-nostdinc++ -isystem $sdk/include/c++/v1 -nostdinc -isystem $sdk/include -isystem $llvm_lib/clang/19/include"
(cd "$work/app" && cargo +vexa-std build --target "$target" -Zbuild-std=core,alloc,std,panic_abort)

strip -o "$work/vexa-js-rust" "$work/app/target/x86_64-unknown-vexa/debug/vexa-js-rust"
echo "built $work/vexa-js-rust"
