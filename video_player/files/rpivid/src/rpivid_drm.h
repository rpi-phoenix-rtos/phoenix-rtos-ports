/*
 * Phoenix-RTOS
 *
 * hevc_rpivid zero-copy output: AV_PIX_FMT_DRM_PRIME frames (public interface)
 *
 * With the decoder option rpivid_out=drm_prime (or FFMPEG_RPIVID_OUT=drm_prime) the block
 * decodes each picture into a buffer the GPU can import, and the decoder returns it without
 * a CPU copy: an AV_PIX_FMT_DRM_PRIME frame whose data[0] is an AVDRMFrameDescriptor
 * (libavutil/hwcontext_drm.h) of one object and one layer:
 *
 *   layer   DRM_FORMAT_NV12, 2 planes
 *   plane 0 luma,   offset 0,                        pitch = coded width (bytes)
 *   plane 1 CbCr,   offset (height + 15 & ~15) * 128, pitch = coded width
 *   object  fd = a dma-buf of the buffer (-1 without buffer operations, below), size,
 *           format_modifier = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(column height)
 *
 * (the Linux NV12_COL128 layout: 128-byte columns, each holding the picture's luma rows and
 * then its chroma rows). Mesa's v3d imports it with EGL_EXT_image_dma_buf_import_modifiers
 * and samples it through GL_TEXTURE_EXTERNAL_OES (docs/gpu-new-lane/M10b-video-zero-copy.md
 * in the coordination repository).
 *
 * The frame holds a reference to the buffer: the decoder writes into it again only after
 * every frame referencing it has been released (av_frame_unref) and, with buffer operations,
 * after wait_idle() says the GPU no longer reads it. A consumer that samples a frame keeps a
 * reference until its last GPU use has completed or been submitted (wait_idle orders the
 * reuse after submitted work).
 *
 * Only 8-bit streams, and only without frame threading (thread_count 1 or thread_type
 * FF_THREAD_SLICE), are decoded this way; otherwise, and for pictures the CPU decodes (a
 * stream or picture the block cannot take, a hardware failure), the frames are system-memory
 * frames as without the option: a consumer handles both formats, frame by frame.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AVCODEC_RPIVID_DRM_H
#define AVCODEC_RPIVID_DRM_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/frame.h"

/* the values of the decoder option rpivid_out */
#define RPIVID_OUT_PLANAR     0
#define RPIVID_OUT_DRM_PRIME  1

/* One picture buffer: physically contiguous, mapped uncached (the CPU reads it only for
 * rpivid_drm_frame_to_planar). */
typedef struct RpividDrmBuffer {
	void *cpu;                     /* the CPU mapping */
	uint64_t pa;                   /* the physical address of cpu (the block's DMA address) */
	size_t size;
	int fd;                        /* a dma-buf of it, or -1 */
	uint32_t handle;               /* the allocator's own name for it */
	void *priv;
} RpividDrmBuffer;

/* Where the buffers come from (rpivid_bo_drm.c: V3D BOs of the render server). alloc()
 * returns 0 or a negative errno; wait_idle() returns when no GPU work submitted so far
 * reads the buffer (0, or a negative errno: the buffer is then not reused). */
typedef struct RpividDrmBufferOps {
	void *opaque;
	int (*alloc)(void *opaque, size_t size, RpividDrmBuffer *b);
	void (*free)(void *opaque, RpividDrmBuffer *b);
	int (*wait_idle)(void *opaque, const RpividDrmBuffer *b);
} RpividDrmBufferOps;

/* Process-wide; decoders attached afterwards use them (the structure is copied). NULL: the
 * buffers are the decoder's own contiguous memory and the frames carry fd -1 (CPU readback
 * only: tests). Call before opening the decoder. */
void rpivid_drm_set_buffer_ops(const RpividDrmBufferOps *ops);

/* A DRM_PRIME frame of hevc_rpivid into system memory: yuv420p, the frame's visible
 * picture (its cropping applied). dst is allocated when it has no buffer, else it must be
 * a yuv420p frame of at least that size. The frame's properties are copied. 0, or
 * AVERROR(EINVAL) for a frame that is not one of this decoder's DRM_PRIME frames. Reads
 * uncached memory: ~8 ms per 1080p picture on the Pi 4. */
int rpivid_drm_frame_to_planar(AVFrame *dst, const AVFrame *src);

#endif
