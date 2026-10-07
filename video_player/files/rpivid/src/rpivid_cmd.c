/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: the phase-1 command buffer and phase-2 register values
 *
 * The command sequence (which registers, in which order, for slices, tiles and
 * wavefront rows) is the one the Raspberry Pi Linux hevc_dec driver emits; the
 * coordination repository's tools/hevc-decode re-derived and proved it on the Pi 4
 * for single-slice pictures (hevc-m2.c build_command_buffer). This is that code
 * generalised to several slice segments and tiles, written against codec-neutral
 * parameter structs (rpivid_cmd.h).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "rpivid_cmd.h"
#include "rpivid_regs.h"

/* A picture's command buffer beyond this is a corrupt stream, not a big picture */
#define RPIVID_CMD_MAX (1u << 20)

#define SLICE_B 0u
#define SLICE_P 1u
#define SLICE_I 2u


void rpivid_cmd_init(rpivid_cmd_t *c)
{
	memset(c, 0, sizeof(*c));
}


void rpivid_cmd_free(rpivid_cmd_t *c)
{
	free(c->e);
	free(c->bfbase);
	memset(c, 0, sizeof(*c));
}


static int cmd_grow(rpivid_cmd_t *c)
{
	uint32_t ncap = (c->cap != 0u) ? c->cap * 2u : 4096u;
	uint64_t *ne;

	if (ncap > RPIVID_CMD_MAX) {
		c->err = -EINVAL;
		return -1;
	}
	ne = realloc(c->e, (size_t)ncap * sizeof(*ne));
	if (ne == NULL) {
		c->err = -ENOMEM;
		return -1;
	}
	c->e = ne;
	c->cap = ncap;
	return 0;
}


static void put(rpivid_cmd_t *c, uint32_t off, uint32_t v)
{
	if ((c->err != 0) || ((c->len == c->cap) && (cmd_grow(c) < 0))) {
		return;
	}
	c->e[c->len++] = (uint64_t)off | ((uint64_t)v << 32);
}


static void put_bfbase(rpivid_cmd_t *c, uint32_t data_offset)
{
	if (c->nbfbase == c->capbfbase) {
		uint32_t ncap = (c->capbfbase != 0u) ? c->capbfbase * 2u : 64u;
		uint32_t *nb = realloc(c->bfbase, (size_t)ncap * sizeof(*nb));

		if (nb == NULL) {
			c->err = -ENOMEM;
			return;
		}
		c->bfbase = nb;
		c->capbfbase = ncap;
	}
	c->bfbase[c->nbfbase++] = c->len;
	put(c, RPI_BFBASE, data_offset >> 6);
}


void rpivid_cmd_rebase(rpivid_cmd_t *c, uint64_t bs_base)
{
	uint32_t i;

	for (i = 0; i < c->nbfbase; i++) {
		c->e[c->bfbase[i]] += (uint64_t)RPI_VC_ADDR(bs_base) << 32;
	}
}


/* CABAC context initialisation (H.265 9.3.2.2) in the block's order: init value per
 * context for initType 0 (I), 1 and 2 (P/B, swapped by cabac_init_flag) */
static const uint8_t prob_init[3][156] = {
	{
		153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 184, 154, 154,
		154, 184, 63, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154,
		154, 154, 154, 153, 138, 138, 111, 141, 94, 138, 182, 154, 154,
		154, 140, 92, 137, 138, 140, 152, 138, 139, 153, 74, 149, 92,
		139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197, 110,
		110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
		79, 108, 123, 63, 110, 110, 124, 125, 140, 153, 125, 127, 140,
		109, 111, 143, 127, 111, 79, 108, 123, 63, 91, 171, 134, 141,
		138, 153, 136, 167, 152, 152, 139, 139, 111, 111, 125, 110, 110,
		94, 124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141,
		179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182,
		152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111, 0, 0,
	},
	{
		153, 185, 107, 139, 126, 197, 185, 201, 154, 149, 154, 139, 154,
		154, 154, 152, 110, 122, 95, 79, 63, 31, 31, 153, 153, 168,
		140, 198, 79, 124, 138, 94, 153, 111, 149, 107, 167, 154, 154,
		154, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 137, 169, 194, 166, 167, 154, 167, 137, 182, 125,
		110, 94, 110, 95, 79, 125, 111, 110, 78, 110, 111, 111, 95,
		94, 108, 123, 108, 125, 110, 94, 110, 95, 79, 125, 111, 110,
		78, 110, 111, 111, 95, 94, 108, 123, 108, 121, 140, 61, 154,
		107, 167, 91, 122, 107, 167, 139, 139, 155, 154, 139, 153, 139,
		123, 123, 63, 153, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123,
		107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140, 0, 0,
	},
	{
		153, 160, 107, 139, 126, 197, 185, 201, 154, 134, 154, 139, 154,
		154, 183, 152, 154, 137, 95, 79, 63, 31, 31, 153, 153, 168,
		169, 198, 79, 224, 167, 122, 153, 111, 149, 92, 167, 154, 154,
		154, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 122, 169, 208, 166, 167, 154, 152, 167, 182, 125,
		110, 124, 110, 95, 94, 125, 111, 111, 79, 125, 126, 111, 111,
		79, 108, 123, 93, 125, 110, 124, 110, 95, 94, 125, 111, 111,
		79, 125, 126, 111, 111, 79, 108, 123, 93, 121, 140, 61, 154,
		107, 167, 91, 107, 107, 167, 139, 139, 170, 154, 139, 153, 139,
		123, 123, 63, 124, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138,
		122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140, 0, 0,
	},
};


static void write_prob(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	unsigned int init_type, i;
	int q = sl->slice_qp;
	const uint8_t *p;
	uint8_t dst[156];

	/* initType (9.3.2.2): I 0, P 1, B 2; cabac_init_flag swaps P and B */
	if ((sl->cabac_init != 0u) && (sl->slice_type != SLICE_I)) {
		init_type = sl->slice_type + 1u;
	}
	else {
		init_type = 2u - sl->slice_type;
	}
	p = prob_init[init_type];
	q = (q < 0) ? 0 : ((q > 51) ? 51 : q);

	for (i = 0; i < 154u; i++) {
		int m = (p[i] >> 4) * 5 - 45;
		int n = ((p[i] & 15) << 3) - 16;
		int pre = 2 * (((m * q) >> 4) + n) - 127;

		pre ^= pre >> 31;
		if (pre > 124) {
			pre = 124 + (pre & 1);
		}
		dst[i] = (uint8_t)pre;
	}
	dst[154] = dst[155] = 0;

	for (i = 0; i < 156u; i += 4u) {
		put(c, RPI_PROBBASE + i, dst[i] | ((uint32_t)dst[i + 1u] << 8) | ((uint32_t)dst[i + 2u] << 16) | ((uint32_t)dst[i + 3u] << 24));
	}
	/* keep a copy of the initial state: tiles and wavefront rows reload it */
	put(c, RPI_TRANSFER, RPI_PROB_BACKUP);
}


static void write_bitstream(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	uint32_t off = sl->data_offset & 63u;

	put_bfbase(c, sl->data_offset);
	put(c, RPI_BFNUM, sl->data_len);
	put(c, RPI_BFCONTROL, off | RPI_BFCONTROL_STOP);
	/* the slice data keeps its emulation prevention bytes */
	put(c, RPI_BFCONTROL, off | RPI_BFCONTROL_EMU);
}


static uint32_t slice_reg_const(const rpivid_slice_t *sl)
{
	uint32_t x = sl->max_num_merge_cand | ((uint32_t)sl->nb_refs[0] << 4) | ((uint32_t)sl->nb_refs[1] << 8) |
		((uint32_t)sl->slice_type << 12);

	if (sl->sao_luma != 0u) {
		x |= 1u << 14;
	}
	if (sl->sao_chroma != 0u) {
		x |= 1u << 15;
	}
	if ((sl->slice_type == SLICE_B) && (sl->mvd_l1_zero != 0u)) {
		x |= 1u << 16;
	}
	return x;
}


static void new_slice_segment(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t i, sps1;

	put(c, RPI_SPS0, (uint32_t)p->log2_min_cb | ((uint32_t)p->log2_ctb << 4) | ((uint32_t)p->log2_min_tb << 8) |
		((uint32_t)p->log2_max_tb << 12) | ((uint32_t)p->bit_depth << 16) | ((uint32_t)p->bit_depth << 20) |
		((uint32_t)p->max_trafo_depth_intra << 24) | ((uint32_t)p->max_trafo_depth_inter << 28));

	/* without PCM the PCM fields hold what their syntax elements give when all zero */
	if (p->pcm != 0u) {
		sps1 = (uint32_t)p->pcm_bit_depth | ((uint32_t)p->pcm_bit_depth_chroma << 4) |
			((uint32_t)p->log2_min_pcm_cb << 8) | ((uint32_t)p->log2_max_pcm_cb << 12);
	}
	else {
		sps1 = 1u | (1u << 4) | (3u << 8) | (3u << 12);
	}
	sps1 |= ((uint32_t)p->chroma_format_idc << 16) | ((uint32_t)(p->amp != 0u) << 18) | ((uint32_t)(p->pcm != 0u) << 19) |
		((uint32_t)(p->scaling_list != 0u) << 20) | ((uint32_t)(p->strong_intra_smoothing != 0u) << 21);
	put(c, RPI_SPS1, sps1);

	put(c, RPI_PPS, ((uint32_t)(p->log2_ctb - p->diff_cu_qp_delta_depth)) | ((uint32_t)(p->cu_qp_delta != 0u) << 4) |
		((uint32_t)(p->transquant_bypass != 0u) << 5) | ((uint32_t)(p->transform_skip != 0u) << 6) |
		((uint32_t)(p->sign_data_hiding != 0u) << 7) |
		(((uint32_t)(p->pps_cb_qp_offset + sl->slice_cb_qp_offset) & 255u) << 8) |
		(((uint32_t)(p->pps_cr_qp_offset + sl->slice_cr_qp_offset) & 255u) << 16) |
		((uint32_t)(p->constrained_intra_pred != 0u) << 24));

	if ((c->start_ts == 0u) && (p->scaling_list != 0u) && (p->scaling_factors != NULL)) {
		const uint8_t *f = p->scaling_factors;

		for (i = 0; i < RPIVID_SCALING_BYTES; i += 4u) {
			put(c, RPI_SCALINGBASE + i, f[i] | ((uint32_t)f[i + 1u] << 8) | ((uint32_t)f[i + 2u] << 16) | ((uint32_t)f[i + 3u] << 24));
		}
	}

	if (sl->dependent == 0u) {
		c->reg_slicestart = (sl->slice_segment_addr % c->ctb_w) | ((sl->slice_segment_addr / c->ctb_w) << 16);
	}
	put(c, RPI_SLICESTART, c->reg_slicestart);
}


static void msg(rpivid_cmd_t *c, uint32_t m)
{
	if (c->nmsgs < sizeof(c->msgs) / sizeof(c->msgs[0])) {
		c->msgs[c->nmsgs++] = (uint16_t)m;
	}
	else {
		c->err = -EINVAL;
	}
}


static void ref_msgs(rpivid_cmd_t *c, const rpivid_slice_t *sl, unsigned int list, int weighted)
{
	unsigned int i;

	for (i = 0; i < sl->nb_refs[list]; i++) {
		msg(c, sl->ref_slot[list][i] | ((sl->ref_long_term[list][i] != 0u) ? (1u << 4) : 0u) | (weighted ? (3u << 5) : 0u));
		msg(c, (uint32_t)sl->ref_poc[list][i] & 0xffffu);
		if (weighted) {
			msg(c, sl->luma_log2_weight_denom | (((uint32_t)sl->luma_weight[list][i] & 0x1ffu) << 3));
			msg(c, (uint32_t)sl->luma_offset[list][i] & 0xffu);
			msg(c, sl->chroma_log2_weight_denom | (((uint32_t)sl->chroma_weight[list][i][0] & 0x1ffu) << 3));
			msg(c, (uint32_t)sl->chroma_offset[list][i][0] & 0xffu);
			msg(c, sl->chroma_log2_weight_denom | (((uint32_t)sl->chroma_weight[list][i][1] & 0x1ffu) << 3));
			msg(c, (uint32_t)sl->chroma_offset[list][i][1] & 0xffu);
		}
	}
}


/* The slice message array: slice kind, reference list descriptors, deblocking, QP offsets */
static void slice_msgs(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t cmd_slice, deblock, qpoff, i;
	int coll_l0, no_backward, lfas;

	c->nmsgs = 0;
	cmd_slice = (sl->slice_type == SLICE_I) ? 1u : ((sl->slice_type == SLICE_P) ? 2u : 3u);
	cmd_slice |= ((uint32_t)sl->nb_refs[0] << 2) | ((uint32_t)sl->nb_refs[1] << 6) | ((uint32_t)sl->max_num_merge_cand << 11);
	coll_l0 = (p->slice_temporal_mvp == 0u) || (sl->slice_type != SLICE_B) || (sl->collocated_from_l0 != 0u);
	cmd_slice |= (uint32_t)coll_l0 << 14;

	if (sl->slice_type != SLICE_I) {
		/* NoBackwardPredFlag (8.5.3.2.2): no reference follows the current picture */
		no_backward = 1;
		for (i = 0; i < sl->nb_refs[0]; i++) {
			if (sl->ref_poc[0][i] > sl->cur_poc) {
				no_backward = 0;
			}
		}
		for (i = 0; i < sl->nb_refs[1]; i++) {
			if (sl->ref_poc[1][i] > sl->cur_poc) {
				no_backward = 0;
			}
		}
		msg(c, cmd_slice | ((uint32_t)no_backward << 10));
		i = (sl->slice_type == SLICE_P) ? p->weighted_pred : p->weighted_bipred;
		ref_msgs(c, sl, 0, (int)i);
		ref_msgs(c, sl, 1, (int)i);
	}
	else {
		msg(c, cmd_slice);
	}

	/* slice_loop_filter_across_slices_enabled_flag governs the boundaries between slices:
	 * a one-slice picture has none, and sends it set as in the proven programming */
	lfas = (p->one_slice != 0u) || (sl->loop_filter_across_slices != 0u);
	deblock = ((uint32_t)sl->beta_offset_div2 & 15u) | (((uint32_t)sl->tc_offset_div2 & 15u) << 4) |
		((sl->deblocking_disabled != 0u) ? (1u << 8) : 0u) | (lfas ? (1u << 9) : 0u);
	/* across-tiles filtering means nothing without tiles; left clear as the proven form has it */
	if ((p->tiles != 0u) && (p->loop_filter_across_tiles != 0u)) {
		deblock |= 1u << 10;
	}
	qpoff = (((uint32_t)sl->slice_cr_qp_offset & 31u) << 5) | ((uint32_t)sl->slice_cb_qp_offset & 31u);
	/* Every slice sends all its messages, an I slice too, as the driver does. An I
	 * picture that sent none (the form tools/hevc-decode used) was decoded with the
	 * previous picture's deblocking state: on the Pi (build 54) every such failure
	 * followed a picture with deblocking off or offset, from the same stream or the
	 * previous one decoded in the process */
	msg(c, deblock);
	msg(c, qpoff);
}


static void program_slicecmds(rpivid_cmd_t *c)
{
	uint32_t i;

	put(c, RPI_SLICECMDS, c->nmsgs | (c->slice_idx << 8));
	for (i = 0; i < c->nmsgs; i++) {
		put(c, RPI_SLICEMSGBASE + 4u * i, c->msgs[i]);
	}
}


static void write_slice(rpivid_cmd_t *c, uint32_t slice_const, uint32_t ctb_col, uint32_t ctb_row)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t cs = 1u << p->log2_ctb, w_last = p->width & (cs - 1u), h_last = p->height & (cs - 1u);
	/* the width/height of the entry's last CTB column/row: partial at the picture edge */
	uint32_t w = (((ctb_col + 1u) < c->ctb_w) || (w_last == 0u)) ? cs : w_last;
	uint32_t h = (((ctb_row + 1u) < c->ctb_h) || (h_last == 0u)) ? cs : h_last;

	put(c, RPI_SLICE, slice_const | (w << 17) | (h << 24));
}


static void new_entry_point(rpivid_cmd_t *c, int do_bte, int reset_qp, uint32_t pause_mode, uint32_t tile_x, uint32_t tile_y,
	uint32_t ctb_col, uint32_t ctb_row, uint32_t slice_qp, uint32_t slice_const)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t endx = p->col_bd[tile_x + 1u] - 1u;
	uint32_t endy = (pause_mode == RPI_MODE_WPP) ? ctb_row : (p->row_bd[tile_y + 1u] - 1u);

	put(c, RPI_TILESTART, p->col_bd[tile_x] | (p->row_bd[tile_y] << 16));
	put(c, RPI_TILEEND, endx | (endy << 16));
	if (do_bte) {
		put(c, RPI_BEGINTILEEND, endx | (endy << 16));
	}
	write_slice(c, slice_const, endx, endy);
	if (reset_qp) {
		put(c, RPI_QP, 6u * ((uint32_t)p->bit_depth - 8u) + slice_qp);
	}
	put(c, RPI_MODE, pause_mode | ((endx == c->ctb_w - 1u) ? RPI_MODE_LASTCOL : 0u) | ((endy == c->ctb_h - 1u) ? RPI_MODE_LASTROW : 0u));
	put(c, RPI_CONTROL, ctb_col | (ctb_row << 16));

	c->entry_tile_x = tile_x;
	c->entry_tile_y = tile_y;
	c->entry_ctb_x = ctb_col;
	c->entry_ctb_y = ctb_row;
	c->entry_qp = slice_qp;
	c->entry_slice = slice_const;
}


/* ---- wavefront (entropy_coding_sync) ---- */

static void wpp_pause(rpivid_cmd_t *c, uint32_t ctb_row)
{
	put(c, RPI_STATUS, (ctb_row << 18) | 0x25u);
	put(c, RPI_TRANSFER, RPI_PROB_BACKUP);
	put(c, RPI_MODE, (ctb_row == c->ctb_h - 1u) ? 0x70000u : 0x30000u);
	put(c, RPI_CONTROL, (ctb_row << 16) + 2u);
}


static void wpp_entry_fill(rpivid_cmd_t *c, uint32_t last_y)
{
	uint32_t last_x = c->ctb_w - 1u;

	while ((c->entry_ctb_y < last_y) && (c->err == 0)) {
		if (c->ctb_w > 2u) {
			wpp_pause(c, c->entry_ctb_y);
		}
		put(c, RPI_STATUS, (c->entry_ctb_y << 18) | (last_x << 5) | 2u);
		/* two CTBs wide: the saved state is still the initial one */
		put(c, RPI_TRANSFER, (c->ctb_w == 2u) ? RPI_PROB_BACKUP : RPI_PROB_RELOAD);
		new_entry_point(c, 0, 1, RPI_MODE_WPP, 0, 0, 0, c->entry_ctb_y + 1u, c->entry_qp, c->entry_slice);
	}
}


static void wpp_end_previous_slice(rpivid_cmd_t *c)
{
	wpp_entry_fill(c, c->prev_ctb_y);
	if ((c->entry_ctb_x < 2u) && ((c->entry_ctb_y < c->start_ctb_y) || (c->start_ctb_x > 2u)) && (c->ctb_w > 2u)) {
		wpp_pause(c, c->prev_ctb_y);
	}
	put(c, RPI_STATUS, 1u | (c->prev_ctb_x << 5) | (c->prev_ctb_y << 18));
	if ((c->start_ctb_x == 2u) || ((c->ctb_w == 2u) && (c->entry_ctb_y < c->start_ctb_y))) {
		put(c, RPI_TRANSFER, RPI_PROB_BACKUP);
	}
}


static void wpp_slice(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	int indep = (sl->dependent == 0u), reset_qp = 1;

	if (c->start_ts != 0u) {
		wpp_end_previous_slice(c);
	}
	slice_msgs(c, sl);
	write_bitstream(c, sl);
	if ((c->start_ts == 0u) || indep || (c->ctb_w == 1u)) {
		write_prob(c, sl);
	}
	else if (c->start_ctb_x == 0u) {
		put(c, RPI_TRANSFER, RPI_PROB_RELOAD);
	}
	else {
		reset_qp = 0;
	}
	program_slicecmds(c);
	new_slice_segment(c, sl);
	new_entry_point(c, indep, reset_qp, RPI_MODE_WPP, 0, 0, c->start_ctb_x, c->start_ctb_y, (uint32_t)sl->slice_qp, slice_reg_const(sl));
}


/* ---- tiles (and the single implicit tile) ---- */

static uint32_t ctb_to_tile(uint32_t ctb, const uint32_t *bd)
{
	uint32_t i = 1;

	while (ctb >= bd[i]) {
		i++;
	}
	return i - 1u;
}


static void tile_entry_fill(rpivid_cmd_t *c, uint32_t last_tile_x, uint32_t last_tile_y)
{
	const rpivid_pic_t *p = c->pic;

	while (((c->entry_tile_y < last_tile_y) || ((c->entry_tile_y == last_tile_y) && (c->entry_tile_x < last_tile_x))) && (c->err == 0)) {
		uint32_t t_x = c->entry_tile_x, t_y = c->entry_tile_y;

		put(c, RPI_STATUS, 2u | ((p->col_bd[t_x + 1u] - 1u) << 5) | ((p->row_bd[t_y + 1u] - 1u) << 18));
		put(c, RPI_TRANSFER, RPI_PROB_RELOAD);
		if (++t_x >= p->num_tile_cols) {
			t_x = 0;
			t_y++;
		}
		new_entry_point(c, 0, 1, RPI_MODE_TILE, t_x, t_y, p->col_bd[t_x], p->row_bd[t_y], c->entry_qp, c->entry_slice);
	}
}


static void tile_slice(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t tile_x = ctb_to_tile(c->start_ctb_x, p->col_bd), tile_y = ctb_to_tile(c->start_ctb_y, p->row_bd);
	uint32_t prev_tx = ctb_to_tile(c->prev_ctb_x, p->col_bd), prev_ty = ctb_to_tile(c->prev_ctb_y, p->row_bd);
	int reset_qp;

	if (c->start_ts != 0u) {
		tile_entry_fill(c, prev_tx, prev_ty);
		put(c, RPI_STATUS, 1u | (c->prev_ctb_x << 5) | (c->prev_ctb_y << 18));
	}
	slice_msgs(c, sl);
	write_bitstream(c, sl);
	reset_qp = (c->start_ts == 0u) || (sl->dependent == 0u) || (tile_x != prev_tx) || (tile_y != prev_ty);
	if (reset_qp) {
		write_prob(c, sl);
	}
	program_slicecmds(c);
	new_slice_segment(c, sl);
	new_entry_point(c, sl->dependent == 0u, reset_qp, RPI_MODE_TILE, tile_x, tile_y, c->start_ctb_x, c->start_ctb_y,
		(uint32_t)sl->slice_qp, slice_reg_const(sl));
}


void rpivid_cmd_begin(rpivid_cmd_t *c, const rpivid_pic_t *pic)
{
	uint32_t cs = 1u << pic->log2_ctb;

	c->len = 0;
	c->nbfbase = 0;
	c->err = 0;
	c->pic = pic;
	c->ctb_w = (pic->width + cs - 1u) >> pic->log2_ctb;
	c->ctb_h = (pic->height + cs - 1u) >> pic->log2_ctb;
	c->slice_idx = 0;
	c->entry_tile_x = c->entry_tile_y = c->entry_ctb_x = c->entry_ctb_y = 0;
	c->entry_qp = c->entry_slice = 0;
	c->reg_slicestart = 0;
}


int rpivid_cmd_slice(rpivid_cmd_t *c, const rpivid_slice_t *sl)
{
	const rpivid_pic_t *p = c->pic;
	uint32_t prev_rs, nctb = c->ctb_w * c->ctb_h;

	if (c->err != 0) {
		return c->err;
	}
	if ((sl->slice_segment_addr >= nctb) || (sl->slice_type > SLICE_I) || (sl->nb_refs[0] > 16u) || (sl->nb_refs[1] > 16u) ||
			((c->slice_idx == 0u) && (sl->slice_segment_addr != 0u))) {
		c->err = -EINVAL;
		return c->err;
	}
	c->start_ts = (p->ctb_addr_rs_to_ts != NULL) ? (uint32_t)p->ctb_addr_rs_to_ts[sl->slice_segment_addr] : sl->slice_segment_addr;
	c->start_ctb_x = sl->slice_segment_addr % c->ctb_w;
	c->start_ctb_y = sl->slice_segment_addr / c->ctb_w;
	if (c->start_ts == 0u) {
		prev_rs = 0;
	}
	else {
		prev_rs = (p->ctb_addr_ts_to_rs != NULL) ? (uint32_t)p->ctb_addr_ts_to_rs[c->start_ts - 1u] : c->start_ts - 1u;
	}
	c->prev_ctb_x = prev_rs % c->ctb_w;
	c->prev_ctb_y = prev_rs / c->ctb_w;

	if (p->entropy_coding_sync != 0u) {
		wpp_slice(c, sl);
	}
	else {
		tile_slice(c, sl);
	}
	c->slice_idx++;
	return c->err;
}


int rpivid_cmd_end(rpivid_cmd_t *c)
{
	if ((c->err == 0) && (c->slice_idx == 0u)) {
		c->err = -EINVAL;
	}
	if (c->err != 0) {
		return c->err;
	}
	if (c->pic->entropy_coding_sync != 0u) {
		wpp_entry_fill(c, c->ctb_h - 1u);
		if ((c->entry_ctb_x < 2u) && (c->ctb_w > 2u)) {
			wpp_pause(c, c->ctb_h - 1u);
		}
	}
	else {
		tile_entry_fill(c, c->pic->num_tile_cols - 1u, c->pic->num_tile_rows - 1u);
	}
	put(c, RPI_STATUS, 1u | ((c->ctb_w - 1u) << 5) | ((c->ctb_h - 1u) << 18));
	return (c->err != 0) ? c->err : (int)c->len;
}


uint32_t rpivid_config2(const rpivid_pic_t *p, int write_colmv, int read_colmv)
{
	uint32_t c = (uint32_t)p->bit_depth | ((uint32_t)p->bit_depth << 4);

	if (p->bit_depth != 8u) {
		c |= (1u << 8) | (1u << 9);
	}
	c |= (uint32_t)p->log2_ctb << 10;
	if (p->constrained_intra_pred != 0u) {
		c |= 1u << 13;
	}
	if (p->strong_intra_smoothing != 0u) {
		c |= 1u << 14;
	}
	if (write_colmv) {
		c |= 1u << 15;
	}
	c |= (uint32_t)p->log2_parallel_merge_level << 16;
	if (read_colmv) {
		c |= 1u << 19;
	}
	if ((p->pcm != 0u) && (p->pcm_loop_filter_disabled != 0u)) {
		c |= 1u << 20;
	}
	c |= ((uint32_t)p->pps_cb_qp_offset & 31u) << 21;
	c |= ((uint32_t)p->pps_cr_qp_offset & 31u) << 26;
	return c;
}


/* Upsample an 8x8 list to n x n (n = 16 or 32), DC replacing the first factor */
static void expand_list(uint8_t *dst, const uint8_t *src, unsigned int n, uint8_t dc)
{
	unsigned int x, y, f = n / 8u;

	for (y = 0; y < n; y++) {
		for (x = 0; x < n; x++) {
			dst[y * n + x] = src[(y / f) * 8u + x / f];
		}
	}
	dst[0] = dc;
}


void rpivid_scaling_factors(uint8_t out[RPIVID_SCALING_BYTES], const uint8_t sl4[6][16], const uint8_t sl8[6][64],
	const uint8_t sl16[6][64], const uint8_t dc16[6], const uint8_t sl32[2][64], const uint8_t dc32[2])
{
	unsigned int m;

	/* layout: 4x4 lists at 0x000 (16 B each), 8x8 at 0x060 (64 B), 16x16 at 0x1e0 (256 B),
	 * 32x32 at 0x7e0 (1024 B, intra and inter luma only) */
	memset(out, 0, RPIVID_SCALING_BYTES);
	for (m = 0; m < 6u; m++) {
		memcpy(out + 0x000u + 16u * m, sl4[m], 16);
		memcpy(out + 0x060u + 64u * m, sl8[m], 64);
		expand_list(out + 0x1e0u + 256u * m, sl16[m], 16, dc16[m]);
	}
	for (m = 0; m < 2u; m++) {
		expand_list(out + 0x7e0u + 1024u * m, sl32[m], 32, dc32[m]);
	}
}


void rpivid_geom(rpivid_geom_t *g, uint32_t width, uint32_t height, unsigned int bit_depth)
{
	uint32_t col_bytes;

	/* a column holds the whole picture height (rounded to 16 rows) of 128 bytes:
	 * 128 samples at 8 bits, 96 (3 per 32-bit word) at 10 bits */
	g->luma_stride = ((height + 15u) & ~15u) * 128u;
	g->chroma_stride = g->luma_stride / 2u;
	if (bit_depth == 8u) {
		col_bytes = (width + 127u) & ~127u;
	}
	else {
		col_bytes = ((((width + 2u) / 3u) + 31u) & ~31u) * 4u;
	}
	g->cols = (col_bytes + 127u) / 128u;
	g->luma_size = (size_t)g->luma_stride * g->cols + 4096u;
	g->chroma_size = (size_t)g->chroma_stride * g->cols + 4096u;
	/* collocated motion vectors (the driver's setup_colmv sizing) */
	g->colmv_stride = (width + 63u) & ~63u;
	g->colmv_size = (size_t)g->colmv_stride * (((height + 63u) & ~63u) >> 4);
	g->col_height = 0;
	g->chroma_offset = 0;
}


void rpivid_geom_col128(rpivid_geom_t *g, uint32_t width, uint32_t height, unsigned int bit_depth)
{
	uint32_t h16 = (height + 15u) & ~15u;

	rpivid_geom(g, width, height, bit_depth);
	g->col_height = h16 + h16 / 2u;
	g->luma_stride = g->col_height * 128u;
	g->chroma_stride = g->luma_stride;
	g->chroma_offset = (size_t)h16 * 128u;
	g->luma_size = (size_t)g->luma_stride * g->cols;
	g->chroma_size = 0;
}
