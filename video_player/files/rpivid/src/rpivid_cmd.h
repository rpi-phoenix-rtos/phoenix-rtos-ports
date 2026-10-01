/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: the phase-1 command buffer and phase-2 register values
 *
 * The block decodes a picture in two phases. Phase 1 (entropy decode) runs a command
 * buffer: a list of 64-bit entries, register offset | value << 32, that sets the
 * sequence/picture/slice parameters, points the block at each slice's data and walks
 * the picture's tiles or wavefront rows. Phase 2 (reconstruction) is programmed with
 * plain register writes: output and reference planes, CONFIG2, the collocated motion
 * vector buffers.
 *
 * This module turns already parsed parameters (the codec-neutral structs below, filled
 * from any HEVC parser) into both. It has no hardware access and no allocation besides
 * its own growing entry array, so it builds and is tested on any host.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_CMD_H
#define RPIVID_CMD_H

#include <stddef.h>
#include <stdint.h>

#define RPIVID_MAX_TILE_COLS 20
#define RPIVID_MAX_TILE_ROWS 22
#define RPIVID_SCALING_BYTES 4064 /* 0xbe0 + 0x400: the block's scaling factor array */

/* Per picture: the active SPS/PPS fields the registers carry */
typedef struct {
	uint32_t width, height;        /* pic_{width,height}_in_luma_samples */
	uint8_t bit_depth;             /* 8 or 10 (luma == chroma) */
	uint8_t chroma_format_idc;     /* 1 */
	uint8_t log2_min_cb, log2_ctb, log2_min_tb, log2_max_tb;
	uint8_t max_trafo_depth_intra, max_trafo_depth_inter;
	uint8_t amp, strong_intra_smoothing, scaling_list;
	uint8_t pcm, pcm_bit_depth, pcm_bit_depth_chroma, log2_min_pcm_cb, log2_max_pcm_cb, pcm_loop_filter_disabled;

	uint8_t cu_qp_delta, diff_cu_qp_delta_depth;
	uint8_t transquant_bypass, transform_skip, sign_data_hiding, constrained_intra_pred;
	int8_t pps_cb_qp_offset, pps_cr_qp_offset;
	uint8_t weighted_pred, weighted_bipred;
	uint8_t entropy_coding_sync;
	uint8_t tiles, loop_filter_across_tiles;
	uint32_t num_tile_cols, num_tile_rows;
	uint32_t col_bd[RPIVID_MAX_TILE_COLS + 1]; /* tile column boundaries in CTBs, col_bd[n] = width in CTBs */
	uint32_t row_bd[RPIVID_MAX_TILE_ROWS + 1];
	const int *ctb_addr_rs_to_ts;  /* NULL: no tiles (raster == tile scan) */
	const int *ctb_addr_ts_to_rs;
	uint8_t log2_parallel_merge_level;

	const uint8_t *scaling_factors; /* RPIVID_SCALING_BYTES, see rpivid_scaling_factors(); NULL if !scaling_list */

	uint8_t slice_temporal_mvp;    /* slice_temporal_mvp_enabled_flag (the same in every slice of a picture) */

	/* Mode: emit no slice messages for the first slice of an I picture whose messages
	 * would all be the defaults (deblocking on, no offsets, no chroma QP offsets) --
	 * the form tools/hevc-decode proved bit-exact; otherwise the driver's three */
	uint8_t compat_intra_no_msgs;
} rpivid_pic_t;

/* Per slice segment */
typedef struct {
	uint32_t data_offset;          /* offset of slice_segment_data() in the bitstream buffer */
	uint32_t data_len;             /* bytes of slice data (emulation prevention bytes kept) */
	uint32_t slice_segment_addr;   /* in CTBs, raster scan */
	uint8_t dependent;             /* dependent_slice_segment_flag */
	uint8_t slice_type;            /* 0 B, 1 P, 2 I */
	int slice_qp;                  /* SliceQpY */
	int8_t slice_cb_qp_offset, slice_cr_qp_offset;
	uint8_t sao_luma, sao_chroma;
	uint8_t mvd_l1_zero, cabac_init, collocated_from_l0;
	uint8_t max_num_merge_cand;    /* 0 for I */
	uint8_t nb_refs[2];
	uint8_t ref_slot[2][16];       /* the phase-2 reference slot of each list entry */
	int32_t ref_poc[2][16];
	uint8_t ref_long_term[2][16];
	int32_t cur_poc;
	uint8_t weighted;              /* pred_weight_table() present for this slice */
	uint8_t luma_log2_weight_denom, chroma_log2_weight_denom;
	int16_t luma_weight[2][16], luma_offset[2][16];
	int16_t chroma_weight[2][16][2], chroma_offset[2][16][2];
	int8_t beta_offset_div2, tc_offset_div2;
	uint8_t deblocking_disabled, loop_filter_across_slices;
} rpivid_slice_t;

typedef struct {
	uint64_t *e;                   /* entries: offset | value << 32 */
	uint32_t len, cap;
	int err;                       /* sticky: -ENOMEM or -EINVAL */

	/* entries whose value is a bitstream buffer address: rebased in rpivid_cmd_rebase() */
	uint32_t *bfbase;
	uint32_t nbfbase, capbfbase;

	const rpivid_pic_t *pic;
	uint32_t ctb_w, ctb_h;
	uint32_t slice_idx;

	/* the last entry point written (tile or wavefront row) */
	uint32_t entry_tile_x, entry_tile_y, entry_ctb_x, entry_ctb_y, entry_qp, entry_slice;
	uint32_t reg_slicestart;

	/* the slice being added */
	uint32_t start_ts, start_ctb_x, start_ctb_y, prev_ctb_x, prev_ctb_y;

	uint16_t msgs[2 + 2 * 16 * 8 + 2];
	uint32_t nmsgs;
} rpivid_cmd_t;

void rpivid_cmd_init(rpivid_cmd_t *c);
void rpivid_cmd_free(rpivid_cmd_t *c);

/* Start a picture. The picture struct must stay valid until rpivid_cmd_end(). */
void rpivid_cmd_begin(rpivid_cmd_t *c, const rpivid_pic_t *pic);

/* Add the picture's next slice segment (in decoding order) */
int rpivid_cmd_slice(rpivid_cmd_t *c, const rpivid_slice_t *sl);

/* Close the picture: the remaining tile / wavefront entries and the end marker.
 * Returns the entry count or a negative errno. */
int rpivid_cmd_end(rpivid_cmd_t *c);

/* Turn bitstream-relative addresses into bus addresses (base must be 64-byte aligned) */
void rpivid_cmd_rebase(rpivid_cmd_t *c, uint64_t bs_base);

/* CONFIG2 of a picture: write_colmv = this picture's motion vectors are kept (it may be
 * a collocated picture), read_colmv = slice_temporal_mvp_enabled_flag */
uint32_t rpivid_config2(const rpivid_pic_t *pic, int write_colmv, int read_colmv);

/* Expand HEVC scaling lists into the block's scaling factor layout. Lists are in raster
 * order: sl4[6][16], sl8[6][64], sl16[6][64] (8x8 + DC), sl32[2][64] (8x8 + DC; the
 * intra and inter luma lists). */
void rpivid_scaling_factors(uint8_t out[RPIVID_SCALING_BYTES], const uint8_t sl4[6][16], const uint8_t sl8[6][64],
	const uint8_t sl16[6][64], const uint8_t dc16[6], const uint8_t sl32[2][64], const uint8_t dc32[2]);

/* SAND (column 128) plane geometry the block writes, shared by decoder and de-tiler */
typedef struct {
	uint32_t luma_stride;          /* bytes from one 128-byte column to the next, luma plane */
	uint32_t chroma_stride;
	uint32_t cols;                 /* number of 128-byte columns */
	size_t luma_size, chroma_size; /* allocation sizes */
	uint32_t colmv_stride;         /* collocated motion vector buffer stride (MVSTRIDE/COLSTRIDE) */
	size_t colmv_size;             /* and size, per picture */
} rpivid_geom_t;

void rpivid_geom(rpivid_geom_t *g, uint32_t width, uint32_t height, unsigned int bit_depth);

#endif
