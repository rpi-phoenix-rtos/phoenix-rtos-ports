/*
 * Phoenix-RTOS
 *
 * gtk-video: a small GTK 3 video player (M10, docs/gpu-new-lane/M10-video-player.md).
 *
 * FFmpeg demuxes and decodes (the player build of the ffmpeg 6.1 port, CPU decoders); the
 * picture is scaled by libswscale to the video area's size (aspect kept) and painted with
 * cairo; the sound goes through libswresample to /dev/audio0 (44100 Hz, S16, stereo; its
 * blocking write paces the audio thread). A pausable wall clock is the master: video and
 * audio are both synchronised to it (late frames dropped, early ones waited for), and it
 * is held at a seek target until the first picture after the seek is ready.
 *
 * Controls: the toolbar (open, play/pause, stop, the seek bar, the time, fullscreen) and the
 * keyboard -- space pause, s stop, left/right -/+10 s, down/up -/+60 s, f or F11 fullscreen,
 * Escape leave fullscreen, q quit; a double click on the picture toggles fullscreen.
 *
 * Usage: gtk-video [--fullscreen] [--autoexit] [file]
 * Environment: GTK_VIDEO_STAT_MS (default 2000; 0 = no stat lines), GTK_VIDEO_AUTOKEYS
 * ("<s>:<action>,..." with pause stop fs left right up down quit: unattended control tests),
 * GTK_VIDEO_AUDIO (0 = no sound), GTK_VIDEO_AUDIO_DEV (default /dev/audio0),
 * GTK_VIDEO_AUDIO_FILL_MS (audio queued ahead in the device, default 120),
 * GTK_VIDEO_SWS_THREADS (libswscale threads, default 2), GTK_VIDEO_THREADS (decoder
 * threads, default 0 = FFmpeg's auto).
 *
 * Every line it prints starts with "GTK-VIDEO " (grading).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define AUDIO_RATE     44100
#define QUEUE_MAX_SIZE (16 * 1024 * 1024)
#define QUEUE_MAX_PKTS 256
#define LATE_DROP_S    0.10   /* a picture later than this is dropped */
#define AUDIO_FILL_S   0.12   /* audio kept queued in the device ring (underrun headroom);
                                * GTK_VIDEO_AUDIO_FILL_MS overrides (the ring holds ~0.19 s) */
#define AUDIO_CHUNK_S  0.01   /* silence is written in chunks of this */
#define AUDIO_LATE_S   0.05   /* a sound frame heard later than this is dropped */
#define AUDIO_EARLY_S  0.02   /* ... earlier than this waits (silence in front of it) */


/* --- packet queue ---------------------------------------------------------------------- */

typedef struct pkt_node {
	AVPacket *pkt;
	struct pkt_node *next;
} pkt_node_t;

typedef struct {
	pkt_node_t *first, *last;
	int n;
	int64_t bytes;
	int serial; /* bumped by a flush (seek): a consumer seeing a new one flushes its decoder */
	int abort;
	GMutex m;
	GCond c;
} pkt_queue_t;


static void pq_init(pkt_queue_t *q)
{
	memset(q, 0, sizeof(*q));
	g_mutex_init(&q->m);
	g_cond_init(&q->c);
}


static void pq_clear_locked(pkt_queue_t *q)
{
	pkt_node_t *n, *next;

	for (n = q->first; n != NULL; n = next) {
		next = n->next;
		av_packet_free(&n->pkt);
		g_free(n);
	}
	q->first = q->last = NULL;
	q->n = 0;
	q->bytes = 0;
}


static void pq_put(pkt_queue_t *q, AVPacket *pkt)
{
	pkt_node_t *n = g_new0(pkt_node_t, 1);

	n->pkt = av_packet_alloc();
	av_packet_move_ref(n->pkt, pkt);
	g_mutex_lock(&q->m);
	if (q->last != NULL) {
		q->last->next = n;
	}
	else {
		q->first = n;
	}
	q->last = n;
	q->n++;
	q->bytes += n->pkt->size;
	g_cond_signal(&q->c);
	g_mutex_unlock(&q->m);
}


/* 1 = a packet (moved into pkt, *serial = the queue's serial), 0 = none within timeout_us,
 * -1 = aborted */
static int pq_get(pkt_queue_t *q, AVPacket *pkt, int *serial, gint64 timeout_us)
{
	gint64 end = g_get_monotonic_time() + timeout_us;
	pkt_node_t *n;
	int ret;

	g_mutex_lock(&q->m);
	while ((q->first == NULL) && !q->abort) {
		if (!g_cond_wait_until(&q->c, &q->m, end)) {
			break;
		}
	}
	if (q->abort) {
		ret = -1;
	}
	else if ((n = q->first) != NULL) {
		q->first = n->next;
		if (q->first == NULL) {
			q->last = NULL;
		}
		q->n--;
		q->bytes -= n->pkt->size;
		av_packet_move_ref(pkt, n->pkt);
		av_packet_free(&n->pkt);
		g_free(n);
		ret = 1;
	}
	else {
		ret = 0;
	}
	*serial = q->serial;
	g_mutex_unlock(&q->m);
	return ret;
}


static void pq_flush(pkt_queue_t *q)
{
	g_mutex_lock(&q->m);
	pq_clear_locked(q);
	q->serial++;
	g_cond_broadcast(&q->c);
	g_mutex_unlock(&q->m);
}


static int pq_serial(pkt_queue_t *q)
{
	int s;

	g_mutex_lock(&q->m);
	s = q->serial;
	g_mutex_unlock(&q->m);
	return s;
}


/* --- the player ------------------------------------------------------------------------ */

typedef struct {
	uint8_t *buf;
	int w, h, stride;
	int size;
	int64_t id;
} picture_t;

typedef struct {
	char *path;
	AVFormatContext *ic;
	int vidx, aidx;
	AVCodecContext *vctx, *actx;
	double duration;

	GThread *demux_th, *video_th, *audio_th;
	pkt_queue_t vq, aq;
	volatile int quit;

	/* seek request (main -> demux) */
	GMutex seek_m;
	int seek_req;
	double seek_target;
	volatile int eof, vdone, adone;

	/* the master clock */
	GMutex clk_m;
	double clk_pts;
	gint64 clk_us;
	int paused;
	int hold; /* held at clk_pts until the next picture is ready (start, seek) */

	/* the picture: the video thread fills back, swaps it with front under pic_m */
	GMutex pic_m;
	picture_t front, back;
	int64_t next_id, drawn_id;
	volatile gint present_pending;
	volatile gint area_w, area_h;

	/* audio device */
	int afd;
	SwrContext *swr;

	/* counters */
	volatile gint shown, dropped, decoded;
	double last_pts;
} player_t;

typedef struct {
	GtkWidget *win, *area, *bar, *play_btn, *scale, *time_lbl, *fs_btn;
	player_t *p;
	int fullscreen;
	int autoexit;
	int finished_reported;
	gint64 user_seek_us; /* the seek bar is not refreshed shortly after a user change */
	gint64 t0_us;
	gint64 last_stat_us;
	int last_stat_shown;
} app_t;

static app_t app;


static double clock_now(player_t *p)
{
	double t;

	g_mutex_lock(&p->clk_m);
	t = p->clk_pts;
	if (!p->paused && !p->hold) {
		t += (g_get_monotonic_time() - p->clk_us) / 1000000.0;
	}
	g_mutex_unlock(&p->clk_m);
	return t;
}


static void clock_set(player_t *p, double pts, int hold)
{
	g_mutex_lock(&p->clk_m);
	p->clk_pts = pts;
	p->clk_us = g_get_monotonic_time();
	p->hold = hold;
	g_mutex_unlock(&p->clk_m);
}


static int clock_running(player_t *p)
{
	int r;

	g_mutex_lock(&p->clk_m);
	r = !p->paused && !p->hold;
	g_mutex_unlock(&p->clk_m);
	return r;
}


static void player_set_paused(player_t *p, int paused)
{
	g_mutex_lock(&p->clk_m);
	if (paused && !p->paused) {
		if (!p->hold) {
			p->clk_pts += (g_get_monotonic_time() - p->clk_us) / 1000000.0;
		}
		p->paused = 1;
	}
	else if (!paused && p->paused) {
		p->clk_us = g_get_monotonic_time();
		p->paused = 0;
	}
	g_mutex_unlock(&p->clk_m);
}


static void player_seek(player_t *p, double target)
{
	if (target < 0) {
		target = 0;
	}
	if ((p->duration > 0) && (target > p->duration)) {
		target = p->duration;
	}
	g_mutex_lock(&p->seek_m);
	p->seek_target = target;
	p->seek_req = 1;
	g_mutex_unlock(&p->seek_m);
}


static int present_cb(gpointer data)
{
	(void)data;
	g_atomic_int_set(&app.p->present_pending, 0);
	gtk_widget_queue_draw(app.area);
	return G_SOURCE_REMOVE;
}


static gpointer demux_thread(gpointer arg)
{
	player_t *p = arg;
	AVPacket *pkt = av_packet_alloc();
	double target;
	int64_t ts;
	int req, ret;

	while (!p->quit) {
		g_mutex_lock(&p->seek_m);
		req = p->seek_req;
		target = p->seek_target;
		p->seek_req = 0;
		g_mutex_unlock(&p->seek_m);
		if (req) {
			ts = (int64_t)(target * AV_TIME_BASE);
			if (p->ic->start_time != AV_NOPTS_VALUE) {
				ts += p->ic->start_time;
			}
			ret = avformat_seek_file(p->ic, -1, INT64_MIN, ts, ts, 0);
			printf("GTK-VIDEO seek target=%.2f rc=%d\n", target, ret);
			pq_flush(&p->vq);
			pq_flush(&p->aq);
			/* no picture stream: the audio thread releases the hold at its first frame */
			clock_set(p, target, 1);
			p->eof = p->vdone = p->adone = 0;
		}
		if (p->eof || (p->vq.bytes + p->aq.bytes > QUEUE_MAX_SIZE) ||
				((p->vidx < 0 || p->vq.n > QUEUE_MAX_PKTS) && (p->aidx < 0 || p->aq.n > QUEUE_MAX_PKTS))) {
			g_usleep(10000);
			continue;
		}
		ret = av_read_frame(p->ic, pkt);
		if (ret < 0) {
			if ((ret != AVERROR_EOF) && !avio_feof(p->ic->pb)) {
				printf("GTK-VIDEO read error %d (%s)\n", ret, av_err2str(ret));
			}
			p->eof = 1;
			continue;
		}
		if (pkt->stream_index == p->vidx) {
			pq_put(&p->vq, pkt);
		}
		else if (pkt->stream_index == p->aidx) {
			pq_put(&p->aq, pkt);
		}
		else {
			av_packet_unref(pkt);
		}
	}
	av_packet_free(&pkt);
	return NULL;
}


/* the picture area for a WxH video with this aspect, aspect-fit in the widget */
static void fit_size(player_t *p, int vw, int vh, AVRational sar, int *tw, int *th)
{
	int aw = g_atomic_int_get(&p->area_w), ah = g_atomic_int_get(&p->area_h);
	double dar = (double)vw / vh;

	if ((sar.num > 0) && (sar.den > 0)) {
		dar *= (double)sar.num / sar.den;
	}
	if ((aw < 16) || (ah < 16)) {
		/* not allocated yet */
		aw = vw;
		ah = vh;
	}
	*tw = aw;
	*th = (int)(aw / dar + 0.5);
	if (*th > ah) {
		*th = ah;
		*tw = (int)(ah * dar + 0.5);
	}
	*tw &= ~1;
	*th &= ~1;
	if (*tw < 2) {
		*tw = 2;
	}
	if (*th < 2) {
		*th = 2;
	}
}


static int show_picture(player_t *p, AVFrame *f, struct SwsContext **sws, int *sws_key)
{
	int tw, th, key, stride, threads;
	picture_t tmp;
	AVFrame *dst;
	const char *e;

	fit_size(p, f->width, f->height, f->sample_aspect_ratio, &tw, &th);
	key = tw * 131071 + th * 257 + f->format * 7 + f->width * 3 + f->height;
	if ((*sws == NULL) || (*sws_key != key)) {
		sws_freeContext(*sws);
		*sws = sws_alloc_context();
		e = getenv("GTK_VIDEO_SWS_THREADS");
		threads = (e != NULL) ? atoi(e) : 2;
		av_opt_set_int(*sws, "srcw", f->width, 0);
		av_opt_set_int(*sws, "srch", f->height, 0);
		av_opt_set_int(*sws, "src_format", f->format, 0);
		av_opt_set_int(*sws, "dstw", tw, 0);
		av_opt_set_int(*sws, "dsth", th, 0);
		av_opt_set_int(*sws, "dst_format", AV_PIX_FMT_BGRA, 0);
		av_opt_set_int(*sws, "sws_flags", SWS_BILINEAR, 0);
		av_opt_set_int(*sws, "threads", threads > 0 ? threads : 1, 0);
		if (sws_init_context(*sws, NULL, NULL) < 0) {
			printf("GTK-VIDEO FAIL swscale %dx%d fmt %d -> %dx%d\n", f->width, f->height, f->format, tw, th);
			sws_freeContext(*sws);
			*sws = NULL;
			return -1;
		}
		*sws_key = key;
		printf("GTK-VIDEO scale %dx%d %s -> %dx%d (threads %d)\n", f->width, f->height,
			av_get_pix_fmt_name(f->format), tw, th, threads);
	}
	stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, tw);
	if (p->back.size < stride * th) {
		av_free(p->back.buf);
		p->back.size = stride * th;
		p->back.buf = av_malloc(p->back.size);
		if (p->back.buf == NULL) {
			p->back.size = 0;
			return -1;
		}
	}
	dst = av_frame_alloc();
	dst->format = AV_PIX_FMT_BGRA;
	dst->width = tw;
	dst->height = th;
	dst->data[0] = p->back.buf;
	dst->linesize[0] = stride;
	if (sws_scale_frame(*sws, dst, f) < 0) {
		av_frame_free(&dst);
		return -1;
	}
	av_frame_free(&dst);
	p->back.w = tw;
	p->back.h = th;
	p->back.stride = stride;

	g_mutex_lock(&p->pic_m);
	p->back.id = ++p->next_id;
	tmp = p->front;
	p->front = p->back;
	p->back = tmp;
	g_mutex_unlock(&p->pic_m);
	if (g_atomic_int_compare_and_exchange(&p->present_pending, 0, 1)) {
		g_idle_add(present_cb, NULL);
	}
	return 0;
}


static gpointer video_thread(gpointer arg)
{
	player_t *p = arg;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *f = av_frame_alloc();
	struct SwsContext *sws = NULL;
	int serial = -1, qserial, sws_key = 0, got, drained = 0, first = 1;
	double pts, d = 0, tb = av_q2d(p->ic->streams[p->vidx]->time_base);
	gint64 last_show_us = 0;

	while (!p->quit) {
		got = pq_get(&p->vq, pkt, &qserial, 20000);
		if (got < 0) {
			break;
		}
		if (qserial != serial) {
			avcodec_flush_buffers(p->vctx);
			serial = qserial;
			drained = 0;
			first = 1;
		}
		if (got == 0) {
			if (!p->eof || drained) {
				continue;
			}
			avcodec_send_packet(p->vctx, NULL);
			drained = 1;
		}
		else {
			avcodec_send_packet(p->vctx, pkt);
			av_packet_unref(pkt);
		}
		while (!p->quit && (avcodec_receive_frame(p->vctx, f) == 0)) {
			g_atomic_int_inc(&p->decoded);
			pts = (f->best_effort_timestamp != AV_NOPTS_VALUE) ? f->best_effort_timestamp * tb : p->last_pts;
			if (p->ic->start_time != AV_NOPTS_VALUE) {
				pts -= p->ic->start_time / (double)AV_TIME_BASE;
			}
			if (first) {
				/* the first picture at the start or after a seek: frames before the
				 * target (decoded from the preceding keyframe) are skipped, then the
				 * clock starts at the picture shown */
				g_mutex_lock(&p->seek_m);
				d = p->seek_target;
				g_mutex_unlock(&p->seek_m);
				if (pts < d - 0.02) {
					av_frame_unref(f);
					continue;
				}
				if (show_picture(p, f, &sws, &sws_key) == 0) {
					clock_set(p, pts, 0);
					p->last_pts = pts;
					last_show_us = g_get_monotonic_time();
					first = 0;
				}
				av_frame_unref(f);
				continue;
			}
			/* wait for its time (the clock stands still while paused) */
			while (!p->quit && (pq_serial(&p->vq) == serial)) {
				d = pts - clock_now(p);
				if (!clock_running(p) || (d > 0.002)) {
					g_usleep(d > 0.02 || !clock_running(p) ? 10000 : (gulong)(d * 1000000));
					continue;
				}
				break;
			}
			if (p->quit || (pq_serial(&p->vq) != serial)) {
				av_frame_unref(f);
				break;
			}
			/* late: drop it -- unless nothing was shown for half a second (a decoder slower
			 * than the stream still shows a picture now and then) */
			if ((d < -LATE_DROP_S) && (g_get_monotonic_time() - last_show_us < 500000)) {
				g_atomic_int_inc(&p->dropped);
			}
			else if (show_picture(p, f, &sws, &sws_key) == 0) {
				last_show_us = g_get_monotonic_time();
			}
			p->last_pts = pts;
			av_frame_unref(f);
		}
		if (drained && (pq_serial(&p->vq) == serial)) {
			p->vdone = 1;
		}
	}
	sws_freeContext(sws);
	av_frame_free(&f);
	av_packet_free(&pkt);
	return NULL;
}


/*
 * The audio feeder. /dev/audio0 (rpi4-audio) plays a free-running ~0.19 s DMA ring: write()
 * blocks while the ring is full, and whatever the ring holds is played again if the writer
 * falls behind or stops -- also after close(). So the ring is never left without fresh
 * samples: silence is written while paused, held (start, seek) or starved, and 0.25 s of it
 * before the device is closed. The fill (audio queued but not heard yet) is modelled as
 * seconds written minus seconds elapsed since the first write (the ring drains at the
 * playback rate), kept near AUDIO_FILL_S; a frame is due when it will be HEARD at its pts:
 * at clock + fill.
 */
static gint64 afeed_t0;
static double afeed_written;
static double afeed_target = AUDIO_FILL_S;


static double audio_fill(void)
{
	double f;

	if (afeed_t0 == 0) {
		return 0;
	}
	f = afeed_written - (g_get_monotonic_time() - afeed_t0) / 1000000.0;
	if (f < 0) {
		/* the ring ran dry (or the model drifted): start the model again */
		afeed_t0 = 0;
		afeed_written = 0;
		f = 0;
	}
	return f;
}


static void audio_write(player_t *p, const uint8_t *buf, int len)
{
	ssize_t n;

	if (afeed_t0 == 0) {
		afeed_t0 = g_get_monotonic_time();
	}
	afeed_written += len / (4.0 * AUDIO_RATE);
	while ((len > 0) && (p->afd >= 0)) {
		n = write(p->afd, buf, len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			printf("GTK-VIDEO audio write failed (%s): sound off\n", strerror(errno));
			close(p->afd);
			p->afd = -1;
			return;
		}
		buf += n;
		len -= n;
	}
}


static void audio_silence(player_t *p, double secs)
{
	static const uint8_t zero[4 * (AUDIO_RATE / 100)];
	int len = (int)(secs * AUDIO_RATE) * 4;

	while ((len > 0) && (p->afd >= 0)) {
		audio_write(p, zero, len < (int)sizeof(zero) ? len : (int)sizeof(zero));
		len -= sizeof(zero);
	}
}


static gpointer audio_thread(gpointer arg)
{
	player_t *p = arg;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *f = av_frame_alloc();
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	uint8_t *out = NULL;
	int out_size = 0, out_len = 0, serial = -1, qserial, got, drained = 0, max, n, ret, have = 0, have_serial = 0;
	double pts = 0, dur = 0, fill, heard, tb = av_q2d(p->ic->streams[p->aidx]->time_base);
	const char *e = getenv("GTK_VIDEO_AUDIO_FILL_MS");

	if ((e != NULL) && (atoi(e) >= 10)) {
		afeed_target = atoi(e) / 1000.0;
	}
	while (!p->quit) {
		fill = audio_fill();
		if ((p->afd >= 0) && (fill > afeed_target)) {
			g_usleep((gulong)(fill - afeed_target > 0.01 ? 10000 : (fill - afeed_target) * 1000000 + 500));
			continue;
		}
		if (have && (pq_serial(&p->aq) != have_serial)) {
			have = 0;   /* decoded before a seek */
		}
		if (!have) {
			ret = avcodec_receive_frame(p->actx, f);
			if (ret == AVERROR(EAGAIN)) {
				got = pq_get(&p->aq, pkt, &qserial, p->afd >= 0 ? 5000 : 20000);
				if (got < 0) {
					break;
				}
				if (qserial != serial) {
					avcodec_flush_buffers(p->actx);
					serial = qserial;
					drained = 0;
				}
				if (got > 0) {
					avcodec_send_packet(p->actx, pkt);
					av_packet_unref(pkt);
				}
				else if (p->eof && !drained) {
					avcodec_send_packet(p->actx, NULL);
					drained = 1;
				}
			}
			else if (ret == AVERROR_EOF) {
				if (pq_serial(&p->aq) == serial) {
					p->adone = 1;
				}
				got = pq_get(&p->aq, pkt, &qserial, p->afd >= 0 ? 5000 : 20000);   /* idle until a seek */
				if (got < 0) {
					break;
				}
				if (qserial != serial) {
					avcodec_flush_buffers(p->actx);
					serial = qserial;
					drained = 0;
					p->adone = 0;
				}
				if (got > 0) {
					avcodec_send_packet(p->actx, pkt);
					av_packet_unref(pkt);
				}
			}
			else if ((ret == 0) && (f->pts != AV_NOPTS_VALUE) && (p->afd >= 0)) {
				pts = f->pts * tb;
				if (p->ic->start_time != AV_NOPTS_VALUE) {
					pts -= p->ic->start_time / (double)AV_TIME_BASE;
				}
				dur = (double)f->nb_samples / f->sample_rate;
				if (p->swr == NULL) {
					if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
						av_channel_layout_default(&f->ch_layout, f->ch_layout.nb_channels);
					}
					if ((swr_alloc_set_opts2(&p->swr, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE, &f->ch_layout, f->format,
							f->sample_rate, 0, NULL) < 0) || (swr_init(p->swr) < 0)) {
						printf("GTK-VIDEO FAIL audio resampler: sound off\n");
						swr_free(&p->swr);
						close(p->afd);
						p->afd = -1;
					}
					else {
						printf("GTK-VIDEO audio %s %d Hz %d ch -> S16 %d Hz stereo, fill target %.0f ms\n",
							av_get_sample_fmt_name(f->format), f->sample_rate, f->ch_layout.nb_channels, AUDIO_RATE,
							afeed_target * 1000);
					}
				}
				if (p->swr != NULL) {
					max = swr_get_out_samples(p->swr, f->nb_samples);
					if (out_size < max * 4) {
						av_free(out);
						out_size = max * 4;
						out = av_malloc(out_size);
					}
					n = swr_convert(p->swr, &out, max, (const uint8_t **)f->extended_data, f->nb_samples);
					out_len = (n > 0) ? n * 4 : 0;
					have = (out_len > 0);
					have_serial = serial;
				}
				av_frame_unref(f);
			}
			else if (ret == 0) {
				av_frame_unref(f);   /* no pts, or no sound: consumed */
			}
		}
		if (p->afd < 0) {
			have = 0;
			continue;
		}
		if (have && (p->vidx < 0)) {
			/* no picture stream: the first sound releases the clock */
			g_mutex_lock(&p->clk_m);
			if (p->hold) {
				p->clk_pts = pts;
				p->clk_us = g_get_monotonic_time();
				p->hold = 0;
			}
			g_mutex_unlock(&p->clk_m);
		}
		if (!have || !clock_running(p)) {
			audio_silence(p, AUDIO_CHUNK_S);
			continue;
		}
		heard = clock_now(p) + audio_fill();
		if (pts + dur < heard - AUDIO_LATE_S) {
			have = 0;   /* late: dropped */
		}
		else if (pts > heard + AUDIO_EARLY_S) {
			audio_silence(p, pts - heard < AUDIO_CHUNK_S ? pts - heard : AUDIO_CHUNK_S);
		}
		else {
			audio_write(p, out, out_len);
			have = 0;
		}
	}
	av_free(out);
	av_frame_free(&f);
	av_packet_free(&pkt);
	return NULL;
}


static AVCodecContext *open_decoder(AVStream *st)
{
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *ctx;
	const char *e;

	if (codec == NULL) {
		printf("GTK-VIDEO no decoder for %s\n", avcodec_get_name(st->codecpar->codec_id));
		return NULL;
	}
	ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, st->codecpar);
	ctx->pkt_timebase = st->time_base;
	e = getenv("GTK_VIDEO_THREADS");
	ctx->thread_count = (e != NULL) ? atoi(e) : 0;
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		printf("GTK-VIDEO FAIL open decoder %s\n", codec->name);
		avcodec_free_context(&ctx);
		return NULL;
	}
	printf("GTK-VIDEO decoder %s (%s) threads=%d\n", codec->name, avcodec_get_name(st->codecpar->codec_id), ctx->thread_count);
	return ctx;
}


static void player_close(player_t *p)
{
	if (p == NULL) {
		return;
	}
	p->quit = 1;
	g_mutex_lock(&p->vq.m);
	p->vq.abort = 1;
	g_cond_broadcast(&p->vq.c);
	g_mutex_unlock(&p->vq.m);
	g_mutex_lock(&p->aq.m);
	p->aq.abort = 1;
	g_cond_broadcast(&p->aq.c);
	g_mutex_unlock(&p->aq.m);
	if (p->demux_th != NULL) {
		g_thread_join(p->demux_th);
	}
	if (p->video_th != NULL) {
		g_thread_join(p->video_th);
	}
	if (p->audio_th != NULL) {
		g_thread_join(p->audio_th);
	}
	pq_clear_locked(&p->vq);
	pq_clear_locked(&p->aq);
	avcodec_free_context(&p->vctx);
	avcodec_free_context(&p->actx);
	avformat_close_input(&p->ic);
	swr_free(&p->swr);
	if (p->afd >= 0) {
		/* the ring replays what it holds after close: leave it full of silence */
		audio_silence(p, 0.25);
		close(p->afd);
	}
	afeed_t0 = 0;
	afeed_written = 0;
	av_free(p->front.buf);
	av_free(p->back.buf);
	g_free(p->path);
	g_free(p);
}


static player_t *player_open(const char *path)
{
	player_t *p = g_new0(player_t, 1);
	const char *e;
	int ret;

	p->path = g_strdup(path);
	p->vidx = p->aidx = -1;
	p->afd = -1;
	pq_init(&p->vq);
	pq_init(&p->aq);
	g_mutex_init(&p->seek_m);
	g_mutex_init(&p->clk_m);
	g_mutex_init(&p->pic_m);
	p->hold = 1;

	ret = avformat_open_input(&p->ic, path, NULL, NULL);
	if (ret < 0) {
		printf("GTK-VIDEO FAIL open %s: %s\n", path, av_err2str(ret));
		g_free(p->path);
		g_free(p);
		return NULL;
	}
	avformat_find_stream_info(p->ic, NULL);
	av_dump_format(p->ic, 0, path, 0);
	p->duration = (p->ic->duration != AV_NOPTS_VALUE) ? p->ic->duration / (double)AV_TIME_BASE : 0;
	p->vidx = av_find_best_stream(p->ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	p->aidx = av_find_best_stream(p->ic, AVMEDIA_TYPE_AUDIO, -1, p->vidx, NULL, 0);
	if ((p->vidx >= 0) && ((p->vctx = open_decoder(p->ic->streams[p->vidx])) == NULL)) {
		p->vidx = -1;
	}
	if ((p->aidx >= 0) && ((p->actx = open_decoder(p->ic->streams[p->aidx])) == NULL)) {
		p->aidx = -1;
	}
	if ((p->vidx < 0) && (p->aidx < 0)) {
		printf("GTK-VIDEO FAIL %s: no playable stream\n", path);
		avformat_close_input(&p->ic);
		g_free(p->path);
		g_free(p);
		return NULL;
	}
	e = getenv("GTK_VIDEO_AUDIO");
	if ((p->aidx >= 0) && ((e == NULL) || (atoi(e) != 0))) {
		e = getenv("GTK_VIDEO_AUDIO_DEV");
		p->afd = open(e != NULL ? e : "/dev/audio0", O_WRONLY);
		if (p->afd < 0) {
			printf("GTK-VIDEO audio device %s: %s (sound off)\n", e != NULL ? e : "/dev/audio0", strerror(errno));
		}
	}
	printf("GTK-VIDEO open file=%s duration=%.2f video=%s %dx%d audio=%s sound=%s\n", path, p->duration,
		p->vidx >= 0 ? avcodec_get_name(p->vctx->codec_id) : "none", p->vidx >= 0 ? p->vctx->width : 0,
		p->vidx >= 0 ? p->vctx->height : 0, p->aidx >= 0 ? avcodec_get_name(p->actx->codec_id) : "none",
		p->afd >= 0 ? "on" : "off");

	p->demux_th = g_thread_new("demux", demux_thread, p);
	if (p->vidx >= 0) {
		p->video_th = g_thread_new("video", video_thread, p);
	}
	else {
		p->vdone = 1;
	}
	if (p->aidx >= 0) {
		p->audio_th = g_thread_new("audio", audio_thread, p);
	}
	else {
		p->adone = 1;
	}
	return p;
}


/* --- UI -------------------------------------------------------------------------------- */

static void fmt_time(char *buf, size_t n, double t)
{
	int s = (t > 0) ? (int)t : 0;

	snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}


static void ui_sync_play_button(void)
{
	int paused = (app.p == NULL) || app.p->paused;

	gtk_button_set_image(GTK_BUTTON(app.play_btn), gtk_image_new_from_icon_name(
		paused ? "media-playback-start" : "media-playback-pause", GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(app.play_btn, paused ? "Play (space)" : "Pause (space)");
}


static void ui_toggle_pause(void)
{
	if (app.p == NULL) {
		return;
	}
	if (app.p->vdone && app.p->adone) {
		/* at the end: play again from the start */
		player_seek(app.p, 0);
		player_set_paused(app.p, 0);
		app.finished_reported = 0;
	}
	else {
		player_set_paused(app.p, !app.p->paused);
	}
	printf("GTK-VIDEO %s clock=%.2f\n", app.p->paused ? "pause" : "play", clock_now(app.p));
	ui_sync_play_button();
}


static void ui_stop(void)
{
	if (app.p == NULL) {
		return;
	}
	player_set_paused(app.p, 1);
	player_seek(app.p, 0);
	printf("GTK-VIDEO stop\n");
	ui_sync_play_button();
}


static void ui_seek_rel(double d)
{
	if (app.p != NULL) {
		player_seek(app.p, clock_now(app.p) + d);
	}
}


static void ui_toggle_fullscreen(void)
{
	if (app.fullscreen) {
		gtk_window_unfullscreen(GTK_WINDOW(app.win));
	}
	else {
		gtk_window_fullscreen(GTK_WINDOW(app.win));
	}
}


static void ui_load(const char *path)
{
	player_close(app.p);
	app.p = player_open(path);
	app.finished_reported = 0;
	if (app.p != NULL) {
		g_atomic_int_set(&app.p->area_w, gtk_widget_get_allocated_width(app.area));
		g_atomic_int_set(&app.p->area_h, gtk_widget_get_allocated_height(app.area));
		gchar *base = g_path_get_basename(path);
		gtk_window_set_title(GTK_WINDOW(app.win), base);
		g_free(base);
		gtk_range_set_range(GTK_RANGE(app.scale), 0, app.p->duration > 0 ? app.p->duration : 1);
	}
	ui_sync_play_button();
}


static void on_open(GtkWidget *w, gpointer data)
{
	GtkWidget *dlg;
	char *path;

	(void)w;
	(void)data;
	dlg = gtk_file_chooser_dialog_new("Open video", GTK_WINDOW(app.win), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
		if (path != NULL) {
			ui_load(path);
			g_free(path);
		}
	}
	gtk_widget_destroy(dlg);
}


static void on_play(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	ui_toggle_pause();
}


static void on_stop(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	ui_stop();
}


static void on_fs(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	ui_toggle_fullscreen();
}


static gboolean on_change_value(GtkRange *r, GtkScrollType scroll, gdouble value, gpointer data)
{
	(void)r;
	(void)scroll;
	(void)data;
	if (app.p != NULL) {
		player_seek(app.p, value);
		app.user_seek_us = g_get_monotonic_time();
	}
	return FALSE;
}


static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
	player_t *p = app.p;
	cairo_surface_t *s;

	(void)data;
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	if (p == NULL) {
		return TRUE;
	}
	g_mutex_lock(&p->pic_m);
	if (p->front.buf != NULL) {
		s = cairo_image_surface_create_for_data(p->front.buf, CAIRO_FORMAT_RGB24, p->front.w, p->front.h, p->front.stride);
		cairo_set_source_surface(cr, s, (aw - p->front.w) / 2, (ah - p->front.h) / 2);
		cairo_paint(cr);
		cairo_surface_destroy(s);
		if (p->front.id != p->drawn_id) {
			p->drawn_id = p->front.id;
			g_atomic_int_inc(&p->shown);
		}
	}
	g_mutex_unlock(&p->pic_m);
	return TRUE;
}


static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
	(void)w;
	(void)data;
	if (app.p != NULL) {
		g_atomic_int_set(&app.p->area_w, a->width);
		g_atomic_int_set(&app.p->area_h, a->height);
	}
}


static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	(void)w;
	(void)data;
	if ((ev->type == GDK_2BUTTON_PRESS) && (ev->button == 1)) {
		ui_toggle_fullscreen();
	}
	return FALSE;
}


static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev, gpointer data)
{
	(void)w;
	(void)data;
	if (ev->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
		app.fullscreen = (ev->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
		gtk_widget_set_visible(app.bar, !app.fullscreen);
		printf("GTK-VIDEO fullscreen=%d\n", app.fullscreen);
	}
	return FALSE;
}


static int do_action(const char *a)
{
	if (strcmp(a, "pause") == 0) {
		ui_toggle_pause();
	}
	else if (strcmp(a, "stop") == 0) {
		ui_stop();
	}
	else if (strcmp(a, "fs") == 0) {
		ui_toggle_fullscreen();
	}
	else if (strcmp(a, "left") == 0) {
		ui_seek_rel(-10);
	}
	else if (strcmp(a, "right") == 0) {
		ui_seek_rel(10);
	}
	else if (strcmp(a, "down") == 0) {
		ui_seek_rel(-60);
	}
	else if (strcmp(a, "up") == 0) {
		ui_seek_rel(60);
	}
	else if (strcmp(a, "quit") == 0) {
		gtk_main_quit();
	}
	else {
		return -1;
	}
	return 0;
}


static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	(void)w;
	(void)data;
	switch (ev->keyval) {
		case GDK_KEY_space: do_action("pause"); return TRUE;
		case GDK_KEY_s: do_action("stop"); return TRUE;
		case GDK_KEY_f:
		case GDK_KEY_F11: do_action("fs"); return TRUE;
		case GDK_KEY_Escape:
			if (app.fullscreen) {
				do_action("fs");
			}
			return TRUE;
		case GDK_KEY_Left: do_action("left"); return TRUE;
		case GDK_KEY_Right: do_action("right"); return TRUE;
		case GDK_KEY_Down: do_action("down"); return TRUE;
		case GDK_KEY_Up: do_action("up"); return TRUE;
		case GDK_KEY_q: do_action("quit"); return TRUE;
		default: return FALSE;
	}
}


static gboolean autokey_cb(gpointer data)
{
	char *a = data;

	printf("GTK-VIDEO auto t=%.2f action=%s clock=%.2f\n", (g_get_monotonic_time() - app.t0_us) / 1000000.0, a,
		app.p != NULL ? clock_now(app.p) : 0.0);
	if (do_action(a) < 0) {
		printf("GTK-VIDEO auto unknown action '%s'\n", a);
	}
	g_free(a);
	return G_SOURCE_REMOVE;
}


static void schedule_autokeys(const char *spec)
{
	gchar **items = g_strsplit(spec, ",", -1), **it;
	char *colon;
	double at;

	for (it = items; *it != NULL; it++) {
		at = g_ascii_strtod(*it, &colon);
		if ((colon == *it) || (*colon != ':')) {
			printf("GTK-VIDEO auto bad item '%s'\n", *it);
			continue;
		}
		g_timeout_add((guint)(at * 1000), autokey_cb, g_strdup(colon + 1));
	}
	g_strfreev(items);
}


static gboolean tick_cb(gpointer data)
{
	static int stat_ms = -1;
	player_t *p = app.p;
	gint64 now = g_get_monotonic_time();
	char a[16], b[16], buf[40];
	double c;
	int shown;
	const char *e;

	(void)data;
	if (stat_ms < 0) {
		e = getenv("GTK_VIDEO_STAT_MS");
		stat_ms = (e != NULL) ? atoi(e) : 2000;
	}
	if (p == NULL) {
		return G_SOURCE_CONTINUE;
	}
	c = clock_now(p);
	if (now - app.user_seek_us > 500000) {
		gtk_range_set_value(GTK_RANGE(app.scale), c);
	}
	fmt_time(a, sizeof(a), c);
	fmt_time(b, sizeof(b), p->duration);
	snprintf(buf, sizeof(buf), "%s / %s", a, b);
	gtk_label_set_text(GTK_LABEL(app.time_lbl), buf);

	if (p->eof && p->vdone && p->adone && !app.finished_reported) {
		app.finished_reported = 1;
		printf("GTK-VIDEO end clock=%.2f shown=%d dropped=%d decoded=%d\n", c, g_atomic_int_get(&p->shown),
			g_atomic_int_get(&p->dropped), g_atomic_int_get(&p->decoded));
		if (app.autoexit) {
			gtk_main_quit();
		}
		else {
			player_set_paused(p, 1);
			ui_sync_play_button();
		}
	}
	if ((stat_ms > 0) && (now - app.last_stat_us >= (gint64)stat_ms * 1000)) {
		shown = g_atomic_int_get(&p->shown);
		printf("GTK-VIDEO stat t=%.2f clock=%.2f shown=%d fps=%.1f dropped=%d decoded=%d vq=%dKB aq=%dKB paused=%d fs=%d "
			"win=%dx%d picture=%dx%d sound=%s\n",
			(now - app.t0_us) / 1000000.0, c, shown,
			app.last_stat_us ? (shown - app.last_stat_shown) * 1000000.0 / (now - app.last_stat_us) : 0.0,
			g_atomic_int_get(&p->dropped), g_atomic_int_get(&p->decoded), (int)(p->vq.bytes / 1024),
			(int)(p->aq.bytes / 1024), p->paused, app.fullscreen, gtk_widget_get_allocated_width(app.area),
			gtk_widget_get_allocated_height(app.area), p->front.w, p->front.h, p->afd >= 0 ? "on" : "off");
		fflush(stdout);
		app.last_stat_us = now;
		app.last_stat_shown = shown;
	}
	return G_SOURCE_CONTINUE;
}


static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);

	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_can_focus(b, FALSE);
	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}


int main(int argc, char **argv)
{
	const char *file = NULL, *e;
	int i, fs = 0, w = 1280, h = 720;
	GtkWidget *box;

	setvbuf(stdout, NULL, _IOLBF, 0);
	gtk_init(&argc, &argv);
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fullscreen") == 0) {
			fs = 1;
		}
		else if (strcmp(argv[i], "--autoexit") == 0) {
			app.autoexit = 1;
		}
		else if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
			printf("usage: gtk-video [--fullscreen] [--autoexit] [file]\n");
			return 0;
		}
		else {
			file = argv[i];
		}
	}
	printf("GTK-VIDEO start file=%s fullscreen=%d autoexit=%d libavcodec=%s\n", file != NULL ? file : "(none)", fs,
		app.autoexit, av_version_info());
	app.t0_us = g_get_monotonic_time();

	app.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app.win), "Video");
	g_signal_connect(app.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(app.win, "key-press-event", G_CALLBACK(on_key), NULL);
	g_signal_connect(app.win, "window-state-event", G_CALLBACK(on_window_state), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(app.win), box);
	app.area = gtk_drawing_area_new();
	gtk_widget_set_hexpand(app.area, TRUE);
	gtk_widget_set_vexpand(app.area, TRUE);
	gtk_widget_add_events(app.area, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(app.area, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(app.area, "size-allocate", G_CALLBACK(on_size_allocate), NULL);
	g_signal_connect(app.area, "button-press-event", G_CALLBACK(on_button_press), NULL);
	gtk_box_pack_start(GTK_BOX(box), app.area, TRUE, TRUE, 0);

	app.bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(app.bar), 4);
	gtk_box_pack_start(GTK_BOX(app.bar), icon_button("document-open", "Open a file", G_CALLBACK(on_open)), FALSE, FALSE, 0);
	app.play_btn = icon_button("media-playback-pause", "Pause (space)", G_CALLBACK(on_play));
	gtk_box_pack_start(GTK_BOX(app.bar), app.play_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(app.bar), icon_button("media-playback-stop", "Stop (s)", G_CALLBACK(on_stop)), FALSE, FALSE, 0);
	app.scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app.scale), FALSE);
	gtk_widget_set_can_focus(app.scale, FALSE);
	g_signal_connect(app.scale, "change-value", G_CALLBACK(on_change_value), NULL);
	gtk_box_pack_start(GTK_BOX(app.bar), app.scale, TRUE, TRUE, 0);
	app.time_lbl = gtk_label_new("0:00 / 0:00");
	gtk_box_pack_start(GTK_BOX(app.bar), app.time_lbl, FALSE, FALSE, 4);
	app.fs_btn = icon_button("view-fullscreen", "Fullscreen (f)", G_CALLBACK(on_fs));
	gtk_box_pack_start(GTK_BOX(app.bar), app.fs_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), app.bar, FALSE, FALSE, 0);

	if (file != NULL) {
		ui_load(file);
		if ((app.p != NULL) && (app.p->vidx >= 0)) {
			w = app.p->vctx->width;
			h = app.p->vctx->height;
			if (w > 1280) {
				h = h * 1280 / w;
				w = 1280;
			}
		}
	}
	gtk_window_set_default_size(GTK_WINDOW(app.win), w, h + 40);
	gtk_widget_show_all(app.win);
	if (fs) {
		gtk_window_fullscreen(GTK_WINDOW(app.win));
	}
	e = getenv("GTK_VIDEO_AUTOKEYS");
	if ((e != NULL) && (*e != '\0')) {
		schedule_autokeys(e);
	}
	g_timeout_add(250, tick_cb, NULL);
	gtk_main();
	printf("GTK-VIDEO done\n");
	player_close(app.p);
	return 0;
}
