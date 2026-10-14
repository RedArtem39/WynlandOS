#!/bin/sh
# Quickshell smoke test: its log on the serial console
export QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-software}
quickshell -p /usr/share/wynland/qs-smoke/shell.qml 2>&1 | sed 's/^/[qs] /'
echo "[qs-smoke] exit"
