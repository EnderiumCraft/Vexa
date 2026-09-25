#!/bin/sh
# Builds the tree that becomes /linux: what Linux programs see as /lib, /bin,
# /usr/bin... (the Linux subsystem looks there first).
# Usage: tools/make-linux-root.sh <dir> <musl libc.so> <busybox> <busybox.links> <bash>
#                                 <coreutils> <coreutils programs.txt> <python root>
#                                 [test programs for /usr/bin...]
set -eu
root=$1 libc=$2 busybox=$3 links=$4 bash=$5 coreutils=$6 coreutils_programs=$7 python=$8
shift 8
rm -rf "$root"
mkdir -p "$root/lib" "$root/bin" "$root/sbin" "$root/usr/bin" "$root/usr/sbin" "$root/etc"

# musl's libc.so is also its dynamic loader.
cp "$libc" "$root/lib/ld-musl-x86_64.so.1"

cp "$busybox" "$root/bin/busybox"
cp "$bash" "$root/bin/bash"
for program in "$@"; do
    cp "$program" "$root/usr/bin/"
done

# One link per BusyBox command. The links are relative, so they work wherever
# the tree is (absolute ones would point outside /linux).
while read -r path; do
    case "$(dirname "$path")" in
        /bin) target=busybox ;;
        /sbin) target=../bin/busybox ;;
        /usr/bin | /usr/sbin) target=../../bin/busybox ;;
        *) continue ;;
    esac
    [ -e "$root$path" ] || ln -s "$target" "$root$path"
done < "$links"

# GNU coreutils take over the commands they have (ls, cat, sort...); BusyBox
# keeps the rest.
cp "$coreutils" "$root/usr/bin/coreutils"
while read -r program; do
    rm -f "$root/bin/$program" "$root/usr/bin/$program"
    ln -s ../usr/bin/coreutils "$root/bin/$program"
done < "$coreutils_programs"

# Python (its /usr/bin/python3 and /usr/lib/python3.x).
cp -a "$python/usr/." "$root/usr/"

# One user so far: root.
echo 'root:x:0:0:root:/root:/bin/sh' > "$root/etc/passwd"
echo 'root:x:0:' > "$root/etc/group"
printf '127.0.0.1 localhost\n::1 localhost\n' > "$root/etc/hosts"
mkdir -p "$root/root"

cat > "$root/etc/os-release" <<'RELEASE'
NAME="Vexa Linux subsystem"
ID=vexa
PRETTY_NAME="Vexa (Linux subsystem)"
RELEASE
