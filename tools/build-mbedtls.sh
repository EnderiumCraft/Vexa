#!/bin/sh
# Builds Mbed TLS (TLS 1.2 and 1.3, X.509, crypto) for Vexa, against libvexa:
#
#   tools/build-mbedtls.sh mbedtls-x.y.z.tar.bz2 WORK_DIR PREFIX CC CFLAGS...
#
# Its default configuration; randomness from /dev/urandom, certificates from
# files. PREFIX gets include/mbedtls, include/psa and lib/libmbedtls.a,
# libmbedx509.a and libmbedcrypto.a (and a pkg-config file, for the SDK).
set -e
TARBALL=$(realpath "$1")
WORK=$2
mkdir -p "$3"
PREFIX=$(realpath "$3")
CC=$4
shift 4
JOBS=$(nproc 2>/dev/null || echo 4)

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xjf "$TARBALL" -C "$WORK"
SRC=$(echo "$WORK"/mbedtls-*)
VERSION=${SRC##*/mbedtls-}
cd "$SRC"
mkdir -p obj
ls library/*.c | xargs -P "$JOBS" -I{} sh -c '
    cc=$1; shift
    "$cc" "$@" -Iinclude -Ilibrary -c "{}" -o "obj/$(basename "{}" .c).o"' sh "$CC" "$@"
# (The same split as Mbed TLS's own build: crypto, X.509, TLS.)
X509="x509 x509_create x509_crl x509_crt x509_csr x509write x509write_crt x509write_csr pkcs7"
TLS="debug mps_reader mps_trace net_sockets ssl_cache ssl_ciphersuites ssl_client ssl_cookie \
ssl_debug_helpers_generated ssl_msg ssl_ticket ssl_tls ssl_tls12_client ssl_tls12_server \
ssl_tls13_keys ssl_tls13_client ssl_tls13_server ssl_tls13_generic"
crypto=""
for o in obj/*.o; do
    name=$(basename "$o" .o)
    case " $X509 $TLS " in *" $name "*) ;; *) crypto="$crypto $o" ;; esac
done
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include" "$PREFIX/licenses"
# shellcheck disable=SC2086
ar rcs "$PREFIX/lib/libmbedcrypto.a" $crypto
ar rcs "$PREFIX/lib/libmbedx509.a" $(for n in $X509; do [ -f "obj/$n.o" ] && echo "obj/$n.o"; done)
ar rcs "$PREFIX/lib/libmbedtls.a" $(for n in $TLS; do [ -f "obj/$n.o" ] && echo "obj/$n.o"; done)
cp -R include/mbedtls include/psa "$PREFIX/include/"
cp LICENSE "$PREFIX/licenses/mbedtls.txt"
cat > "$PREFIX/lib/pkgconfig/mbedtls.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: mbedtls
Description: Mbed TLS (TLS, X.509 and crypto) for Vexa
Version: $VERSION
Libs: -L\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto
Cflags: -I\${includedir}
PC
echo "Mbed TLS $VERSION: $(ls obj | wc -l) files, in $PREFIX"
