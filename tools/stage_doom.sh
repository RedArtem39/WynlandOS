#!/bin/bash
# WynlandOS - stage Freedoom for the image: Chocolate Doom (SDL2) and the
# Freedoom WADs (Phase 1 and Phase 2: free levels, art and sound for the
# Doom engine; BSD licence), from Ubuntu's packages.
#
# The .debs and every dependency the host does not have are fetched with
# `apt-get download` into a cache and unpacked there. Then
# build/doom_manifest.txt lists the engine, the WADs, a launcher
# (/usr/bin/freedoom, /usr/bin/freedoom1) with a desktop entry for each,
# and the ldd closure of the engine -- plus the libraries SDL2 opens by
# itself (Wayland, EGL, the sound) -- minus what the other manifests ship.
#
# usage: tools/stage_doom.sh            (from the repo root, inside WSL)
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
CACHE=${DOOM_CACHE:-$HOME/.cache/wynland-doom}
ROOT=$CACHE/root
LIB=$ROOT/usr/lib/x86_64-linux-gnu
OUT=$REPO/build
MAN=$OUT/doom_manifest.txt
GEN=$OUT/doom

TOP="chocolate-doom freedoom"

# 1. fetch + unpack: the packages and every dependency missing on the host
mkdir -p "$CACHE/debs" "$ROOT" "$GEN"
deps=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts \
           --no-breaks --no-replaces --no-enhances $TOP 2>/dev/null \
       | grep -v '^ ' | grep -v '^<' | sort -u)
NEW=0
for p in $deps; do
    if dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed'; then
        case " $TOP " in *" $p "*) ;; *) continue ;; esac
    fi
    if ! ls "$CACHE/debs/${p}_"*.deb >/dev/null 2>&1; then
        (cd "$CACHE/debs" && apt-get download "$p" >/dev/null 2>&1) || echo "  DOOM       (no package $p)"
        NEW=1
    fi
done
if [ $NEW = 1 ] || [ ! -f "$CACHE/.unpacked" ]; then
    for d in "$CACHE"/debs/*.deb; do dpkg-deb -x "$d" "$ROOT"; done
    touch "$CACHE/.unpacked"
fi

ENGINE=$ROOT/usr/games/chocolate-doom
WADS=$ROOT/usr/share/games/doom
[ -x "$ENGINE" ] || { echo "  DOOM       no chocolate-doom in $ROOT"; exit 1; }
for w in freedoom1.wad freedoom2.wad; do [ -f "$WADS/$w" ] || { echo "  DOOM       no $w"; exit 1; }; done
echo "  DOOM       Chocolate Doom $(ls "$CACHE"/debs/chocolate-doom_*.deb | sed 's/.*_\([0-9.]*\)-.*/\1/'), Freedoom $(ls "$CACHE"/debs/freedoom_*.deb | sed 's/.*_\([0-9.]*\)-.*/\1/')"

# 2. the launchers: SDL2 on Wayland (Zerp), the WAD chosen; the saves and
#    the settings in ~/.local/share/chocolate-doom (the engine's own) --
#    the first time, ours: WASD to walk, the mouse to turn (not to walk),
#    its left button fire, E or its right button use, its wheel the
#    weapons, Shift to run (the engine's own: the arrows, Ctrl, Space).
#    Doom's keys are PC scancodes: w 17, a 30, s 31, d 32, e 18, Ctrl 29,
#    left Shift 42, Alt 56, the arrows 72 75 77 80.
cat > "$GEN/default.cfg" <<'EOF'
mouse_sensitivity             5
use_mouse                     1
mouseb_fire                   0
mouseb_strafe                 -1
mouseb_forward                -1
key_right                     77
key_left                      75
key_up                        17
key_down                      31
key_strafeleft                30
key_straferight               32
key_fire                      29
key_use                       18
key_strafe                    56
key_speed                     42
screenblocks                  10
detaillevel                   0
EOF
cat > "$GEN/chocolate-doom.cfg" <<'EOF'
fullscreen                    1
grabmouse                     1
novert                        1
mouseb_use                    1
mouseb_prevweapon             4
mouseb_nextweapon             3
key_menu_activate             1
EOF
for n in 1 2; do
    cat > "$GEN/freedoom$n" <<EOF
#!/bin/sh
# Freedoom: Phase $n on Chocolate Doom (tools/stage_doom.sh)
export SDL_VIDEODRIVER=\${SDL_VIDEODRIVER:-wayland}
# the first time: WynlandOS's controls (WASD + mouse), not the arrows
D=\$HOME/.local/share/chocolate-doom
if [ ! -f "\$D/.wynland-controls" ]; then
    mkdir -p "\$D"
    cp /usr/share/wynland/doom/default.cfg /usr/share/wynland/doom/chocolate-doom.cfg "\$D/" 2>/dev/null
    : > "\$D/.wynland-controls"
fi
exec /usr/games/chocolate-doom -iwad /usr/share/games/doom/freedoom$n.wad "\$@"
EOF
    chmod 755 "$GEN/freedoom$n"
done
cat > "$GEN/freedoom.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Freedoom: Phase 2
GenericName=Doom
Comment=Свободный Doom: уровни, графика и звук Freedoom на движке Chocolate Doom
Exec=/usr/bin/freedoom2
Icon=freedoom2
Categories=Game;ActionGame;
Keywords=doom;freedoom;shooter;игра;
EOF
cat > "$GEN/freedoom1.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Freedoom: Phase 1
GenericName=Doom
Comment=Свободный Doom, первая часть: эпизоды в духе Ultimate Doom
Exec=/usr/bin/freedoom1
Icon=freedoom1
Categories=Game;ActionGame;
Keywords=doom;freedoom;shooter;игра;
EOF

# what SDL2 opens itself (dlopen: ldd does not see it)
DLOPEN=""
# (Ubuntu's SDL2 is sdl2-compat: SDL3 underneath, opened at run time)
for so in libSDL3.so.0 libwayland-client.so.0 libwayland-egl.so.1 libwayland-cursor.so.0 libxkbcommon.so.0 libdecor-0.so.0 \
          libEGL.so.1 libGL.so.1 libGLESv2.so.2 libpipewire-0.3.so.0 libpulse.so.0 libasound.so.2; do
    for d in "$LIB" /usr/lib/x86_64-linux-gnu; do
        if [ -e "$d/$so" ]; then DLOPEN="$DLOPEN $d/$so"; break; fi
    done
done

{
    echo "# generated by tools/stage_doom.sh -- do not edit"
    echo "D /usr/games"
    echo "D /usr/share/games"
    echo "D /usr/share/games/doom"
    echo "F /usr/games/chocolate-doom $ENGINE"
    echo "F /usr/share/games/doom/freedoom1.wad $WADS/freedoom1.wad"
    echo "F /usr/share/games/doom/freedoom2.wad $WADS/freedoom2.wad"
    echo "D /usr/share/wynland/doom"
    echo "F /usr/share/wynland/doom/default.cfg $GEN/default.cfg"
    echo "F /usr/share/wynland/doom/chocolate-doom.cfg $GEN/chocolate-doom.cfg"
    echo "F /usr/bin/freedoom1 $GEN/freedoom1"
    echo "F /usr/bin/freedoom2 $GEN/freedoom2"
    echo "F /usr/bin/freedoom $GEN/freedoom2"
    echo "F /usr/share/applications/freedoom.desktop $GEN/freedoom.desktop"
    echo "F /usr/share/applications/freedoom1.desktop $GEN/freedoom1.desktop"
    # the icons, where the packages have them
    for n in 1 2; do
        for sz in 128x128 64x64 48x48; do
            ic=$(ls "$ROOT"/usr/share/icons/hicolor/$sz/apps/freedoom$n.png 2>/dev/null | head -1 || true)
            if [ -n "$ic" ]; then
                echo "D /usr/share/icons/hicolor/$sz"
                echo "D /usr/share/icons/hicolor/$sz/apps"
                echo "F /usr/share/icons/hicolor/$sz/apps/freedoom$n.png $ic"
            fi
        done
        ic=$(ls "$ROOT"/usr/share/icons/hicolor/scalable/apps/freedoom$n.svg 2>/dev/null | head -1 || true)
        if [ -n "$ic" ]; then
            echo "D /usr/share/icons/hicolor/scalable"
            echo "D /usr/share/icons/hicolor/scalable/apps"
            echo "F /usr/share/icons/hicolor/scalable/apps/freedoom$n.svg $ic"
        fi
    done
} > "$MAN"

# the libraries: the engine's and SDL2's own, minus what is shipped already
OTHER="ext2_manifest.txt $(ls "$OUT"/*_manifest.txt | grep -v -e doom_manifest -e ext2_manifest_full -e ext2_manifest_live | tr '
' ' ')"
for f in "$ENGINE" $DLOPEN; do
    LD_LIBRARY_PATH="$LIB" ldd "$f" | awk '/=>/ && $3 ~ /^\// {print $1, $3}'
    case "$f" in */lib*.so*) echo "$(basename "$f") $f" ;; esac
done | sort -u | while read soname path; do
    if awk -v want="/lib64/$soname" '($1=="F"||$1=="Fo"||$1=="S") && $2==want {found=1} END {exit !found}' $OTHER; then continue; fi
    echo "F /lib64/$soname $(readlink -f "$path")"
done | sort -u >> "$MAN"

missing=$(LD_LIBRARY_PATH="$LIB" ldd "$ENGINE" | grep 'not found' || true)
if [ -n "$missing" ]; then echo "  DOOM       WARNING: unresolved:"; echo "$missing"; fi
echo "  DOOM       $(grep -c '^F' "$MAN") files in $MAN"
