/*
 * Phoenix-RTOS
 *
 * hevc_rpivid pictures on a display overlay of rpi4-kms (docs/misc/2026-11-21-yuv-sand-plane-design.md)
 *
 * The decoder's drm_prime output writes into rpi4-kms dumb buffers (its scan-out pool, below
 * 1 GiB: all the firmware plane fetches); each picture, one NV12 buffer in SAND128 columns,
 * becomes a framebuffer once and is put on the overlay. No CPU pixel work.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_PLANE_H
#define RPIVID_PLANE_H

#include <stdint.h>

#include <libavutil/frame.h>

/* Open /dev/dri/card0, find an overlay that takes NV12 and install the decoder's buffer
 * operations (rpivid_drm_set_buffer_ops): call before the decoder is opened. 0, or a
 * negative errno (no card, no such plane: the caller keeps its own display path). */
int rpivid_plane_open(void);

/* Show an AV_PIX_FMT_DRM_PRIME picture of hevc_rpivid on the overlay, scaled into the screen
 * rectangle (x, y, w, h). The frame is referenced until two later ones were shown, so the
 * decoder never reuses a buffer the plane may still scan. The first picture sets the
 * overlay's COLOR_ENCODING / COLOR_RANGE from the frame's tags. 0, or a negative errno
 * (the picture is not shown: the caller shows it its own way). */
int rpivid_plane_show(const AVFrame *frame, int x, int y, int w, int h);

/* The display mode's size (the screen the rectangles of rpivid_plane_show() are in). */
void rpivid_plane_screen(uint32_t *w, uint32_t *h);

/* Take the picture off the overlay and drop the held frames. */
void rpivid_plane_close(void);

/* Called with a picture buffer's dma-buf descriptor just before the buffer is freed (the
 * decoder's thread): a user that made something from the descriptor (rpivid_wl.c: a
 * compositor buffer) forgets it. NULL by default. */
extern void (*rpivid_plane_buffer_freed)(int fd);

#endif
