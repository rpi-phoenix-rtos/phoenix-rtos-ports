/*
 * Phoenix-RTOS
 *
 * Xorg-drm: stand-ins for libphoenix functions the X server links but Phoenix lacks
 *
 * Each stand-in is compiled only while libphoenix.a lacks the symbol (build.sh checks
 * with nm, XORG_DRM_NEED_<NAME>), so it can never shadow a real implementation.
 * Delete each piece when libphoenix gains the function.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

/* (populated from the Xorg-drm link; see docs/gpu-new-lane/M4-xorg-modesetting.md) */
typedef int xorg_drm_compat_nonempty;
