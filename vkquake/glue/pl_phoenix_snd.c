/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_snd.c — Phoenix audio backend for Quakespasm (replaces snd_sdl.c).
 *
 * Model: snd_dma.c keeps a ring buffer (shm->buffer) into which the mixer paints
 * strictly FORWARD (paintedtime -> endtime, wrapping), and it reads the hardware
 * play cursor only through SNDDMA_GetDMAPos() (monotonic, mod shm->samples). It
 * never writes at arbitrary back-offsets. So we do NOT need to share the ring with
 * the audio device: a feeder thread reads the ring sequentially behind the paint
 * cursor and write()s it to /dev/audio0, which blocks at the playback rate (the
 * rpi4-audio PIO FIFO provides backpressure). The play cursor = bytes the device
 * has actually accepted, which is what GetSoundtime() needs.
 *
 * This is the lighter alternative to mmap-over-msg: correctness first on the PIO
 * write path; the driver can later switch to continuous DMA without touching this.
 *
 * Audible sign-off (headphones on the 3.5 mm jack) is the one manual check.
 */
#include "quakedef.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SND_SPEED      44100
#define SND_CHANNELS   2
#define SND_BITS       16
#define SND_BYTES      (SND_BITS / 8)
#define SND_RING_PAIRS 16384                              /* ~0.37 s @44.1 kHz stereo */
#define SND_RING_SMP   (SND_RING_PAIRS * SND_CHANNELS)    /* "mono samples" (interleaved count) */
#define SND_RING_BYTES (SND_RING_SMP * SND_BYTES)
#define SND_CHUNK_SMP  1024                               /* ~12 ms per write()              */

static int snd_fd = -1;
static unsigned char *snd_ring;            /* == shm->buffer */
static volatile unsigned int snd_playpos;  /* mono samples the device has accepted (monotonic) */
static volatile int snd_run;
static pthread_t snd_feeder;

/* Read the ring sequentially behind the mixer's paint cursor and push to the device.
 * write() blocks at the PWM drain rate, so snd_playpos advances at real playback rate. */
static void *snd_feeder_fn(void *arg)
{
	unsigned int pos = 0;   /* mono-sample read cursor */
	(void)arg;

	while (snd_run) {
		unsigned int idx = pos % SND_RING_SMP;
		unsigned int to_end = SND_RING_SMP - idx;        /* don't wrap within one write */
		unsigned int want = (SND_CHUNK_SMP < to_end) ? SND_CHUNK_SMP : to_end;
		ssize_t wr = write(snd_fd, snd_ring + (size_t)idx * SND_BYTES, (size_t)want * SND_BYTES);

		if (wr > 0) {
			pos += (unsigned int)wr / SND_BYTES;
			snd_playpos = pos;
		}
		else {
			usleep(2000);   /* device stalled (clock dead): back off, keep the cursor steady */
		}
	}
	return NULL;
}

qboolean SNDDMA_Init(dma_t *dma)
{
	snd_fd = open("/dev/audio0", O_WRONLY);
	if (snd_fd < 0) {
		Con_Printf("PL_SND: /dev/audio0 open failed; sound disabled\n");
		return false;
	}

	snd_ring = (unsigned char *)calloc(1, SND_RING_BYTES);
	if (snd_ring == NULL) {
		close(snd_fd);
		snd_fd = -1;
		return false;
	}

	memset(dma, 0, sizeof(*dma));
	dma->channels = SND_CHANNELS;
	dma->samples = SND_RING_SMP;
	dma->submission_chunk = 1;
	dma->samplebits = SND_BITS;
	dma->signed8 = 0;
	dma->speed = SND_SPEED;
	dma->buffer = snd_ring;
	shm = dma;                /* the caller reads shm->* immediately after Init */

	snd_playpos = 0;
	snd_run = 1;
	if (pthread_create(&snd_feeder, NULL, snd_feeder_fn, NULL) != 0) {
		snd_run = 0;
		free(snd_ring);
		snd_ring = NULL;
		close(snd_fd);
		snd_fd = -1;
		return false;
	}

	Con_Printf("PL_SND: /dev/audio0 %d Hz %dch %d-bit, %u-pair ring, feeder thread up\n",
		SND_SPEED, SND_CHANNELS, SND_BITS, (unsigned)SND_RING_PAIRS);
	return true;
}

/* Hardware play cursor in mono samples, mod the ring — what GetSoundtime() expects. */
int SNDDMA_GetDMAPos(void)
{
	return (int)(snd_playpos % SND_RING_SMP);
}

void SNDDMA_Shutdown(void)
{
	if (snd_run) {
		snd_run = 0;
		pthread_join(snd_feeder, NULL);
	}
	if (snd_ring != NULL) {
		free(snd_ring);
		snd_ring = NULL;
	}
	if (snd_fd >= 0) {
		close(snd_fd);
		snd_fd = -1;
	}
}

/* The mixer paints ahead of the feeder's read cursor; aarch64 word reads of
 * snd_playpos are atomic, so no cross-thread lock is needed for the ring. */
void SNDDMA_LockBuffer(void)
{
}

void SNDDMA_Submit(void)
{
}

void SNDDMA_BlockSound(void)
{
}

void SNDDMA_UnblockSound(void)
{
}
