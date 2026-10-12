#!/bin/sh
# Builds Servo's render test for x86_64-unknown-vexa: Servo 0.7.0 (its script, layout,
# networking and WebRender, with SpiderMonkey through mozjs_sys) and the crates under it, from
# the cargo registry, with Vexa's edits applied:
#   third_party/servo-vexa.patch      Servo's crates: no sandbox and no ICU, FreeType fonts,
#                                     ring for TLS and WebCrypto, Vexa's navigator platform,
#                                     in-process IPC, and inventory's constructors on Vexa
#   third_party/surfman-vexa.patch    surfman's OSMesa backend, which is Mesa's software OpenGL
#   third_party/libc-vexa.patch       libc 0.2.190, with the Vexa module that
#                                     tools/rust/gen-vexa-libc.py and gen-vexa-libc-extra.py write
#   third_party/mozjs-sys-vexa.patch  mozjs_sys 153.3.0-1 (SpiderMonkey), and
#   third_party/mozjs-vexa.patch      the SpiderMonkey tree inside it
#   third_party/crates-vexa.patch     mio, socket2, tokio, getrandom and ring
# The programs are in tools/servo/servo-render. servo-render loads a local HTML file, writes the
# frame to a PNG, and prints "servo-render: passed" when the page's pixels are right.
# servo-window opens a window on the Vexa desktop and shows the page in it.
#
#   build-servo-vexa.sh WORKDIR [--prepare-only]
#
# Output: WORKDIR/servo-render and WORKDIR/servo-window, stripped x86_64-unknown-vexa
# executables. With --prepare-only, the sources are made (WORKDIR/crates and WORKDIR/app) and
# nothing is built.
# Needs: the vexa-std toolchain (tools/rust/build-std-vexa.sh), the SDK (build/sdk/vexa-sdk),
# clang 19 as the host compiler (HOST_CC and HOST_CXX; they default to /usr/lib/llvm-19), the
# repo's zlib for Vexa (make build/zlib-native/libz.a: freetype-sys builds FreeType and libpng
# from source, and libpng needs zlib), cargo with network access to crates.io, and patch,
# python3, strip and cp. WORKDIR is recreated on each run. The build is long: SpiderMonkey is
# most of it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
prepare_only=no
[ "${2:-}" = "--prepare-only" ] && prepare_only=yes
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

echo "== the crates, fetched at the versions in the lock"
rm -rf "$work/fetch"
mkdir -p "$work/fetch"
cp -r "$here/servo-render/." "$work/fetch/"
# The fetch is the app without its patches: those point at the directories made below.
sed -i '/^\[patch.crates-io\]/,/^$/d' "$work/fetch/Cargo.toml"
(cd "$work/fetch" && cargo fetch -q)

echo "== the registry crates, with the patches for Vexa"
rm -rf "$work/crates"
mkdir -p "$work/crates"
for c in getrandom-0.2.17 getrandom-0.3.4 getrandom-0.4.3 mio-1.2.4 socket2-0.6.5 tokio-1.53.2 \
    ring-0.17.14 servo-0.7.0 servo-constellation-0.7.0 servo-fonts-0.7.0 servo-fonts-traits-0.7.0 \
    rustix-1.1.5 servo-script-0.7.0 servo-script-bindings-0.7.0 servo-net-0.7.0 rustls-0.23.45 \
    tokio-rustls-0.26.6 inventory-0.3.25 surfman-0.14.0 libc-0.2.190 mozjs_sys-153.3.0-1; do
    cp -r "$registry/$c" "$work/crates/$c"
done
chmod -R u+w "$work/crates"
patch -p1 -s -d "$work/crates" < "$repo/third_party/crates-vexa.patch"
patch -p1 -s -d "$work/crates" < "$repo/third_party/servo-vexa.patch"
patch -p1 -s -d "$work/crates/surfman-0.14.0" < "$repo/third_party/surfman-vexa.patch"
patch -p2 -s -d "$work/crates/libc-0.2.190" < "$repo/third_party/libc-vexa.patch"
python3 "$repo/tools/rust/gen-vexa-libc.py" "$repo/libvexa/include" \
    "$work/crates/libc-0.2.190/src/unix/vexa" "$work/crates/libc-0.2.190/src/unix/mod.rs"
python3 "$repo/tools/rust/gen-vexa-libc-extra.py" "$work/crates/libc-0.2.190/src" \
    "$work/crates/libc-0.2.190/src/unix/vexa/extra.rs"
patch -p1 -s -d "$work/crates/mozjs_sys-153.3.0-1" < "$repo/third_party/mozjs-sys-vexa.patch"
patch -p2 -s -d "$work/crates/mozjs_sys-153.3.0-1/mozjs" < "$repo/third_party/mozjs-vexa.patch"

echo "== the program, next to those crates (its manifest points at ../crates)"
rm -rf "$work/app"
cp -r "$here/servo-render" "$work/app"
if [ "$prepare_only" = yes ]; then
    echo "prepared $work (not built)"
    exit 0
fi

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

for program in servo-render servo-window; do
    strip -o "$work/$program" "$work/app/target/x86_64-unknown-vexa/debug/$program"
done
echo "built $work/servo-render and $work/servo-window"
