#!/bin/sh
# Builds the tree that becomes /linux: what Linux programs see as /lib, /bin,
# /usr/bin... (the Linux subsystem looks there first).
# Usage: tools/make-linux-root.sh <dir> <musl libc.so> <busybox> <busybox.links> <bash>
#                                 <coreutils> <coreutils programs.txt> <python root>
#                                 <X11 sysroot> [test programs for /usr/bin...]
set -eu
root=$1 libc=$2 busybox=$3 links=$4 bash=$5 coreutils=$6 coreutils_programs=$7 python=$8
x11=$9
shift 9
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

# X (tools/build-x11.sh): the shared libraries, the Xvexa server, xkbcomp and
# the keyboard descriptions, xterm, and a few terminal descriptions.
cp -a "$x11"/usr/lib/*.so* "$root/usr/lib/"
cp -a "$x11/usr/lib/X11" "$root/usr/lib/"
for program in Xvexa xkbcomp xterm resize tput; do
    rm -f "$root/usr/bin/$program" # Not through a BusyBox link: that would overwrite BusyBox.
    cp "$x11/usr/bin/$program" "$root/usr/bin/"
done
mkdir -p "$root/usr/share/X11"
cp -a "$x11/usr/share/X11/locale" "$x11/usr/share/X11/XErrorDB" "$root/usr/share/X11/"
# A real directory: an absolute link would point outside /linux.
cp -a "$x11/usr/share/xkeyboard-config-2" "$root/usr/share/X11/xkb"
cp "$(dirname "$0")/linux-files/xsession" "$root/usr/bin/xsession"
for entry in x/xterm x/xterm-256color x/xterm-color v/vt100 v/vt220 l/linux d/dumb; do
    mkdir -p "$root/usr/share/terminfo/$(dirname $entry)"
    cp -L "$x11/usr/share/terminfo/$entry" "$root/usr/share/terminfo/$entry"
done

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
