#!/bin/sh
# Caelestia on Zerp 2.0 (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/caelestia.sh"): its log on the serial console.
# OpenGL: on the GPU (virgl) through linux-dmabuf; its
# shader effects -- the bar, the borders -- need it (Qt's software backend skips them).
export QSG_INFO=1   # the GL renderer in the log: virgl or llvmpipe
export QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-opengl}
quickshell -c caelestia 2>&1 | sed 's/^/[cae] /'
echo "[cae] exit"
