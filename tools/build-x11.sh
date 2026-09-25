#!/bin/sh
# Builds the X Window System for Vexa's Linux subsystem from source, with musl:
# the libraries (libX11 and friends), Xvexa (an X server that shows its screen
# in a window on the Vexa desktop; see third_party/xvexa), xkbcomp and the
# keyboard data, fonts (FreeType, fontconfig, Xft and DejaVu), and xterm.
#
# Usage: tools/build-x11.sh <sources list> <tarballs> <work dir> <sysroot> <linux headers>
#
# The sources list (third_party/x11-sources.txt) has a SHA-256 and a URL per
# line; missing tarballs are downloaded into <tarballs> and checked.
#
# Each package is configured with --prefix=/usr and installed into <sysroot>
# (a staging copy of /usr); the next packages build against what's there.
# tools/make-linux-root.sh then copies what programs need at run time. Each
# finished package leaves a stamp in <work dir>, so a rerun picks up where it
# stopped.
set -eu

sources=$(realpath "$1")
mkdir -p "$2"
tarballs=$(realpath "$2")
work=$(realpath -m "$3")
sysroot=$(realpath -m "$4")
headers=$(realpath "$5")
here=$(dirname "$(realpath "$0")")
vexa=$(dirname "$here")
jobs=$(nproc)

mkdir -p "$work" "$sysroot"

# Only musl and what's built here: no host headers or libraries.
export MUSL_CC="${MUSL_CC:-musl-gcc}"
export CC="$here/musl-cc-wrapper.sh"
export CXX="$here/musl-cxx-wrapper.sh" # Only for C++ that needs no C++ runtime (HarfBuzz).
export CFLAGS="-O2 -fPIC -I$sysroot/usr/include -isystem $headers"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-I$sysroot/usr/include -isystem $headers"
export LDFLAGS="-L$sysroot/usr/lib -Wl,-rpath-link,$sysroot/usr/lib"
export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
export ACLOCAL_PATH="$sysroot/usr/share/aclocal"

log() {
    printf '[x11] %s\n' "$*"
}

# Downloads what's missing, and checks every tarball.
grep -v '^#' "$sources" | while read -r sha256 url; do
    file="$tarballs/$(basename "$url")"
    if [ ! -f "$file" ]; then
        log "downloading $(basename "$url")"
        curl -fsSL -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sha256  $file" | sha256sum -c --quiet
done

# unpack <tarball> <directory it makes>: a fresh copy of the source in $work.
unpack() {
    rm -rf "$work/$2"
    tar -xf "$tarballs/$1" -C "$work"
    cd "$work/$2"
}

# Libtool archives point at /usr/lib, which would mean the host's libraries.
tidy() {
    find "$sysroot" -name '*.la' -delete
}

autotools() {
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --disable-static --enable-shared "$@" > configure.log 2>&1 ||
        { tail -30 configure.log; return 1; }
    make -j"$jobs" > build.log 2>&1 || { tail -40 build.log; return 1; }
    make install DESTDIR="$sysroot" > install.log 2>&1 || { tail -20 install.log; return 1; }
    tidy
}

mesonbuild() {
    meson setup _build --prefix=/usr --libdir=lib --buildtype=release \
        -Ddefault_library=shared "$@" > configure.log 2>&1 ||
        { tail -40 configure.log; return 1; }
    ninja -C _build -j"$jobs" > build.log 2>&1 || { tail -40 build.log; return 1; }
    DESTDIR="$sysroot" meson install -C _build --no-rebuild > install.log 2>&1 ||
        { tail -20 install.log; return 1; }
    tidy
}

# package <name> [files...]: builds it unless its stamp is there (and newer
# than the files).
package() {
    name=$1
    shift
    if [ -f "$work/.done-$name" ] &&
        { [ $# -eq 0 ] || [ -z "$(find "$@" -newer "$work/.done-$name")" ]; }; then
        return
    fi
    set -- "$name"
    log "$1"
    "build_$1"
    touch "$work/.done-$1"
}

build_xorgproto() {
    unpack xorgproto_2024.1.orig.tar.gz xorgproto-2024.1
    mesonbuild -Dlegacy=false
}

build_libxau() {
    unpack libxau_1.0.11.orig.tar.gz libXau-1.0.11
    autotools
}

build_libmd() {
    unpack libmd_1.1.0.orig.tar.xz libmd-1.1.0
    autotools
}

build_xcbproto() {
    # Installed straight into the sysroot (not staged under /usr): libxcb
    # reads its XML and Python files at build time, from the paths in its .pc.
    unpack xcb-proto_1.17.0.orig.tar.gz xcb-proto-1.17.0
    ./configure --prefix="$sysroot/usr" > configure.log 2>&1
    make install > install.log 2>&1
    # PKG_CONFIG_SYSROOT_DIR would add the sysroot a second time.
    sed -i "s|^prefix=.*|prefix=/usr|; s|$sysroot||g" "$sysroot/usr/share/pkgconfig/xcb-proto.pc"
    sed -i "s|^xcbincludedir=.*|xcbincludedir=$sysroot/usr/share/xcb|" \
        "$sysroot/usr/share/pkgconfig/xcb-proto.pc"
}

build_libxcb() {
    unpack libxcb_1.17.0.orig.tar.gz libxcb-1.17.0
    pythondir=$(find "$sysroot/usr/lib" -maxdepth 3 -name site-packages -type d | head -1)
    autotools --without-doxygen --disable-devel-docs \
        XCBPROTO_XCBINCLUDEDIR="$sysroot/usr/share/xcb" XCBPROTO_XCBPYTHONDIR="$pythondir"
}

build_xtrans() {
    unpack xtrans_1.6.0.orig.tar.gz xtrans-1.6.0
    autotools --disable-docs
}

build_libx11() {
    unpack libx11_1.8.13.orig.tar.gz libX11-1.8.13
    autotools --disable-specs --without-xmlto --without-fop --disable-xf86bigfont
}

build_libxext() {
    unpack libxext_1.3.4.orig.tar.gz libXext-1.3.4
    autotools --disable-specs --without-xmlto --without-fop
}

build_zlib() {
    unpack zlib_1.3.dfsg.orig.tar.xz zlib-1.3.dfsg
    ./configure --prefix=/usr --shared > configure.log 2>&1
    make -j"$jobs" > build.log 2>&1
    make install DESTDIR="$sysroot" > install.log 2>&1
    rm -f "$sysroot/usr/lib/libz.a"
}

build_libfontenc() {
    unpack libfontenc_1.1.8.orig.tar.gz libfontenc-1.1.8
    autotools --with-fontrootdir=/usr/share/fonts/X11
}

build_libxfont2() {
    unpack libxfont_2.0.6.orig.tar.gz libXfont2-2.0.6
    autotools --disable-freetype --disable-devel-docs --without-xmlto --without-fop \
        --without-bzip2
}

build_pixman() {
    unpack pixman_0.44.0.orig.tar.gz pixman-0.44.0
    mesonbuild -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
        -Dopenmp=disabled
}

build_libxkbfile() {
    unpack libxkbfile_1.1.0.orig.tar.gz libxkbfile-1.1.0
    autotools
}

build_xkbcomp() {
    unpack x11-xkb-utils_7.7+9build1.tar.xz x11-xkb-utils-7.7+9build1
    cd xkbcomp
    autotools
}

build_xkeyboardconfig() {
    unpack xkeyboard-config_2.46.orig.tar.xz xkeyboard-config-2.46
    mesonbuild -Dxorg-rules-symlinks=true
}

build_xserver() {
    unpack xorg-server_21.1.24.orig.tar.gz xorg-server-21.1.24
    # Xvexa lives next to Xephyr in kdrive.
    cp -R "$vexa/third_party/xvexa" hw/kdrive/vexa
    printf "\noption('xvexa', type: 'boolean', value: false, description: 'Xvexa: an X server in a Vexa desktop window')\n" >> meson_options.txt
    sed -i "s/^if get_option('xephyr')$/if get_option('xephyr') or get_option('xvexa')/" hw/meson.build
    sed -i "s/^subdir('ephyr')$/if get_option('xephyr')\n    subdir('ephyr')\nendif\nif get_option('xvexa')\n    subdir('vexa')\nendif/" \
        hw/kdrive/meson.build
    mesonbuild -Dxorg=false -Dxephyr=false -Dxnest=false -Dxvfb=false \
        -Dxquartz=false -Dxwin=false -Dxvexa=true \
        -Dudev=false -Dudev_kms=false -Dhal=false -Dsystemd_logind=false \
        -Dglamor=false -Dglx=false -Ddri1=false -Ddri2=false -Ddri3=false -Ddrm=false \
        -Dxdmcp=false -Dxdm-auth-1=false -Dsecure-rpc=false -Dxselinux=false \
        -Dxcsecurity=false -Dmitshm=false -Dxv=false -Dxvmc=false -Ddpms=false \
        -Dlisten_tcp=false -Dlisten_unix=true -Dlisten_local=false \
        -Dsha1=libmd -Dlibunwind=false -Ddocs=false -Ddevel-docs=false \
        -Dxkb_dir=/usr/share/X11/xkb -Dxkb_bin_dir=/usr/bin \
        -Dxkb_output_dir=/tmp -Ddefault_font_path=built-ins
}

build_freetype() {
    unpack freetype_2.13.2+dfsg.orig.tar.xz freetype-2.13.2
    autotools --with-zlib=yes --with-bzip2=no --with-png=no --with-harfbuzz=no \
        --with-brotli=no
}

build_expat() {
    unpack expat-2.6.1.tar.xz expat-2.6.1
    autotools --without-docbook --without-examples --without-tests --without-xmlwf
}

build_fontconfig() {
    unpack fontconfig_2.15.0.orig.tar.xz fontconfig-2.15.0
    # Fonts in /usr/share/fonts; the cache in /var/cache/fontconfig (made
    # when a program first needs it, as the file system starts empty).
    autotools --disable-docs --disable-nls --disable-cache-build \
        --with-default-fonts=/usr/share/fonts --with-cache-dir=/var/cache/fontconfig
}

build_libxrender() {
    unpack libxrender_0.9.12.orig.tar.gz libXrender-0.9.12
    autotools
}

build_libxft() {
    unpack xft_2.3.6.orig.tar.gz libXft-2.3.6
    autotools
}

build_dejavu() {
    unpack dejavu-fonts-ttf-2.37.tar.bz2 dejavu-fonts-ttf-2.37
    mkdir -p "$sysroot/usr/share/fonts/dejavu" "$sysroot/etc/fonts/conf.d"
    cp ttf/DejaVuSans.ttf ttf/DejaVuSans-Bold.ttf ttf/DejaVuSansMono.ttf \
        ttf/DejaVuSansMono-Bold.ttf ttf/DejaVuSerif.ttf ttf/DejaVuSerif-Bold.ttf \
        LICENSE "$sysroot/usr/share/fonts/dejavu/"
    cp fontconfig/57-dejavu-sans.conf fontconfig/57-dejavu-sans-mono.conf \
        fontconfig/57-dejavu-serif.conf "$sysroot/etc/fonts/conf.d/"
}

build_libice() {
    unpack libice_1.1.1.orig.tar.gz libICE-1.1.1
    autotools --disable-docs --disable-specs --without-xmlto --without-fop
}

build_libsm() {
    unpack libsm_1.2.4.orig.tar.gz libSM-1.2.4
    autotools --without-libuuid --disable-docs --without-xmlto --without-fop
}

build_libxt() {
    unpack libxt_1.2.1.orig.tar.gz libXt-1.2.1
    autotools --disable-specs --without-xmlto --without-fop
}

build_libxmu() {
    unpack libxmu_1.1.3.orig.tar.gz libXmu-1.1.3
    autotools --disable-docs --without-xmlto --without-fop
}

build_libxpm() {
    # Just the library: its sample programs want gettext's msgfmt.
    unpack libxpm_3.5.17.orig.tar.gz libXpm-3.5.17
    ./configure --prefix=/usr --disable-static --disable-open-zfile --disable-tests \
        > configure.log 2>&1
    for dir in include src; do
        make -C $dir -j"$jobs" install DESTDIR="$sysroot" > build.log 2>&1 ||
            { tail -30 build.log; return 1; }
    done
    make install-pkgconfigDATA DESTDIR="$sysroot" > install.log 2>&1
    tidy
}

build_libxaw() {
    unpack libxaw_1.0.16.orig.tar.gz libXaw-1.0.16
    autotools --disable-specs --without-xmlto --without-fop --disable-xaw6
}

build_ncurses() {
    unpack ncurses_6.6+20251231.orig.tar.gz ncurses-6.6-20251231
    autotools --with-shared --without-normal --without-debug --without-cxx \
        --without-cxx-binding --without-ada --without-manpages --without-tests \
        --with-termlib --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig \
        --with-default-terminfo-dir=/usr/share/terminfo --disable-stripping
}

build_xterm() {
    unpack xterm_330.orig.tar.gz xterm-330
    # xterm treats Linux without glibc as an old BSD (searching /dev/ptyXX)
    # unless configure's pty tests pass on the build machine. musl has
    # openpty and /dev/pts like glibc: use them, and the same pty handling.
    sed -i 's/(defined(__GLIBC__) \&\& !defined(USE_USG_PTYS))/(defined(__linux__) \&\& !defined(__GLIBC__)) || &/' main.c
    sed -i 's/(defined(linux) \&\& defined(__GLIBC__) \&\& (__GLIBC__ >= 2) \&\& (__GLIBC_MINOR__ >= 1))/defined(linux)/' ptyx.h
    sed -i '0,/^#if (defined (__GLIBC__) \&\& ((__GLIBC__ > 2) || (__GLIBC__ == 2) \&\& (__GLIBC_MINOR__ >= 1)))$/s//#if defined(linux) \/* no handshake *\//' ptyx.h
    grep -q 'defined(__linux__) && !defined(__GLIBC__)' main.c
    # configure runs test programs linked with ncurses' libtinfow, found
    # through an rpath (which means nothing on Vexa, where libraries are in
    # /usr/lib anyway).
    LDFLAGS="$LDFLAGS -Wl,-rpath,$sysroot/usr/lib" autotools --enable-freetype --without-xinerama --disable-setuid --disable-setgid \
        --without-utempter --disable-luit --enable-256-color --disable-tek4014 \
        --with-terminal-type=xterm LIBS=-ltinfow
}

# ---- The GTK 3 stack ----

build_libxfixes() {
    unpack libxfixes_6.0.0.orig.tar.gz libXfixes-6.0.0
    autotools
}

build_libxi() {
    unpack libxi_1.8.1.orig.tar.gz libXi-1.8.1
    autotools --disable-docs --disable-specs --without-xmlto --without-fop --without-asciidoc
}

build_libxrandr() {
    unpack libxrandr_1.5.2.orig.tar.gz libXrandr-1.5.2
    autotools
}

build_libxcursor() {
    unpack libxcursor_1.2.1.orig.tar.gz libXcursor-1.2.1
    autotools
}

build_libxdamage() {
    unpack libxdamage_1.1.6.orig.tar.gz libXdamage-1.1.6
    autotools
}

build_libxcomposite() {
    unpack libxcomposite_0.4.5.orig.tar.gz libXcomposite-0.4.5
    autotools --disable-doc --without-xmlto
}

build_libxinerama() {
    unpack libxinerama_1.1.4.orig.tar.gz libXinerama-1.1.4
    autotools
}

build_libffi() {
    unpack libffi-3.4.6.tar.gz libffi-3.4.6
    autotools --disable-docs --disable-multi-os-directory
}

build_pcre2() {
    unpack pcre2_10.42.orig.tar.gz pcre2-10.42
    autotools --enable-unicode --enable-jit=no
}

build_glib() {
    unpack glib2.0_2.80.0.orig.tar.xz glib-2.80.0
    mesonbuild -Dtests=false -Dintrospection=disabled -Dnls=disabled -Dlibmount=disabled \
        -Dselinux=disabled -Dxattr=false -Dman-pages=disabled -Ddocumentation=false \
        -Dsysprof=disabled -Dlibelf=disabled -Ddtrace=false -Dsystemtap=false
    # Code generators the next packages run while building: those are the
    # build machine's (the same GLib version), not these musl programs.
    for tool in glib-compile-resources glib-compile-schemas glib-mkenums glib-genmarshal \
        gdbus-codegen; do
        ln -sf "$(command -v $tool)" "$sysroot/usr/bin/$tool"
    done
}

build_libpng() {
    unpack libpng1.6_1.6.43.orig.tar.gz libpng-1.6.43
    autotools --disable-tests --disable-tools
}

build_fribidi() {
    unpack fribidi_1.0.13.orig.tar.xz fribidi-1.0.13
    mesonbuild -Ddocs=false -Dtests=false -Dbin=false
}

build_harfbuzz() {
    unpack harfbuzz_8.3.0.orig.tar.xz harfbuzz-8.3.0
    mesonbuild -Dtests=disabled -Ddocs=disabled -Dcairo=disabled -Dglib=enabled \
        -Dfreetype=enabled -Dicu=disabled -Dgobject=disabled -Dintrospection=disabled \
        -Dutilities=disabled -Dbenchmark=disabled
}

build_cairo() {
    unpack cairo_1.18.0.orig.tar.xz cairo-1.18.0
    mesonbuild -Dtests=disabled -Dxlib=enabled -Dxcb=enabled -Dpng=enabled \
        -Dfreetype=enabled -Dfontconfig=enabled -Dglib=enabled -Dzlib=enabled \
        -Dspectre=disabled -Dsymbol-lookup=disabled -Dgtk2-utils=disabled
}

build_pango() {
    unpack "pango1.0_1.52.1+ds.orig.tar.xz" pango-1.52.1
    mesonbuild -Dintrospection=disabled -Dxft=enabled -Dfreetype=enabled -Dcairo=enabled \
        -Dfontconfig=enabled -Dsysprof=disabled -Dlibthai=disabled -Dgtk_doc=false
}

build_gdkpixbuf() {
    unpack "gdk-pixbuf_2.42.10+dfsg.orig.tar.xz" gdk-pixbuf-2.42.10
    # The loaders are built in, so no loaders cache is needed.
    mesonbuild -Dpng=enabled -Djpeg=disabled -Dtiff=disabled -Dbuiltin_loaders=all -Dintrospection=disabled -Dman=false -Dgtk_doc=false \
        -Dinstalled_tests=false -Dtests=false -Dgio_sniffing=false
}

build_libepoxy() {
    # OpenGL is looked up when a program asks for it (there is none yet).
    unpack libepoxy_1.5.10.orig.tar.gz libepoxy-1.5.10
    mesonbuild -Dglx=yes -Degl=no -Dx11=true -Dtests=false -Ddocs=false
}

build_dbus() {
    # The library: GTK's accessibility bridge links with it. There is no bus
    # to talk to yet (NO_AT_BRIDGE=1 keeps it quiet).
    unpack dbus_1.14.10.orig.tar.xz dbus-1.14.10
    autotools --disable-systemd --without-x --disable-tests --disable-xml-docs \
        --disable-doxygen-docs --disable-selinux --disable-libaudit --disable-apparmor \
        --disable-ducktype-docs
}

build_atspi() {
    unpack at-spi2-core_2.52.0.orig.tar.xz at-spi2-core-2.52.0
    # libxml2 is only for its tests.
    sed -i "s/^libxml_dep = dependency('libxml-2.0', version: libxml_req_version)/libxml_dep = dependency('libxml-2.0', required: false)/" meson.build
    sed -i "s/^  subdir('tests')/  # subdir('tests')/" meson.build
    # Configure runs a test program that needs libdbus (found through an
    # rpath on the build machine; meaningless on Vexa).
    LDFLAGS="$LDFLAGS -Wl,-rpath,$sysroot/usr/lib" mesonbuild -Dx11=disabled -Dintrospection=disabled -Ddocs=false -Duse_systemd=false
}

build_gtk3() {
    unpack "gtk+3.0_3.24.41.orig.tar.xz" gtk+-3.24.41
    # The build runs gdk-pixbuf-pixdata (and some of its own programs): musl
    # programs, which musl's loader can run here with the sysroot's libraries.
    cat > "$work/run-musl" <<RUN
#!/bin/sh
exec /lib/ld-musl-x86_64.so.1 --library-path "$sysroot/usr/lib" "\$@"
RUN
    chmod +x "$work/run-musl"
    printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$work/run-musl" "$sysroot/usr/bin/gdk-pixbuf-pixdata" \
        > "$work/gdk-pixbuf-pixdata"
    chmod +x "$work/gdk-pixbuf-pixdata"
    export GDK_PIXBUF_PIXDATA="$work/gdk-pixbuf-pixdata"
    mesonbuild -Dx11_backend=true -Dwayland_backend=false -Dbroadway_backend=false \
        -Dprint_backends=file -Dcolord=no -Dintrospection=false -Ddemos=true \
        -Dexamples=false -Dtests=false -Dinstalled_tests=false -Dxinerama=yes \
        -Dgtk_doc=false -Dman=false -Dcloudproviders=false -Dtracker3=false
    # The settings schemas (GSettings), compiled once here.
    glib-compile-schemas "$sysroot/usr/share/glib-2.0/schemas"
}

build_hicolor() {
    unpack hicolor-icon-theme_0.17.orig.tar.xz hicolor-icon-theme-0.17
    ./configure --prefix=/usr > configure.log 2>&1
    make install DESTDIR="$sysroot" > install.log 2>&1
}


package xorgproto
package libxau
package libmd
package xcbproto
package libxcb
package xtrans
package libx11
package libxext
package zlib
package libfontenc
package libxfont2
package pixman
package libxkbfile
package xkbcomp
package xkeyboardconfig
package xserver "$vexa/third_party/xvexa"
package freetype
package expat
package fontconfig
package libxrender
package libxft
package dejavu
package libice
package libsm
package libxt
package libxmu
package libxpm
package libxaw
package ncurses
package xterm
package libxfixes
package libxi
package libxrandr
package libxcursor
package libxdamage
package libxcomposite
package libxinerama
package libffi
package pcre2
package glib
package libpng
package fribidi
package harfbuzz
package cairo
package pango
package gdkpixbuf
package libepoxy
package dbus
package atspi
package gtk3
package hicolor

log "done"
