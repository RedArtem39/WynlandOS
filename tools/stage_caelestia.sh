#!/bin/bash
# WynlandOS - the Caelestia desktop shell (Quickshell's), built against
# Ubuntu's Qt (the image's own), for Zerp 2.0 -- whose Hyprland IPC and
# wlr-layer-shell it talks to.
#
# Into a cache of its own: Caelestia's source at a pinned commit, the
# Ubuntu .debs its plugin builds with (unpacked beside Quickshell's, in
# tools/stage_quickshell.sh's cache), cava's core as libcava (the one
# library Ubuntu has no package of), the Material Symbols and Rubik fonts.
# build/caelestia_manifest.txt: the shell's QML in /etc/xdg/quickshell/
# caelestia (`quickshell -c caelestia`), its plugin, fonts, libraries.
#
# usage: tools/stage_caelestia.sh   (from the repo root, inside WSL; after
#                                    tools/stage_quickshell.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
QS_CACHE=${QS_CACHE:-$HOME/.cache/wynland-quickshell}
ROOT=$QS_CACHE/root                 # Qt and the rest, from stage_quickshell.sh
CACHE=${CAE_CACHE:-$HOME/.cache/wynland-caelestia}
SRC=$CACHE/src
BLD=$CACHE/build
PREFIX=$CACHE/prefix                # libcava
OUT=$REPO/build
CAE_URL=https://github.com/caelestia-dots/shell.git
CAE_COMMIT=a3a8fe5
CAVA_URL=https://github.com/LukashonakV/cava.git
CAVA_COMMIT=${CAVA_COMMIT:-}

[ -d "$ROOT/usr/lib/x86_64-linux-gnu/cmake/Qt6" ] || { echo "  CAE        run tools/stage_quickshell.sh first"; exit 1; }

PKGS="libqalculate-dev libqalculate23 libcln-dev libcln6 libmpfr-dev libgmp-dev libcurl4-gnutls-dev libxml2-dev
 libaubio-dev libaubio5 libfftw3-dev libfftw3-double3 libfftw3-single3 libsensors-dev libsensors5
 libqt6sql6 libqt6sql6-sqlite libqt6concurrent6 qt6-base-dev
 qt6-image-formats-plugins libwebp7 libwebpdemux2 libwebpmux3 libsharpyuv0"
mkdir -p "$CACHE/debs"
NEW=0
for p in $PKGS; do
    if ! ls "$CACHE/debs/${p}_"*.deb >/dev/null 2>&1; then
        (cd "$CACHE/debs" && apt-get download "$p" >/dev/null 2>&1) || echo "  CAE        (no package $p)"
        NEW=1
    fi
done
if [ $NEW = 1 ] || [ ! -f "$CACHE/.unpacked" ]; then
    for d in "$CACHE"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
    touch "$CACHE/.unpacked"
fi

# libcava: cava's core (cavacore.c, MIT), a library with its .pc
if [ ! -f "$PREFIX/lib/libcava.so" ]; then
    rm -rf "$CACHE/cava"
    git clone -q --depth 1 "$CAVA_URL" "$CACHE/cava"
    [ -n "$CAVA_COMMIT" ] && (cd "$CACHE/cava" && git fetch -q --depth 1 origin "$CAVA_COMMIT" && git checkout -q FETCH_HEAD)
    mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include/cava"
    gcc -O2 -fPIC -shared -Wl,-soname,libcava.so.0 -I"$CACHE/cava/include" -I"$ROOT/usr/include" -o "$PREFIX/lib/libcava.so.0" \
        "$CACHE/cava/cavacore.c" -L"$ROOT/usr/lib/x86_64-linux-gnu" -lfftw3 -lm
    ln -sf libcava.so.0 "$PREFIX/lib/libcava.so"
    cp "$CACHE/cava/include/cava/cavacore.h" "$PREFIX/include/cava/"
    cat > "$PREFIX/lib/pkgconfig/libcava.pc" <<EOF
prefix=$PREFIX
Name: libcava
Description: cava's core (WynlandOS build)
Version: 0.10
Libs: -L\${prefix}/lib -lcava
Cflags: -I\${prefix}/include
EOF
fi

if [ ! -d "$SRC/.git" ]; then git clone -q "$CAE_URL" "$SRC"; fi
(cd "$SRC" && { git cat-file -e "$CAE_COMMIT^{commit}" 2>/dev/null || git fetch -q origin; } && git checkout -q -f "$CAE_COMMIT")
# Caelestia wants Qt 6.11's QML; the image has 6.10 (Ubuntu's): shims
python3 tools/caelestia-qt610/apply.py "$SRC"

QTLIB=$ROOT/usr/lib/x86_64-linux-gnu
# the .pc files with /usr moved into the cache (as stage_quickshell.sh)
mkdir -p "$QS_CACHE/pc"
for pc in "$QTLIB"/pkgconfig/*.pc "$ROOT"/usr/share/pkgconfig/*.pc; do
    sed "s|=/usr\$|=$ROOT/usr|; s|=/usr/|=$ROOT/usr/|; s|-I/usr/|-I$ROOT/usr/|g; s|-L/usr/|-L$ROOT/usr/|g; s|-isystem /usr/include/mit-krb5||g" "$pc" > "$QS_CACHE/pc/$(basename "$pc")"
done
# and without include paths of packages not in the cache (curl's krb5,
# p11-kit: nothing here includes them, CMake refuses a missing one)
python3 - "$QS_CACHE/pc" <<'PY'
import os, re, sys
d = sys.argv[1]
for f in os.listdir(d):
    p = os.path.join(d, f)
    s = open(p).read()
    def keep(m):
        return m.group(0) if os.path.isdir(m.group(2)) else ""
    s = re.sub(r"(-I|-isystem\s+)(/[^\s]+)", keep, s)
    open(p, "w").write(s)
PY
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$QS_CACHE/pc:$QTLIB/pkgconfig:$ROOT/usr/share/pkgconfig
export LD_LIBRARY_PATH=$QTLIB:$PREFIX/lib
cmake -S "$SRC" -B "$BLD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DVERSION=1.0.0 -DGIT_REVISION="$CAE_COMMIT" -DDISTRIBUTOR=WynlandOS \
    -DCMAKE_PREFIX_PATH="$QTLIB/cmake;$ROOT/usr/share/cmake;$ROOT/usr/lib/cmake" \
    -DCMAKE_INSTALL_PREFIX=/usr -DINSTALL_LIBDIR=lib/caelestia \
    -DINSTALL_QMLDIR=lib/x86_64-linux-gnu/qt6/qml -DINSTALL_QSCONFDIR=/etc/xdg/quickshell/caelestia \
    -DCMAKE_INCLUDE_PATH="$ROOT/usr/include" -DCMAKE_LIBRARY_PATH="$QTLIB" \
    -DQT_QML_GENERATE_QMLLS_INI=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$QTLIB:$PREFIX/lib" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath-link,$QTLIB:$PREFIX/lib" \
    > "$CACHE/configure.log" 2>&1 || { tail -40 "$CACHE/configure.log"; exit 1; }
cmake --build "$BLD" -j "${CAE_JOBS:-4}" > "$CACHE/build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$CACHE/build.log"; exit 1; }
rm -rf "$CACHE/install"
DESTDIR="$CACHE/install" cmake --install "$BLD" > /dev/null
echo "  CAE        built Caelestia ($(git -C "$SRC" rev-parse --short HEAD))"

# M3Shapes (Material 3 shapes, Apache-2.0): a QML module Caelestia imports,
# at the commit Caelestia's flake pins
M3=$CACHE/m3shapes
if [ ! -d "$M3/.git" ]; then git clone -q https://github.com/soramanew/m3shapes.git "$M3"; fi
(cd "$M3" && git checkout -q 32ad9ce)
cmake -S "$M3" -B "$CACHE/m3build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QTLIB/cmake" -DCMAKE_INSTALL_PREFIX=/ -DINSTALL_QMLDIR=usr/lib/x86_64-linux-gnu/qt6/qml \
    -DQT_QML_GENERATE_QMLLS_INI=OFF -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath-link,$QTLIB" \
    > "$CACHE/m3configure.log" 2>&1 || { tail -30 "$CACHE/m3configure.log"; exit 1; }
cmake --build "$CACHE/m3build" -j "${CAE_JOBS:-4}" > "$CACHE/m3build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$CACHE/m3build.log"; exit 1; }
DESTDIR="$CACHE/install" cmake --install "$CACHE/m3build" > /dev/null
echo "  CAE        built M3Shapes"
# Qt's WebP image plugin (Caelestia's wallpapers are .webp)
IMGF=usr/lib/x86_64-linux-gnu/qt6/plugins/imageformats
mkdir -p "$CACHE/install/$IMGF"
cp "$ROOT/$IMGF/libqwebp.so" "$CACHE/install/$IMGF/"

# fonts: Material Symbols Rounded (Apache-2.0) and Rubik (OFL), from Google
FONTS=$CACHE/fonts
mkdir -p "$FONTS"
fetch() { [ -s "$FONTS/$2" ] || curl -fsSL -o "$FONTS/$2" "$1"; }
fetch "https://github.com/google/material-design-icons/raw/master/variablefont/MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf" MaterialSymbolsRounded.ttf
fetch "https://github.com/google/fonts/raw/main/ofl/rubik/Rubik%5Bwght%5D.ttf" Rubik.ttf

# the manifest
OTHER="ext2_manifest.txt"
for m in qt6 gst wpe base quickshell; do [ -f "$OUT/${m}_manifest.txt" ] && OTHER="$OTHER $OUT/${m}_manifest.txt"; done
python3 - "$CACHE/install" "$OUT/caelestia_manifest.txt" "$FONTS" "$PREFIX/lib" $OTHER <<'PY'
import os, subprocess, sys
inst, man, fonts, cavalib, others = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
have_d, have_f = set(), set()
for o in others:
    for line in open(o):
        p = line.split()
        if len(p) >= 2 and p[0] == "D": have_d.add(p[1])
        if len(p) >= 2 and p[0] in ("F", "Fo", "S"): have_f.add(p[1])
out = ["# generated by tools/stage_caelestia.sh -- do not edit"]
def d(path):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts) + 1):
        q = "/" + "/".join(parts[:i])
        if q not in have_d:
            have_d.add(q); out.append("D " + q)
elfs = []
for root, dirs, files in sorted(os.walk(inst)):
    rel = "/" + os.path.relpath(root, inst)
    if rel == "/.": continue
    d(rel)
    for f in sorted(files):
        src = os.path.join(root, f)
        if " " in f: continue
        if os.path.islink(src):
            out.append("S %s/%s %s" % (rel, f, os.readlink(src)))
            continue
        out.append("F %s/%s %s" % (rel, f, src))
        with open(src, "rb") as fh:
            if fh.read(4) == b"\x7fELF": elfs.append(src)
d("/usr/share/fonts/truetype/caelestia")
for f in sorted(os.listdir(fonts)):
    out.append("F /usr/share/fonts/truetype/caelestia/%s %s" % (f, os.path.join(fonts, f)))
out.append("F /lib64/libcava.so.0 " + os.path.join(cavalib, "libcava.so.0"))
have_f.add("/lib64/libcava.so.0")
for e in elfs:
    ldd = subprocess.run(["ldd", e], capture_output=True, text=True).stdout
    for line in ldd.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] == "=>" and p[2].startswith("/"):
            want = "/lib64/" + p[0]
            if want not in have_f and not p[2].startswith(inst):
                have_f.add(want); out.append("F %s %s" % (want, os.path.realpath(p[2])))
        elif "not found" in line and "caelestia" not in line and "/lib64/" + line.split()[0] not in have_f:
            print("  CAE        WARNING: %s: unresolved %s" % (os.path.basename(e), line.strip()))
open(man, "w").write("\n".join(out) + "\n")
print("  CAE        %s: %d lines" % (man, len(out)))
PY
