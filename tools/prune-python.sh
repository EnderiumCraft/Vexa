#!/bin/sh
# Trims an installed Python (DESTDIR root) to what runs on Vexa: no test
# suite, GUI, build files, static library or extra optimization levels.
# Usage: tools/prune-python.sh <root> <version, e.g. 3.12>
set -eu
root=$1 version=$2
lib="$root/usr/lib/python$version"
rm -rf "$lib/test" "$lib/idlelib" "$lib/tkinter" "$lib/turtledemo" "$lib/ensurepip" \
    "$lib/lib2to3" "$lib/pydoc_data" "$lib/unittest/test" "$lib"/config-* \
    "$root/usr/include" "$root/usr/share" "$root/usr/lib/pkgconfig" "$root/usr/lib"/libpython*.a
rm -f "$root/usr/bin"/2to3* "$root/usr/bin"/idle3* "$root/usr/bin"/pydoc3* \
    "$root/usr/bin"/python3*-config
find "$lib" -name '*.opt-1.pyc' -delete -o -name '*.opt-2.pyc' -delete
strip "$root/usr/bin/python$version" "$lib"/lib-dynload/*.so
