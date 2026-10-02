/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: hardware access
 *
 * The bring-up sequence, barriers and completion wait are the ones the coordination
 * repository's tools/hevc-decode (hevc-m2.c) proved on the Pi 4; see its README for
 * why each barrier is a full-system dsb and what was ruled out for the open
 * intermittent-corruption issue (gotcha 8).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpivid_hw.h"
#include "rpivid_regs.h"


#ifdef __aarch64__
void rpivid_dma_fence(void)
{
	/* the block is a non-coherent system master: dmb ish does not order Normal-NC
	 * stores against it, dsb sy waits for them to reach the endpoint */
	__asm__ volatile("dsb sy" ::: "memory");
}
#else
void rpivid_dma_fence(void)
{
	__sync_synchronize();
}
#endif


#ifdef __phoenix__

#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <sys/interrupt.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/threads.h>

/* VideoCore firmware property tags (mailbox channel 8) */
#define VC_GET_CLOCK_STATE    0x00030001u
#define VC_GET_CLOCK_RATE     0x00030002u
#define VC_GET_MAX_CLOCK_RATE 0x00030004u
#define VC_SET_CLOCK_STATE    0x00038001u
#define VC_SET_CLOCK_RATE     0x00038002u
#define VC_CLK_HEVC           11u

#define RPIVID_LOCK_PATH "/tmp/.rpivid.lock"

#define P1_TIMEOUT_MS 1000
#define P2_TIMEOUT_MS 2000
#define P1_MAX_RUNS   5

/* The /dev/vcmbox request/response layout (phoenix-rtos-devices misc/rpi4-vcmbox,
 * libvcmbox.h), carried in msg.i.raw / msg.o.raw of an mtDevCtl message */
#define VCMBOX_MAX_WORDS 12u

typedef struct {
	uint32_t tag, valBufSize, nIn;
	uint32_t in[VCMBOX_MAX_WORDS];
} vcmbox_req_t;

typedef struct {
	int err;
	uint32_t nOut;
	uint32_t out[VCMBOX_MAX_WORDS];
} vcmbox_resp_t;

struct rpivid_hw {
	volatile uint8_t *regs, *intc;
	uint32_t clock;
	int lockfd;
	handle_t irq_cond, irq_mtx, irq_h;
	int have_irq;
	rpivid_dma_t cmd, pu, coeff;
	size_t pu_size, coeff_size;    /* the sizes asked for: the strides derive from these */
};

/* one owner per process: the interrupt handler finds the block through this */
static pthread_mutex_t hw_lock = PTHREAD_MUTEX_INITIALIZER;
static rpivid_hw_t *hw_owner;
static int hw_wedged;
static volatile uint32_t irq_active;
/* the interrupt controller as the handler sees it: set before the handler is
 * registered, cleared before it is removed (the kernel deletes it with the handle) */
static volatile uint8_t *volatile isr_intc;


#ifndef RPIVID_MMIO_HOOKS
static inline uint32_t rd(const volatile uint8_t *base, uint32_t off)
{
	return *(const volatile uint32_t *)(base + off);
}


static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}
#else
/* the host test (files/rpivid/hosttest) records the register traffic */
uint32_t rd(const volatile uint8_t *base, uint32_t off);
void wr(volatile uint8_t *base, uint32_t off, uint32_t v);
#endif


static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}


static int vcmbox(uint32_t tag, const uint32_t *in, uint32_t nin, uint32_t *out, uint32_t nout)
{
	static oid_t oid;
	static int resolved;
	msg_t m;
	vcmbox_req_t *req = (vcmbox_req_t *)m.i.raw;
	const vcmbox_resp_t *resp = (const vcmbox_resp_t *)m.o.raw;
	uint32_t i;
	int err;

	if (!resolved) {
		if (lookup("/dev/vcmbox", NULL, &oid) < 0) {
			return -ENODEV;
		}
		resolved = 1;
	}
	memset(&m, 0, sizeof(m));
	m.type = mtDevCtl;
	m.oid = oid;
	req->tag = tag;
	req->valBufSize = 4u * ((nin > nout) ? nin : nout);
	req->nIn = nin;
	for (i = 0; i < nin; i++) {
		req->in[i] = in[i];
	}
	err = msgSend(oid.port, &m);
	if (err < 0) {
		return err;
	}
	if (resp->err != 0) {
		return resp->err;
	}
	for (i = 0; (i < nout) && (i < resp->nOut); i++) {
		out[i] = resp->out[i];
	}
	return 0;
}


/* The HEVC clock at the firmware's maximum */
static int clock_on(uint32_t *rate)
{
	uint32_t in[3], out[2] = { 0, 0 }, max;

	in[0] = VC_CLK_HEVC;
	if ((vcmbox(VC_GET_MAX_CLOCK_RATE, in, 1, out, 2) < 0) || (out[1] == 0u)) {
		return -EIO;
	}
	max = out[1];
	in[1] = 1;
	if (vcmbox(VC_SET_CLOCK_STATE, in, 2, out, 2) < 0) {
		return -EIO;
	}
	in[1] = max;
	in[2] = 0;
	if (vcmbox(VC_SET_CLOCK_RATE, in, 3, out, 2) < 0) {
		return -EIO;
	}
	*rate = out[1];
	if (vcmbox(VC_GET_CLOCK_RATE, in, 1, out, 2) == 0 && out[1] != 0u) {
		*rate = out[1];
	}
	return (*rate != 0u) ? 0 : -EIO;
}


static void *map_phys(uint32_t base, uint32_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_DEVICE | MAP_UNCACHED | MAP_PHYSMEM | MAP_ANONYMOUS, -1, (off_t)base);

	return (p == MAP_FAILED) ? NULL : p;
}


static int hw_isr(unsigned int n, void *arg)
{
	volatile uint8_t *intc = isr_intc;
	uint32_t ic, a;

	(void)n;
	(void)arg;
	if (intc == NULL) {
		return -1; /* not ours (any more) */
	}
	ic = rd(intc, ARG_IC_ICTRL);
	a = ic & (ACTIVE1_INT_SET | ACTIVE2_INT_SET);
	if (a != 0u) {
		irq_active |= a;
		wr(intc, ARG_IC_ICTRL, ic & ~SET_ZERO_MASK);
	}
	return 1;
}


/* Wait for a phase's ACTIVE bit: blocked on the interrupt, re-checking the controller
 * each wake so a lost interrupt only costs 2 ms */
static int wait_active(rpivid_hw_t *hw, uint32_t bit, int timeout_ms)
{
	int waited_us = 0, rc = -ETIMEDOUT;
	uint32_t ic;

	if (hw->have_irq) {
		mutexLock(hw->irq_mtx);
	}
	for (;;) {
		if ((irq_active & bit) != 0u) {
			irq_active &= ~bit;
			rc = 0;
			break;
		}
		ic = rd(hw->intc, ARG_IC_ICTRL);
		if ((ic & bit) != 0u) {
			wr(hw->intc, ARG_IC_ICTRL, ic & ~SET_ZERO_MASK);
			rc = 0;
			break;
		}
		if (waited_us >= timeout_ms * 1000) {
			break;
		}
		if (hw->have_irq) {
			condWait(hw->irq_cond, hw->irq_mtx, 2000);
			waited_us += 2000;
		}
		else {
			usleep(20);
			waited_us += 20;
		}
	}
	if (hw->have_irq) {
		mutexUnlock(hw->irq_mtx);
	}
	if (rc == 0) {
		/* the completion before any read of the output */
		rpivid_dma_fence();
	}
	return rc;
}


/* Process-wide exclusion. A lock that cannot be taken because the file system has no
 * /tmp (or no record locks) does not stop us: on such a system nothing else uses it. */
static int take_lock(char *why, size_t whylen)
{
	struct flock fl;
	int fd = open(RPIVID_LOCK_PATH, O_RDWR | O_CREAT, 0666);

	if (fd < 0) {
		return -1;
	}
	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (fcntl(fd, F_SETLK, &fl) < 0) {
		if ((errno == EAGAIN) || (errno == EACCES)) {
			snprintf(why, whylen, "the block is in use by another process (%s)", RPIVID_LOCK_PATH);
			close(fd);
			return -2;
		}
		close(fd);
		return -1;
	}
	return fd;
}


uint32_t rpivid_hw_clock(const rpivid_hw_t *hw)
{
	return hw->clock;
}


int rpivid_hw_irq(const rpivid_hw_t *hw)
{
	return hw->have_irq;
}


int rpivid_dma_alloc(rpivid_dma_t *d, size_t size)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	void *p;

	size = (size + pg - 1u) & ~(pg - 1u);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_UNCACHED | MAP_CONTIGUOUS | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		memset(d, 0, sizeof(*d));
		return -ENOMEM;
	}
	memset(p, 0, size);
	d->cpu = p;
	d->pa = (uint64_t)va2pa(p);
	d->size = size;
	return 0;
}


void rpivid_dma_free(rpivid_dma_t *d)
{
	if (d->cpu != NULL) {
		munmap(d->cpu, d->size);
	}
	memset(d, 0, sizeof(*d));
}


static void hw_release(rpivid_hw_t *hw)
{
	if (hw->have_irq) {
		isr_intc = NULL;
		rpivid_dma_fence();
		resourceDestroy(hw->irq_h);
		resourceDestroy(hw->irq_cond);
		resourceDestroy(hw->irq_mtx);
	}
	rpivid_dma_free(&hw->cmd);
	rpivid_dma_free(&hw->pu);
	rpivid_dma_free(&hw->coeff);
	if (hw->regs != NULL) {
		munmap((void *)hw->regs, RPIVID_HEVC_SIZE);
	}
	if (hw->intc != NULL) {
		munmap((void *)hw->intc, RPIVID_INTC_SIZE);
	}
	if (hw->lockfd >= 0) {
		close(hw->lockfd);
	}
	free(hw);
}


int rpivid_hw_open(rpivid_hw_t **out, char *why, size_t whylen)
{
	rpivid_hw_t *hw;
	uint32_t ver;
	int rc;

	*out = NULL;
	snprintf(why, whylen, "unknown");
	pthread_mutex_lock(&hw_lock);
	if (hw_wedged) {
		snprintf(why, whylen, "the block stopped responding earlier in this process");
		pthread_mutex_unlock(&hw_lock);
		return -EIO;
	}
	if (hw_owner != NULL) {
		snprintf(why, whylen, "another decoder in this process owns the block");
		pthread_mutex_unlock(&hw_lock);
		return -EBUSY;
	}
	hw = calloc(1, sizeof(*hw));
	if (hw == NULL) {
		pthread_mutex_unlock(&hw_lock);
		return -ENOMEM;
	}
	hw->lockfd = take_lock(why, whylen);
	if (hw->lockfd == -2) {
		hw->lockfd = -1;
		rc = -EBUSY;
		goto fail;
	}

	rc = clock_on(&hw->clock);
	if (rc < 0) {
		snprintf(why, whylen, "the VideoCore mailbox (/dev/vcmbox) did not enable the HEVC clock (%d)", rc);
		goto fail;
	}
	hw->regs = map_phys(RPIVID_HEVC_BASE, RPIVID_HEVC_SIZE);
	hw->intc = map_phys(RPIVID_INTC_BASE, RPIVID_INTC_SIZE);
	if ((hw->regs == NULL) || (hw->intc == NULL)) {
		snprintf(why, whylen, "cannot map the block's registers");
		rc = -ENOMEM;
		goto fail;
	}
	ver = rd(hw->regs, RPI_VERSION);
	if (ver != RPIVID_EXPECT_VER) {
		snprintf(why, whylen, "unexpected block version 0x%x (want 0x%x)", ver, RPIVID_EXPECT_VER);
		rc = -ENODEV;
		goto fail;
	}
	if (rpivid_dma_alloc(&hw->cmd, 64u * 1024u) < 0) {
		snprintf(why, whylen, "no contiguous memory for the command buffer");
		rc = -ENOMEM;
		goto fail;
	}

	/* interrupt controller: enable both phase interrupts, clear what is pending */
	wr(hw->intc, ARG_IC_ICTRL, ACTIVE1_EN_SET | ACTIVE2_EN_SET);
	wr(hw->intc, ARG_IC_ICTRL, rd(hw->intc, ARG_IC_ICTRL));
	irq_active = 0;
	if ((condCreate(&hw->irq_cond) == 0) && (mutexCreate(&hw->irq_mtx) == 0)) {
		isr_intc = hw->intc;
		rpivid_dma_fence();
		if (interrupt(RPIVID_IRQ, hw_isr, NULL, hw->irq_cond, &hw->irq_h) >= 0) {
			hw->have_irq = 1;
		}
		else {
			isr_intc = NULL;
			resourceDestroy(hw->irq_cond);
			resourceDestroy(hw->irq_mtx);
		}
	}

	hw_owner = hw;
	pthread_mutex_unlock(&hw_lock);
	*out = hw;
	return 0;

fail:
	hw_release(hw);
	pthread_mutex_unlock(&hw_lock);
	return rc;
}


void rpivid_hw_close(rpivid_hw_t *hw)
{
	if (hw == NULL) {
		return;
	}
	pthread_mutex_lock(&hw_lock);
	if (hw_owner == hw) {
		hw_owner = NULL;
	}
	hw_release(hw);
	pthread_mutex_unlock(&hw_lock);
}


/* Make a buffer at least `want` bytes (contents need not survive) */
static int ensure(rpivid_dma_t *d, size_t want)
{
	if (d->size >= want) {
		return 0;
	}
	rpivid_dma_free(d);
	return rpivid_dma_alloc(d, want);
}


/* round_up_size of the driver: at least 256, else the next 3 * 2^n or 4 * 2^n */
static size_t round_up_size(size_t x)
{
	unsigned int n = 0;
	size_t t;

	if (x < 256u) {
		return 256u;
	}
	for (t = x; t > 1u; t >>= 1) {
		n++;
	}
	return (x >= ((size_t)3u << n)) ? ((size_t)4u << n) : ((size_t)3u << n);
}


/* A phase-1 output buffer of the given logical size (PU: w*h/4, coefficients: w*h,
 * each rounded up; after an overflow the next rounded size) */
static int ensure_p1(rpivid_dma_t *d, size_t *cur, size_t want)
{
	if (*cur >= want) {
		return 0;
	}
	if (ensure(d, want) < 0) {
		*cur = 0;
		return -ENOMEM;
	}
	*cur = want;
	return 0;
}


int rpivid_hw_decode(rpivid_hw_t *hw, const rpivid_job_t *j, rpivid_hw_stat_t *st)
{
	size_t wh = (size_t)j->width * j->height;
	uint32_t pu_stride, coeff_stride, i, status = 0;
	uint64_t t0, t1;
	int runs;

	memset(st, 0, sizeof(*st));
	if ((j->ctb_rows == 0u) || (j->cmd_len == 0u)) {
		return -EINVAL;
	}
	if ((ensure_p1(&hw->pu, &hw->pu_size, round_up_size(wh / 4u)) < 0) ||
			(ensure_p1(&hw->coeff, &hw->coeff_size, round_up_size(wh)) < 0) ||
			(ensure(&hw->cmd, (size_t)j->cmd_len * 8u) < 0)) {
		return -ENOMEM;
	}

	/* drain earlier uncached stores (the previous picture's output reads, our copies)
	 * before writing this picture's command buffer */
	rpivid_dma_fence();
	memcpy(hw->cmd.cpu, j->cmd, (size_t)j->cmd_len * 8u);

	t0 = now_ns();
	for (runs = 1;; runs++) {
		pu_stride = (uint32_t)((hw->pu_size / j->ctb_rows) & ~(size_t)63u);
		coeff_stride = (uint32_t)((hw->coeff_size / j->ctb_rows) & ~(size_t)63u);
		rpivid_dma_fence();
		wr(hw->regs, RPI_PUWBASE, RPI_VC_ADDR(hw->pu.pa));
		wr(hw->regs, RPI_PUWSTRIDE, RPI_VC_LEN(pu_stride));
		wr(hw->regs, RPI_COEFFWBASE, RPI_VC_ADDR(hw->coeff.pa));
		wr(hw->regs, RPI_COEFFWSTRIDE, RPI_VC_LEN(coeff_stride));
		wr(hw->regs, RPI_CFNUM, j->cmd_len);
		wr(hw->regs, RPI_CFBASE, RPI_VC_ADDR(hw->cmd.pa));
		if (wait_active(hw, ACTIVE1_INT_SET, P1_TIMEOUT_MS) < 0) {
			hw_wedged = 1;
			st->p1_runs = (uint32_t)runs;
			return -ETIMEDOUT;
		}
		st->cfstatus = rd(hw->regs, RPI_CFSTATUS);
		st->cfnum = rd(hw->regs, RPI_CFNUM);
		if (st->cfstatus == st->cfnum) {
			break;
		}
		status = rd(hw->regs, RPI_STATUS);
		st->status = status;
		status &= RPI_STATUS_PU_EXHAUSTED | RPI_STATUS_COEFF_EXHAUSTED;
		if ((status == 0u) || (runs >= P1_MAX_RUNS)) {
			st->p1_runs = (uint32_t)runs;
			return -EIO;
		}
		if ((((status & RPI_STATUS_PU_EXHAUSTED) != 0u) && (ensure_p1(&hw->pu, &hw->pu_size, round_up_size(hw->pu_size + 1u)) < 0)) ||
				(((status & RPI_STATUS_COEFF_EXHAUSTED) != 0u) &&
				(ensure_p1(&hw->coeff, &hw->coeff_size, round_up_size(hw->coeff_size + 1u)) < 0))) {
			return -ENOMEM;
		}
	}
	t1 = now_ns();
	st->p1_ns = t1 - t0;
	st->p1_runs = (uint32_t)runs;

	wr(hw->regs, RPI_PURBASE, RPI_VC_ADDR(hw->pu.pa));
	wr(hw->regs, RPI_PURSTRIDE, RPI_VC_LEN(pu_stride));
	wr(hw->regs, RPI_COEFFRBASE, RPI_VC_ADDR(hw->coeff.pa));
	wr(hw->regs, RPI_COEFFRSTRIDE, RPI_VC_LEN(coeff_stride));
	wr(hw->regs, RPI_OUTYBASE, RPI_VC_ADDR(j->out_y));
	wr(hw->regs, RPI_OUTCBASE, RPI_VC_ADDR(j->out_c));
	wr(hw->regs, RPI_OUTYSTRIDE, RPI_VC_LEN(j->luma_stride));
	wr(hw->regs, RPI_OUTCSTRIDE, RPI_VC_LEN(j->chroma_stride));
	for (i = 0; i < 16u; i++) {
		uint32_t r = RPI_REFBASE + i * RPI_REFREGS_SIZE;

		wr(hw->regs, r + 0u, RPI_VC_ADDR(j->ref[i][0]));
		wr(hw->regs, r + 4u, RPI_VC_LEN(j->luma_stride));
		wr(hw->regs, r + 8u, RPI_VC_ADDR(j->ref[i][1]));
		wr(hw->regs, r + 12u, RPI_VC_LEN(j->chroma_stride));
	}
	wr(hw->regs, RPI_CONFIG2, j->config2);
	wr(hw->regs, RPI_FRAMESIZE, (j->height << 16) | j->width);
	wr(hw->regs, RPI_CURRPOC, j->currpoc);
	wr(hw->regs, RPI_COLSTRIDE, RPI_VC_LEN(j->colmv_stride));
	wr(hw->regs, RPI_MVSTRIDE, RPI_VC_LEN(j->colmv_stride));
	wr(hw->regs, RPI_MVBASE, RPI_VC_ADDR(j->mv));
	wr(hw->regs, RPI_COLBASE, RPI_VC_ADDR(j->col));
	rpivid_dma_fence();
	wr(hw->regs, RPI_NUMROWS, j->ctb_rows);
	if (wait_active(hw, ACTIVE2_INT_SET, P2_TIMEOUT_MS) < 0) {
		hw_wedged = 1;
		return -ETIMEDOUT;
	}
	st->p2_ns = now_ns() - t1;
	return 0;
}

#else /* !__phoenix__: no block; ordinary memory so the code around it can be tested */

struct rpivid_hw {
	int unused;
};


int rpivid_hw_open(rpivid_hw_t **out, char *why, size_t whylen)
{
	*out = NULL;
	snprintf(why, whylen, "the rpivid block is only driven on Phoenix-RTOS");
	return -ENOSYS;
}


void rpivid_hw_close(rpivid_hw_t *hw)
{
	(void)hw;
}


uint32_t rpivid_hw_clock(const rpivid_hw_t *hw)
{
	(void)hw;
	return 0;
}


int rpivid_hw_irq(const rpivid_hw_t *hw)
{
	(void)hw;
	return 0;
}


int rpivid_hw_decode(rpivid_hw_t *hw, const rpivid_job_t *job, rpivid_hw_stat_t *st)
{
	(void)hw;
	(void)job;
	memset(st, 0, sizeof(*st));
	return -ENOSYS;
}


int rpivid_dma_alloc(rpivid_dma_t *d, size_t size)
{
	size = (size + 4095u) & ~(size_t)4095u;
	d->cpu = calloc(1, size);
	if (d->cpu == NULL) {
		memset(d, 0, sizeof(*d));
		return -ENOMEM;
	}
	d->pa = (uint64_t)(uintptr_t)d->cpu;
	d->size = size;
	return 0;
}


void rpivid_dma_free(rpivid_dma_t *d)
{
	free(d->cpu);
	memset(d, 0, sizeof(*d));
}

#endif
