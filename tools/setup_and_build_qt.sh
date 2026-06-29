#!/bin/bash
# ==============================================================================
# WynlandOS - Qt6 Setup & Build Bootstrap Script (WSL Speed Optimized)
# ==============================================================================

set -e

SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TMP_DIR="/tmp/qt_build_tmp"

echo "==> Step 1: Setting up native WSL build directory..."
rm -rf "${BUILD_TMP_DIR}"
mkdir -p "${BUILD_TMP_DIR}"

echo "==> Step 2: Copying qtbase into native WSL filesystem..."
cp -r "${SDK_DIR}/qtbase" "${BUILD_TMP_DIR}/qtbase"

echo "==> Step 3: Integrating QWynlandFb platform plugin..."
mkdir -p "${BUILD_TMP_DIR}/qtbase/src/plugins/platforms/qwynlandfb"
cp -r "${SDK_DIR}/qt_qpa"/* "${BUILD_TMP_DIR}/qtbase/src/plugins/platforms/qwynlandfb/"

# Add QPA subdirectory to CMake
CMAKE_PLUGINS_FILE="${BUILD_TMP_DIR}/qtbase/src/plugins/platforms/CMakeLists.txt"
if ! grep -q "qwynlandfb" "${CMAKE_PLUGINS_FILE}"; then
    echo "add_subdirectory(qwynlandfb)" >> "${CMAKE_PLUGINS_FILE}"
fi

echo "==> Step 4: Configuring native-speed CMake build..."
mkdir -p "${BUILD_TMP_DIR}/build_qt"
cd "${BUILD_TMP_DIR}/build_qt"

cmake "${BUILD_TMP_DIR}/qtbase" \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_DIR}/tools/wynland-toolchain.cmake" \
    -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_FORCE_FEATURE_gui=ON \
    -DQT_FORCE_FEATURE_widgets=ON \
    -DQT_FEATURE_xcb=OFF \
    -DQT_FEATURE_eglfs=OFF \
    -DQT_FEATURE_opengl=OFF \
    -DFEATURE_qwynlandfb=ON

echo "==> Step 5: Compiling qtbase on native WSL filesystem..."
cmake --build . --parallel $(nproc)

echo "==> Step 6: Copying output libraries to WynlandOS workspace..."
mkdir -p "${SDK_DIR}/build/lib"
cp -d lib/libQt6Core.so* "${SDK_DIR}/build/lib/"
cp -d lib/libQt6Gui.so* "${SDK_DIR}/build/lib/"
cp -d lib/libQt6Widgets.so* "${SDK_DIR}/build/lib/"

mkdir -p "${SDK_DIR}/build/plugins/platforms"
cp plugins/platforms/libqwynlandfb.so "${SDK_DIR}/build/plugins/platforms/"

echo "==> Build complete! Output libraries copied to: ${SDK_DIR}/build/lib/"
