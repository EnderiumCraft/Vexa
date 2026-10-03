#!/bin/sh
# A C++ compiler for musl with a C++ runtime: the host g++ with musl-gcc's
# settings, and LLVM's libc++, libc++abi and libunwind built for musl (by
# tools/build-mesa.sh, in $LIBCXX_ROOT/usr). libc++'s headers come before
# musl's (-I is searched before system directories), as its wrappers need.
# musl-gcc's link settings leave out --eh-frame-hdr, which exceptions need.
# Before the runtime exists (while building it), LIBCXX_ROOT is unset.
link=1
for arg in "$@"; do
    case "$arg" in
        -c | -E | -S | -M | -MM) link=0 ;;
    esac
done
set -- -specs /usr/lib/x86_64-linux-musl/musl-gcc.specs -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -nostdinc++ -nostdlib++ "$@"
if [ -n "${LIBCXX_ROOT:-}" ]; then
    set -- -I"$LIBCXX_ROOT/usr/include/c++/v1" "$@"
    if [ $link = 1 ]; then
        set -- "$@" -Wl,--eh-frame-hdr -L"$LIBCXX_ROOT/usr/lib" \
            -Wl,-rpath-link,"$LIBCXX_ROOT/usr/lib" -lc++ -lc++abi -lunwind
    fi
elif [ $link = 1 ]; then
    set -- "$@" -Wl,--eh-frame-hdr
fi
exec g++ "$@"
