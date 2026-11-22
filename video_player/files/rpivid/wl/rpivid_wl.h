/*
 * Phoenix-RTOS
 *
 * hevc_rpivid pictures in a Wayland window, composited by the compositor from the decoder's
 * own buffers (docs/misc/2026-11-21-yuv-sand-plane-design.md §5, step 2a)
 *
 * Each picture is the decoder's NV12 buffer in SAND128 columns (an rpi4-kms dumb buffer, see
 * rpivid_plane.c). It is sent once as a linux-dmabuf buffer and shown on a subsurface of the
 * player's window, scaled by wp_viewporter: no CPU colour conversion or texture upload in
 * the player; the compositor samples it on the GPU.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_WL_H
#define RPIVID_WL_H

#include <libavutil/frame.h>

struct wl_display;
struct wl_surface;

/* Use the player's connection and window surface (SDL_GetWindowWMInfo): bind what is needed on
 * a private event queue and create the video subsurface. 0, or a negative errno (the
 * compositor lacks linux-dmabuf v3, wp_viewporter or NV12 + SAND128: the caller keeps its own
 * display path). Call from the thread that draws the window. */
int rpivid_wl_open(struct wl_display *display, struct wl_surface *parent);

/* Show an AV_PIX_FMT_DRM_PRIME picture of hevc_rpivid at the window rectangle (x, y, w, h).
 * The frame stays referenced until the compositor releases its buffer. 0, or a negative errno
 * (not shown: the caller shows it its own way). */
int rpivid_wl_show(const AVFrame *frame, int x, int y, int w, int h);

/* Hide the subsurface (e.g. the next picture goes another way). */
void rpivid_wl_hide(void);

#endif
