/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: <linux/ioctl.h> (Weston's copy of the sync_file UAPI). Phoenix
 * encodes ioctl numbers in the BSD layout (<sys/ioctl.h>); libdrm-phoenix's ioctl
 * interposer (-Wl,--wrap=ioctl) decodes sync_file requests from their low 16 bits,
 * which both layouts share (M3 section 2.1, M5 section 9.3).
 */
#include <sys/ioctl.h>
