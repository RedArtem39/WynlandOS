# ==============================================================================
# WynlandOS - CMake Toolchain File for Qt6/qtbase Cross-Compilation
# ==============================================================================
# Target: x86_64-wynland-musl
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/wynland-toolchain.cmake <path/to/qtbase>
# ==============================================================================

# Target operating system
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Since host and target are both x86_64 Linux ABI compatible, the host can execute target binaries.
# Force cross-compiling to false to compile tools and libraries in a single pass without needing QT_HOST_PATH.
set(CMAKE_CROSSCOMPILING FALSE CACHE BOOL "Force native compilation pass" FORCE)

# Define the SDK root folder (autodetect if not provided)
if(NOT DEFINED WYNLAND_SDK)
    set(WYNLAND_SDK "${CMAKE_CURRENT_LIST_DIR}/..")
    get_filename_component(WYNLAND_SDK "${WYNLAND_SDK}" ABSOLUTE)
endif()

message(STATUS "WynlandOS SDK Root set to: ${WYNLAND_SDK}")

# Set the compiler to the downloaded musl cross compiler
set(CMAKE_C_COMPILER "${WYNLAND_SDK}/tools/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${WYNLAND_SDK}/tools/x86_64-linux-musl-cross/bin/x86_64-linux-musl-g++")

# System directories search configuration
set(CMAKE_SYSROOT "")
set(CMAKE_FIND_ROOT_PATH "${WYNLAND_SDK}")

# Common compilation flags for user applications on WynlandOS
# Includes the WynlandOS platform API headers and sets the __wynland__ macro
set(WYNLAND_FLAGS "-I${WYNLAND_SDK}/include -D__wynland__")

set(CMAKE_C_FLAGS "${WYNLAND_FLAGS} ${CMAKE_C_FLAGS}" CACHE STRING "WynlandOS C flags" FORCE)
set(CMAKE_CXX_FLAGS "${WYNLAND_FLAGS} ${CMAKE_CXX_FLAGS}" CACHE STRING "WynlandOS C++ flags" FORCE)

# User mode linker configuration: embed rpath for library resolution on both host (WSL) and target (WynlandOS)
set(WYNLAND_RPATH "/lib:${WYNLAND_SDK}/tools/x86_64-linux-musl-cross/x86_64-linux-musl/lib")
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-rpath,${WYNLAND_RPATH}" CACHE STRING "WynlandOS EXE link flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-rpath,${WYNLAND_RPATH}" CACHE STRING "WynlandOS SO link flags" FORCE)

# Control search behaviors
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Force-disable OpenGL features and inputs at the cache level to bypass CMake's boolean conversion pitfalls
set(INPUT_opengl "no" CACHE STRING "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl_desktop OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengles2 OFF CACHE BOOL "Force disable OpenGL" FORCE)
set(QT_FEATURE_opengl_dynamic OFF CACHE BOOL "Force disable OpenGL" FORCE)
