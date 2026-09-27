/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix <dlfcn.h> has no RTLD_NOLOAD. Weston asks
 * dlopen(RTLD_NOLOAD) only for a module missing from its builtin table (weston
 * patch 0001), and that load fails on Phoenix either way.
 */
#include_next <dlfcn.h>

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0x0004
#endif
