#!/bin/sh
# Caelestia on Zerp 2.0 (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/caelestia.sh"): its log on the serial console.
# OpenGL: Mesa falls back to llvmpipe (no GPU path for Wayland clients yet), its
# shader effects -- the bar, the borders -- draw; Qt's software backend skips them.
export QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-opengl}
quickshell -c caelestia 2>&1 | sed 's/^/[cae] /'
echo "[cae] exit"
