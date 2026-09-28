/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix has no coarse clocks. Weston names them in its
 * presentation-clock preference list and its clock-name table; with Linux's
 * numbers (unused by libphoenix, whose clocks are 0-3) clock_gettime() refuses
 * them and Weston moves on to CLOCK_MONOTONIC -- the only clock the DRM backend
 * reports as supported anyway.
 */
#include_next <time.h>

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE  5
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 6
#endif
