/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: libphoenix gap -- no _SC_PHYS_PAGES (Mesa
 * os_get_total_physical_memory). 103 is the value branch gpu-lane/libc-gaps
 * (3162bd4) gives it; today's sysconf() answers -1/EINVAL for it and Mesa fails
 * soft (no "total memory" figure).
 */
#include_next <unistd.h>
#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES 103
#endif
