#!/bin/bash
# ==============================================================================
# WynlandOS - Qt6 Cross-Compilation Script
# ==============================================================================

set -e

# Define directories
SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT_SRC_DIR="${SDK_DIR}/qtbase"
BUILD_DIR="${SDK_DIR}/build_qt"

# Check if qtbase source code exists
if [ ! -d "${QT_SRC_DIR}" ]; then
    echo "Error: qtbase directory not found. Please clone qtbase into: ${QT_SRC_DIR}"
    exit 1
fi

echo "==> Configuring Qt6 (qtbase) with WynlandOS Toolchain..."
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${QT_SRC_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_DIR}/tools/wynland-toolchain.cmake" \
    -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_FORCE_FEATURE_gui=ON \
    -DQT_FORCE_FEATURE_widgets=ON \
    -DQT_FEATURE_xcb=OFF \
    -DQT_FEATURE_eglfs=OFF \
    -DINPUT_opengl=no \
    -DFEATURE_opengl=OFF \
    -DFEATURE_qwynlandfb=ON \
    -DQT_FEATURE_network=OFF \
    -DQT_FEATURE_dbus=OFF \
    -DQT_FEATURE_sql=OFF \
    -DQT_FEATURE_printsupport=OFF \
    -DQT_FEATURE_xml=OFF

echo "==> Compiling qtbase..."
cmake --build . --parallel $(nproc)

echo "==> Compilation complete! Output binaries located in: ${BUILD_DIR}/lib"
