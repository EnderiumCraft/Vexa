#!/bin/sh
# Builds NetSurf (a small web browser: HTML, CSS, images, a little
# JavaScript) as a native Vexa program, with the SDK: its own libraries,
# then its framebuffer front end, drawing into a desktop window through
# libnsfb's Vexa surface (third_party/netsurf-vexa.patch), with FreeType
# text and curl (over Mbed TLS) for HTTP and HTTPS.
#
# Usage: tools/build-netsurf.sh <netsurf-all tarball> <SDK dir> <deps prefix>
#                               <work dir> <out dir>
# (<deps prefix>: from tools/build-netsurf-deps.sh. <out dir> gets the
# program, netsurf, and its resources, res/.)
set -eu
tarball=$(realpath "$1") sdk=$(realpath "$2") deps=$(realpath "$3")
mkdir -p "$4" "$5"
work=$(realpath "$4") out=$(realpath "$5")
here=$(dirname "$(realpath "$0")")
jobs=$(nproc)
vexa_cc="$sdk/bin/vexa-cc"

log() {
    printf '[netsurf] %s\n' "$*"
}

name=$(tar -tzf "$tarball" | head -1 | cut -d/ -f1)
rm -rf "${work:?}/$name" "$work/inst"
tar -xzf "$tarball" -C "$work"
cd "$work/$name"
patch -p1 -s < "$here/../third_party/netsurf-vexa.patch"
inst="$work/inst"
export PKG_CONFIG_PATH="$inst/lib/pkgconfig:$deps/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
export PATH="$inst/bin:$PATH"
# (In the environment, which NetSurf's makefiles add to.)
export CFLAGS="-I$deps/include" LDFLAGS="-L$deps/lib"
nsshared="$work/$name/buildsystem"

run() { # <what> <command>...: its output to a log, and the end of it on failure
    what=$1
    shift
    "$@" > "$work/$what.log" 2>&1 || { tail -40 "$work/$what.log"; exit 1; }
}

log "buildsystem, nsgenbind (for this machine)"
run buildsystem make -C buildsystem install PREFIX="$inst"
CFLAGS= LDFLAGS= run nsgenbind make -C nsgenbind install PREFIX="$inst" NSSHARED="$nsshared" CC=cc

for lib in libnslog libwapcaplet libparserutils libcss libhubbub libdom libnsbmp libnsgif \
        libnsutils libutf8proc libnspsl libsvgtiny libnsfb; do
    log "$lib"
    run "$lib" make -C "$lib" install -j"$jobs" PREFIX="$inst" NSSHARED="$nsshared" \
        CC="$vexa_cc" AR=ar COMPONENT_TYPE=lib-static Q= \
        WARNFLAGS='-Wall -Wno-error' NSFB_VEXA_AVAILABLE=yes NSFB_SDL_AVAILABLE=no \
        NSFB_XCB_AVAILABLE=no NSFB_VNC_AVAILABLE=no NSFB_WLD_AVAILABLE=no
done

log "netsurf (framebuffer front end)"
cat > netsurf/Makefile.config <<'CONFIG'
# Vexa: a window on the desktop, FreeType text, curl, PNG, JPEG, SVG, GIF,
# BMP, and Duktape for JavaScript.
NETSURF_FB_FRONTEND := vexa
NETSURF_FB_FONTLIB := freetype
NETSURF_USE_CURL := YES
NETSURF_USE_OPENSSL := NO
NETSURF_USE_PNG := YES
NETSURF_USE_JPEG := YES
NETSURF_USE_WEBP := NO
NETSURF_USE_NSSVG := YES
NETSURF_USE_ROSPRITE := NO
NETSURF_USE_VIDEO := NO
NETSURF_USE_DUKTAPE := YES
NETSURF_USE_HARU_PDF := NO
NETSURF_USE_LIBICONV_PLUG := NO
NETSURF_FB_FONTPATH := /share/fonts
NETSURF_FB_FONT_SANS_SERIF := DejaVuSans.ttf
NETSURF_FB_FONT_SANS_SERIF_BOLD := DejaVuSans-Bold.ttf
NETSURF_FB_FONT_SANS_SERIF_ITALIC := DejaVuSans.ttf
NETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD := DejaVuSans-Bold.ttf
NETSURF_FB_FONT_SERIF := DejaVuSans.ttf
NETSURF_FB_FONT_SERIF_BOLD := DejaVuSans-Bold.ttf
NETSURF_FB_FONT_MONOSPACE := DejaVuSansMono.ttf
NETSURF_FB_FONT_MONOSPACE_BOLD := DejaVuSansMono.ttf
NETSURF_FB_FONT_CURSIVE := DejaVuSans.ttf
NETSURF_FB_FONT_FANTASY := DejaVuSans.ttf
NETSURF_FRAMEBUFFER_RESOURCES := /apps/NetSurf.vxapp/Contents/Resources
NETSURF_FB_RESPATH := $${HOME}/.netsurf/:$${NETSURFRES}:/apps/NetSurf.vxapp/Contents/Resources
CONFIG
run netsurf make -C netsurf -j"$jobs" TARGET=framebuffer CC="$vexa_cc" HOST_CC=cc BUILD_CC=cc \
    PREFIX="$inst" NSSHARED="$nsshared" Q= WARNFLAGS='-Wall -Wno-error'
rm -rf "$out"
mkdir -p "$out/res"
strip --strip-unneeded -o "$out/netsurf" netsurf/nsfb
cp -RL netsurf/frontends/framebuffer/res/. "$out/res/"
cp netsurf/COPYING "$out/COPYING"
log "done: $out"
