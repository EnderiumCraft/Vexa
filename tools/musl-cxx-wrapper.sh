#!/bin/sh
# A C++ compiler for musl, for code that doesn't need a C++ runtime library
# (HarfBuzz, which avoids exceptions, RTTI and libstdc++ on purpose): the
# host g++ with musl-gcc's settings, libstdc++'s headers (templates only),
# and no libstdc++ when linking. No _FORTIFY_SOURCE: musl has no __*_chk.
version=$(g++ -dumpversion)
exec g++ -specs /usr/lib/x86_64-linux-musl/musl-gcc.specs -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -isystem "/usr/include/c++/$version" -isystem "/usr/include/x86_64-linux-gnu/c++/$version" \
    '-D__GLIBC_PREREQ(major, minor)=0' -nostdlib++ "$@"
