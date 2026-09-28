/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: Mesa's own include/drm-uapi/drm.h (like libdrm's) includes the
 * BSD <sys/ioccom.h> on every non-Linux OS. Phoenix has no such header, but its
 * _IO/_IOR/_IOW/_IOWR in <sys/ioctl.h> ARE the BSD layout (13-bit size,
 * IOC_OUT 0x40000000, IOC_IN 0x80000000; IOC_VOID is 0 and 0x20000000 means
 * IOC_NESTED). libphoenix ioctl() copies IOCPARM_LEN(request) bytes, so raw
 * ioctl()s Mesa issues (vc4_drm_winsys.c DRM_IOCTL_VC4_GET_PARAM, libsync.h) must
 * use THIS layout -- never the old lane's Linux-layout shim-include/sys/ioccom.h.
 * Kept in this private include dir, never in the shared sysroot.
 */
#include <sys/ioctl.h>
