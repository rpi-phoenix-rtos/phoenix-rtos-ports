/*
 * Phoenix-RTOS
 *
 * FFmpeg HEVC decoder + the BCM2711 rpivid block (see rpivid_hevc.h)
 *
 * FFmpeg's HEVCContext supplies what the Linux hevc_dec driver gets from V4L2's
 * stateless HEVC controls (SPS, PPS, slice headers, the DPB, the reference lists);
 * rpivid_cmd.c turns it into the block's command buffer, rpivid_hw.c runs it,
 * rpivid_sand.c converts the result into the frame hevcdec.c allocated.
 *
 * Per picture: the slices' NAL units are copied into one bitstream buffer while the
 * command buffer is built (decode_slice), then the block decodes the picture into the
 * SAND buffers owned by the frame's hwaccel private data (end_frame). Those buffers stay
 * with the HEVCFrame for as long as hevcdec.c keeps it, which is exactly as long as the
 * picture can be a reference.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "get_bits.h"
#include "golomb.h"
#include "hevcdec.h"
#include "hwaccel_internal.h"
#include "internal.h"
#include "refstruct.h"

#include "rpivid_cmd.h"
#include "rpivid_hevc.h"
#include "rpivid_hw.h"
#include "rpivid_sand.h"

#define STAT_EVERY 300

typedef struct RPIVIDBuf {
	rpivid_dma_t y, c, mv;
	struct RPIVIDBuf *next;
} RPIVIDBuf;

/* The SAND buffers of one stream geometry. Frames hold a reference, so buffers
 * outlive the hwaccel when frames do. */
typedef struct RPIVIDPool {
	AVMutex lock;
	int refs;
	rpivid_geom_t g;
	int with_mv;
	RPIVIDBuf *free;
	int nbufs;
} RPIVIDPool;

/* hwaccel_picture_private of an HEVCFrame */
typedef struct RPIVIDFrame {
	RPIVIDPool *pool;
	RPIVIDBuf *buf;
	int decoded;                   /* the block wrote buf: usable as a reference */
	int has_mv;                    /* and its motion vectors (a collocated picture) */
} RPIVIDFrame;

typedef struct RPIVIDContext {
	rpivid_hw_t *hw;
	RPIVIDPool *pool;
	uint32_t enabled;              /* the tools (T_*) the block may be given */
	uint32_t sps_tools;            /* the active SPS's */
	uint32_t used;                 /* every tool the stream used so far */
	rpivid_dma_t bs;               /* the picture's slice NAL units */
	size_t bs_used;
	rpivid_dma_t dummy_mv;         /* a zeroed collocated MV buffer for a missing picture */
	rpivid_cmd_t cmd;
	rpivid_pic_t pic;
	uint8_t scaling[RPIVID_SCALING_BYTES];

	/* the picture's reference slots, in first-use order over its slices' lists */
	const HEVCFrame *slot[16];
	uint64_t slot_addr[16][2];
	int nslots;
	int write_mv;
	int nslices;
	int one_slice;                 /* the picture is one slice segment (picture_ok) */
	int missing_refs;
	uint64_t slice_ns;             /* the picture's decode_slice calls: bitstream copy, slice commands */
	const char *frame_err;
	int failed;

	/* statistics (ns): all frames, and the current reporting window */
	uint64_t n, sum_slices, sum_p1, sum_p2, sum_detile;
	uint64_t wn, w_slices, w_p1, w_p2, w_detile;
	uint64_t t_start;
	uint32_t p1_reruns, missing_total;
} RPIVIDContext;

static const FFHWAccel ff_hevc_rpivid_hwaccel;


/* ---- the SAND buffer pool ---- */

static RPIVIDPool *pool_new(const rpivid_geom_t *g, int with_mv)
{
	RPIVIDPool *p = av_mallocz(sizeof(*p));

	if (p == NULL) {
		return NULL;
	}
	ff_mutex_init(&p->lock, NULL);
	p->refs = 1;
	p->g = *g;
	p->with_mv = with_mv;
	return p;
}


static void buf_free(RPIVIDBuf *b)
{
	rpivid_dma_free(&b->y);
	rpivid_dma_free(&b->c);
	rpivid_dma_free(&b->mv);
	av_free(b);
}


static RPIVIDPool *pool_ref(RPIVIDPool *p)
{
	ff_mutex_lock(&p->lock);
	p->refs++;
	ff_mutex_unlock(&p->lock);
	return p;
}


static void pool_unref(RPIVIDPool *p)
{
	RPIVIDBuf *b;
	int last;

	if (p == NULL) {
		return;
	}
	ff_mutex_lock(&p->lock);
	last = (--p->refs == 0);
	ff_mutex_unlock(&p->lock);
	if (!last) {
		return;
	}
	while ((b = p->free) != NULL) {
		p->free = b->next;
		buf_free(b);
	}
	ff_mutex_destroy(&p->lock);
	av_free(p);
}


static RPIVIDBuf *pool_get(RPIVIDPool *p)
{
	RPIVIDBuf *b;

	ff_mutex_lock(&p->lock);
	b = p->free;
	if (b != NULL) {
		p->free = b->next;
	}
	ff_mutex_unlock(&p->lock);
	if (b != NULL) {
		b->next = NULL;
		return b;
	}

	b = av_mallocz(sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	if ((rpivid_dma_alloc(&b->y, p->g.luma_size) < 0) || (rpivid_dma_alloc(&b->c, p->g.chroma_size) < 0) ||
			(p->with_mv && (rpivid_dma_alloc(&b->mv, p->g.colmv_size) < 0))) {
		buf_free(b);
		return NULL;
	}
	ff_mutex_lock(&p->lock);
	p->nbufs++;
	ff_mutex_unlock(&p->lock);
	return b;
}


static void pool_put(RPIVIDPool *p, RPIVIDBuf *b)
{
	ff_mutex_lock(&p->lock);
	b->next = p->free;
	p->free = b;
	ff_mutex_unlock(&p->lock);
}


static void rpivid_frame_free(FFRefStructOpaque opaque, void *data)
{
	RPIVIDFrame *f = data;

	(void)opaque;
	if (f->buf != NULL) {
		pool_put(f->pool, f->buf);
	}
	pool_unref(f->pool);
}


/* ---- what the block takes ----
 *
 * Hard limits -- the block's own, as the Raspberry Pi Linux hevc_dec driver states them
 * (Main and Main 10 4:2:0, 32..4096 luma samples a side, CTB <= 64, TB <= 32) -- send a
 * stream to the CPU at any level. Inside them every coding tool beyond the set the block
 * was first proved bit-exact on (x265's defaults: CTB 64, CB >= 8, TB 4..32, a transform
 * tree of depth 0, one slice segment per picture, CU QP delta at depth 1, sign data
 * hiding, deblocking with no offsets, parallel merge level 2; WPP, SAO, TMVP, weighted
 * prediction, 8 and 10 bit are part of that set) is a tool of its own below, so a tool
 * that hevc-rpivid-check finds wrong on the Pi turns off alone: in tools[] (its default)
 * or at run time, FFMPEG_RPIVID_TOOLS="-amp,-tiles" (+name turns one on, "none" leaves
 * the proven set, "all" every tool).
 *
 * Each tool is a field the block's programming already carries (SPS0/SPS1/PPS register
 * fields, the scaling factor array, the slice messages, the entry-point sequence of
 * slices and tiles: rpivid_cmd.c, written as the Linux driver programs the block); "on"
 * puts it in the default set, the others are for level 2 (rare in real-world streams). */

enum {
	T_CTB32, T_CTB16, T_BLOCKS, T_TU_DEPTH_INTRA, T_TU_DEPTH_INTER, T_AMP, T_SCALING_LIST, T_PCM, T_LONG_TERM,
	T_NO_STRONG_SMOOTHING, T_CU_QP_DELTA, T_NO_SIGN_HIDING, T_TRANSFORM_SKIP, T_TRANSQUANT_BYPASS, T_CONSTRAINED_INTRA,
	T_CABAC_INIT, T_CHROMA_QP_OFFSET, T_DEBLOCKING, T_MERGE_LEVEL, T_TILES, T_SLICES, T_DEPENDENT_SLICES, T_NTOOLS
};

#define TOOL(t) (1u << (t))

static const struct {
	const char *name;
	const char *what;
	int on;                        /* in the default set (level 1) */
} tools[T_NTOOLS] = {
	[T_CTB32] = { "ctb32", "32x32 CTBs", 1 },
	[T_CTB16] = { "ctb16", "16x16 CTBs", 1 },
	[T_BLOCKS] = { "blocks", "coding blocks >= 16 or transform blocks other than 4..32", 1 },
	[T_TU_DEPTH_INTRA] = { "tu_depth_intra", "transform tree depth > 0 in intra CUs", 1 },
	[T_TU_DEPTH_INTER] = { "tu_depth_inter", "transform tree depth > 0 in inter CUs", 1 },
	[T_AMP] = { "amp", "asymmetric motion partitions", 1 },
	[T_SCALING_LIST] = { "scaling_list", "scaling lists (default or coded)", 1 },
	[T_PCM] = { "pcm", "PCM coding units", 0 },
	[T_LONG_TERM] = { "long_term", "long-term reference pictures", 0 },
	[T_NO_STRONG_SMOOTHING] = { "no_strong_smoothing", "strong intra smoothing off", 1 },
	[T_CU_QP_DELTA] = { "cu_qp_delta", "CU QP delta off or at a depth other than 1", 1 },
	[T_NO_SIGN_HIDING] = { "no_sign_hiding", "sign data hiding off", 1 },
	[T_TRANSFORM_SKIP] = { "transform_skip", "transform skip", 1 },
	[T_TRANSQUANT_BYPASS] = { "transquant_bypass", "transquant bypass (lossless CUs)", 0 },
	[T_CONSTRAINED_INTRA] = { "constrained_intra", "constrained intra prediction", 0 },
	[T_CABAC_INIT] = { "cabac_init", "cabac_init_flag (swapped P/B CABAC init tables)", 1 },
	[T_CHROMA_QP_OFFSET] = { "chroma_qp_offset", "picture or slice chroma QP offsets", 1 },
	[T_DEBLOCKING] = { "deblocking", "deblocking off, offsets or per-slice control", 1 },
	[T_MERGE_LEVEL] = { "merge_level", "parallel merge level other than 2", 1 },
	[T_TILES] = { "tiles", "tiles", 0 },
	[T_SLICES] = { "slices", "several slice segments per picture", 1 },
	[T_DEPENDENT_SLICES] = { "dependent_slices", "dependent slice segments (in a picture)", 0 },
};


static uint32_t tools_of_level(int level)
{
	uint32_t m = 0;
	int t;

	for (t = 0; t < T_NTOOLS; t++) {
		if ((level >= RPIVID_LEVEL_ALL) || tools[t].on) {
			m |= TOOL(t);
		}
	}
	return m;
}


/* The level's tools as FFMPEG_RPIVID_TOOLS changes them */
static uint32_t tools_enabled(void *logctx, int level)
{
	const char *e = getenv("FFMPEG_RPIVID_TOOLS");
	uint32_t m = tools_of_level(level);
	char item[32];
	size_t n;
	int t;

	while ((e != NULL) && (*e != '\0')) {
		int on = 1;

		e += strspn(e, ", ");
		n = strcspn(e, ", ");
		if ((n == 0u) || (n >= sizeof(item))) {
			e += n;
			continue;
		}
		memcpy(item, e, n);
		item[n] = '\0';
		e += n;
		if ((item[0] == '+') || (item[0] == '-')) {
			on = (item[0] == '+');
			memmove(item, item + 1, n);
		}
		if (strcmp(item, "none") == 0) {
			m = on ? 0u : m;
			continue;
		}
		if (strcmp(item, "all") == 0) {
			m = on ? tools_of_level(RPIVID_LEVEL_ALL) : 0u;
			continue;
		}
		for (t = 0; (t < T_NTOOLS) && (strcmp(item, tools[t].name) != 0); t++) {
		}
		if (t == T_NTOOLS) {
			av_log(logctx, AV_LOG_WARNING, "rpivid: FFMPEG_RPIVID_TOOLS: no tool \"%s\"\n", item);
		}
		else if (on) {
			m |= TOOL(t);
		}
		else {
			m &= ~TOOL(t);
		}
	}
	return m;
}


/* "a,b,c" of the tools in m ("-" for none), then " (not in the default set: c)" when
 * some of them are not (hevc-rpivid-check reads both lists) */
static const char *tool_list(char *buf, size_t len, uint32_t m)
{
	uint32_t nd = m & ~tools_of_level(RPIVID_LEVEL_DEFAULT);
	size_t o = 0;
	int t, pass;

	buf[0] = '\0';
	for (pass = 0; pass < 2; pass++) {
		uint32_t sel = (pass == 0) ? m : nd;
		int k = 0;

		if ((pass == 1) && (nd != 0u)) {
			o += (size_t)snprintf(buf + o, len - o, " (not in the default set: ");
		}
		for (t = 0; (t < T_NTOOLS) && (o < len); t++) {
			if ((sel & TOOL(t)) != 0u) {
				o += (size_t)snprintf(buf + o, len - o, "%s%s", (k++ != 0) ? "," : "", tools[t].name);
			}
		}
		if ((pass == 0) && (k == 0) && (o < len)) {
			o += (size_t)snprintf(buf + o, len - o, "-");
		}
		if ((pass == 1) && (nd != 0u) && (o < len)) {
			o += (size_t)snprintf(buf + o, len - o, ")");
		}
	}
	return buf;
}


#define NOPE(...) do { snprintf(why, whylen, __VA_ARGS__); return -1; } while (0)

/* The SPS's tools beyond the proven set, or -1 (why) outside the block's limits */
static int64_t sps_tools(const HEVCSPS *sps, char *why, size_t whylen)
{
	uint32_t m = 0;

	if ((sps->chroma_format_idc != 1) || sps->separate_colour_plane_flag) {
		NOPE("chroma format %d (the block decodes 4:2:0)", sps->chroma_format_idc);
	}
	if ((sps->bit_depth != sps->bit_depth_chroma) || ((sps->bit_depth != 8) && (sps->bit_depth != 10))) {
		NOPE("bit depth %d/%d (the block decodes 8 or 10 bit)", sps->bit_depth, sps->bit_depth_chroma);
	}
	if ((sps->width < 32) || (sps->width > 4096) || (sps->height < 32) || (sps->height > 4096)) {
		NOPE("picture size %dx%d (the block decodes 32..4096)", sps->width, sps->height);
	}
	if (sps->transform_skip_rotation_enabled_flag || sps->transform_skip_context_enabled_flag ||
			sps->implicit_rdpcm_enabled_flag || sps->explicit_rdpcm_enabled_flag ||
			sps->extended_precision_processing_flag || sps->intra_smoothing_disabled_flag ||
			sps->high_precision_offsets_enabled_flag || sps->persistent_rice_adaptation_enabled_flag ||
			sps->cabac_bypass_alignment_enabled_flag) {
		NOPE("range extension coding tools");
	}
	if (sps->sps_scc_extension_flag || sps->sps_multilayer_extension_flag || sps->sps_3d_extension_flag) {
		NOPE("screen content / multilayer / 3D extensions");
	}
	if ((sps->log2_ctb_size > 6) || (sps->log2_max_trafo_size > 5) || (sps->log2_max_trafo_size > sps->log2_ctb_size)) {
		NOPE("block sizes CTB %d TB %d", 1 << sps->log2_ctb_size, 1 << sps->log2_max_trafo_size);
	}

	if (sps->log2_ctb_size == 5) {
		m |= TOOL(T_CTB32);
	}
	else if (sps->log2_ctb_size == 4) {
		m |= TOOL(T_CTB16);
	}
	if ((sps->log2_min_cb_size != 3) || (sps->log2_min_tb_size != 2) || (sps->log2_max_trafo_size != 5)) {
		m |= TOOL(T_BLOCKS);
	}
	if (sps->max_transform_hierarchy_depth_intra != 0) {
		m |= TOOL(T_TU_DEPTH_INTRA);
	}
	if (sps->max_transform_hierarchy_depth_inter != 0) {
		m |= TOOL(T_TU_DEPTH_INTER);
	}
	if (sps->amp_enabled_flag) {
		m |= TOOL(T_AMP);
	}
	if (sps->scaling_list_enable_flag) {
		m |= TOOL(T_SCALING_LIST);
	}
	if (sps->pcm_enabled_flag) {
		m |= TOOL(T_PCM);
	}
	if (sps->long_term_ref_pics_present_flag) {
		m |= TOOL(T_LONG_TERM);
	}
	if (!sps->sps_strong_intra_smoothing_enable_flag) {
		m |= TOOL(T_NO_STRONG_SMOOTHING);
	}
	return m;
}


/* The PPS's tools beyond the proven set, or -1 (why) outside the block's limits */
static int64_t pps_tools(const HEVCPPS *pps, char *why, size_t whylen)
{
	uint32_t m = 0;

	if (pps->pps_range_extensions_flag && (pps->cross_component_prediction_enabled_flag || pps->chroma_qp_offset_list_enabled_flag ||
			pps->log2_sao_offset_scale_luma || pps->log2_sao_offset_scale_chroma || (pps->log2_max_transform_skip_block_size > 2))) {
		NOPE("range extension coding tools (PPS)");
	}
	if (pps->pps_scc_extension_flag || pps->pps_multilayer_extension_flag || pps->pps_3d_extension_flag) {
		NOPE("screen content / multilayer / 3D extensions (PPS)");
	}
	if (pps->tiles_enabled_flag && ((pps->num_tile_columns > RPIVID_MAX_TILE_COLS) || (pps->num_tile_rows > RPIVID_MAX_TILE_ROWS) ||
			(pps->entropy_coding_sync_enabled_flag && ((pps->num_tile_columns > 1) || (pps->num_tile_rows > 1))))) {
		NOPE("%dx%d tiles%s", pps->num_tile_columns, pps->num_tile_rows, pps->entropy_coding_sync_enabled_flag ? " with WPP" : "");
	}

	if (pps->tiles_enabled_flag) {
		m |= TOOL(T_TILES);
	}
	if (!pps->cu_qp_delta_enabled_flag || (pps->diff_cu_qp_delta_depth != 1)) {
		m |= TOOL(T_CU_QP_DELTA);
	}
	if (!pps->sign_data_hiding_flag) {
		m |= TOOL(T_NO_SIGN_HIDING);
	}
	if (pps->transform_skip_enabled_flag) {
		m |= TOOL(T_TRANSFORM_SKIP);
	}
	if (pps->transquant_bypass_enable_flag) {
		m |= TOOL(T_TRANSQUANT_BYPASS);
	}
	if (pps->constrained_intra_pred_flag) {
		m |= TOOL(T_CONSTRAINED_INTRA);
	}
	if (pps->cabac_init_present_flag) {
		m |= TOOL(T_CABAC_INIT);
	}
	if (pps->cb_qp_offset || pps->cr_qp_offset || pps->pic_slice_level_chroma_qp_offsets_present_flag) {
		m |= TOOL(T_CHROMA_QP_OFFSET);
	}
	if (pps->disable_dbf || pps->beta_offset || pps->tc_offset || pps->deblocking_filter_override_enabled_flag) {
		m |= TOOL(T_DEBLOCKING);
	}
	if (pps->log2_parallel_merge_level != 2) {
		m |= TOOL(T_MERGE_LEVEL);
	}
	return m;
}


/* A slice segment NAL unit after the first of its picture: is it a dependent one?
 * (7.3.6.1: first_slice_segment_in_pic_flag, no_output_of_prior_pics_flag for IRAP
 * pictures, slice_pic_parameter_set_id, dependent_slice_segment_flag if the PPS allows it) */
static int dependent_segment(const HEVCContext *s, const H2645NAL *nal)
{
	const HEVCPPS *pps;
	GetBitContext gb;
	unsigned int id;

	if ((nal->size <= 2) || (init_get_bits8(&gb, nal->data + 2, nal->size - 2) < 0) || get_bits1(&gb)) {
		return 0;
	}
	if ((nal->type >= HEVC_NAL_BLA_W_LP) && (nal->type <= HEVC_NAL_RSV_IRAP_VCL23)) {
		skip_bits1(&gb);
	}
	id = get_ue_golomb_long(&gb);
	pps = (id < HEVC_MAX_PPS_COUNT) ? s->ps.pps_list[id] : NULL;
	return (pps != NULL) && pps->dependent_slice_segments_enabled_flag && get_bits1(&gb);
}


/* 0, or -1 with why naming the first tool of need that is not enabled */
static int tools_ok(uint32_t need, uint32_t enabled, char *why, size_t whylen)
{
	int t;

	for (t = 0; t < T_NTOOLS; t++) {
		if (((need & ~enabled) & TOOL(t)) != 0u) {
			snprintf(why, whylen, "%s (tool %s off)", tools[t].what, tools[t].name);
			return -1;
		}
	}
	return 0;
}


/* The stream's tools so far grew: say which (hevc-rpivid-check reports them) */
static void tools_used(AVCodecContext *avctx, RPIVIDContext *ctx, uint32_t m)
{
	char list[512];

	if ((m & ~ctx->used) == 0u) {
		return;
	}
	ctx->used |= m;
	av_log(avctx, AV_LOG_INFO, "rpivid: tools in use: %s\n", tool_list(list, sizeof(list), ctx->used));
}


/* ---- attach / detach ---- */

static int rpivid_uninit(AVCodecContext *avctx)
{
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;

	if (ctx == NULL) {
		return 0;
	}
	if (ctx->n != 0u) {
		av_log(avctx, AV_LOG_INFO, "rpivid: %" PRIu64 " pictures on the block: hw %.2f ms (phase 1 %.2f, phase 2 %.2f), "
			"SAND->planar %.2f ms, slices (bitstream copy, commands) %.2f ms per picture; %u phase-1 reruns, %u missing references, %d buffers\n",
			ctx->n, (ctx->sum_p1 + ctx->sum_p2) / 1e6 / ctx->n, ctx->sum_p1 / 1e6 / ctx->n, ctx->sum_p2 / 1e6 / ctx->n,
			ctx->sum_detile / 1e6 / ctx->n, ctx->sum_slices / 1e6 / ctx->n, ctx->p1_reruns, ctx->missing_total,
			(ctx->pool != NULL) ? ctx->pool->nbufs : 0);
	}
	rpivid_cmd_free(&ctx->cmd);
	rpivid_dma_free(&ctx->bs);
	rpivid_dma_free(&ctx->dummy_mv);
	pool_unref(ctx->pool);
	ctx->pool = NULL;
	rpivid_hw_close(ctx->hw);
	ctx->hw = NULL;
	return 0;
}


int ff_rpivid_hevc_active(const AVCodecContext *avctx)
{
	return avctx->hwaccel == &ff_hevc_rpivid_hwaccel.p;
}


int ff_rpivid_hevc_failed(const AVCodecContext *avctx)
{
	const RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;

	return ff_rpivid_hevc_active(avctx) && (ctx != NULL) && ctx->failed;
}


int ff_rpivid_hevc_attach(AVCodecContext *avctx, int level)
{
	const HEVCContext *s = avctx->priv_data;
	const HEVCSPS *sps = s->ps.sps;
	RPIVIDContext *ctx;
	rpivid_geom_t g;
	char why[192], list[512];
	uint32_t enabled, need;
	int64_t m;
	int i, rc;

	if ((level <= RPIVID_LEVEL_OFF) || (sps == NULL) || (avctx->hwaccel != NULL)) {
		return -1;
	}
	enabled = tools_enabled(avctx, level);
	m = sps_tools(sps, why, sizeof(why));
	if ((m < 0) || (tools_ok((uint32_t)m, enabled, why, sizeof(why)) < 0)) {
		av_log(avctx, AV_LOG_INFO, "rpivid: CPU decode: %s\n", why);
		return -1;
	}
	need = (uint32_t)m;
	/* the PPSs of this SPS received so far; a later one is checked by its first picture */
	for (i = 0; i < HEVC_MAX_PPS_COUNT; i++) {
		const HEVCPPS *pps = s->ps.pps_list[i];

		if ((pps == NULL) || (s->ps.sps_list[pps->sps_id] != sps)) {
			continue;
		}
		m = pps_tools(pps, why, sizeof(why));
		if ((m < 0) || (tools_ok((uint32_t)m, enabled, why, sizeof(why)) < 0)) {
			av_log(avctx, AV_LOG_INFO, "rpivid: CPU decode: %s\n", why);
			return -1;
		}
		need |= (uint32_t)m;
	}

	ctx = av_mallocz(sizeof(*ctx));
	if (ctx == NULL) {
		return AVERROR(ENOMEM);
	}
	rc = rpivid_hw_open(&ctx->hw, why, sizeof(why));
	if (rc < 0) {
		av_log(avctx, AV_LOG_INFO, "rpivid: CPU decode: %s\n", why);
		av_free(ctx);
		return -1;
	}
	rpivid_geom(&g, (uint32_t)sps->width, (uint32_t)sps->height, (unsigned int)sps->bit_depth);
	ctx->pool = pool_new(&g, sps->sps_temporal_mvp_enabled_flag);
	if ((ctx->pool == NULL) || (rpivid_dma_alloc(&ctx->dummy_mv, g.colmv_size) < 0)) {
		av_log(avctx, AV_LOG_INFO, "rpivid: CPU decode: out of memory\n");
		pool_unref(ctx->pool);
		rpivid_dma_free(&ctx->dummy_mv);
		rpivid_hw_close(ctx->hw);
		av_free(ctx);
		return -1;
	}
	rpivid_cmd_init(&ctx->cmd);
	ctx->enabled = enabled;
	ctx->sps_tools = (uint32_t)sps_tools(sps, why, sizeof(why));
	ctx->used = need;
	ctx->t_start = (uint64_t)av_gettime_relative();

	avctx->internal->hwaccel_priv_data = ctx;
	avctx->hwaccel = &ff_hevc_rpivid_hwaccel.p;
	/* "tools: " lists what the stream uses beyond the proven set (hevc-rpivid-check reports it) */
	av_log(avctx, AV_LOG_INFO, "rpivid: hardware HEVC decode %dx%d %d-bit, tools: %s, HEVC clock %u MHz, completion %s\n", sps->width,
		sps->height, sps->bit_depth, tool_list(list, sizeof(list), need), rpivid_hw_clock(ctx->hw) / 1000000u,
		rpivid_hw_irq(ctx->hw) ? "by interrupt" : "POLLED (no interrupt)");
	return 0;
}


/* ---- per picture ---- */

static void fill_pic(RPIVIDContext *ctx, const HEVCContext *s)
{
	const HEVCSPS *sps = s->ps.sps;
	const HEVCPPS *pps = s->ps.pps;
	rpivid_pic_t *p = &ctx->pic;
	unsigned int i;

	memset(p, 0, sizeof(*p));
	p->width = (uint32_t)sps->width;
	p->height = (uint32_t)sps->height;
	p->bit_depth = (uint8_t)sps->bit_depth;
	p->chroma_format_idc = (uint8_t)sps->chroma_format_idc;
	p->log2_min_cb = (uint8_t)sps->log2_min_cb_size;
	p->log2_ctb = (uint8_t)sps->log2_ctb_size;
	p->log2_min_tb = (uint8_t)sps->log2_min_tb_size;
	p->log2_max_tb = (uint8_t)sps->log2_max_trafo_size;
	p->max_trafo_depth_intra = (uint8_t)sps->max_transform_hierarchy_depth_intra;
	p->max_trafo_depth_inter = (uint8_t)sps->max_transform_hierarchy_depth_inter;
	p->amp = sps->amp_enabled_flag;
	p->strong_intra_smoothing = sps->sps_strong_intra_smoothing_enable_flag;
	p->pcm = (uint8_t)sps->pcm_enabled_flag;
	if (p->pcm) {
		p->pcm_bit_depth = sps->pcm.bit_depth;
		p->pcm_bit_depth_chroma = sps->pcm.bit_depth_chroma;
		p->log2_min_pcm_cb = (uint8_t)sps->pcm.log2_min_pcm_cb_size;
		p->log2_max_pcm_cb = (uint8_t)sps->pcm.log2_max_pcm_cb_size;
		p->pcm_loop_filter_disabled = sps->pcm.loop_filter_disable_flag;
	}
	p->scaling_list = sps->scaling_list_enable_flag;
	if (p->scaling_list) {
		const ScalingList *sl = pps->scaling_list_data_present_flag ? &pps->scaling_list : &sps->scaling_list;
		uint8_t sl4[6][16], sl8[6][64], sl16[6][64], dc16[6], sl32[2][64], dc32[2];

		for (i = 0; i < 6u; i++) {
			memcpy(sl4[i], sl->sl[0][i], 16);
			memcpy(sl8[i], sl->sl[1][i], 64);
			memcpy(sl16[i], sl->sl[2][i], 64);
			dc16[i] = sl->sl_dc[0][i];
		}
		memcpy(sl32[0], sl->sl[3][0], 64);
		memcpy(sl32[1], sl->sl[3][3], 64);
		dc32[0] = sl->sl_dc[1][0];
		dc32[1] = sl->sl_dc[1][3];
		rpivid_scaling_factors(ctx->scaling, sl4, sl8, sl16, dc16, sl32, dc32);
		p->scaling_factors = ctx->scaling;
	}

	p->cu_qp_delta = pps->cu_qp_delta_enabled_flag;
	p->diff_cu_qp_delta_depth = (uint8_t)pps->diff_cu_qp_delta_depth;
	p->transquant_bypass = pps->transquant_bypass_enable_flag;
	p->transform_skip = pps->transform_skip_enabled_flag;
	p->sign_data_hiding = pps->sign_data_hiding_flag;
	p->constrained_intra_pred = pps->constrained_intra_pred_flag;
	p->pps_cb_qp_offset = (int8_t)pps->cb_qp_offset;
	p->pps_cr_qp_offset = (int8_t)pps->cr_qp_offset;
	p->weighted_pred = pps->weighted_pred_flag;
	p->weighted_bipred = pps->weighted_bipred_flag;
	p->entropy_coding_sync = pps->entropy_coding_sync_enabled_flag;
	p->tiles = pps->tiles_enabled_flag;
	p->loop_filter_across_tiles = pps->loop_filter_across_tiles_enabled_flag;
	if (p->tiles) {
		p->num_tile_cols = pps->num_tile_columns;
		p->num_tile_rows = pps->num_tile_rows;
		for (i = 0; i <= p->num_tile_cols; i++) {
			p->col_bd[i] = pps->col_bd[i];
		}
		for (i = 0; i <= p->num_tile_rows; i++) {
			p->row_bd[i] = pps->row_bd[i];
		}
		p->ctb_addr_rs_to_ts = pps->ctb_addr_rs_to_ts;
		p->ctb_addr_ts_to_rs = pps->ctb_addr_ts_to_rs;
	}
	else {
		p->num_tile_cols = p->num_tile_rows = 1;
		p->col_bd[1] = (uint32_t)sps->ctb_width;
		p->row_bd[1] = (uint32_t)sps->ctb_height;
	}
	p->log2_parallel_merge_level = (uint8_t)pps->log2_parallel_merge_level;
	p->slice_temporal_mvp = s->sh.slice_temporal_mvp_enabled_flag;
	/* a one-slice picture keeps the exact form tools/hevc-decode proved (rpivid_cmd.h);
	 * a picture of several slices follows the Linux driver throughout */
	p->one_slice = (uint8_t)ctx->one_slice;
	p->compat_intra_no_msgs = (uint8_t)ctx->one_slice;
}


int ff_rpivid_hevc_picture_ok(AVCodecContext *avctx)
{
	const HEVCContext *s = avctx->priv_data;
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;
	RPIVIDFrame *f = s->ref->hwaccel_picture_private;
	size_t need = 64;
	char why[192];
	int64_t m;
	int i, vcl = 0, pics = 0;

	m = pps_tools(s->ps.pps, why, sizeof(why));
	if ((m < 0) || (tools_ok((uint32_t)m, ctx->enabled, why, sizeof(why)) < 0)) {
		av_log(avctx, AV_LOG_WARNING, "rpivid: picture POC %d: %s\n", s->poc, why);
		return -1;
	}
	for (i = 0; i < s->pkt.nb_nals; i++) {
		const H2645NAL *nal = &s->pkt.nals[i];

		if ((nal->nuh_layer_id == 0) && (nal->type <= HEVC_NAL_RSV_VCL31)) {
			vcl++;
			need += (size_t)nal->raw_size + 64u;
			/* first_slice_segment_in_pic_flag: the first bit after the NAL unit header */
			if ((nal->size > 2) && ((nal->data[2] & 0x80u) != 0u)) {
				pics++;
			}
			else if (dependent_segment(s, nal)) {
				m |= TOOL(T_DEPENDENT_SLICES);
			}
		}
	}
	/* a packet is one access unit (a picture) but count pictures all the same */
	ctx->one_slice = (vcl <= ((pics > 0) ? pics : 1));
	if (!ctx->one_slice) {
		m |= TOOL(T_SLICES);
		if ((ctx->enabled & TOOL(T_SLICES)) == 0u) {
			av_log(avctx, AV_LOG_WARNING, "rpivid: picture POC %d: %d slice segments (tool %s off)\n", s->poc, vcl, tools[T_SLICES].name);
			return -1;
		}
	}
	if (tools_ok((uint32_t)m, ctx->enabled, why, sizeof(why)) < 0) {
		av_log(avctx, AV_LOG_WARNING, "rpivid: picture POC %d: %s\n", s->poc, why);
		return -1;
	}
	tools_used(avctx, ctx, ctx->sps_tools | (uint32_t)m);
	if (ctx->bs.size < need) {
		rpivid_dma_free(&ctx->bs);
		if (rpivid_dma_alloc(&ctx->bs, need + need / 2u) < 0) {
			av_log(avctx, AV_LOG_WARNING, "rpivid: no memory for a %zu-byte bitstream buffer\n", need);
			return -1;
		}
	}
	if ((f == NULL) || (f->pool != NULL)) {
		av_log(avctx, AV_LOG_WARNING, "rpivid: picture POC %d has no fresh frame data\n", s->poc);
		return -1;
	}
	f->buf = pool_get(ctx->pool);
	if (f->buf == NULL) {
		av_log(avctx, AV_LOG_WARNING, "rpivid: no contiguous memory for another picture buffer (%d held)\n", ctx->pool->nbufs);
		return -1;
	}
	f->pool = pool_ref(ctx->pool);
	return 0;
}


static int rpivid_start_frame(AVCodecContext *avctx, const uint8_t *buf, uint32_t size)
{
	const HEVCContext *s = avctx->priv_data;
	const HEVCSPS *sps = s->ps.sps;
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;
	unsigned int t = (unsigned int)s->nal_unit_type;

	(void)buf;
	(void)size;
	fill_pic(ctx, s);
	rpivid_cmd_begin(&ctx->cmd, &ctx->pic);
	ctx->bs_used = 0;
	ctx->nslots = 0;
	ctx->nslices = 0;
	ctx->missing_refs = 0;
	ctx->slice_ns = 0;
	ctx->frame_err = NULL;
	ctx->failed = 0;
	/* keep this picture's motion vectors if any later picture may use it as the
	 * collocated one: a reference NAL type, or a sub-layer below the highest */
	ctx->write_mv = sps->sps_temporal_mvp_enabled_flag &&
		(((t & ~0xeu) != 0u) || ((sps->max_sub_layers - 1) >= (s->temporal_id + 1)));
	return 0;
}


/* The slice data offset in the raw NAL unit (emulation prevention bytes counted) of an
 * offset in its RBSP */
static uint32_t raw_offset(const uint8_t *b, uint32_t size, uint32_t rbsp_off)
{
	uint32_t raw = 0, rb = 0, zeros = 0;

	while ((rb < rbsp_off) && (raw < size)) {
		if ((zeros >= 2u) && (b[raw] == 3u)) {
			zeros = 0;
			raw++;
			continue;
		}
		zeros = (b[raw] == 0u) ? zeros + 1u : 0u;
		raw++;
		rb++;
	}
	return raw;
}


static int ref_slot(RPIVIDContext *ctx, const HEVCFrame *ref, const RPIVIDFrame *cur)
{
	const RPIVIDFrame *rf;
	int k;

	for (k = 0; k < ctx->nslots; k++) {
		if (ctx->slot[k] == ref) {
			return k;
		}
	}
	if (ctx->nslots == 16) {
		return -1;
	}
	k = ctx->nslots++;
	ctx->slot[k] = ref;
	rf = (ref != NULL) ? ref->hwaccel_picture_private : NULL;
	if ((rf != NULL) && (rf->buf != NULL) && rf->decoded) {
		ctx->slot_addr[k][0] = rf->buf->y.pa;
		ctx->slot_addr[k][1] = rf->buf->c.pa;
	}
	else {
		/* a picture the stream lost (hevcdec.c made it up) or one the block did not
		 * decode: point at the current picture, as the Linux driver does */
		ctx->slot_addr[k][0] = cur->buf->y.pa;
		ctx->slot_addr[k][1] = cur->buf->c.pa;
		ctx->missing_refs++;
	}
	return k;
}


static int decode_slice(AVCodecContext *avctx, const uint8_t *buf, uint32_t size)
{
	const HEVCContext *s = avctx->priv_data;
	const SliceHeader *sh = &s->sh;
	const HEVCPPS *pps = s->ps.pps;
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;
	const RPIVIDFrame *cur = s->ref->hwaccel_picture_private;
	rpivid_slice_t sl;
	uint32_t rbsp_off, dbo, len, off;
	unsigned int list, i;
	int k;

	if (ctx->frame_err != NULL) {
		return 0;
	}
	if ((ctx->nslices > 0) && (ctx->one_slice || ((ctx->enabled & TOOL(T_SLICES)) == 0u))) {
		ctx->frame_err = "more slice segments than picture_ok counted";
		return 0;
	}
	if ((cur == NULL) || (cur->buf == NULL)) {
		ctx->frame_err = "no picture buffer";
		return 0;
	}

	/* slice_segment_data() starts after byte_alignment(): the RBSP position after the
	 * header + the stop bit, rounded up; the block wants it in the raw NAL unit */
	rbsp_off = ((uint32_t)get_bits_count(&s->HEVClc->gb) + 1u + 7u) / 8u;
	dbo = raw_offset(buf, size, rbsp_off);
	len = size;
	while ((len > dbo) && (buf[len - 1u] == 0u)) {
		len--;
	}
	off = ((uint32_t)ctx->bs_used + 63u) & ~63u;
	if ((dbo >= len) || ((size_t)off + len > ctx->bs.size)) {
		ctx->frame_err = "slice data out of range";
		return 0;
	}
	memcpy((uint8_t *)ctx->bs.cpu + off, buf, len);
	ctx->bs_used = (size_t)off + len;

	memset(&sl, 0, sizeof(sl));
	sl.data_offset = off + dbo;
	sl.data_len = len - dbo;
	sl.slice_segment_addr = sh->slice_segment_addr;
	sl.dependent = sh->dependent_slice_segment_flag;
	sl.slice_type = (uint8_t)sh->slice_type;
	sl.slice_qp = sh->slice_qp;
	sl.slice_cb_qp_offset = (int8_t)sh->slice_cb_qp_offset;
	sl.slice_cr_qp_offset = (int8_t)sh->slice_cr_qp_offset;
	sl.sao_luma = sh->slice_sample_adaptive_offset_flag[0];
	sl.sao_chroma = sh->slice_sample_adaptive_offset_flag[1];
	sl.mvd_l1_zero = sh->mvd_l1_zero_flag;
	sl.cabac_init = sh->cabac_init_flag;
	sl.collocated_from_l0 = (sh->collocated_list == L0);
	sl.cur_poc = s->poc;
	sl.beta_offset_div2 = (int8_t)(sh->beta_offset / 2);
	sl.tc_offset_div2 = (int8_t)(sh->tc_offset / 2);
	sl.deblocking_disabled = sh->disable_deblocking_filter_flag;
	sl.loop_filter_across_slices = sh->slice_loop_filter_across_slices_enabled_flag;

	if (sh->slice_type != HEVC_SLICE_I) {
		const RefPicList *rpl = s->ref->refPicList;

		sl.max_num_merge_cand = sh->max_num_merge_cand;
		sl.nb_refs[0] = (uint8_t)sh->nb_refs[L0];
		sl.nb_refs[1] = (sh->slice_type == HEVC_SLICE_B) ? (uint8_t)sh->nb_refs[L1] : 0;
		for (list = 0; list < 2u; list++) {
			if ((sl.nb_refs[list] > 16u) || ((sl.nb_refs[list] != 0u) && ((rpl == NULL) || (rpl[list].nb_refs < sl.nb_refs[list])))) {
				ctx->frame_err = "reference list shorter than the slice header says";
				return 0;
			}
			for (i = 0; i < sl.nb_refs[list]; i++) {
				k = ref_slot(ctx, rpl[list].ref[i], cur);
				if (k < 0) {
					ctx->frame_err = "more than 16 reference pictures";
					return 0;
				}
				sl.ref_slot[list][i] = (uint8_t)k;
				sl.ref_poc[list][i] = rpl[list].list[i];
				sl.ref_long_term[list][i] = (uint8_t)(rpl[list].isLongTerm[i] != 0);
			}
		}
		if (((sh->slice_type == HEVC_SLICE_P) && pps->weighted_pred_flag) || ((sh->slice_type == HEVC_SLICE_B) && pps->weighted_bipred_flag)) {
			sl.weighted = 1;
			sl.luma_log2_weight_denom = sh->luma_log2_weight_denom;
			sl.chroma_log2_weight_denom = (uint8_t)sh->chroma_log2_weight_denom;
			for (i = 0; i < 16u; i++) {
				sl.luma_weight[0][i] = sh->luma_weight_l0[i];
				sl.luma_offset[0][i] = sh->luma_offset_l0[i];
				sl.luma_weight[1][i] = sh->luma_weight_l1[i];
				sl.luma_offset[1][i] = sh->luma_offset_l1[i];
				for (k = 0; k < 2; k++) {
					sl.chroma_weight[0][i][k] = sh->chroma_weight_l0[i][k];
					sl.chroma_offset[0][i][k] = sh->chroma_offset_l0[i][k];
					sl.chroma_weight[1][i][k] = sh->chroma_weight_l1[i][k];
					sl.chroma_offset[1][i][k] = sh->chroma_offset_l1[i][k];
				}
			}
		}
	}

	if (rpivid_cmd_slice(&ctx->cmd, &sl) < 0) {
		ctx->frame_err = "command buffer";
		return 0;
	}
	ctx->nslices++;
	return 0;
}


static int rpivid_decode_slice(AVCodecContext *avctx, const uint8_t *buf, uint32_t size)
{
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;
	uint64_t t0 = (uint64_t)av_gettime_relative();
	int ret = decode_slice(avctx, buf, size);

	ctx->slice_ns += ((uint64_t)av_gettime_relative() - t0) * 1000u;
	return ret;
}


static void stat_window(AVCodecContext *avctx, RPIVIDContext *ctx)
{
	if (ctx->wn < STAT_EVERY) {
		return;
	}
	av_log(avctx, AV_LOG_INFO, "rpivid-stat pictures=%" PRIu64 " hw=%.2fms (p1 %.2f p2 %.2f) sand=%.2fms slices=%.2fms\n", ctx->n,
		(ctx->w_p1 + ctx->w_p2) / 1e6 / ctx->wn, ctx->w_p1 / 1e6 / ctx->wn, ctx->w_p2 / 1e6 / ctx->wn, ctx->w_detile / 1e6 / ctx->wn,
		ctx->w_slices / 1e6 / ctx->wn);
	ctx->wn = ctx->w_slices = ctx->w_p1 = ctx->w_p2 = ctx->w_detile = 0;
}


static int rpivid_end_frame(AVCodecContext *avctx)
{
	HEVCContext *s = avctx->priv_data;
	const HEVCSPS *sps = s->ps.sps;
	RPIVIDContext *ctx = avctx->internal->hwaccel_priv_data;
	RPIVIDFrame *cur = s->ref->hwaccel_picture_private;
	const rpivid_geom_t *g = &ctx->pool->g;
	AVFrame *out = s->ref->frame;
	rpivid_hw_stat_t st;
	rpivid_job_t j;
	uint64_t t0, t1, t2;
	int n, i, rc = 0;

	t0 = (uint64_t)av_gettime_relative();
	if ((ctx->frame_err == NULL) && (ctx->nslices == 0)) {
		ctx->frame_err = "no slices";
	}
	if ((ctx->frame_err == NULL) && ((n = rpivid_cmd_end(&ctx->cmd)) < 0)) {
		ctx->frame_err = "command buffer";
	}
	if (ctx->frame_err != NULL) {
		goto fail;
	}
	rpivid_cmd_rebase(&ctx->cmd, ctx->bs.pa);

	memset(&j, 0, sizeof(j));
	j.cmd = ctx->cmd.e;
	j.cmd_len = (uint32_t)n;
	j.width = (uint32_t)sps->width;
	j.height = (uint32_t)sps->height;
	j.ctb_rows = (uint32_t)sps->ctb_height;
	j.out_y = cur->buf->y.pa;
	j.out_c = cur->buf->c.pa;
	j.luma_stride = g->luma_stride;
	j.chroma_stride = g->chroma_stride;
	for (i = 0; i < 16; i++) {
		j.ref[i][0] = (i < ctx->nslots) ? ctx->slot_addr[i][0] : j.out_y;
		j.ref[i][1] = (i < ctx->nslots) ? ctx->slot_addr[i][1] : j.out_c;
	}
	j.config2 = rpivid_config2(&ctx->pic, ctx->write_mv, ctx->pic.slice_temporal_mvp);
	j.currpoc = (uint32_t)s->poc;
	if (sps->sps_temporal_mvp_enabled_flag) {
		j.colmv_stride = g->colmv_stride;
		if (ctx->write_mv) {
			j.mv = cur->buf->mv.pa;
		}
		if (ctx->pic.slice_temporal_mvp) {
			const RPIVIDFrame *col = (s->collocated_ref != NULL) ? s->collocated_ref->hwaccel_picture_private : NULL;

			if ((col != NULL) && (col->buf != NULL) && col->decoded && col->has_mv) {
				j.col = col->buf->mv.pa;
			}
			else {
				j.col = ctx->dummy_mv.pa;
				ctx->missing_refs++;
			}
		}
	}

	t1 = (uint64_t)av_gettime_relative();
	rc = rpivid_hw_decode(ctx->hw, &j, &st);
	t2 = (uint64_t)av_gettime_relative();
	if (st.p1_runs > 1u) {
		ctx->p1_reruns += st.p1_runs - 1u;
	}
	if (rc < 0) {
		av_log(avctx, AV_LOG_ERROR, "rpivid: the block failed picture POC %d: %s (CFSTATUS %u CFNUM %u STATUS 0x%x, %u phase-1 runs)\n",
			s->poc, (rc == -ETIMEDOUT) ? "timeout" : ((rc == -EIO) ? "decode error" : "out of memory"), st.cfstatus, st.cfnum,
			st.status, st.p1_runs);
		ctx->frame_err = "hardware";
	}

	/* the planar frame, also from a failed decode: what the block wrote beats a stale buffer */
	if (sps->bit_depth == 8) {
		rpivid_sand8_to_planar(out->data[0], out->linesize[0], out->data[1], out->linesize[1], out->data[2], out->linesize[2],
			cur->buf->y.cpu, cur->buf->c.cpu, g->luma_stride, g->chroma_stride, j.width, j.height);
	}
	else {
		rpivid_sand10_to_planar16(out->data[0], out->linesize[0], out->data[1], out->linesize[1], out->data[2], out->linesize[2],
			cur->buf->y.cpu, cur->buf->c.cpu, g->luma_stride, g->chroma_stride, j.width, j.height);
	}
	if (ctx->frame_err != NULL) {
		goto fail;
	}
	cur->decoded = 1;
	cur->has_mv = ctx->write_mv;
	ctx->missing_total += (uint32_t)ctx->missing_refs;

	ctx->n++;
	ctx->wn++;
	ctx->sum_slices += (t1 - t0) * 1000u + ctx->slice_ns;
	ctx->w_slices += (t1 - t0) * 1000u + ctx->slice_ns;
	ctx->sum_p1 += st.p1_ns;
	ctx->w_p1 += st.p1_ns;
	ctx->sum_p2 += st.p2_ns;
	ctx->w_p2 += st.p2_ns;
	t0 = (uint64_t)av_gettime_relative();
	ctx->sum_detile += (t0 - t2) * 1000u;
	ctx->w_detile += (t0 - t2) * 1000u;
	stat_window(avctx, ctx);
	return 0;

fail:
	av_log(avctx, AV_LOG_WARNING, "rpivid: picture POC %d not decoded on the block (%s)\n", s->poc, ctx->frame_err);
	out->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
	/* without reordering the picture went to the output at its start: flag that copy too */
	if ((s->output_frame->buf[0] != NULL) && (out->buf[0] != NULL) && (s->output_frame->buf[0]->buffer == out->buf[0]->buffer)) {
		s->output_frame->decode_error_flags |= FF_DECODE_ERROR_INVALID_BITSTREAM;
	}
	ctx->failed = 1;
	return 0;
}


static const FFHWAccel ff_hevc_rpivid_hwaccel = {
	.p.name = "hevc_rpivid",
	.p.type = AVMEDIA_TYPE_VIDEO,
	.p.id = AV_CODEC_ID_HEVC,
	.p.pix_fmt = AV_PIX_FMT_YUV420P,
	.start_frame = rpivid_start_frame,
	.decode_slice = rpivid_decode_slice,
	.end_frame = rpivid_end_frame,
	.frame_priv_data_size = sizeof(RPIVIDFrame),
	.free_frame_priv = rpivid_frame_free,
	.priv_data_size = sizeof(RPIVIDContext),
	.uninit = rpivid_uninit,
};
