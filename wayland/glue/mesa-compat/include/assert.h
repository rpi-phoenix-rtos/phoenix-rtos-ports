/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat (new GPU lane, M3 part 3): libphoenix gap -- C11 7.2p3 requires
 * <assert.h> to define static_assert. Fixed on libphoenix branch gpu-lane/libc-gaps
 * (8551094); delete this wrapper when that lands.
 */
#include_next <assert.h>
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
