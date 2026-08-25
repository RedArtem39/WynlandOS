#!/bin/bash
# WynlandOS ported-userspace rebuild from pristine upstream sources,
# replicating the Phase 13-19 flags recorded in external_src/*/build_musl/config.log
set -e
REPO=/mnt/c/Users/User/Desktop/WynlandOs
TOOL=$REPO/tools/x86_64-linux-musl-cross/bin
CCW=$REPO/tools/musl-cc-static
OUTSRC=~/psrc
OUT=$REPO/build/ports
BUILDS=~/pbuilds
mkdir -p "$OUTSRC" "$OUT" "$BUILDS"
# tools/musl-cc-static carries CRLF shebang (checked out on Windows);
# sanitize a runnable copy in the SAME directory so its $(dirname $0)
# toolchain resolution keeps working.
CCW=$REPO/tools/.musl-cc-static-lf
sed 's/\r$//' "$REPO/tools/musl-cc-static" > "$CCW"
chmod +x "$CCW"
cd "$OUTSRC"

dl() { [ -f "$1" ] || wget -q "$2" -O "$1"; }

echo "== downloading =="
dl nano-7.2.tar.gz     https://nano-editor.org/dist/v7/nano-7.2.tar.gz &
dl ncurses-6.4.tar.gz  https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz &
dl libressl-3.9.2.tar.gz https://cdn.openbsd.org/pub/LibreSSL/libressl-3.9.2.tar.gz &
dl curl-8.11.1.tar.xz  https://curl.se/download/curl-8.11.1.tar.xz &
dl cmake-3.28.3.tar.gz https://cmake.org/files/v3.28/cmake-3.28.3.tar.gz &
wait
ls -la *.tar.*

echo "== extracting =="
[ -d nano-7.2 ]    || tar xzf nano-7.2.tar.gz
[ -d ncurses-6.4 ] || tar xzf ncurses-6.4.tar.gz
[ -d libressl-3.9.2 ] || tar xzf libressl-3.9.2.tar.gz
[ -d curl-8.11.1 ] || tar xJf curl-8.11.1.tar.xz
[ -d cmake-3.28.3 ] || tar xzf cmake-3.28.3.tar.gz

echo "== ncurses (out-of-tree, musl) =="
mkdir -p "$BUILDS/ncurses-b" && cd "$BUILDS/ncurses-b"
CC="$CCW" "$OUTSRC/ncurses-6.4/configure" \
  --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu --prefix=/usr \
  --without-cxx --without-cxx-binding --without-ada --without-tests \
  --without-progs --disable-database --enable-termcap \
  --disable-shared --enable-static >/dev/null
make -j"$(nproc)" >/dev/null
make -j"$(nproc)" install DESTDIR="$BUILDS/sysroot" >/dev/null
find "$BUILDS/sysroot" -name 'libncurses*.a' | head -2

echo "== nano (exact Phase flags) =="
mkdir -p "$BUILDS/nano-b" && cd "$BUILDS/nano-b"
CC="$CCW" \
CPPFLAGS="-I$BUILDS/sysroot/usr/include" \
LDFLAGS="-L$BUILDS/sysroot/usr/lib" \
LIBS="-lncurses" \
  "$OUTSRC/nano-7.2/configure" \
  --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
  --disable-nls --prefix=/usr >/dev/null
make -j"$(nproc)" >/dev/null || true
NANO_BIN=$(find "$BUILDS/nano-b" -name nano -type f | head -1)
if [ -z "$NANO_BIN" ]; then echo "NANO BUILD FAILED"; exit 1; fi
cp "$NANO_BIN" "$OUT/nano"

echo "== LibreSSL =="
mkdir -p "$BUILDS/libressl-b" && cd "$BUILDS/libressl-b"
CC="$CCW" "$OUTSRC/libressl-3.9.2/configure" \
  --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
  --prefix=/usr --disable-shared --disable-tests >/dev/null
make -j"$(nproc)" >/dev/null
make -j"$(nproc)" install DESTDIR="$BUILDS/sysroot" >/dev/null
ls "$BUILDS/sysroot/usr/lib" | head

echo "== zlib (into sysroot) =="
cd "$OUTSRC"
dl zlib-1.3.1.tar.gz https://zlib.net/fossils/zlib-1.3.1.tar.gz
[ -d zlib-1.3.1 ] || tar xzf zlib-1.3.1.tar.gz
mkdir -p "$BUILDS/zlib-b" && cd "$BUILDS/zlib-b"
CC="$CCW" CHOST=x86_64-linux-musl "$OUTSRC/zlib-1.3.1/configure" --static >/dev/null
make -j"$(nproc)" >/dev/null
make install DESTDIR="$BUILDS/sysroot" >/dev/null

echo "== curl =="
mkdir -p "$BUILDS/curl-b" && cd "$BUILDS/curl-b"
CC="$CCW" \
CPPFLAGS="-I$BUILDS/sysroot/usr/include" \
LDFLAGS="-L$BUILDS/sysroot/usr/lib" \
"$OUTSRC/curl-8.11.1/configure" \
  --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
  --prefix=/usr --disable-shared --enable-static \
  --with-openssl --with-zlib \
  --without-libpsl --without-libidn2 --without-nghttp2 --without-brotli \
  --without-zstd --without-libssh2 --without-librtmp \
  --disable-ldap --disable-ldaps --disable-manual --disable-ftp --disable-file \
  --disable-smtp --disable-pop3 --disable-imap --disable-smb --disable-gopher \
  --disable-telnet --disable-dict --disable-tftp --disable-mqtt >/dev/null
make -j"$(nproc)" -C lib >/dev/null
make -j"$(nproc)" -C src >/dev/null
cp src/curl "$OUT/curl"

echo "== pkgconf =="
PVER=$(wget -qO- https://distfiles.ariadne.space/pkgconf/ | grep -oE 'pkgconf-[0-9.]+\.tar\.xz' | sort -V | tail -1)
echo "latest: $PVER"
dl "$PVER" "https://distfiles.ariadne.space/pkgconf/$PVER"
PDIR="${PVER%.tar.xz}"
[ -d "$PDIR" ] || tar xJf "$PVER"
mkdir -p "$BUILDS/pkgconf-b" && cd "$BUILDS/pkgconf-b"
CC="$CCW" "$OUTSRC/$PDIR/configure" \
  --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
  --prefix=/usr --disable-shared --enable-static >/dev/null
make -j"$(nproc)" >/dev/null
cp pkgconf "$OUT/pkgconf"

echo "== cmake modules from pristine tarball =="
rm -rf "$OUT/cmake-share"
mkdir -p "$OUT/cmake-share/share"
cp -r "$OUTSRC/cmake-3.28.3/Modules"    "$OUT/cmake-share/share/cmake-3.28_Modules"
cp -r "$OUTSRC/cmake-3.28.3/Templates"  "$OUT/cmake-share/share/cmake-3.28_Templates"
cp "$REPO/tools/cmake-wynlandos" "$OUT/cmake"
chmod +x "$OUT/cmake"

echo "== CA bundle =="
cp /etc/ssl/certs/ca-certificates.crt "$OUT/cert.pem" && wc -c "$OUT/cert.pem"

echo "== results =="
for f in nano curl pkgconf cmake; do
  file "$OUT/$f" | sed 's/, BuildID[^,]*//'
done
ls -la "$OUT"
