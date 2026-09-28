/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* weston-drm shim: the kernel-UAPI fixed-width type names (weston's copy of the
 * sync_file UAPI and libbacklight use them). */
#ifndef WLPHX_LINUX_TYPES_H
#define WLPHX_LINUX_TYPES_H
#include <stdint.h>
typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;
#endif
