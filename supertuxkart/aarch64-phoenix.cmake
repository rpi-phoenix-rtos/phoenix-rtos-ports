# aarch64-phoenix.cmake — CMake cross toolchain file for the SuperTuxKart port.
#
# SuperTuxKart is a large CMake tree that add_subdirectory()s many bundled
# projects (Irrlicht, bullet, angelscript, mojoal, graphics_engine, and the
# heavy shaderc/glslang/SPIRV-Tools stack). A toolchain file — rather than the
# inline -D flags the smaller framework ports use — is used here so the cross
# settings and, crucially, the find-root confinement propagate into every one
# of those sub-projects. Without the confinement a find_package() deep inside a
# bundled project can silently resolve a HOST /usr library of the wrong
# architecture.
#
# All build-specific absolute paths (compiler prefix, the shared framework
# install prefix, the phoenix sysroot, the flag surface) are taken from the
# environment the port framework exports (CROSS / CFLAGS / LDFLAGS) plus two
# variables the port's p_build sets (STK_PREFIX / STK_SYSROOT). Keeping them out
# of the committed file makes it path-agnostic and reusable.
#
# Copyright 2026 Phoenix Systems
# SPDX-License-Identifier: BSD-3-Clause

# Generic (not Linux) marks a bare cross build with no host detection. This is
# the same choice every sibling framework port makes; see the port.def.sh for
# the analysis of STK's UNIX-gated defaults it implies (USE_GLES2 must be passed
# explicitly, system-enet path is skipped in favour of bundled enet, and STK's
# FindFreetype.cmake pkg-config branch is patched to a manual find).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross compilers. ${CROSS} is exported by the port build environment
# (e.g. "aarch64-phoenix-").
set(_cross "$ENV{CROSS}")
set(CMAKE_C_COMPILER   "${_cross}gcc")
set(CMAKE_CXX_COMPILER "${_cross}g++")
set(CMAKE_ASM_COMPILER "${_cross}gcc")

# We build only static archives for Phoenix; skip the shared-lib compiler probe.
set(BUILD_SHARED_LIBS OFF)

# Flag surface from the framework. CFLAGS carries --sysroot / -mcpu=cortex-a72
# etc. Reuse it for C++ (identical surface — the proven fltk/harfbuzz pattern,
# since the port env exports no CXXFLAGS). Fold CFLAGS into the linker flags so
# CMake's link-time probes (e.g. STK's std::atomic<uint64_t> check and shaderc's
# compiler-flag checks) carry the sysroot and cpu flags and actually link.
#
# The framework CFLAGS carries -std=gnu17, a C-only option. It is harmless for
# the configure-time C++ probes (a warning), but several of STK's bundled
# sub-projects (graphics_engine, shaderc, ...) compile C++ with -Werror, where
# GCC's "'-std=gnu17' is valid for C/ObjC but not for C++" warning becomes a
# hard error. Strip it from the C++ flags; STK's own CMAKE_CXX_STANDARD selects
# the C++ dialect. Keep it for C (enet/dnsc/zlib TUs).
string(REPLACE "-std=gnu17" "" _stk_cxx_flags "$ENV{CFLAGS}")
set(CMAKE_C_FLAGS   "$ENV{CFLAGS}"      CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_stk_cxx_flags}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "${_stk_cxx_flags} $ENV{LDFLAGS}" CACHE STRING "" FORCE)

# Find-root confinement. Search the shared framework install prefix (ported
# libs/headers) and the phoenix sysroot for libraries and headers; never let a
# find_* escape to the host /usr tree. Programs (python3 for SPIRV-Tools grammar
# generation, cmake, etc.) must still come from the host, so PROGRAM=NEVER.
set(CMAKE_FIND_ROOT_PATH "$ENV{STK_PREFIX}" "$ENV{STK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
