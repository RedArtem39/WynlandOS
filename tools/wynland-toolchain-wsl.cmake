# ==============================================================================
# WynlandOS - CMake Toolchain File for WSL Native Speed Optimized Qt6 Build
# ==============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Force cross-compiling to false
set(CMAKE_CROSSCOMPILING FALSE CACHE BOOL "Force native compilation pass" FORCE)

# Hardcode the SDK root folder to the local fast /tmp directory
set(WYNLAND_SDK "/tmp/wynland_sdk")
message(STATUS "WynlandOS SDK Root set to: ${WYNLAND_SDK}")

# Set compiler paths to the native fast /tmp directory
set(CMAKE_C_COMPILER "/tmp/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "/tmp/x86_64-linux-musl-cross/bin/x86_64-linux-musl-g++")

set(CMAKE_SYSROOT "")
set(CMAKE_FIND_ROOT_PATH "${WYNLAND_SDK}")

set(WYNLAND_FLAGS "-I${WYNLAND_SDK}/include -D__wynland__")
set(CMAKE_C_FLAGS "${WYNLAND_FLAGS} ${CMAKE_C_FLAGS}" CACHE STRING "WynlandOS C flags" FORCE)
set(CMAKE_CXX_FLAGS "${WYNLAND_FLAGS} ${CMAKE_CXX_FLAGS}" CACHE STRING "WynlandOS C++ flags" FORCE)

# Embed rpath pointing to target (/lib) and host local (/tmp) library paths
set(WYNLAND_RPATH "/lib:/tmp/x86_64-linux-musl-cross/x86_64-linux-musl/lib")
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-rpath,${WYNLAND_RPATH}" CACHE STRING "WynlandOS EXE link flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-rpath,${WYNLAND_RPATH}" CACHE STRING "WynlandOS SO link flags" FORCE)

# Control search behaviors
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Force-disable OpenGL features and inputs at the cache level
set(INPUT_opengl "no" CACHE STRING "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl_desktop OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengles2 OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl_dynamic OFF CACHE BOOL "Force disable OpenGL" FORCE)
