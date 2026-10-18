#!/bin/sh
# Builds LLVM for native Vexa programs, from source: only the libraries Mesa's llvmpipe needs
# (the x86 back end and the JIT), as static libraries, with the SDK's vexa-cc and vexa-c++
# (libvexa and the SDK's libc++). Mesa's llvmpipe turns OpenGL's shaders into machine code with
# them, which is many times faster than softpipe, which interprets them.
#
# LLVM's build makes tables with llvm-tblgen, which has to run here: it is built first, for
# this machine. Mesa asks llvm-config how to use LLVM; the real one would be a Vexa program,
# so WORK/bin/llvm-config is a small script that answers for the libraries built here.
#
# Usage: tools/build-llvm-vexa.sh <llvm-project tarball> <SDK dir> <work dir>
# Each step is skipped if it's done already (stamps in the work directory).
set -eu
llvm_tarball=$(realpath "$1") sdk=$(realpath "$2")
mkdir -p "$3"
work=$(realpath "$3")
jobs=$(nproc)
src="$work/$(basename "$llvm_tarball" .tar.xz)"
version=$(basename "$llvm_tarball" .src.tar.xz | sed 's/^llvm-project-//')

log() {
    printf '[llvm-vexa] %s\n' "$*"
}

step() {
    if [ -f "$work/.done-$1" ]; then
        return
    fi
    log "$1"
    "build_$1"
    touch "$work/.done-$1"
}

build_source() {
    rm -rf "$src"
    tar -xJf "$llvm_tarball" -C "$work" "$(basename "$src")/llvm" "$(basename "$src")/cmake" \
        "$(basename "$src")/third-party"
}

# llvm-tblgen for this machine (with this machine's compiler).
build_tblgen() {
    rm -rf "$work/host"
    cmake -G Ninja -S "$src/llvm" -B "$work/host" -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
        > "$work/host.log" 2>&1 || { tail -30 "$work/host.log"; return 1; }
    ninja -C "$work/host" -j"$jobs" llvm-tblgen >> "$work/host.log" 2>&1 ||
        { grep -A5 'error:' "$work/host.log" | head -40; return 1; }
}

# The libraries, for Vexa. (Vexa is no Linux, but CMake's "Linux" gives LLVM its Unix code;
# the SDK's compilers leave __linux__ undefined, and LLVM's checks find what libvexa has.)
build_llvm() {
    rm -rf "$work/build"
    cmake -G Ninja -S "$src/llvm" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER="$sdk/bin/vexa-cc" -DCMAKE_CXX_COMPILER="$sdk/bin/vexa-c++" \
        -DCMAKE_AR="$(command -v ar)" -DCMAKE_RANLIB="$(command -v ranlib)" \
        -DLLVM_TABLEGEN="$work/host/bin/llvm-tblgen" \
        -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_TARGET_ARCH=X86 \
        -DLLVM_HOST_TRIPLE=x86_64-unknown-linux-gnu \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-gnu \
        -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_PIC=ON \
        -DLLVM_ENABLE_THREADS=ON -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
        -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_BACKTRACES=OFF -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
        -DLLVM_ENABLE_UNWIND_TABLES=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TOOLS=OFF \
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
        > "$work/llvm.log" 2>&1 || { tail -30 "$work/llvm.log"; return 1; }
    ninja -C "$work/build" -j"$jobs" -k 0 llvm-libraries >> "$work/llvm.log" 2>&1 ||
        { grep -B2 -A8 'error:' "$work/llvm.log" | head -80; return 1; }
}

# llvm-config's answers for Mesa's build.
build_config() {
    mkdir -p "$work/bin"
    libs=$(cd "$work/build/lib" && ls libLLVM*.a | sed 's/^lib\(.*\)\.a$/\1/' | tr '\n' ' ')
    cat > "$work/bin/llvm-config" <<EOF
#!/bin/sh
# llvm-config for the LLVM built for Vexa in $work (tools/build-llvm-vexa.sh).
out=""
for arg in "\$@"; do
    case \$arg in
    --version) out="\$out $version" ;;
    --prefix) out="\$out $work/build" ;;
    --bindir) out="\$out $work/bin" ;;
    --libdir) out="\$out $work/build/lib" ;;
    --includedir) out="\$out $src/llvm/include" ;;
    --cmakedir) out="\$out $work/build/lib/cmake/llvm" ;;
    --cppflags) out="\$out -I$src/llvm/include -I$work/build/include -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS" ;;
    --cflags) out="\$out -I$src/llvm/include -I$work/build/include" ;;
    --cxxflags) out="\$out -I$src/llvm/include -I$work/build/include -std=c++17 -fno-exceptions -fno-rtti -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS" ;;
    --ldflags) out="\$out -L$work/build/lib" ;;
    --libs) out="\$out -Wl,--start-group $(for l in $libs; do printf -- '-l%s ' "$l"; done)-Wl,--end-group" ;;
    --libnames) out="\$out $(for l in $libs; do printf 'lib%s.a ' "$l"; done)" ;;
    --libfiles) out="\$out $(for l in $libs; do printf '%s/build/lib/lib%s.a ' "$work" "$l"; done)" ;;
    --system-libs) ;;
    --components) out="\$out all all-targets analysis asmparser asmprinter bitreader bitwriter codegen core coroutines engine executionengine instcombine ipo irreader linker mc mcdisassembler mcjit mcparser native nativecodegen object orcjit passes runtimedyld scalaropts selectiondag support target transformutils vectorize x86 x86asmparser x86codegen x86desc x86disassembler x86info" ;;
    --targets-built) out="\$out X86" ;;
    --host-target) out="\$out x86_64-unknown-linux-gnu" ;;
    --build-mode) out="\$out Release" ;;
    --assertion-mode) out="\$out OFF" ;;
    --has-rtti) out="\$out NO" ;;
    --shared-mode) out="\$out static" ;;
    --link-static | --link-shared | --ignore-libllvm) ;;
    -*) ;;
    esac
done
echo \$out
EOF
    chmod +x "$work/bin/llvm-config"
}

step source
step tblgen
step llvm
step config
log "done: $work/bin/llvm-config"
