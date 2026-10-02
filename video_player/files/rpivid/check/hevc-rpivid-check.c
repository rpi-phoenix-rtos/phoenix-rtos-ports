/*
 * Phoenix-RTOS
 *
 * hevc-rpivid-check: decode an HEVC file with the rpivid hardware decoder and with the
 * CPU decoder, compare every frame and time both
 *
 *     hevc-rpivid-check [-n frames] [-t cpu_threads] [-T hw_threads] [-l level] [-hw | -cpu] [-md5] [-crc] <file>
 *
 *   -n     stop after this many frames (default: the whole file)
 *   -t     CPU decoder threads (default 4)
 *   -T     hardware decoder threads (default 1: one block; more only tests FFmpeg's frame threading)
 *   -l     the hevc_rpivid "rpivid" level: 1 the verified tool set, 2 all tools (default: the
 *          decoder's own, i.e. FFMPEG_RPIVID or 1)
 *   -hw    only the hardware pass; -cpu only the CPU pass (timing; nothing compared, so
 *          nothing hashed unless -md5)
 *   -md5   one "MD5 <pass> <frame> <pts> <md5>" line per frame (the md5 of the frame's
 *          planes as ffmpeg -f framemd5 hashes them, so it can be checked against a host)
 *   -crc   check every frame against the stream's own picture hash SEI (x265 --hash 1),
 *          independently of the other pass; counts in the pass lines. The decoder then
 *          computes an MD5 of every picture itself: that is in decode_ms/frame
 *
 * Every line of ours starts with "RPIVID-CHECK"; libav* messages (the decoder's
 * "rpivid:" lines among them) are printed to stdout too. Exit status: 0 when the passes
 * agree (or the one pass ran), 1 on a mismatch, 2 on an error.
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __phoenix__
#include <unistd.h>
#include <sys/threads.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

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
} pass_t;

static int hw_used, hw_fallback;
static int print_md5, crc_mode, crc_checked, crc_bad;


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
	}
	if (strstr(line, "continuing on the CPU decoder") != NULL) {
		hw_fallback = 1;
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


static int add_frame(pass_t *p, const AVFrame *f)
{
	int64_t t0 = av_gettime_relative();

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
	p->fmt = f->format;
	if (print_md5) {
		char hex[33];
		int i;

		for (i = 0; i < 16; i++) {
			snprintf(hex + 2 * i, 3, "%02x", p->md5[p->n][i]);
		}
		printf("MD5 %s %d %" PRId64 " %s\n", p->name, p->n, f->pts, hex);
	}
	p->n++;
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

out:
	avcodec_free_context(&cc);
	avformat_close_input(&fc);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	av_freep(&p->md5ctx);
	return ret;
}


int main(int argc, char **argv)
{
	pass_t hw = { 0 }, cpu = { 0 };
	const char *file = NULL;
	int max_frames = 0, threads = 4, hw_threads = 1, level = -1, do_hw = 1, do_cpu = 1, crc = 0, i, bad = 0, first_bad = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && (i + 1 < argc)) {
			max_frames = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-t") && (i + 1 < argc)) {
			threads = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-T") && (i + 1 < argc)) {
			hw_threads = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-l") && (i + 1 < argc)) {
			level = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-hw")) {
			do_cpu = 0;
		}
		else if (!strcmp(argv[i], "-cpu")) {
			do_hw = 0;
		}
		else if (!strcmp(argv[i], "-md5")) {
			print_md5 = 1;
		}
		else if (!strcmp(argv[i], "-crc")) {
			crc = 1;
		}
		else if ((argv[i][0] != '-') && (file == NULL)) {
			file = argv[i];
		}
		else {
			file = NULL;
			break;
		}
	}
	if ((file == NULL) || (!do_hw && !do_cpu)) {
		printf("usage: hevc-rpivid-check [-n frames] [-t cpu_threads] [-T hw_threads] [-l level] [-hw | -cpu] [-md5] [-crc] <file>\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	crc_mode = crc;
	/* the frames are hashed to compare the two passes, or to print the hashes */
	hw.hash = cpu.hash = (do_hw && do_cpu) || print_md5;
	av_log_set_callback(log_cb);
	printf("RPIVID-CHECK file=%s max_frames=%d cpu_threads=%d level=%d\n", file, max_frames, threads, level);

	/* the hardware pass first and alone */
	if (do_hw && (run_pass(&hw, file, "hevc_rpivid", hw_threads, level, max_frames, crc) < 0)) {
		return 2;
	}
	if (do_hw) {
		printf("RPIVID-CHECK hw_used=%d hw_fallback=%d\n", hw_used, hw_fallback);
	}
	crc_checked = crc_bad = 0;
	if (do_cpu && (run_pass(&cpu, file, "hevc", threads, 0, max_frames, crc) < 0)) {
		return 2;
	}
	if (!do_hw || !do_cpu) {
		av_free(hw.md5);
		av_free(hw.pts);
		av_free(cpu.md5);
		av_free(cpu.pts);
		return 0;
	}

	for (i = 0; (i < hw.n) && (i < cpu.n); i++) {
		if (memcmp(hw.md5[i], cpu.md5[i], 16) != 0) {
			if (bad < 20) {
				printf("RPIVID-CHECK mismatch frame=%d pts=%" PRId64 "\n", i, hw.pts[i]);
			}
			if (first_bad < 0) {
				first_bad = i;
			}
			bad++;
		}
	}
	if (hw.n != cpu.n) {
		printf("RPIVID-CHECK frame counts differ: hw %d cpu %d\n", hw.n, cpu.n);
	}
	printf("RPIVID-CHECK verdict=%s frames=%d bad=%d first_bad=%d hw_used=%d hw_fallback=%d speedup=%.2fx\n",
		((bad == 0) && (hw.n == cpu.n)) ? "BIT-EXACT" : "MISMATCH", hw.n, bad, first_bad, hw_used, hw_fallback,
		(hw.wall > 0.0) ? cpu.wall / hw.wall : 0.0);
	i = ((bad == 0) && (hw.n == cpu.n)) ? 0 : 1;
	av_free(hw.md5);
	av_free(hw.pts);
	av_free(cpu.md5);
	av_free(cpu.pts);
	return i;
}
