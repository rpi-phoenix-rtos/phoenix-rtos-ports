/*
 * Phoenix-RTOS
 *
 * FFmpeg HEVC decoder + the BCM2711 rpivid block
 *
 * The rpivid block is driven as an hwaccel of FFmpeg's own HEVC decoder (hevcdec.c
 * parses everything, keeps the DPB and the output order) that writes ordinary
 * system-memory frames: each picture is decoded by the block into contiguous SAND
 * buffers (kept as the picture's reference) and converted to the stream's planar
 * format in the frame the decoder allocated. Players see a software decoder; a
 * stream or picture the block cannot take, or a hardware failure, continues on the
 * CPU decoder.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AVCODEC_RPIVID_HEVC_H
#define AVCODEC_RPIVID_HEVC_H

#include "avcodec.h"

/* How much of the stream syntax goes to the block:
 *   0  none (CPU decoder)
 *   1  the default tool set: what the block was proved bit-exact on (the x265 tool set:
 *      CTB 64, one slice segment per picture, no tiles, WPP, SAO, TMVP, weighted
 *      prediction, 8/10 bit) and the further tools on by default in rpivid_hevc.c's
 *      tools[] (other block sizes, transform tree depth, AMP, scaling lists, several
 *      slices, QP and deblocking variants, ...)
 *   2  every tool the command programming implements (also tiles, dependent slices,
 *      PCM, transquant bypass, constrained intra prediction, long-term references)
 * FFMPEG_RPIVID_TOOLS=-name,+name turns single tools off or on (rpivid_hevc.c). */
#define RPIVID_LEVEL_OFF      0
#define RPIVID_LEVEL_DEFAULT  1
#define RPIVID_LEVEL_ALL      2

/* From get_format(), with the SPS active and a software format chosen: take the
 * block if the stream fits the level (else log why not and leave the CPU decoder).
 * 0 = attached. */
int ff_rpivid_hevc_attach(AVCodecContext *avctx, int level);

int ff_rpivid_hevc_active(const AVCodecContext *avctx);

/* Before the first slice of a picture goes to the hwaccel: can the block decode it?
 * (A PPS or slice structure the stream-level check could not see.) */
int ff_rpivid_hevc_picture_ok(AVCodecContext *avctx);

/* After end_frame: the block failed the picture; the caller continues on the CPU */
int ff_rpivid_hevc_failed(const AVCodecContext *avctx);

#endif
