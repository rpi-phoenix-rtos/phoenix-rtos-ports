/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: SAND (column 128) output to planar YUV 4:2:0
 *
 * The source is read once, front to back: column by column, each column's rows in
 * address order, in 64-byte loads; only the destination is written with a stride. Partial columns at the right edge go through a stack
 * buffer so nothing is written past the picture width.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "rpivid_sand.h"

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif


/* 128 bytes, one column row */
static inline void copy128(uint8_t *d, const uint8_t *s)
{
#ifdef __ARM_NEON
	uint8x16x4_t a = vld1q_u8_x4(s), b = vld1q_u8_x4(s + 64);

	vst1q_u8_x4(d, a);
	vst1q_u8_x4(d + 64, b);
#else
	memcpy(d, s, 128);
#endif
}


/* 64 Cb, Cr pairs to 64 Cb and 64 Cr */
static inline void deint128(uint8_t *u, uint8_t *v, const uint8_t *s)
{
#ifdef __ARM_NEON
	int i;

	for (i = 0; i < 4; i++) {
		uint8x16x2_t uv = vld2q_u8(s + 32 * i);

		vst1q_u8(u + 16 * i, uv.val[0]);
		vst1q_u8(v + 16 * i, uv.val[1]);
	}
#else
	uint8_t t[128];
	int i;

	memcpy(t, s, sizeof(t));
	for (i = 0; i < 64; i++) {
		u[i] = t[2 * i];
		v[i] = t[2 * i + 1];
	}
#endif
}


/* 32 words of 3 x 10 bits to 96 16-bit samples */
static inline void unpack96(uint16_t *d, const uint8_t *s)
{
#ifdef __ARM_NEON
	const uint32x4_t m = vdupq_n_u32(0x3ffu);
	int i;

	for (i = 0; i < 8; i++) {
		uint32x4_t w = vld1q_u32((const uint32_t *)(const void *)(s + 16 * i));
		uint16x4x3_t o;

		o.val[0] = vmovn_u32(vandq_u32(w, m));
		o.val[1] = vmovn_u32(vandq_u32(vshrq_n_u32(w, 10), m));
		o.val[2] = vmovn_u32(vandq_u32(vshrq_n_u32(w, 20), m));
		vst3_u16(d + 12 * i, o);
	}
#else
	uint8_t t[128];
	int i;

	memcpy(t, s, sizeof(t));
	for (i = 0; i < 32; i++) {
		uint32_t w = (uint32_t)t[4 * i] | ((uint32_t)t[4 * i + 1] << 8) | ((uint32_t)t[4 * i + 2] << 16) | ((uint32_t)t[4 * i + 3] << 24);

		d[3 * i] = (uint16_t)(w & 0x3ffu);
		d[3 * i + 1] = (uint16_t)((w >> 10) & 0x3ffu);
		d[3 * i + 2] = (uint16_t)((w >> 20) & 0x3ffu);
	}
#endif
}


/* TEMPORARY A/B (FFMPEG_RPIVID_SAND=cols|rows): destination-row order, every row of the picture
 * written front to back, each column's source read 128 bytes further per row */
static void sand8_rows(uint8_t *dy, ptrdiff_t ly, uint8_t *du, ptrdiff_t lu, uint8_t *dv, ptrdiff_t lv,
	const uint8_t *sy, const uint8_t *sc, uint32_t luma_stride, uint32_t chroma_stride, uint32_t w, uint32_t h)
{
	uint32_t full = w / 128u, rem = w % 128u, cw = (w + 1u) / 2u, ch = (h + 1u) / 2u, c, y;
	uint8_t tu[128], tv[64];

	for (y = 0; y < h; y++) {
		const uint8_t *s = sy + (size_t)y * 128u;
		uint8_t *d = dy + (size_t)y * (size_t)ly;

		for (c = 0; c < full; c++) {
			__builtin_prefetch(s + (size_t)c * luma_stride + 4u * 128u, 0, 0);
			copy128(d + (size_t)c * 128u, s + (size_t)c * luma_stride);
		}
		if (rem != 0u) {
			copy128(tu, s + (size_t)full * luma_stride);
			memcpy(d + (size_t)full * 128u, tu, rem);
		}
	}

	/* chroma: a column row is 64 Cb, Cr pairs */
	full = cw / 64u;
	rem = cw % 64u;
	for (y = 0; y < ch; y++) {
		const uint8_t *s = sc + (size_t)y * 128u;
		uint8_t *u = du + (size_t)y * (size_t)lu, *v = dv + (size_t)y * (size_t)lv;

		for (c = 0; c < full; c++) {
			__builtin_prefetch(s + (size_t)c * chroma_stride + 4u * 128u, 0, 0);
			deint128(u + (size_t)c * 64u, v + (size_t)c * 64u, s + (size_t)c * chroma_stride);
		}
		if (rem != 0u) {
			deint128(tu, tv, s + (size_t)full * chroma_stride);
			memcpy(u + (size_t)full * 64u, tu, rem);
			memcpy(v + (size_t)full * 64u, tv, rem);
		}
	}
}


void rpivid_sand8_to_planar(uint8_t *dy, ptrdiff_t ly, uint8_t *du, ptrdiff_t lu, uint8_t *dv, ptrdiff_t lv,
	const uint8_t *sy, const uint8_t *sc, uint32_t luma_stride, uint32_t chroma_stride, uint32_t w, uint32_t h)
{
	static int rows = -1;
	uint32_t cols = (w + 127u) / 128u, cw = (w + 1u) / 2u, ch = (h + 1u) / 2u, c, y, n;
	uint8_t tu[128], tv[64];

	if (rows < 0) {
		const char *e = getenv("FFMPEG_RPIVID_SAND");

		rows = ((e != NULL) && (strcmp(e, "rows") == 0)) ? 1 : 0;
	}
	if (rows != 0) {
		sand8_rows(dy, ly, du, lu, dv, lv, sy, sc, luma_stride, chroma_stride, w, h);
		return;
	}

	for (c = 0; c < cols; c++) {
		const uint8_t *s = sy + (size_t)c * luma_stride;
		uint8_t *d = dy + (size_t)c * 128u;

		n = ((w - c * 128u) < 128u) ? (w - c * 128u) : 128u;
		for (y = 0; y < h; y++, s += 128, d += ly) {
			if (n == 128u) {
				copy128(d, s);
			}
			else {
				copy128(tu, s);
				memcpy(d, tu, n);
			}
		}
	}

	/* chroma: a column row is 64 Cb, Cr pairs */
	cols = (2u * cw + 127u) / 128u;
	for (c = 0; c < cols; c++) {
		const uint8_t *s = sc + (size_t)c * chroma_stride;
		uint8_t *u = du + (size_t)c * 64u, *v = dv + (size_t)c * 64u;

		n = ((cw - c * 64u) < 64u) ? (cw - c * 64u) : 64u;
		for (y = 0; y < ch; y++, s += 128, u += lu, v += lv) {
			if (n == 64u) {
				deint128(u, v, s);
			}
			else {
				deint128(tu, tv, s);
				memcpy(u, tu, n);
				memcpy(v, tv, n);
			}
		}
	}
}


void rpivid_sand10_to_planar16(uint8_t *dy, ptrdiff_t ly, uint8_t *du, ptrdiff_t lu, uint8_t *dv, ptrdiff_t lv,
	const uint8_t *sy, const uint8_t *sc, uint32_t luma_stride, uint32_t chroma_stride, uint32_t w, uint32_t h)
{
	uint32_t cols = (w + 95u) / 96u, cw = (w + 1u) / 2u, ch = (h + 1u) / 2u, c, y, n, i;
	uint16_t t[96];

	for (c = 0; c < cols; c++) {
		const uint8_t *s = sy + (size_t)c * luma_stride;
		uint8_t *d = dy + (size_t)c * 96u * 2u;

		n = ((w - c * 96u) < 96u) ? (w - c * 96u) : 96u;
		for (y = 0; y < h; y++, s += 128, d += ly) {
			if (n == 96u) {
				unpack96((uint16_t *)(void *)d, s);
			}
			else {
				unpack96(t, s);
				memcpy(d, t, (size_t)n * 2u);
			}
		}
	}

	/* chroma: a column row is 48 Cb, Cr pairs */
	cols = (2u * cw + 95u) / 96u;
	for (c = 0; c < cols; c++) {
		const uint8_t *s = sc + (size_t)c * chroma_stride;
		uint8_t *u = du + (size_t)c * 48u * 2u, *v = dv + (size_t)c * 48u * 2u;

		n = ((cw - c * 48u) < 48u) ? (cw - c * 48u) : 48u;
		for (y = 0; y < ch; y++, s += 128, u += lu, v += lv) {
			uint16_t *pu = (uint16_t *)(void *)u, *pv = (uint16_t *)(void *)v;

			unpack96(t, s);
#ifdef __ARM_NEON
			if (n == 48u) {
				for (i = 0; i < 48u; i += 8u) {
					uint16x8x2_t uv = vld2q_u16(t + 2u * i);

					vst1q_u16(pu + i, uv.val[0]);
					vst1q_u16(pv + i, uv.val[1]);
				}
				continue;
			}
#endif
			for (i = 0; i < n; i++) {
				pu[i] = t[2u * i];
				pv[i] = t[2u * i + 1u];
			}
		}
	}
}
