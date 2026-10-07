/*
 * Phoenix-RTOS
 *
 * hevc-rpivid-check: decode HEVC files with the rpivid hardware decoder, compare every
 * frame with a reference -- the CPU decoder, or a host's ffmpeg -f framemd5 -- and time it
 *
 *     hevc-rpivid-check [-n frames] [-t cpu_threads] [-T hw_threads] [-l level] [-hw | -cpu] [-md5] [-crc]
 *                       [-cpuref] [-zc] [-hold n] [-q] [-from name] <file | directory>...
 *
 *   -n     stop after this many frames (default: the whole file)
 *   -t     CPU decoder threads (default 4)
 *   -T     hardware decoder threads (default 1: one block; more only tests FFmpeg's frame threading)
 *   -l     the hevc_rpivid "rpivid" level: 1 the default tool set, 2 every tool (default: the
 *          decoder's own, i.e. FFMPEG_RPIVID or 1); FFMPEG_RPIVID_TOOLS=-name,+name changes
 *          single tools (rpivid_hevc.c)
 *   -hw    only the hardware pass; -cpu only the CPU pass (timing; nothing compared, so
 *          nothing hashed unless -md5)
 *   -md5   one "MD5 <pass> <frame> <pts> <md5>" line per frame (the md5 of the frame's
 *          planes as ffmpeg -f framemd5 hashes them, so it can be checked against a host)
 *   -crc   check every frame against the stream's own picture hash SEI (x265 --hash 1),
 *          independently of the other pass; counts in the pass lines. The decoder then
 *          computes an MD5 of every picture itself: that is in decode_ms/frame
 *   -cpuref  compare with the CPU decoder even where a <file>.md5 reference exists
 *   -zc    the hardware pass with zero-copy output (hevc_rpivid rpivid_out=drm_prime,
 *          libavcodec/rpivid_drm.h): AV_PIX_FMT_DRM_PRIME frames of the block's buffers --
 *          GPU buffers (render-server BOs with a dma-buf) where this build has them, else the
 *          decoder's own memory -- each read back to system memory on the CPU
 *          (rpivid_drm_frame_to_planar) to be hashed: the zero-copy path held to the same
 *          references. Frame threads are off in this mode (-T > 1 = slice threads).
 *   -hold  with -zc: keep the last n frames referenced (default 4), as a compositor holds
 *          pictures, and hash each again when it is let go: a buffer reused while still
 *          held shows as held_bad= (counted as mismatches)
 *   -q     only this tool's lines and the decoder's rpivid lines (default with a
 *          directory or several files)
 *   -from  skip the files listed before the one of this name (to go on after a stream that
 *          left the block unusable for the rest of the process: "cpu_why=the block stopped
 *          responding")
 *
 * The reference: <file>.md5 next to the file when there is one (ffmpeg -fps_mode
 * passthrough -f framemd5 output of the host: only the hardware pass runs), else the
 * CPU decoder. A directory stands for the files its MANIFEST names (the first word of
 * each line not starting with '#', in that order: tools/hevc-decode/rpivid-check/gen-set.sh
 * writes one), else for its HEVC files (.265 .hevc .h265 .bit .mp4 .mkv .mov) in name
 * order. Every file ends with one line
 *
 *     RPIVID-CHECK stream=<name> frames=<n> mismatches=<m> fallback=<0|1> fps=<x> result=<r> ...
 *
 * mismatches: frames whose md5 differs from the reference's, plus the difference of the
 * frame counts; fallback=1: the hardware decoder did not take the stream, or left it for
 * the CPU decoder part way; result: PASS (fallback=0, mismatches=0), CPU (fallback, no
 * mismatch), FAIL or ERROR; then the reference, the first bad frame, SEI hash failures,
 * the stream's tools beyond the block's proven set (tools=, rpivid_hevc.c) and those
 * outside the decoder's default set (nondefault=). Several files end with a summary line.
 *
 * Every line of ours starts with "RPIVID-CHECK"; libav* messages (the decoder's
 * "rpivid:" lines among them) are printed to stdout too. Exit status: 0 when every file
 * passed (or only one pass ran), 1 on a mismatch, 2 on an error.
 *
 * Timing: ms/frame is the pass's wall time per frame; of that, check_ms/frame is this
 * tool's own work on the decoded frames (hashing them to compare the passes),
 * demux_ms/frame reading the file (av_read_frame) and decode_ms/frame the rest: the
 * decoder (avcodec_send_packet / avcodec_receive_frame).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __phoenix__
#include <unistd.h>
#include <sys/threads.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavcodec/rpivid_drm.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#ifdef RPIVID_CHECK_DRM
#include "rpivid_bo_drm.h"
#endif

typedef struct {
	const char *name;
	uint8_t (*md5)[16];
	int64_t *pts;
	int n, cap;
	int errors;                    /* frames with decode_error_flags */
	int hash;                      /* hash the frames (to compare them, or for -md5) */
	struct AVMD5 *md5ctx;
	double wall, cpu;
	double check_s;                /* of the wall time: this tool's own per-frame work */
	double demux_s;                /* of the wall time: reading the file (av_read_frame) */
	int w, h;
	enum AVPixelFormat fmt;
	/* -zc */
	int zc_frames;                 /* DRM_PRIME frames received */
	AVFrame *rb;                   /* their system-memory copy */
	AVFrame *held[64];
	uint8_t held_md5[64][16];
	int nheld, held_checked, held_bad;
} pass_t;

static int zc_mode, zc_hold = 4, zc_gpu = -1;
static int hw_used, hw_fallback;
static int print_md5, crc_mode, crc_checked, crc_bad, quiet;
static char hw_tools[512] = "-", hw_nondefault[512] = "-", hw_cpu_why[200];


/* Add the comma-separated tool list after `key` in a decoder line to the list in out
 * ("-" when empty): a stream that changes SPS reports its tools again, and keeps the earlier */
static void grab_list(const char *line, const char *key, char *out, size_t len)
{
	const char *p = strstr(line, key);
	char item[64];
	size_t n, k, o;

	if (p == NULL) {
		return;
	}
	p += strlen(key);
	n = strcspn(p, " );\r\n");
	while (n > 0u) {
		k = strcspn(p, ",");
		k = (k < n) ? k : n;
		if ((k != 0u) && (k < sizeof(item)) && ((k != 1u) || (p[0] != '-'))) {
			const char *q = out;

			memcpy(item, p, k);
			item[k] = '\0';
			/* already listed? */
			while ((q = strstr(q, item)) != NULL) {
				if (((q == out) || (q[-1] == ',')) && ((q[k] == '\0') || (q[k] == ','))) {
					break;
				}
				q += k;
			}
			if (q == NULL) {
				if (strcmp(out, "-") == 0) {
					out[0] = '\0';
				}
				o = strlen(out);
				snprintf(out + o, len - o, "%s%s", (o != 0u) ? "," : "", item);
			}
		}
		k += (k < n) ? 1u : 0u;
		p += k;
		n -= k;
	}
}


static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
	static int print_prefix = 1;
	char line[1024];

	/* of the debug messages only the SEI hash verdict, so the others cost no formatting */
	if ((level > AV_LOG_INFO) && (!crc_mode || (level > AV_LOG_DEBUG) || (strstr(fmt, "Verifying checksum") == NULL))) {
		return;
	}
	av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
	/* the SEI hash check reports a match at debug level, a mismatch as an error */
	if (strstr(line, "Verifying checksum for frame") != NULL) {
		crc_checked++;
		if (level > AV_LOG_ERROR) {
			return;
		}
		crc_bad++;
	}
	else if (level > AV_LOG_INFO) {
		return;
	}
	if (strstr(line, "rpivid: hardware HEVC decode") != NULL) {
		hw_used = 1;
		grab_list(line, "tools: ", hw_tools, sizeof(hw_tools));
		grab_list(line, "not in the default set: ", hw_nondefault, sizeof(hw_nondefault));
	}
	if (strstr(line, "rpivid: tools in use: ") != NULL) {
		grab_list(line, "tools in use: ", hw_tools, sizeof(hw_tools));
		grab_list(line, "not in the default set: ", hw_nondefault, sizeof(hw_nondefault));
	}
	/* a picture the block could not take or failed: the rest of the stream went to the CPU */
	if ((hw_cpu_why[0] == '\0') && (strstr(line, "rpivid: picture POC ") != NULL)) {
		snprintf(hw_cpu_why, sizeof(hw_cpu_why), "%s", strstr(line, "rpivid: picture POC ") + 8);
		hw_cpu_why[strcspn(hw_cpu_why, "\r\n")] = '\0';
	}
	if (strstr(line, "rpivid: CPU decode: ") != NULL) {
		snprintf(hw_cpu_why, sizeof(hw_cpu_why), "%s", strstr(line, "rpivid: CPU decode: ") + 20);
		hw_cpu_why[strcspn(hw_cpu_why, "\r\n")] = '\0';
	}
	if (strstr(line, "continuing on the CPU decoder") != NULL) {
		hw_fallback = 1;
	}
	/* "rpivid:", not "rpivid": the decoder's own name prefixes every line it logs */
	if (quiet && (strstr(line, "rpivid:") == NULL)) {
		return;
	}
	fputs(line, stdout);
	fflush(stdout);
}


/* The CPU time of this process's threads (decoder threads included while they exist:
 * a pass reads it before closing its decoder). Phoenix-RTOS: the kernel's per-thread
 * accounting, as top shows it. */
static double cpu_seconds(void)
{
#if defined(__phoenix__)
	int n = threadcount(), i;
	threadinfo_t *ti;
	pid_t me = getpid();
	uint64_t us = 0;

	if (n <= 0) {
		return -1.0;
	}
	n += 16;
	ti = av_malloc_array(n, sizeof(*ti));
	if (ti == NULL) {
		return -1.0;
	}
	n = threadsinfo(n, PH_THREADINFO_BASIC, ti);
	for (i = 0; i < n; i++) {
		if (ti[i].pid == me) {
			us += (uint64_t)ti[i].cpuTime;
		}
	}
	av_free(ti);
	return (n > 0) ? (double)us / 1e6 : -1.0;
#elif defined(CLOCK_PROCESS_CPUTIME_ID)
	struct timespec t;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0) {
		return (double)t.tv_sec + t.tv_nsec / 1e9;
	}
#endif
	return -1.0;
}


/* The MD5 of the frame's planes packed without padding, as av_image_copy_to_buffer(..., 1)
 * lays them out (and ffmpeg -f framemd5 hashes them), fed to MD5 a row at a time from the
 * frame itself: no copy of the picture, and no picture-sized allocation per frame (one
 * that libphoenix malloc maps and unmaps, page fault by page fault, every frame) */
static int frame_md5(struct AVMD5 *md5, uint8_t out[16], const AVFrame *f)
{
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
	int linesize[4], nb_planes = 0, i, j, h, shift;

	if ((desc == NULL) || ((desc->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL)) != 0) ||
			(av_image_fill_linesizes(linesize, f->format, f->width) < 0)) {
		return -1;
	}
	for (i = 0; i < desc->nb_components; i++) {
		nb_planes = FFMAX(desc->comp[i].plane, nb_planes);
	}
	nb_planes++;

	av_md5_init(md5);
	for (i = 0; i < nb_planes; i++) {
		shift = ((i == 1) || (i == 2)) ? desc->log2_chroma_h : 0;
		h = (f->height + (1 << shift) - 1) >> shift;
		for (j = 0; j < h; j++) {
			av_md5_update(md5, f->data[i] + (ptrdiff_t)j * f->linesize[i], linesize[i]);
		}
	}
	av_md5_final(md5, out);
	return 0;
}


/* -zc: a DRM_PRIME frame read back into p->rb (reused), at the frame's own size */
static int read_back(pass_t *p, const AVFrame *frame)
{
	if ((p->rb == NULL) && ((p->rb = av_frame_alloc()) == NULL)) {
		return -1;
	}
	if ((p->rb->buf[0] != NULL) && ((p->rb->width < frame->width) || (p->rb->height < frame->height))) {
		av_frame_unref(p->rb);
	}
	if (rpivid_drm_frame_to_planar(p->rb, frame) < 0) {
		return -1;
	}
	/* a buffer of a larger picture: hash exactly this frame's size */
	p->rb->width = frame->width;
	p->rb->height = frame->height;
	return 0;
}


/* -zc: let the oldest held frame go, hashing it again first */
static void release_held(pass_t *p)
{
	uint8_t md5[16];
	int i;

	if (p->nheld == 0) {
		return;
	}
	p->held_checked++;
	if ((read_back(p, p->held[0]) < 0) || (frame_md5(p->md5ctx, md5, p->rb) < 0) || (memcmp(md5, p->held_md5[0], 16) != 0)) {
		if (p->held_bad++ < 5) {
			printf("RPIVID-CHECK zc held frame pts=%" PRId64 " changed while it was held\n", p->held[0]->pts);
		}
	}
	av_frame_free(&p->held[0]);
	for (i = 1; i < p->nheld; i++) {
		p->held[i - 1] = p->held[i];
		memcpy(p->held_md5[i - 1], p->held_md5[i], 16);
	}
	p->nheld--;
}


static int add_frame(pass_t *p, const AVFrame *frame)
{
	int64_t t0 = av_gettime_relative();
	const AVFrame *f = frame;

	if (frame->format == AV_PIX_FMT_DRM_PRIME) {
		/* -zc: the block's buffer, read back for the hash */
		if (read_back(p, frame) < 0) {
			printf("RPIVID-CHECK error: a DRM_PRIME frame that is not hevc_rpivid's\n");
			return -1;
		}
		f = p->rb;
		p->zc_frames++;
	}

	if (p->n == p->cap) {
		int ncap = p->cap ? p->cap * 2 : 1024;
		void *a = av_realloc_array(p->md5, ncap, sizeof(*p->md5)), *b;

		if (a == NULL) {
			return -1;
		}
		p->md5 = a;
		b = av_realloc_array(p->pts, ncap, sizeof(*p->pts));
		if (b == NULL) {
			return -1;
		}
		p->pts = b;
		p->cap = ncap;
	}
	if (p->hash) {
		if (frame_md5(p->md5ctx, p->md5[p->n], f) < 0) {
			return -1;
		}
	}
	else {
		memset(p->md5[p->n], 0, sizeof(p->md5[p->n]));
	}
	p->pts[p->n] = f->pts;
	if (f->decode_error_flags) {
		p->errors++;
	}
	p->w = f->width;
	p->h = f->height;
	p->fmt = frame->format;
	if (print_md5) {
		char hex[33];
		int i;

		for (i = 0; i < 16; i++) {
			snprintf(hex + 2 * i, 3, "%02x", p->md5[p->n][i]);
		}
		printf("MD5 %s %d %" PRId64 " %s\n", p->name, p->n, f->pts, hex);
	}
	p->n++;
	if ((frame->format == AV_PIX_FMT_DRM_PRIME) && (zc_hold > 0) && p->hash) {
		if (p->nheld == zc_hold) {
			release_held(p);
		}
		p->held[p->nheld] = av_frame_clone(frame);
		if (p->held[p->nheld] == NULL) {
			return -1;
		}
		memcpy(p->held_md5[p->nheld], p->md5[p->n - 1], 16);
		p->nheld++;
	}
	p->check_s += (av_gettime_relative() - t0) / 1e6;
	return 0;
}


static int run_pass(pass_t *p, const char *file, const char *decoder, int threads, int level, int max_frames, int crc)
{
	AVFormatContext *fc = NULL;
	AVCodecContext *cc = NULL;
	const AVCodec *codec;
	AVDictionary *opts = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int st, ret, done = 0;
	int64_t t0;
	double c0;
	char lv[16], cpu[48];

	p->name = decoder;
	if (p->hash) {
		p->md5ctx = av_md5_alloc();
	}
	if ((pkt == NULL) || (frame == NULL) || (p->hash && (p->md5ctx == NULL))) {
		return -1;
	}
	ret = avformat_open_input(&fc, file, NULL, NULL);
	if (ret >= 0) {
		/* the probe decodes a few pictures: on the CPU, so the passes are what they say */
		AVDictionary **po = av_calloc(fc->nb_streams, sizeof(*po));
		unsigned int k;

		for (k = 0; (po != NULL) && (k < fc->nb_streams); k++) {
			av_dict_set(&po[k], "rpivid", "0", 0);
		}
		ret = avformat_find_stream_info(fc, po);
		for (k = 0; (po != NULL) && (k < fc->nb_streams); k++) {
			av_dict_free(&po[k]);
		}
		av_free(po);
	}
	if (ret < 0) {
		printf("RPIVID-CHECK error: cannot read %s: %s\n", file, av_err2str(ret));
		goto out;
	}
	st = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if ((st < 0) || (fc->streams[st]->codecpar->codec_id != AV_CODEC_ID_HEVC)) {
		printf("RPIVID-CHECK error: %s has no HEVC video stream\n", file);
		ret = -1;
		goto out;
	}
	codec = avcodec_find_decoder_by_name(decoder);
	cc = (codec != NULL) ? avcodec_alloc_context3(codec) : NULL;
	if ((cc == NULL) || (avcodec_parameters_to_context(cc, fc->streams[st]->codecpar) < 0)) {
		printf("RPIVID-CHECK error: no decoder %s\n", decoder);
		ret = -1;
		goto out;
	}
	cc->thread_count = threads;
	if (zc_mode && (level != 0)) {
		/* drm_prime output needs no frame threads (rpivid_drm.h) */
		cc->thread_type = FF_THREAD_SLICE;
		av_dict_set(&opts, "rpivid_out", "drm_prime", 0);
	}
	if (crc) {
		cc->err_recognition |= AV_EF_CRCCHECK;
	}
	cc->pkt_timebase = fc->streams[st]->time_base;
	if (level >= 0) {
		snprintf(lv, sizeof(lv), "%d", level);
		av_dict_set(&opts, "rpivid", lv, 0);
	}
	ret = avcodec_open2(cc, codec, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		printf("RPIVID-CHECK error: cannot open %s: %s\n", decoder, av_err2str(ret));
		goto out;
	}

	t0 = av_gettime_relative();
	c0 = cpu_seconds();
	while (!done) {
		int64_t r0 = av_gettime_relative();

		ret = av_read_frame(fc, pkt);
		p->demux_s += (av_gettime_relative() - r0) / 1e6;
		if (ret < 0) {
			ret = avcodec_send_packet(cc, NULL); /* drain */
			done = 1;
		}
		else if (pkt->stream_index == st) {
			ret = avcodec_send_packet(cc, pkt);
			av_packet_unref(pkt);
		}
		else {
			av_packet_unref(pkt);
			continue;
		}
		if ((ret < 0) && (ret != AVERROR(EAGAIN)) && (ret != AVERROR_EOF)) {
			printf("RPIVID-CHECK warning: %s: send: %s\n", decoder, av_err2str(ret));
		}
		for (;;) {
			ret = avcodec_receive_frame(cc, frame);
			if (ret < 0) {
				break;
			}
			if (add_frame(p, frame) < 0) {
				printf("RPIVID-CHECK error: out of memory, or a pixel format it cannot hash\n");
				done = 1;
			}
			av_frame_unref(frame);
			if ((max_frames > 0) && (p->n >= max_frames)) {
				done = 1;
				break;
			}
		}
	}
	while (p->nheld > 0) {
		release_held(p);
	}
	p->wall = (av_gettime_relative() - t0) / 1e6;
	p->cpu = (c0 >= 0.0) ? cpu_seconds() - c0 : -1.0;
	ret = 0;
	if (p->cpu >= 0.0) {
		snprintf(cpu, sizeof(cpu), "%.2fs (%.0f%% of one core)", p->cpu, (p->wall > 0.0) ? 100.0 * p->cpu / p->wall : 0.0);
	}
	else {
		snprintf(cpu, sizeof(cpu), "n/a");
	}
	printf("RPIVID-CHECK pass=%s threads=%d frames=%d size=%dx%d fmt=%s wall=%.2fs ms/frame=%.2f fps=%.1f cpu=%s error_frames=%d"
		" sei_hash_checked=%d sei_hash_bad=%d decode_ms/frame=%.2f demux_ms/frame=%.2f check_ms/frame=%.2f\n", decoder, threads, p->n,
		p->w, p->h, av_get_pix_fmt_name(p->fmt), p->wall, p->n ? p->wall * 1000.0 / p->n : 0.0, (p->wall > 0.0) ? p->n / p->wall : 0.0,
		cpu, p->errors, crc_checked, crc_bad, p->n ? (p->wall - p->check_s - p->demux_s) * 1000.0 / p->n : 0.0,
		p->n ? p->demux_s * 1000.0 / p->n : 0.0, p->n ? p->check_s * 1000.0 / p->n : 0.0);
	if (zc_mode && (level != 0)) {
		printf("RPIVID-CHECK zc frames=%d drm_prime=%d buffers=%s held_checked=%d held_bad=%d\n", p->n, p->zc_frames,
			(zc_gpu > 0) ? "gpu" : "own", p->held_checked, p->held_bad);
	}

out:
	avcodec_free_context(&cc);
	avformat_close_input(&fc);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	while (p->nheld > 0) {
		av_frame_free(&p->held[--p->nheld]);
	}
	av_frame_free(&p->rb);
	av_freep(&p->md5ctx);
	return ret;
}


typedef struct {
	int max_frames, threads, hw_threads, level, do_hw, do_cpu, crc, cpuref;
} opts_t;

typedef struct {
	uint8_t (*md5)[16];
	int n;
} ref_t;


static int hexval(int c)
{
	return isdigit(c) ? (c - '0') : ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10) : ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1;
}


/* <file>.md5: ffmpeg -f framemd5 output, the md5 the last field of each non-comment line.
 * 1 = read, 0 = no such file, -1 = unusable */
static int ref_load(ref_t *r, const char *file)
{
	char path[1024], line[512];
	FILE *f;
	int cap = 0;

	memset(r, 0, sizeof(*r));
	snprintf(path, sizeof(path), "%s.md5", file);
	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *h;
		size_t len;
		int i;

		if (line[0] == '#') {
			continue;
		}
		len = strcspn(line, "\r\n");
		line[len] = '\0';
		h = strrchr(line, ',');
		h = (h != NULL) ? h + 1 : line;
		while (*h == ' ') {
			h++;
		}
		if (strlen(h) != 32u) {
			continue;
		}
		if (r->n == cap) {
			void *a;

			cap = cap ? cap * 2 : 1024;
			a = av_realloc_array(r->md5, cap, sizeof(*r->md5));
			if (a == NULL) {
				fclose(f);
				return -1;
			}
			r->md5 = a;
		}
		for (i = 0; i < 16; i++) {
			int hi = hexval(h[2 * i]), lo = hexval(h[2 * i + 1]);

			if ((hi < 0) || (lo < 0)) {
				break;
			}
			r->md5[r->n][i] = (uint8_t)((hi << 4) | lo);
		}
		if (i == 16) {
			r->n++;
		}
	}
	fclose(f);
	return (r->n > 0) ? 1 : -1;
}


static void pass_free(pass_t *p)
{
	av_freep(&p->md5);
	av_freep(&p->pts);
	memset(p, 0, sizeof(*p));
}


/* Check one file: 0 passed, 1 mismatch (or fell back with -l and nothing to compare), 2 error,
 * 3 the hardware decoder did not (fully) take it */
static int check_file(const char *file, const opts_t *o)
{
	pass_t hw = { 0 }, cpu = { 0 };
	ref_t ref = { 0 };
	const char *name = strrchr(file, '/'), *result, *refname = "none";
	int i, bad = 0, first_bad = -1, nref = 0, mismatches, fallback, ret = 0, have_ref;

	name = (name != NULL) ? name + 1 : file;
	hw_used = hw_fallback = 0;
	crc_checked = crc_bad = 0;
	snprintf(hw_tools, sizeof(hw_tools), "-");
	snprintf(hw_nondefault, sizeof(hw_nondefault), "-");
	hw_cpu_why[0] = '\0';

	have_ref = (o->cpuref || !o->do_hw) ? 0 : ref_load(&ref, file);
	if (have_ref < 0) {
		printf("RPIVID-CHECK warning: %s.md5 unusable: comparing with the CPU decoder\n", file);
		have_ref = 0;
	}
	/* the frames are hashed to compare them, or to print the hashes */
	hw.hash = cpu.hash = (o->do_hw && (o->do_cpu || have_ref)) || print_md5;
	printf("RPIVID-CHECK file=%s max_frames=%d cpu_threads=%d level=%d reference=%s\n", file, o->max_frames, o->threads, o->level,
		have_ref ? "md5" : ((o->do_hw && o->do_cpu) ? "cpu" : "none"));

	/* the hardware pass first and alone */
	if (o->do_hw && (run_pass(&hw, file, "hevc_rpivid", o->hw_threads, o->level, o->max_frames, o->crc) < 0)) {
		ret = 2;
		goto out;
	}
	if (o->do_hw) {
		printf("RPIVID-CHECK hw_used=%d hw_fallback=%d\n", hw_used, hw_fallback);
	}
	if (o->do_hw && have_ref) {
		nref = ref.n;
		if ((o->max_frames > 0) && (nref > o->max_frames)) {
			nref = o->max_frames;
		}
		refname = "md5";
	}
	else if (o->do_cpu) {
		int sei_bad = crc_bad, sei_checked = crc_checked;

		crc_checked = crc_bad = 0;
		if (run_pass(&cpu, file, "hevc", o->threads, 0, o->max_frames, o->crc) < 0) {
			ret = 2;
			goto out;
		}
		crc_checked = sei_checked;
		crc_bad = sei_bad;
		nref = cpu.n;
		refname = "cpu";
	}
	if (!o->do_hw || (!o->do_cpu && !have_ref)) {
		goto out;
	}

	for (i = 0; (i < hw.n) && (i < nref); i++) {
		if (memcmp(hw.md5[i], have_ref ? ref.md5[i] : cpu.md5[i], 16) != 0) {
			if (bad < 20) {
				printf("RPIVID-CHECK mismatch frame=%d pts=%" PRId64 "\n", i, hw.pts[i]);
			}
			if (first_bad < 0) {
				first_bad = i;
			}
			bad++;
		}
	}
	if (hw.n != nref) {
		printf("RPIVID-CHECK frame counts differ: hw %d %s %d\n", hw.n, refname, nref);
		if (first_bad < 0) {
			first_bad = (hw.n < nref) ? hw.n : nref;
		}
	}
	if (!have_ref) {
		/* the two-pass verdict line of earlier versions */
		printf("RPIVID-CHECK verdict=%s frames=%d bad=%d first_bad=%d hw_used=%d hw_fallback=%d speedup=%.2fx\n",
			((bad == 0) && (hw.n == cpu.n)) ? "BIT-EXACT" : "MISMATCH", hw.n, bad, first_bad, hw_used, hw_fallback,
			(hw.wall > 0.0) ? cpu.wall / hw.wall : 0.0);
	}
	mismatches = bad + abs(hw.n - nref) + hw.held_bad;
	if (zc_mode && (hw.zc_frames == 0) && !hw_fallback && hw_used) {
		printf("RPIVID-CHECK zc warning: no DRM_PRIME frame (drm_prime output not taken: see the rpivid: lines)\n");
	}
	fallback = !hw_used || hw_fallback;
	result = (mismatches != 0) ? "FAIL" : (fallback ? "CPU" : "PASS");
	ret = (mismatches != 0) ? 1 : (fallback ? 3 : 0);
	printf("RPIVID-CHECK stream=%s frames=%d mismatches=%d fallback=%d fps=%.1f result=%s ref=%s ref_frames=%d first_bad=%d "
		"sei_checked=%d sei_bad=%d tools=%s nondefault=%s%s%s\n", name, hw.n, mismatches, fallback,
		(hw.wall > 0.0) ? hw.n / hw.wall : 0.0, result, refname, nref, first_bad, crc_checked, crc_bad, hw_tools, hw_nondefault,
		(hw_cpu_why[0] != '\0') ? " cpu_why=" : "", hw_cpu_why);

out:
	if (ret == 2) {
		printf("RPIVID-CHECK stream=%s frames=%d mismatches=0 fallback=%d fps=0.0 result=ERROR\n", name, hw.n, !hw_used || hw_fallback);
	}
	pass_free(&hw);
	pass_free(&cpu);
	av_freep(&ref.md5);
	return ret;
}


static int has_hevc_ext(const char *n)
{
	static const char *const ext[] = { ".265", ".hevc", ".h265", ".bit", ".mp4", ".mkv", ".mov" };
	const char *d = strrchr(n, '.');
	unsigned int i;

	for (i = 0; (d != NULL) && (i < sizeof(ext) / sizeof(ext[0])); i++) {
		if (strcmp(d, ext[i]) == 0) {
			return 1;
		}
	}
	return 0;
}


static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}


static int add_name(char ***list, int *n, int *cap, char *name)
{
	if (name == NULL) {
		return -1;
	}
	if (*n == *cap) {
		char **nl = av_realloc_array(*list, *cap ? *cap * 2 : 64, sizeof(**list));

		if (nl == NULL) {
			av_free(name);
			return -1;
		}
		*list = nl;
		*cap = *cap ? *cap * 2 : 64;
	}
	(*list)[(*n)++] = name;
	return 0;
}


/* Append a file, or a directory's MANIFEST files (else its HEVC files in name order), to the list */
static int add_input(char ***list, int *n, int *cap, const char *arg)
{
	const char *sep = (arg[0] != '\0') && (arg[strlen(arg) - 1] == '/') ? "" : "/";
	struct stat st;
	DIR *d;
	struct dirent *e;
	FILE *mf;
	char line[512];
	int first = *n;

	if ((stat(arg, &st) == 0) && S_ISDIR(st.st_mode)) {
		snprintf(line, sizeof(line), "%s%sMANIFEST", arg, sep);
		mf = fopen(line, "r");
		if (mf != NULL) {
			while (fgets(line, sizeof(line), mf) != NULL) {
				line[strcspn(line, " \t\r\n")] = '\0';
				if ((line[0] == '\0') || (line[0] == '#')) {
					continue;
				}
				if (add_name(list, n, cap, av_asprintf("%s%s%s", arg, sep, line)) < 0) {
					fclose(mf);
					return -1;
				}
			}
			fclose(mf);
			return 0;
		}
		d = opendir(arg);
		if (d == NULL) {
			return -1;
		}
		while ((e = readdir(d)) != NULL) {
			if ((e->d_name[0] == '.') || !has_hevc_ext(e->d_name)) {
				continue;
			}
			if (add_name(list, n, cap, av_asprintf("%s%s%s", arg, sep, e->d_name)) < 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		qsort(*list + first, (size_t)(*n - first), sizeof(**list), cmp_str);
		return 0;
	}
	return add_name(list, n, cap, av_strdup(arg));
}


int main(int argc, char **argv)
{
	opts_t o = { .threads = 4, .hw_threads = 1, .level = -1, .do_hw = 1, .do_cpu = 1 };
	char **files = NULL;
	const char *from = NULL;
	int nfiles = 0, cap = 0, i, rc, npass = 0, nfail = 0, ncpu = 0, nerr = 0, usage = 0, q = -1, nchecked = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && (i + 1 < argc)) {
			o.max_frames = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-t") && (i + 1 < argc)) {
			o.threads = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-T") && (i + 1 < argc)) {
			o.hw_threads = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-l") && (i + 1 < argc)) {
			o.level = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-hw")) {
			o.do_cpu = 0;
		}
		else if (!strcmp(argv[i], "-cpu")) {
			o.do_hw = 0;
		}
		else if (!strcmp(argv[i], "-md5")) {
			print_md5 = 1;
		}
		else if (!strcmp(argv[i], "-crc")) {
			o.crc = 1;
		}
		else if (!strcmp(argv[i], "-cpuref")) {
			o.cpuref = 1;
		}
		else if (!strcmp(argv[i], "-zc")) {
			zc_mode = 1;
		}
		else if (!strcmp(argv[i], "-hold") && (i + 1 < argc)) {
			zc_hold = atoi(argv[++i]);
			zc_hold = (zc_hold < 0) ? 0 : ((zc_hold > 64) ? 64 : zc_hold);
		}
		else if (!strcmp(argv[i], "-q")) {
			q = 1;
		}
		else if (!strcmp(argv[i], "-from") && (i + 1 < argc)) {
			from = argv[++i];
		}
		else if ((argv[i][0] != '-') && (add_input(&files, &nfiles, &cap, argv[i]) == 0)) {
			continue;
		}
		else {
			usage = 1;
			break;
		}
	}
	if (usage || (nfiles == 0) || (!o.do_hw && !o.do_cpu)) {
		printf("usage: hevc-rpivid-check [-n frames] [-t cpu_threads] [-T hw_threads] [-l level] [-hw | -cpu] [-md5] [-crc] [-cpuref] [-zc]"
			" [-hold n] [-q] [-from name] <file | directory>...\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	crc_mode = o.crc;
	quiet = (q >= 0) ? q : (nfiles > 1);
	av_log_set_callback(log_cb);
	if (zc_mode) {
#ifdef RPIVID_CHECK_DRM
		/* GPU buffers: render-server BOs with a dma-buf, as a player gets them */
		int e = rpivid_bo_drm_install();

		zc_gpu = (e == 0);
		if (e != 0) {
			printf("RPIVID-CHECK zc warning: no render node (%s): the decoder's own buffers\n", av_err2str(e));
		}
#else
		zc_gpu = 0;
#endif
	}

	rc = 0;
	for (i = 0; i < nfiles; i++) {
		const char *base = strrchr(files[i], '/');
		int r;

		base = (base != NULL) ? base + 1 : files[i];
		if ((from != NULL) && (strcmp(base, from) != 0)) {
			av_free(files[i]);
			continue;
		}
		from = NULL;
		nchecked++;
		r = check_file(files[i], &o);

		npass += (r == 0);
		nfail += (r == 1);
		nerr += (r == 2);
		ncpu += (r == 3);
		if ((r == 1) || (r == 2)) {
			rc = (rc == 2) ? 2 : r;
		}
		av_free(files[i]);
	}
	av_free(files);
	if (nfiles > 1) {
		printf("RPIVID-CHECK summary streams=%d pass=%d fail=%d cpu=%d error=%d\n", nchecked, npass, nfail, ncpu, nerr);
	}
	return rc;
}
