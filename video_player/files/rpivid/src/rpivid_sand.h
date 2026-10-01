/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: SAND (column 128) output to planar YUV 4:2:0
 *
 * The block writes NV12 in 128-byte wide columns: byte b of row y of column c is at
 * plane + c * stride + y * 128 + b. 8-bit: one sample per byte; 10-bit: three samples
 * in each little-endian 32-bit word, LSB first (96 samples per column). The chroma
 * plane holds interleaved Cb, Cr. These convert a picture to planar 4:2:0, 8-bit or
 * 16-bit little-endian (the yuv420p / yuv420p10le layouts).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_SAND_H
#define RPIVID_SAND_H

#include <stddef.h>
#include <stdint.h>

/* linesizes in bytes */
void rpivid_sand8_to_planar(uint8_t *dy, ptrdiff_t ly, uint8_t *du, ptrdiff_t lu, uint8_t *dv, ptrdiff_t lv,
	const uint8_t *sy, const uint8_t *sc, uint32_t luma_stride, uint32_t chroma_stride, uint32_t w, uint32_t h);

void rpivid_sand10_to_planar16(uint8_t *dy, ptrdiff_t ly, uint8_t *du, ptrdiff_t lu, uint8_t *dv, ptrdiff_t lv,
	const uint8_t *sy, const uint8_t *sc, uint32_t luma_stride, uint32_t chroma_stride, uint32_t w, uint32_t h);

#endif
