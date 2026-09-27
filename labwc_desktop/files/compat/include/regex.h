/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: libphoenix's <regex.h> uses size_t and off_t without
 * including their header (foot includes it first).
 */
#include <sys/types.h>
#include_next <regex.h>
