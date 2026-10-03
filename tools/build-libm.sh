#!/bin/sh
# Builds musl's maths functions (src/math, and src/fenv for the floating-point
# environment) as position-independent objects in an archive, for libvexa.so.
# Where musl has an x86-64 version of a function, that one is used, as in
# musl's own build. libvexa's <math.h> is musl's (libvexa/include/math.h).
#
# Usage: tools/build-libm.sh <musl tarball> <work dir> <output archive>
set -eu
tarball=$(realpath "$1")
mkdir -p "$2" "$(dirname "$3")"
work=$(realpath "$2") out=$(realpath "$(dirname "$3")")/$(basename "$3")
cc=${CC:-cc}
rm -rf "$work" && mkdir -p "$work/obj"
tar -xzf "$tarball" -C "$work"
src=$(echo "$work"/musl-*)
mkdir -p "$src/obj/include/bits"
sed -f "$src/tools/mkalltypes.sed" "$src/arch/x86_64/bits/alltypes.h.in" \
    "$src/include/alltypes.h.in" > "$src/obj/include/bits/alltypes.h"
cp "$src/arch/x86_64/bits/syscall.h.in" "$src/obj/include/bits/syscall.h"
flags="-std=c99 -O2 -pipe -fPIC -ffreestanding -nostdinc -fno-stack-protector \
    -ffp-contract=off -fexcess-precision=standard -frounding-math -fno-builtin -w \
    -D_XOPEN_SOURCE=700 -I$src/arch/x86_64 -I$src/arch/generic -I$src/obj/src/internal \
    -I$src/src/include -I$src/src/internal -I$src/obj/include -I$src/include"
files=
for f in "$src"/src/math/*.c; do
    name=$(basename "$f" .c)
    for alt in c s S; do
        [ -f "$src/src/math/x86_64/$name.$alt" ] && f="$src/src/math/x86_64/$name.$alt" && break
    done
    files="$files $f"
done
# Only in the x86-64 directory (helpers of the assembly versions).
for f in "$src"/src/math/x86_64/*; do
    name=$(basename "$f"); name=${name%.*}
    [ -f "$src/src/math/$name.c" ] || files="$files $f"
done
for f in "$src"/src/fenv/*.c; do
    [ "$(basename "$f")" = fenv.c ] || files="$files $f"
done
files="$files $src/src/fenv/x86_64/fenv.s"
for f in $files; do
    name=$(basename "$f"); name=${name%.*}
    case "$f" in
        */x86_64/*) obj="$work/obj/x86_64-$name.o" ;;
        */fenv/*) obj="$work/obj/fenv-$name.o" ;;
        *) obj="$work/obj/$name.o" ;;
    esac
    $cc $flags -c "$f" -o "$obj"
done
rm -f "$out"
ar rcs "$out" "$work"/obj/*.o
echo "libm: $(ls "$work"/obj | wc -l) objects"
