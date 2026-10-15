#!/bin/sh
# Caelestia on Zerp 2.0 (boot.cfg "wlrun=sh+/usr/share/wynland/qs-smoke/caelestia.sh"): its log on the serial console
export QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-software}
quickshell -c caelestia 2>&1 | sed 's/^/[cae] /'
echo "[cae] exit"
