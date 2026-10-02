# CMake platform module for Phoenix-RTOS (aarch64-phoenix): a static-only UNIX.
# Found through CMAKE_MODULE_PATH (set by phoenix-aarch64.cmake). CMake has no built-in
# Phoenix platform; without this file it falls back to an unknown system with UNIX unset,
# and WebKit's cmake/WebKitCommon.cmake rejects it.
include(Platform/UnixPaths)

set(CMAKE_DL_LIBS "")
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
# Userspace is 100% static (no PT_INTERP, no shared libraries).
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
