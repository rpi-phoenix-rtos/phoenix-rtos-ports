/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat (applications only, not Mesa): libphoenix gap -- the GNU
 * sincos() is missing (kmscube cube-gears.c). compat/mesadrm_compat.c supplies it
 * while libphoenix lacks the symbol.
 */
#include_next <math.h>
#ifndef MESADRM_COMPAT_SINCOS
#define MESADRM_COMPAT_SINCOS
#ifdef __cplusplus
extern "C"
#endif
void sincos(double x, double *s, double *c);
#endif
