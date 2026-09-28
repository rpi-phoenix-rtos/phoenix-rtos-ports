/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: libphoenix gap -- SCNxPTR/SCNuPTR (C99 7.8.1) are missing from
 * <inttypes.h> (Mesa nir_opt_varyings.c fails -Werror=format). Fixed on branch
 * gpu-lane/libc-gaps (c1c2af2).
 */
#include_next <inttypes.h>
#ifndef SCNxPTR
#define SCNxPTR "lx"
#endif
#ifndef SCNuPTR
#define SCNuPTR "lu"
#endif
