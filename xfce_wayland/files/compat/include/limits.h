/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * xfce-wayland compat: NGROUPS_MAX (POSIX <limits.h>; libphoenix has getgroups()
 * but no limit macro). Thunar sizes an on-stack gid_t array with it; Phoenix-RTOS
 * has no supplementary groups, so POSIX's minimum (_POSIX_NGROUPS_MAX = 8) is
 * plenty and keeps that array small.
 */
#include_next <limits.h>

#ifndef NGROUPS_MAX
#define NGROUPS_MAX 8
#endif
