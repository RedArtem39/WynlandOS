#!/bin/bash
# WynlandOS - build Quickshell (the QML shell toolkit Caelestia is made
# with) against Ubuntu's Qt -- the same Qt the image ships (stage_qt6.sh).
#
# Its source at a pinned commit, Ubuntu's development packages unpacked
# into a cache of their own (nothing installed on the host), a CMake build
# there; build/quickshell is the program. Off: what this system has no
# service for yet (polkit, greetd, X11, i3). PipeWire and PAM
# are on: Caelestia imports them (no PipeWire daemon: no sound there yet).
#
# usage: tools/stage_quickshell.sh       (from the repo root, inside WSL)
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
CACHE=${QS_CACHE:-$HOME/.cache/wynland-quickshell}
ROOT=$CACHE/root
SRC=$CACHE/src
BLD=$CACHE/build
OUT=$REPO/build
QS_URL=https://github.com/quickshell-mirror/quickshell.git
QS_COMMIT=4f508be

PKGS="qt6-base-dev qt6-base-private-dev qt6-base-dev-tools qt6-declarative-dev
 qt6-declarative-private-dev qt6-declarative-dev-tools qt6-wayland-dev qt6-wayland-private-dev
 qt6-shadertools-dev qt6-shader-baker qt6-svg-dev
 libqt6core6t64 libqt6gui6 libqt6dbus6 libqt6network6 libqt6opengl6 libqt6qml6
 libqt6qmlmodels6 libqt6qmlmeta6 libqt6qmlworkerscript6 libqt6quick6 libqt6shadertools6
 libqt6svg6 libqt6waylandclient6 libqt6waylandcompositor6 libqt6wlshellintegration6
 libqt6quickcontrols2-6 libqt6quicktemplates2-6 libqt6quickshapes6 libqt6qmlcompiler6
 libqt6widgets6
 qmake6 qmake6-bin libcli11-dev
 libpipewire-0.3-dev libpipewire-0.3-0t64 libspa-0.2-dev libpam0g-dev libpam0g libvulkan-dev wayland-protocols libb2-1 libdouble-conversion3 libmd4c0 libxkbcommon-dev"

mkdir -p "$CACHE/debs" "$ROOT"
NEW=0
for p in $PKGS; do
    if ! ls "$CACHE/debs/${p}_"*.deb >/dev/null 2>&1; then
        (cd "$CACHE/debs" && apt-get download "$p" >/dev/null 2>&1) || echo "  QS         (no package $p)"
        NEW=1
    fi
done
if [ $NEW = 1 ] || [ ! -f "$CACHE/.unpacked" ]; then
    for d in "$CACHE"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
    touch "$CACHE/.unpacked"
fi

if [ ! -d "$SRC/.git" ]; then git clone -q "$QS_URL" "$SRC"; fi
(cd "$SRC" && { git cat-file -e "$QS_COMMIT^{commit}" 2>/dev/null || git fetch -q origin; } && git checkout -q "$QS_COMMIT")

# Qt's tool wrappers exec /usr/lib/qt6/bin/...: here they are in the cache
for w in "$ROOT"/usr/bin/x86_64-linux-gnu-*; do
    t=$(sed -n 's|^exec \(/usr/lib/qt6/[^ ]*\).*|\1|p' "$w" 2>/dev/null | head -1)
    [ -n "$t" ] && printf '#!/bin/sh\nexec %s "$@"\n' "$ROOT$t" > "$w"
done
QTLIB=$ROOT/usr/lib/x86_64-linux-gnu
# the .pc files of the cache name /usr: their headers, libraries and
# protocol XML are here (copies with /usr moved; tools/stage_caelestia.sh
# unpacks into the same root and does the same)
mkdir -p "$CACHE/pc"
for pc in "$QTLIB"/pkgconfig/*.pc "$ROOT"/usr/share/pkgconfig/*.pc; do
    sed "s|=/usr\$|=$ROOT/usr|; s|=/usr/|=$ROOT/usr/|; s|-I/usr/|-I$ROOT/usr/|g; s|-L/usr/|-L$ROOT/usr/|g; s|-isystem /usr/include/mit-krb5||g" "$pc" > "$CACHE/pc/$(basename "$pc")"
done
export PKG_CONFIG_PATH=$CACHE/pc:$QTLIB/pkgconfig:$ROOT/usr/share/pkgconfig
export LD_LIBRARY_PATH=$QTLIB      # Qt's build tools (qmlcachegen...) run from the cache
cmake -S "$SRC" -B "$BLD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$QTLIB/cmake;$ROOT/usr/share/cmake;$ROOT/usr/lib/cmake" \
    -DCMAKE_INSTALL_PREFIX=/usr -DINSTALL_QML_PREFIX=lib/x86_64-linux-gnu/qt6/qml \
    -DDISTRIBUTOR="WynlandOS" -DDISTRIBUTOR_DEBUGINFO_AVAILABLE=NO \
    -DCRASH_HANDLER=OFF -DUSE_JEMALLOC=OFF -DX11=OFF -DI3=OFF \
    -DSERVICE_PIPEWIRE=ON -DSERVICE_PAM=ON -DSERVICE_POLKIT=OFF -DSERVICE_GREETD=OFF \
    -DSCREENCOPY=ON -DBUILD_TESTING=OFF -DQT_QML_GENERATE_QMLLS_INI=OFF \
    -DCMAKE_CXX_FLAGS="-idirafter $ROOT/usr/include" -DCMAKE_C_FLAGS="-idirafter $ROOT/usr/include" -DCMAKE_LIBRARY_PATH="$QTLIB" \n    -DCMAKE_EXE_LINKER_FLAGS="-L$QTLIB -Wl,-rpath-link,$QTLIB" -DCMAKE_SHARED_LINKER_FLAGS="-L$QTLIB -Wl,-rpath-link,$QTLIB" \
    > "$CACHE/configure.log" 2>&1 || { tail -40 "$CACHE/configure.log"; exit 1; }
cmake --build "$BLD" -j "${QS_JOBS:-4}" > "$CACHE/build.log" 2>&1 || { grep -m20 -B2 -A8 'error' "$CACHE/build.log"; exit 1; }
rm -rf "$CACHE/install"
DESTDIR="$CACHE/install" cmake --install "$BLD" > /dev/null
strip -o "$OUT/quickshell" "$CACHE/install/usr/bin/quickshell"
echo "  QS         built build/quickshell ($(git -C "$SRC" rev-parse --short HEAD))"

# the manifest: the program, its QML modules, and the libraries the other
# manifests do not ship already (by soname into /lib64, as theirs)
OTHER="ext2_manifest.txt"
for m in qt6 gst wpe base; do [ -f "$OUT/${m}_manifest.txt" ] && OTHER="$OTHER $OUT/${m}_manifest.txt"; done
MAN=$OUT/quickshell_manifest.txt
python3 - "$CACHE/install" "$MAN" "$OUT/quickshell" $OTHER <<'PY'
import os, subprocess, sys
inst, man, binary, others = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
have_d, have_f = set(), set()
for o in others:
    for line in open(o):
        p = line.split()
        if len(p) >= 2 and p[0] == "D": have_d.add(p[1])
        if len(p) >= 2 and p[0] in ("F", "Fo", "S"): have_f.add(p[1])
out = ["# generated by tools/stage_quickshell.sh -- do not edit"]
def d(path):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts) + 1):
        q = "/" + "/".join(parts[:i])
        if q not in have_d:
            have_d.add(q); out.append("D " + q)
qml = os.path.join(inst, "usr/lib/x86_64-linux-gnu/qt6/qml")
for root, dirs, files in sorted(os.walk(qml)):
    rel = "/" + os.path.relpath(root, inst)
    d(rel)
    for f in sorted(files):
        out.append("F %s/%s %s" % (rel, f, os.path.join(root, f)))
out.append("F /usr/bin/quickshell " + binary)
env = dict(os.environ)
ldd = subprocess.run(["ldd", binary], capture_output=True, text=True, env=env).stdout
for line in ldd.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1] == "=>" and p[2].startswith("/"):
        want = "/lib64/" + p[0]
        if want not in have_f:
            have_f.add(want); out.append("F %s %s" % (want, os.path.realpath(p[2])))
    elif "not found" in line:
        print("  QS         WARNING: unresolved " + line.strip())
open(man, "w").write("\n".join(out) + "\n")
print("  QS         %s: %d lines" % (man, len(out)))
PY
