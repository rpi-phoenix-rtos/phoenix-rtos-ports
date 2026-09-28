/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: <linux/vt.h>. Weston's DRM backend includes it for virtual
 * terminal switching, which on Linux goes through the seat manager (libseat).
 * Phoenix has no virtual terminals and libseat runs its noop backend (no VT
 * switching), so nothing from this header is used.
 */
#ifndef WLPHX_LINUX_VT_H
#define WLPHX_LINUX_VT_H
#endif
