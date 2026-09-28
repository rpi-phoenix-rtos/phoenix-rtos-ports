/*
 * Phoenix-RTOS
 *
 * Raspberry Pi 4 (BCM2711) V3D 4.2 asynchronous render server - smoke-test job generators
 *
 * Pure CPU-side generators (no IPC, no syscalls) for three minimal GPU jobs whose
 * memory result the CPU can check exactly:
 *
 *   clear - a bin + render job with zero draw calls that clears one raster RGBA8
 *           render target and stores it (the CL/binner/TLB/store path);
 *   tfu   - a TFU raster -> UBLINEAR-2-column copy (the TFU path);
 *   csd   - the HW-proven CSCONST compute kernel (the CSD/QPU/TMU path).
 *
 * Every packet is packed by functions copied verbatim from Mesa's generated
 * v3d_packet_v42_pack.h, and the packet sequence is transcribed from Mesa's gallium
 * v3d driver (v3dx_draw.c, v3dx_rcl.c, v3d_job.c) for the exact case generated
 * here. The caller owns every buffer: it allocates, maps (uncached) and passes the
 * CPU pointer + GPU virtual address. The generators only write into those buffers
 * (32-bit aligned stores, safe for Device-memory mappings).
 *
 * Every function returns 0 or a negative errno (-EINVAL bad argument/alignment,
 * -ENOSPC buffer too small) unless stated otherwise.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _V3DA_CLGEN_H_
#define _V3DA_CLGEN_H_

#include <stdint.h>

#include "v3d_drm.h"


typedef struct {
	void *cpu;      /* CPU mapping (uncached), 4-byte aligned */
	uint32_t gpuva; /* GPU virtual address of cpu[0] */
	uint32_t size;  /* bytes usable from cpu[0] */
} v3da_clgen_buf_t;


/* Alignment every GPU address passed to the generators must have. This is the
 * configuration Mesa always runs (every CL, tile_alloc, tile_state, render target
 * and TFU image starts a BO, and BOs are page aligned); weaker alignment was never
 * exercised. Hard minimums from the encodings: tile_alloc 64 B (TILE_LIST_SET_BASE
 * packs the address from bit 6), qts 4 B (the server ORs CT0QTS_ENABLE = bit 1 in at kick, as Linux V3D_CLE_CT0QTS_ENABLE and the old lane),
 * TFU ioa 8 B (bits 0..2 carry DIMTW/format), CSD shader 8 B (cfg[5] bits 0..2). */
#define V3DA_CLGEN_ALIGN 4096u

/* The V3D 4.2 control list executor reads up to 256 bytes past the last packet it
 * executes (Mesa devinfo->cle_readahead). Those bytes must be mapped GPU memory
 * owned by the job; the sizes returned by v3da_clgen_clear_sizes() include them. */
#define V3DA_CLGEN_CLE_READAHEAD 256u


/* ======================================================================== */
/* 1. clear: bin + render job, zero draws, one raster RGBA8 render target    */
/* ======================================================================== */

/* Render target: width x height pixels, PIPE_FORMAT_R8G8B8A8_UNORM, RASTER
 * (linear), stride = width * 4 bytes, no depth/stencil, no MSAA, one layer.
 * 1 <= width, height <= 4096. */
#define V3DA_CLGEN_CLEAR_MAX_DIM 4096u
#define V3DA_CLGEN_CLEAR_RT_STRIDE(w)    ((uint32_t)(w) * 4u)
#define V3DA_CLGEN_CLEAR_RT_SIZE(w, h)   ((uint32_t)(w) * 4u * (uint32_t)(h))

/* Sizes the caller must allocate for a width x height clear:
 *   bcl_size        - BCL buffer, including the CLE readahead slack;
 *   rcl_size        - RCL buffer (main RCL + generic per-tile list), incl. slack;
 *   tile_alloc_size - tile allocation memory (becomes qms), a multiple of 4096;
 *   tile_state_size - tile state data array (qts), 256 bytes per tile.
 * For 64x64: bcl 276 (19 B BCL, padded to 20, + 256), rcl 396 (106 B main RCL,
 * generic list at +112, 28 B, + 256), tile_alloc 12288, tile_state 256.
 * RCL buffer layout: [0, rcl_end) main RCL, then the generic per-tile list at
 * align(main, 16), then the readahead slack; everything past the lists is zeroed.
 * Any pointer may be NULL. */
int v3da_clgen_clear_sizes(uint32_t width, uint32_t height, uint32_t *bcl_size, uint32_t *rcl_size,
	uint32_t *tile_alloc_size, uint32_t *tile_state_size);

/* Fills bcl and rcl and the submit scalars of *out exactly as Mesa's
 * v3d_job_submit() does for V3D >= 4.2 (bcl_start/bcl_end, rcl_start/rcl_end,
 * qma = tile_alloc_gpuva, qms = tile_alloc_size, qts = tile_state_gpuva); every
 * other field of *out (syncs, bo_handles, flags, perfmon, extensions) is zeroed.
 *
 * Requirements (validated): bcl->size / rcl->size / tile_alloc_size at least the
 * values from v3da_clgen_clear_sizes() (tile_alloc_size also a multiple of 4096);
 * bcl->gpuva, rcl->gpuva, rt_gpuva, tile_alloc_gpuva, tile_state_gpuva all
 * V3DA_CLGEN_ALIGN aligned; bcl->cpu / rcl->cpu 4-byte aligned.
 * Not validated (no size argument): the render target must be at least
 * V3DA_CLGEN_CLEAR_RT_SIZE(width, height) bytes and the tile state array at least
 * tile_state_size bytes. The tile alloc / tile state contents need no
 * initialisation (the binner writes them).
 *
 * clear_rgba: the colour as the RGBA8 word R | G << 8 | B << 16 | A << 24.
 *
 * EXPECTED MEMORY RESULT: every pixel (x, y), 0 <= x < width, 0 <= y < height,
 * at rt + y * V3DA_CLGEN_CLEAR_RT_STRIDE(width) + x * 4 holds the bytes
 * R, G, B, A in that order, i.e. reads back as the little-endian uint32_t
 * clear_rgba. Derivation: Mesa packs an INTERNAL_TYPE_8 clear colour with
 * util_pack_color(PIPE_FORMAT_R8G8B8A8_UNORM) (R in bits 7:0) into
 * CLEAR_COLORS_PART1.low_32; the store uses output format RGBA8, r_b_swap = 0,
 * no dither, memory format RASTER. Nothing outside width x height is written
 * (the TLB store clips to the image size). */
int v3da_clgen_clear(uint32_t width, uint32_t height, uint32_t rt_gpuva, uint32_t clear_rgba,
	v3da_clgen_buf_t *bcl, v3da_clgen_buf_t *rcl, uint32_t tile_alloc_gpuva, uint32_t tile_alloc_size,
	uint32_t tile_state_gpuva, struct drm_v3d_submit_cl *out);


/* ======================================================================== */
/* 2. tfu: raster RGBA8 -> UBLINEAR 2-column copy                            */
/* ======================================================================== */

/* Why UBLINEAR_2_COLUMN: V3D 4.2's TFU cannot write RASTER (Mesa v3dX(tfu)
 * refuses it), and LINEARTILE is only defined for an image one utile wide or tall
 * (Mesa's v3d_get_lt_pixel_offset asserts it). A 9..16 pixel wide 32bpp image is
 * the case where Mesa itself lays level 0 out as UBLINEAR_2_COLUMN
 * (v3d_resource.c v3d_setup_slices: width <= 2 UIF blocks) and TFU-blits into it,
 * and that layout has a closed-form address (v3d_tiling.c
 * v3d_get_ublinear_pixel_offset, ublinear_number = 2):
 *
 *   utile     = 4x4 pixels = 64 bytes, pixels in raster order inside it;
 *   UIF block = 2x2 utiles = 8x8 pixels = 256 bytes, utiles ordered
 *               (0,0) (1,0) (0,1) (1,1) i.e. +64 for x & 4, +128 for y & 4;
 *   blocks    = two columns, block rows in raster order: +256 * (2 * (y / 8) + x / 8).
 *
 *   offset(x, y) = 256 * (2 * (y >> 3) + (x >> 3)) + ((x & 4) ? 64 : 0)
 *                + ((y & 4) ? 128 : 0) + (y & 3) * 16 + (x & 3) * 4
 *
 * The job is Mesa's blit form: texture type R32F (Mesa rewrites a 4-byte format to
 * R32_FLOAT for exact TFU copies), no mipmaps, no YUV coefficients. Because the
 * data travels as float32, test data should be normal finite floats: use
 * v3da_clgen_tfu_pattern() (A byte 0x5A keeps every word a normal float).
 *
 * Input:  width x height RGBA8 raster at src_gpuva, stride = width * 4 bytes.
 * Output: dst_gpuva, v3da_clgen_tfu_out_size() bytes; padding pixels
 *         (x >= width or y >= height) are unspecified.
 * 9 <= width <= 16, 1 <= height <= 4096 (16x16 recommended). */
#define V3DA_CLGEN_TFU_MIN_W 9u
#define V3DA_CLGEN_TFU_MAX_W 16u
#define V3DA_CLGEN_TFU_MAX_H 4096u

uint32_t v3da_clgen_tfu_src_size(uint32_t width, uint32_t height); /* width * 4 * height */
uint32_t v3da_clgen_tfu_out_size(uint32_t width, uint32_t height); /* 64 * align(height, 8) */
uint32_t v3da_clgen_tfu_out_offset(uint32_t x, uint32_t y);        /* formula above */
uint32_t v3da_clgen_tfu_pattern(uint32_t x, uint32_t y);           /* suggested src word */

/* Fills *out (icfg, iia, iis, ica, iua, ioa, ios, coef[]; everything else zero).
 * ICFG bit 0 (IOC, "interrupt on completion") is NOT set: the submitter ORs it in
 * at kick time, as Linux v3d_tfu_job_run does. src_gpuva and dst_gpuva must be
 * V3DA_CLGEN_ALIGN aligned; src_size / dst_size are validated against the sizes
 * above. Expected result: for x < width, y < height the uint32_t at
 * dst + v3da_clgen_tfu_out_offset(x, y) equals the uint32_t at src + (y*width + x)*4. */
int v3da_clgen_tfu(uint32_t width, uint32_t height, uint32_t src_gpuva, uint32_t src_size,
	uint32_t dst_gpuva, uint32_t dst_size, struct drm_v3d_submit_tfu *out);


/* ======================================================================== */
/* 3. csd: CSCONST kernel (tools/v3d-driver-port/csd_probe.c STEP2)          */
/* ======================================================================== */

/* One workgroup of 16 invocations, 4-way threaded, not single-segment. Every
 * lane TMU-writes uniform[0] to the address uniform[1].
 * EXPECTED MEMORY RESULT: the uint32_t at out_gpuva becomes V3DA_CLGEN_CSD_VALUE;
 * nothing else is written. Pre-fill the output with a sentinel (the probe used
 * 0xEE bytes) to tell "not written" from "wrote 0". */
#define V3DA_CLGEN_CSD_VALUE        0xC0DE1234u
#define V3DA_CLGEN_CSD_SHADER_SIZE  48u /* 6 QPU instructions */
#define V3DA_CLGEN_CSD_UNIFORM_SIZE 8u  /* 2 uniforms */

/* Copies the kernel into shader (size >= V3DA_CLGEN_CSD_SHADER_SIZE, gpuva
 * V3DA_CLGEN_ALIGN aligned; give it a whole page, the QPU prefetches ahead) and
 * the uniform stream into uniforms (size >= V3DA_CLGEN_CSD_UNIFORM_SIZE, gpuva
 * 4-byte aligned), then fills cfg[0..6] for drm_v3d_submit_csd (coef[] stay 0).
 * out_gpuva: the output SSBO, >= 4 bytes, 4-byte aligned. */
int v3da_clgen_csd(v3da_clgen_buf_t *shader, v3da_clgen_buf_t *uniforms, uint32_t out_gpuva, uint32_t cfg[7]);


#endif
