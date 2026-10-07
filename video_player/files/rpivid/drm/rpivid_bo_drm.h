/*
 * Phoenix-RTOS
 *
 * hevc_rpivid zero-copy output: picture buffers from the V3D render server
 *
 * rpivid_bo_drm_install() gives hevc_rpivid's drm_prime output (libavcodec/rpivid_drm.h)
 * V3D BOs as picture buffers: DRM_IOCTL_V3D_CREATE_BO on /dev/dri/renderD128 (each BO is
 * one physically contiguous block of the render server), its CPU mapping (uncached), its
 * physical address for the block's DMA, and a dma-buf (/v3dbuf/<handle>) that Mesa in this
 * or another process imports as the same BO. Reuse waits with DRM_IOCTL_V3D_WAIT_BO, which
 * for an exported BO covers every client's GPU jobs.
 *
 * A program that links it also links libdrm-phoenix's libdrm.a with -Wl,--wrap=mmap
 * -Wl,--wrap=ioctl -Wl,--wrap=fcntl -Wl,--wrap=dup -Wl,--wrap=dup2.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_BO_DRM_H
#define RPIVID_BO_DRM_H

/* Open the render node (once) and register the buffer operations. 0, or a negative errno
 * (no render node: hevc_rpivid then uses its own memory, with no dma-buf). */
int rpivid_bo_drm_install(void);

#endif
