#!/bin/bash
# WynlandOS - stage Qt 6 (Qt Quick / QML) for the image.
#
# Uses Ubuntu's prebuilt Qt 6 (glibc, like the Mesa stack already in
# /lib64): the .debs are fetched with `apt-get download` (no root, nothing
# installed on the host) and unpacked into a cache. Then:
#   - builds build/qmldemo.elf (apps/qml/qmldemo.cpp + the qwynlandfb QPA
#     plugin from qt_qpa/, linked in statically),
#   - writes build/qt6_manifest.txt: Qt's libraries, QML modules and the
#     ldd closure of everything, minus what ext2_manifest.txt already has.
#
# usage: tools/stage_qt6.sh            (from the repo root, inside WSL)
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
CACHE=${QT6_CACHE:-$HOME/.cache/wynland-qt6}
ROOT=$CACHE/root
LIB=$ROOT/usr/lib/x86_64-linux-gnu
INC=$ROOT/usr/include/x86_64-linux-gnu/qt6
QTVER=$(ls "$INC/QtCore" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1 || true)
OUT=$REPO/build
MAN=$OUT/qt6_manifest.txt

PKGS="qt6-base-dev qt6-base-private-dev qt6-base-dev-tools qt6-declarative-dev
 qt6-declarative-private-dev libqt6core6t64 libqt6gui6 libqt6dbus6 libqt6network6
 libqt6opengl6 libqt6qml6 libqt6qmlmodels6 libqt6qmlmeta6 libqt6qmlworkerscript6
 libqt6quick6 libqt6quickcontrols2-6 libqt6quicktemplates2-6 libqt6quickshapes6
 libqt6shadertools6 libqt6svg6 qml6-module-qtquick qml6-module-qtquick-window
 qml6-module-qtquick-layouts qml6-module-qtquick-controls
 qml6-module-qtquick-templates qml6-module-qtqml-workerscript
 qml6-module-qtquick-shapes qml6-module-qtquick-effects qml6-module-qtqml
 qml6-module-qtqml-models libb2-1 libdouble-conversion3 libmd4c0"

# fontconfig's stock configuration + DejaVu: a hand-written minimal
# fonts.conf has no substitution rules, every font match came back empty
# and Qt crashed in QFontconfigDatabase::setupFontEngine
PKGS="$PKGS fontconfig-config fonts-dejavu-core fonts-jetbrains-mono fonts-adwaita-sans"
# the terminal (apps/term): libvterm parses what programs print
PKGS="$PKGS libvterm0 libvterm-dev"
# eglfs links the input stacks even with input disabled (Zerp 2.0 feeds
# input itself); they only have to load
PKGS="$PKGS libmtdev1t64 libinput10 libts0t64 libevdev2 libwacom9 libgudev-1.0-0"
# Wayland: Zerp 2.0 is a compositor (QtWaylandCompositor, xdg-shell), and
# Qt programs can be its clients (the wayland platform plugin)
PKGS="$PKGS qt6-wayland-dev libqt6waylandcompositor6 libqt6waylandcompositorxdgshell6
 qml6-module-qtwayland-compositor qt6-wayland libqt6waylandclient6 libqt6wlshellintegration6
 libwayland-server0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0
 qt6-wayland-private-dev"

# 1. fetch + unpack (cached; only packages not downloaded yet are fetched)
mkdir -p "$CACHE/debs" "$ROOT"
NEW=0
for p in $PKGS; do
    if ! ls "$CACHE/debs/${p}_"*.deb >/dev/null 2>&1; then
        (cd "$CACHE/debs" && apt-get download "$p")
        NEW=1
    fi
done
if [ $NEW = 1 ] || [ ! -f "$CACHE/.unpacked" ]; then
    for d in "$CACHE"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
    touch "$CACHE/.unpacked"
fi
QTVER=$(ls "$INC/QtCore" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
echo "  QT6        Qt $QTVER from $CACHE"

# Adwaita Mono (the login screen's face; OFL): Ubuntu packages only the
# sans, so the mono comes from GNOME's release
AFONT=$ROOT/usr/share/fonts/truetype/adwaita-mono
if [ ! -f "$AFONT/AdwaitaMono-Regular.ttf" ]; then
    mkdir -p "$AFONT"
    curl -sSfL https://download.gnome.org/sources/adwaita-fonts/49/adwaita-fonts-49.0.tar.xz \
        | tar xJ -C "$AFONT" --strip-components=2 adwaita-fonts-49.0/mono/
fi

# 2. qmldemo.elf
export LD_LIBRARY_PATH=$LIB
B=$OUT/qt6obj; mkdir -p "$B"
CXXFLAGS="-std=c++17 -O2 -fPIC -DQT_STATICPLUGIN -DQT_NO_DEBUG -I$INC"
for m in QtCore QtGui QtQml QtQuick; do
    CXXFLAGS="$CXXFLAGS -I$INC/$m -I$INC/$m/$QTVER -I$INC/$m/$QTVER/$m"
done
CXXFLAGS="$CXXFLAGS -I$INC/QtGui/$QTVER/QtGui/qpa -I$B -Iqt_qpa"
MOCINC=$(for f in $CXXFLAGS; do case $f in -I*) printf '%s ' "$f";; esac; done)
"$ROOT/usr/lib/qt6/libexec/moc" -DQT_STATICPLUGIN $MOCINC -o "$B/qwynlandfbmain.moc" qt_qpa/qwynlandfbmain.cpp
"$ROOT/usr/lib/qt6/libexec/moc" $MOCINC -o "$B/moc_qwynlandfbinput.cpp" qt_qpa/qwynlandfbinput.h
g++ $CXXFLAGS -o "$OUT/qmldemo.elf" \
    apps/qml/qmldemo.cpp qt_qpa/qwynlandfbmain.cpp qt_qpa/qwynlandfb_zerpargs.cpp \
    qt_qpa/qwynlandfbintegration.cpp qt_qpa/qwynlandfbscreen.cpp qt_qpa/qwynlandfbinput.cpp \
    "$B/moc_qwynlandfbinput.cpp" \
    -L"$LIB" -Wl,-rpath-link,"$LIB" -Wl,-rpath,/lib64 -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Core
echo "  QT6        built build/qmldemo.elf"

# Files (apps/files): the file manager, same plumbing as qmldemo
"$ROOT/usr/lib/qt6/libexec/moc" $MOCINC -o "$B/moc_fs.cpp" apps/files/fs.h
g++ $CXXFLAGS -Iapps/files -o "$OUT/files.elf" \
    apps/files/main.cpp "$B/moc_fs.cpp" qt_qpa/qwynlandfbmain.cpp qt_qpa/qwynlandfb_zerpargs.cpp \
    qt_qpa/qwynlandfbintegration.cpp qt_qpa/qwynlandfbscreen.cpp qt_qpa/qwynlandfbinput.cpp \
    "$B/moc_qwynlandfbinput.cpp" \
    -L"$LIB" -Wl,-rpath-link,"$LIB" -Wl,-rpath,/lib64 -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Core
echo "  QT6        built build/files.elf"

# Terminal (apps/term): libvterm in a QRasterWindow, the same QPA plugin
g++ $CXXFLAGS -I"$ROOT/usr/include" -o "$OUT/term.elf" \
    apps/term/main.cpp qt_qpa/qwynlandfbmain.cpp qt_qpa/qwynlandfb_zerpargs.cpp \
    qt_qpa/qwynlandfbintegration.cpp qt_qpa/qwynlandfbscreen.cpp qt_qpa/qwynlandfbinput.cpp \
    "$B/moc_qwynlandfbinput.cpp" \
    -L"$LIB" -Wl,-rpath-link,"$LIB" -Wl,-rpath,/lib64 -lQt6Gui -lQt6Core -lvterm
echo "  QT6        built build/term.elf"

# Zerp 2.0 (apps/zerp2): Qt Quick on eglfs/KMS, no QPA plugin of ours
Z2INC="-I$INC -I$INC/QtGui/$QTVER/QtGui $(for m in QtCore QtGui QtQml QtQuick QtWaylandCompositor QtNetwork; do printf -- '-I%s/%s ' "$INC" "$m"; done)"
"$ROOT/usr/lib/qt6/libexec/moc" $Z2INC -o "$B/moc_zerp2.cpp" apps/zerp2/zerp2.h
"$ROOT/usr/lib/qt6/libexec/moc" $Z2INC -o "$B/moc_hypripc.cpp" apps/zerp2/hypripc.h
# wlr-layer-shell: libwayland's C glue and Qt's server classes from the XML
LS=apps/zerp2/protocols/wlr-layer-shell-unstable-v1.xml
wayland-scanner server-header "$LS" "$B/wayland-wlr-layer-shell-unstable-v1-server-protocol.h"
wayland-scanner private-code "$LS" "$B/wayland-wlr-layer-shell-unstable-v1-protocol.c"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-header "$LS" > "$B/qwayland-server-wlr-layer-shell-unstable-v1.h"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-code "$LS" > "$B/qwayland-server-wlr-layer-shell-unstable-v1.cpp"
gcc -O2 -fPIC -c -o "$B/wlr-layer-shell-protocol.o" "$B/wayland-wlr-layer-shell-unstable-v1-protocol.c"
# (its get_popup names xdg_popup: xdg-shell's interfaces, which Qt keeps to itself)
wayland-scanner private-code apps/zerp2/protocols/xdg-shell.xml "$B/xdg-shell-protocol.c"
gcc -O2 -fPIC -c -o "$B/xdg-shell-protocol.o" "$B/xdg-shell-protocol.c"
"$ROOT/usr/lib/qt6/libexec/moc" $Z2INC -I"$B" -o "$B/moc_zlayershell.cpp" apps/zerp2/zlayershell.h
# Hyprland's global shortcuts: the same, from hyprland-protocols' XML
GS=apps/zerp2/protocols/hyprland-global-shortcuts-v1.xml
wayland-scanner server-header "$GS" "$B/wayland-hyprland-global-shortcuts-v1-server-protocol.h"
wayland-scanner private-code "$GS" "$B/wayland-hyprland-global-shortcuts-v1-protocol.c"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-header "$GS" > "$B/qwayland-server-hyprland-global-shortcuts-v1.h"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-code "$GS" > "$B/qwayland-server-hyprland-global-shortcuts-v1.cpp"
gcc -O2 -fPIC -c -o "$B/hyprland-global-shortcuts-protocol.o" "$B/wayland-hyprland-global-shortcuts-v1-protocol.c"
"$ROOT/usr/lib/qt6/libexec/moc" $Z2INC -I"$B" -o "$B/moc_zglobalshortcuts.cpp" apps/zerp2/zglobalshortcuts.h
"$ROOT/usr/lib/qt6/libexec/moc" $Z2INC -o "$B/moc_greeter.cpp" apps/zerp2/greeter.h
gcc -O2 -fPIC -Wall -c -o "$B/wlcompat.o" apps/zerp2/wlcompat.c
# wlcompat's two libwayland-server functions must win over the library's
# own, for every library that calls them
g++ -std=c++17 -O2 -fPIC -DQT_NO_DEBUG $Z2INC -Iapps/zerp2 -o "$OUT/zerp2.elf" \
    apps/zerp2/main.cpp apps/zerp2/zerp2.cpp apps/zerp2/greeter.cpp apps/zerp2/hypripc.cpp apps/zerp2/zlayershell.cpp     "$B/qwayland-server-wlr-layer-shell-unstable-v1.cpp" "$B/wlr-layer-shell-protocol.o" "$B/xdg-shell-protocol.o" -I"$B"     "$B/moc_zerp2.cpp" "$B/moc_greeter.cpp" "$B/moc_hypripc.cpp" "$B/moc_zlayershell.cpp" \
    apps/zerp2/zglobalshortcuts.cpp apps/zerp2/keybinds.cpp "$B/qwayland-server-hyprland-global-shortcuts-v1.cpp" \
    "$B/hyprland-global-shortcuts-protocol.o" "$B/moc_zglobalshortcuts.cpp" \
    "$B/wlcompat.o" -Wl,--export-dynamic-symbol=wl_global_create -Wl,--export-dynamic-symbol=wl_resource_post_event \
    -Wl,--export-dynamic-symbol=mkstemp -Wl,--export-dynamic-symbol=wl_resource_create \
    "$LIB/libwayland-server.so.0" \
    -L"$LIB" -Wl,-rpath-link,"$LIB" -Wl,-rpath,/lib64 -lQt6WaylandCompositorXdgShell -lQt6WaylandCompositor \
    -lQt6Quick -lQt6Qml -lQt6Network -lQt6Gui -lQt6Core
echo "  QT6        built build/zerp2.elf"
# hyprctl: Hyprland's command line, for Zerp 2.0's Hyprland IPC (apps/zerp2/hypripc.cpp)
gcc -O2 -Wall -o "$OUT/hyprctl" apps/zerp2/hyprctl.c

# linux-dmabuf: GPU buffers from Wayland clients (Mesa's EGL, Vulkan WSI).
# Qt's own client buffer plugin -- Ubuntu does not build it -- from
# qtwayland's source at this Qt's version, taught protocol version 4
# (tools/zerp2-dmabuf/apply.py: Mesa finds the GPU through its feedback)
QW=$CACHE/qtwayland
if [ ! -d "$QW/.git" ]; then git -c advice.detachedHead=false clone -q --depth 1 --branch "v$QTVER" https://code.qt.io/qt/qtwayland.git "$QW"; fi
DB=$B/dmabuf; rm -rf "$DB"; mkdir -p "$DB"
cp "$QW"/src/hardwareintegration/compositor/linux-dmabuf-unstable-v1/*.{cpp,h} \
   "$QW"/src/plugins/hardwareintegration/compositor/linux-dmabuf-unstable-v1/{main.cpp,linux-dmabuf-unstable-v1.json} "$DB/"
python3 tools/zerp2-dmabuf/apply.py "$DB"
DX=apps/zerp2/protocols/linux-dmabuf-unstable-v1.xml
wayland-scanner server-header "$DX" "$DB/wayland-linux-dmabuf-unstable-v1-server-protocol.h"
wayland-scanner private-code "$DX" "$DB/linux-dmabuf-protocol.c"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-header "$DX" > "$DB/qwayland-server-linux-dmabuf-unstable-v1.h"
LD_LIBRARY_PATH="$LIB" "$ROOT/usr/lib/qt6/libexec/qtwaylandscanner" server-code "$DX" > "$DB/qwayland-server-linux-dmabuf-unstable-v1.cpp"
DBINC="$Z2INC -I$INC/QtOpenGL -I$INC/QtWaylandCompositor/$QTVER -I$INC/QtWaylandCompositor/$QTVER/QtWaylandCompositor -I$INC/QtGui/$QTVER/QtGui/qpa -I$INC/QtCore/$QTVER -I$INC/QtCore/$QTVER/QtCore -I$ROOT/usr/include/libdrm -I/usr/include/libdrm -I$DB"
"$ROOT/usr/lib/qt6/libexec/moc" $DBINC -o "$DB/main.moc" "$DB/main.cpp"
gcc -O2 -fPIC -c -o "$DB/linux-dmabuf-protocol.o" "$DB/linux-dmabuf-protocol.c"
g++ -std=c++17 -O2 -fPIC -shared -DQT_NO_DEBUG -DQT_PLUGIN $DBINC -o "$OUT/libqt-wayland-compositor-linux-dmabuf-unstable-v1.so" \
    "$DB/main.cpp" "$DB/linuxdmabuf.cpp" "$DB/linuxdmabufclientbufferintegration.cpp" \
    "$DB/qwayland-server-linux-dmabuf-unstable-v1.cpp" "$DB/linux-dmabuf-protocol.o" \
    -L"$LIB" -Wl,-rpath-link,"$LIB" -lQt6WaylandCompositor -lQt6OpenGL -lQt6Gui -lQt6Core -lEGL -lGLESv2 -lwayland-server
echo "  QT6        built build/libqt-wayland-compositor-linux-dmabuf-unstable-v1.so"

# 3. manifest: QML modules + ldd closure of the app and every QML plugin
WLPLUG="wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client wayland-graphics-integration-server"
QMLSRC=$LIB/qt6/qml
QMLDST=/usr/lib/x86_64-linux-gnu/qt6/qml
{
    echo "# generated by tools/stage_qt6.sh -- do not edit"
    echo "D /usr/lib/x86_64-linux-gnu"
    echo "D /usr/lib/x86_64-linux-gnu/qt6"
    echo "D $QMLDST"
    (cd "$QMLSRC" && find . -mindepth 1 -type d | sort) | sed "s|^\.|D $QMLDST|"
    (cd "$QMLSRC" && find . -type f | sort) | while read f; do
        echo "F $QMLDST/${f#./} $QMLSRC/${f#./}"
    done
    echo "D /usr/share/zerp"
    echo "D /usr/share/zerp/qml"
    echo "F /usr/share/zerp/qml/demo.qml apps/qml/demo.qml"
    echo "D /usr/share/zerp/qml/files"
    for q in apps/files/qml/*.qml; do echo "F /usr/share/zerp/qml/files/$(basename "$q") $q"; done
    echo "F /usr/bin/files build/files.elf"
    echo "F /usr/bin/term build/term.elf"
    # fontconfig configuration and fonts, from the packages (symlinks in
    # conf.d are stored as copies of their targets)
    echo "D /var"
    echo "D /var/cache"
    echo "D /var/cache/fontconfig"
    for tree in etc/fonts usr/share/fontconfig usr/share/fonts; do
        (cd "$ROOT" && find "$tree" -type d | sort) | sed 's|^|D /|'
        (cd "$ROOT" && find "$tree" \( -type f -o -type l \) | sort) | while read f; do
            echo "F /$f $(readlink -f "$ROOT/$f")"
        done
    done
    echo "F /usr/bin/qmldemo build/qmldemo.elf"
    echo "F /usr/bin/zerp2 build/zerp2.elf"
    echo "F /usr/bin/hyprctl build/hyprctl"
    echo "D /usr/share/zerp2"
    echo "D /usr/share/zerp2/qml"
    echo "F /usr/share/zerp2/kms.json apps/zerp2/kms.json"
    for q in apps/zerp2/qml/*.qml; do echo "F /usr/share/zerp2/qml/$(basename "$q") $q"; done
    # eglfs + its KMS/GBM backend (Zerp 2.0 draws through them)
    PL=$LIB/qt6/plugins
    echo "D /usr/lib/x86_64-linux-gnu/qt6/plugins"
    echo "D /usr/lib/x86_64-linux-gnu/qt6/plugins/platforms"
    echo "D /usr/lib/x86_64-linux-gnu/qt6/plugins/egldeviceintegrations"
    echo "F /usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqeglfs.so $PL/platforms/libqeglfs.so"
    echo "F /usr/lib/x86_64-linux-gnu/qt6/plugins/egldeviceintegrations/libqeglfs-kms-integration.so $PL/egldeviceintegrations/libqeglfs-kms-integration.so"
    # Wayland: the client platform plugin (Qt programs on Zerp's
    # compositor) and the plugins both sides load
    echo "F /usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqwayland.so $PL/platforms/libqwayland.so"
    for d in $WLPLUG; do
        echo "D /usr/lib/x86_64-linux-gnu/qt6/plugins/$d"
        for f in "$PL/$d"/*.so; do echo "F /usr/lib/x86_64-linux-gnu/qt6/plugins/$d/$(basename "$f") $f"; done
    done
    echo "F /usr/lib/x86_64-linux-gnu/qt6/plugins/wayland-graphics-integration-server/libqt-wayland-compositor-linux-dmabuf-unstable-v1.so build/libqt-wayland-compositor-linux-dmabuf-unstable-v1.so"
    echo "F /usr/bin/qt.conf rootfs/usr/bin/qt.conf"
} > "$MAN"

# libraries: everything the app and the QML plugins load, by soname into
# /lib64 -- skipping what the base manifest already ships there
{ echo "$OUT/qmldemo.elf"; echo "$OUT/zerp2.elf"; echo "$OUT/files.elf"; echo "$OUT/term.elf"; find "$QMLSRC" -name "*.so";
  echo "$LIB/qt6/plugins/platforms/libqeglfs.so"; echo "$LIB/qt6/plugins/egldeviceintegrations/libqeglfs-kms-integration.so";
  echo "$LIB/qt6/plugins/platforms/libqwayland.so"; for d in $WLPLUG; do ls "$LIB/qt6/plugins/$d"/*.so; done; } | while read f; do
    ldd "$f" | awk '/=>/ && $3 ~ /^\// {print $1, $3}'
done | sort -u | while read soname path; do
    if grep -qE "^F +/lib64/$soname( |$)" ext2_manifest.txt; then continue; fi
    echo "F /lib64/$soname $(readlink -f "$path")"
done | sort -u >> "$MAN"

if ldd "$OUT/qmldemo.elf" | grep -q "not found"; then
    echo "  QT6        WARNING: unresolved libraries:"; ldd "$OUT/qmldemo.elf" | grep "not found"
fi
echo "  QT6        $(grep -c '^F' "$MAN") files in $MAN"
