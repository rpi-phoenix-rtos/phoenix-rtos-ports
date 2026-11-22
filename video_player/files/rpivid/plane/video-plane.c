/*
 * Phoenix-RTOS
 *
 * video-plane: HEVC on the rpivid block, shown on a display overlay with no CPU pixel work
 * (docs/misc/2026-11-21-yuv-sand-plane-design.md, step 2; the display side is
 * rpivid/drm/rpivid_plane.c, shared with ffplay).
 *
 *   video-plane [-n frames] [-x] file.mp4|.mkv|.hevc
 *     -n  stop after that many pictures      -x  as fast as decoded (no pacing)
 *
 * Lines start with "VIDEOPLANE "; the last is
 *   VIDEOPLANE result=PASS|FAIL decoded=N shown=N wall_s=W cpu_s=C cpu_pct=P fps=F
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

#include "rpivid_plane.h"


static double cpu_seconds(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) {
		return -1.0;
	}
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}


int main(int argc, char *argv[])
{
	AVFormatContext *fc = NULL;
	AVCodecContext *cc = NULL;
	const AVCodec *codec;
	AVDictionary *opts = NULL;
	AVPacket *pkt = av_packet_alloc();
	uint32_t mw, mh;
	long max_frames = 0, decoded = 0, shown = 0;
	int st, ret, done = 0, nopace = 0, ok = 1, opt;
	int64_t t0 = 0, pts0 = AV_NOPTS_VALUE;
	double c0, wall, cpu;
	AVRational tb;

	while ((opt = getopt(argc, argv, "n:x")) != -1) {
		if (opt == 'n') {
			max_frames = strtol(optarg, NULL, 0);
		}
		else if (opt == 'x') {
			nopace = 1;
		}
		else {
			fprintf(stderr, "usage: video-plane [-n frames] [-x] file\n");
			return 2;
		}
	}
	if ((optind >= argc) || (pkt == NULL)) {
		fprintf(stderr, "usage: video-plane [-n frames] [-x] file\n");
		return 2;
	}
	ret = rpivid_plane_open();
	if (ret != 0) {
		printf("VIDEOPLANE result=FAIL no overlay that takes NV12 (rpi4-kms -o): %s\n", strerror(-ret));
		return 1;
	}
	rpivid_plane_screen(&mw, &mh);

	ret = avformat_open_input(&fc, argv[optind], NULL, NULL);
	if (ret >= 0) {
		ret = avformat_find_stream_info(fc, NULL);
	}
	if (ret < 0) {
		printf("VIDEOPLANE result=FAIL cannot read %s: %s\n", argv[optind], av_err2str(ret));
		return 1;
	}
	st = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	codec = avcodec_find_decoder_by_name("hevc_rpivid");
	if ((st < 0) || (fc->streams[st]->codecpar->codec_id != AV_CODEC_ID_HEVC) || (codec == NULL)) {
		printf("VIDEOPLANE result=FAIL no HEVC stream, or no hevc_rpivid\n");
		return 1;
	}
	cc = avcodec_alloc_context3(codec);
	if ((cc == NULL) || (avcodec_parameters_to_context(cc, fc->streams[st]->codecpar) < 0)) {
		printf("VIDEOPLANE result=FAIL decoder context\n");
		return 1;
	}
	cc->thread_count = 1;
	cc->thread_type = FF_THREAD_SLICE;   /* drm_prime output needs no frame threads */
	cc->pkt_timebase = fc->streams[st]->time_base;
	av_dict_set(&opts, "rpivid_out", "drm_prime", 0);
	ret = avcodec_open2(cc, codec, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		printf("VIDEOPLANE result=FAIL cannot open hevc_rpivid: %s\n", av_err2str(ret));
		return 1;
	}
	tb = fc->streams[st]->time_base;
	printf("VIDEOPLANE %s: %dx%d on a %ux%u screen\n", argv[optind], cc->width, cc->height, mw, mh);
	fflush(stdout);

	c0 = cpu_seconds();
	t0 = av_gettime_relative();
	while (!done) {
		AVFrame *f;

		ret = av_read_frame(fc, pkt);
		if (ret < 0) {
			(void)avcodec_send_packet(cc, NULL);   /* drain */
			done = 1;
		}
		else if (pkt->stream_index == st) {
			(void)avcodec_send_packet(cc, pkt);
			av_packet_unref(pkt);
		}
		else {
			av_packet_unref(pkt);
			continue;
		}
		for (;;) {
			f = av_frame_alloc();
			if ((f == NULL) || (avcodec_receive_frame(cc, f) < 0)) {
				av_frame_free(&f);
				break;
			}
			decoded++;
			if (!nopace && (f->pts != AV_NOPTS_VALUE)) {
				int64_t due;

				if (pts0 == AV_NOPTS_VALUE) {
					pts0 = f->pts;
					t0 = av_gettime_relative();
				}
				due = t0 + av_rescale_q(f->pts - pts0, tb, AV_TIME_BASE_Q);
				while (av_gettime_relative() < due) {
					av_usleep((unsigned int)(due - av_gettime_relative()));
				}
			}
			/* fit the picture to the screen, keeping its aspect */
			{
				uint32_t dw = mw, dh = (uint32_t)((uint64_t)mw * (uint32_t)f->height / (uint32_t)f->width);
				if (dh > mh) {
					dh = mh;
					dw = (uint32_t)((uint64_t)mh * (uint32_t)f->width / (uint32_t)f->height);
				}
				ret = rpivid_plane_show(f, (int)(mw - dw) / 2, (int)(mh - dh) / 2, (int)dw, (int)dh);
				if (ret == 0) {
					shown++;
				}
				else if (ok) {
					printf("VIDEOPLANE picture %ld (format %d) not shown: %s\n", decoded, f->format, strerror(-ret));
					ok = 0;
				}
			}
			av_frame_free(&f);   /* rpivid_plane_show() holds its own reference */
			if ((max_frames > 0) && (decoded >= max_frames)) {
				done = 1;
				break;
			}
		}
	}
	wall = (av_gettime_relative() - t0) / 1e6;
	cpu = cpu_seconds() - c0;

	rpivid_plane_close();
	avcodec_free_context(&cc);
	avformat_close_input(&fc);
	av_packet_free(&pkt);

	printf("VIDEOPLANE result=%s decoded=%ld shown=%ld wall_s=%.2f cpu_s=%.2f cpu_pct=%.1f fps=%.1f\n",
		(ok && (shown == decoded) && (decoded > 0)) ? "PASS" : "FAIL", decoded, shown, wall, cpu,
		(wall > 0.0) ? 100.0 * cpu / wall : 0.0, (wall > 0.0) ? (double)shown / wall : 0.0);
	return (ok && (shown == decoded)) ? 0 : 1;
}
