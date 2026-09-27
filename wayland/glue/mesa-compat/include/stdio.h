/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: libphoenix gap -- open_memstream() (POSIX.1-2008) is missing.
 * Mesa's only users (util/memstream.c for nir_print/spirv debug text) fail soft on
 * NULL. compat/mesadrm_compat.c supplies an ENOSYS stub only while libphoenix lacks
 * the symbol (build.sh checks); the real one is on branch gpu-lane/libc-gaps (7cc5628).
 */
#include_next <stdio.h>
#ifndef MESADRM_COMPAT_OPEN_MEMSTREAM
#define MESADRM_COMPAT_OPEN_MEMSTREAM
#ifdef __cplusplus
extern "C"
#endif
FILE *open_memstream(char **bufp, size_t *sizep);
#endif
