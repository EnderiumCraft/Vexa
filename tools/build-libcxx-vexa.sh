#!/bin/sh
# Builds a C++ standard library for native Vexa programs: LLVM's libc++ and
# libc++abi, against libvexa (with the SDK's vexa-cc and vexa-c++), as static
# libraries. No exceptions, no locales or iostreams, no <filesystem>; the
# rest of the library is there.
#
# Usage: tools/build-libcxx-vexa.sh <llvm-project tarball> <SDK dir> <work dir> <out dir>
# <out dir> gets include/c++/v1 and lib/libc++.a, lib/libc++abi.a.
set -eu
tarball=$(realpath "$1")
sdk=$(realpath "$2")
mkdir -p "$3" "$4"
work=$(realpath "$3")
out=$(realpath "$4")
jobs=$(nproc)
src="$work/$(basename "$tarball" .tar.xz)"

rm -rf "$src" "$work/build"
# Only the parts the runtimes' build needs.
tar -xJf "$tarball" -C "$work" --wildcards \
    '*/runtimes/*' '*/libcxx/*' '*/libcxxabi/*' '*/libunwind/*' '*/cmake/*' \
    '*/llvm/cmake/*' '*/llvm/utils/llvm-lit/*' '*/llvm/utils/lit/*'

export VEXA_CXX_BOOTSTRAP=1
cmake -G Ninja -S "$src/runtimes" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/cmake/vexa.cmake" \
    -DCMAKE_CXX_COMPILER="$sdk/bin/vexa-c++" -DCMAKE_CXX_COMPILER_WORKS=1 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/ \
    -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx" \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_EXCEPTIONS=OFF -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_LOCALIZATION=OFF -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM=OFF -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF \
    -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF -DLIBCXX_HAS_MUSL_LIBC=OFF \
    -DLIBCXX_HAS_PTHREAD_API=ON -DLIBCXXABI_HAS_PTHREAD_API=ON \
    -DLIBCXX_HAS_ATOMIC_LIB=OFF -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DLIBCXXABI_ENABLE_THREADS=ON \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXX_CXX_ABI=libcxxabi \
    > "$work/configure.log" 2>&1 || { tail -40 "$work/configure.log"; exit 1; }
ninja -C "$work/build" -j"$jobs" cxx cxxabi > "$work/build.log" 2>&1 ||
    { grep -B2 -A8 'error:' "$work/build.log" | head -60; exit 1; }
DESTDIR="$out" ninja -C "$work/build" install-cxx install-cxxabi > "$work/install.log" 2>&1 ||
    { tail -20 "$work/install.log"; exit 1; }
