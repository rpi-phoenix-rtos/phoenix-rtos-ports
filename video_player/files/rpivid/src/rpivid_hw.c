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
#ifndef RPIVID_DEAD_PATH
#define RPIVID_DEAD_PATH "/tmp/.rpivid.dead"   /* the host test names its own */
#endif

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
	int resetting;                 /* the block is tried again after it stopped responding */
	char note[256];
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


/* Switch the HEVC clock off: a block that stopped responding then cannot finish a transfer
 * into memory freed after this (Linux gates the same clock whenever the block is idle) */
static int clock_off(void)
{
	uint32_t in[2], out[2] = { 0, 0 };

	in[0] = VC_CLK_HEVC;
	in[1] = 0;
	return (vcmbox(VC_SET_CLOCK_STATE, in, 2, out, 2) < 0) ? -EIO : 0;
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
		__atomic_fetch_or(&irq_active, a, __ATOMIC_SEQ_CST);
		wr(intc, ARG_IC_ICTRL, ic & ~SET_ZERO_MASK);
	}
	return 1;
}


/* One completion, two observers: the interrupt handler and the waiter (which also polls the
 * controller) can both see it, and the handler's mark can land after the waiter consumed the
 * completion. Left there, it would end the next wait of that phase at once, before the block
 * finished -- the output read while being written, and the next phase started on a busy block
 * (which then stops responding). So before a phase starts, a completion of it that is already
 * pending is the previous one's: forgotten. 1 if there was one. */
static int clear_stale(rpivid_hw_t *hw, uint32_t bit)
{
	uint32_t ic;
	int stale = 0;

	if (hw->have_irq) {
		mutexLock(hw->irq_mtx);
	}
	if ((__atomic_fetch_and(&irq_active, ~bit, __ATOMIC_SEQ_CST) & bit) != 0u) {
		stale = 1;
	}
	ic = rd(hw->intc, ARG_IC_ICTRL);
	if ((ic & bit) != 0u) {
		/* write-1-to-clear this phase's bit only; the enables written back as read */
		wr(hw->intc, ARG_IC_ICTRL, (ic & ~SET_ZERO_MASK & ~(ACTIVE1_INT_SET | ACTIVE2_INT_SET)) | bit);
		stale = 1;
	}
	if (hw->have_irq) {
		mutexUnlock(hw->irq_mtx);
	}
	return stale;
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
		if ((__atomic_fetch_and(&irq_active, ~bit, __ATOMIC_SEQ_CST) & bit) != 0u) {
			rc = 0;
			break;
		}
		ic = rd(hw->intc, ARG_IC_ICTRL);
		if ((ic & bit) != 0u) {
			wr(hw->intc, ARG_IC_ICTRL, ic & ~SET_ZERO_MASK);
			/* the handler may have marked the same completion meanwhile */
			__atomic_fetch_and(&irq_active, ~bit, __ATOMIC_SEQ_CST);
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


/* ---- the block's state across processes, for this boot ---- */

/* The marker RPIVID_DEAD_PATH holds one line, "<state> pid=<pid> t=<ms since boot> <what>":
 * state "wedged" (a phase timed out) or "reset-tried" (an open switched the clock off and on
 * to try the block again; a success removes the marker, so a "reset-tried" left behind means
 * the try failed or its process died during it). 0: none, 1: wedged, 2: reset-tried. */
static int dead_read(char *line, size_t len)
{
	unsigned long long t;
	const char *p;
	ssize_t n;
	int fd = open(RPIVID_DEAD_PATH, O_RDONLY);

	line[0] = '\0';
	if (fd < 0) {
		return 0;
	}
	n = read(fd, line, len - 1u);
	close(fd);
	if (n < 0) {
		n = 0;
	}
	while ((n > 0) && ((line[n - 1] == '\n') || (line[n - 1] == '\r'))) {
		n--;
	}
	line[n] = '\0';
	/* the monotonic clock starts at boot: a later time is a previous boot's (a /tmp that is
	 * not the RAM file system the system images make) */
	p = strstr(line, " t=");
	if ((p != NULL) && (sscanf(p + 3, "%llu", &t) == 1) && (t > now_ns() / 1000000u)) {
		(void)unlink(RPIVID_DEAD_PATH);
		line[0] = '\0';
		return 0;
	}
	/* an empty or unreadable marker (its writer died) counts as "wedged" */
	return (strncmp(line, "reset-tried", 11) == 0) ? 2 : 1;
}


static void dead_write(const char *state, const char *what)
{
	char line[256];
	ssize_t w;
	int n, fd = open(RPIVID_DEAD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	if (fd < 0) {
		return;
	}
	n = snprintf(line, sizeof(line), "%s pid=%d t=%llu %s\n", state, (int)getpid(), (unsigned long long)(now_ns() / 1000000u), what);
	if (n > 0) {
		w = write(fd, line, ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1u);
		(void)w;
	}
	close(fd);
}


/* A phase did not finish: the block is not used again, by this process or (the marker) any
 * other, and its clock goes off. Its registers are read for the log first (a block in this
 * state answered register reads on the Pi). */
static int block_dead(rpivid_hw_t *hw, uint32_t phase, rpivid_hw_stat_t *st)
{
	char what[160];

	hw_wedged = 1;
	st->timeout_phase = phase;
	st->cfstatus = rd(hw->regs, RPI_CFSTATUS);
	st->cfnum = rd(hw->regs, RPI_CFNUM);
	st->status = rd(hw->regs, RPI_STATUS);
	snprintf(what, sizeof(what), "phase %u timed out (CFSTATUS %u CFNUM %u STATUS 0x%x)%s", phase, st->cfstatus, st->cfnum, st->status,
		hw->resetting ? " after a clock reset" : "");
	dead_write(hw->resetting ? "reset-tried" : "wedged", what);
	/* nothing pending and the handler detached before the clock goes: it must not read the
	 * controller of a block without a clock */
	(void)clear_stale(hw, ACTIVE1_INT_SET);
	(void)clear_stale(hw, ACTIVE2_INT_SET);
	isr_intc = NULL;
	rpivid_dma_fence();
	st->clock_off = (clock_off() == 0) ? 1u : 0u;
	return -ETIMEDOUT;
}


int rpivid_hw_dead(void)
{
	return hw_wedged;
}


const char *rpivid_hw_note(const rpivid_hw_t *hw)
{
	return hw->note;
}


uint32_t rpivid_hw_clock(const rpivid_hw_t *hw)
{
	return hw->clock;
}


int rpivid_hw_irq(const rpivid_hw_t *hw)
{
	return hw->have_irq;
}


static int dma_map(rpivid_dma_t *d, size_t size, int flags)
{
	size_t pg = (size_t)sysconf(_SC_PAGESIZE);
	void *p;

	size = (size + pg - 1u) & ~(pg - 1u);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags | MAP_CONTIGUOUS | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		memset(d, 0, sizeof(*d));
		return -ENOMEM;
	}
	memset(p, 0, size);
	d->pa = (uint64_t)va2pa(p);
	if ((d->pa == 0u) || (d->pa + size > RPIVID_DMA_LIMIT)) {
		/* not the block's to reach (a guard: a Pi 4 has no RAM there) */
		munmap(p, size);
		memset(d, 0, sizeof(*d));
		return -ENOMEM;
	}
	d->cpu = p;
	d->size = size;
	return 0;
}


#ifdef __aarch64__
/* Clean and invalidate [va, va + len) by VA. DC CIVAC, not DC IVAC: only the former is
 * allowed at EL0 (SCTLR_EL1.UCI), and the CPU never dirties these lines, so the clean
 * writes nothing back. */
static void dcache_civac(const void *va, size_t len)
{
	uint64_t ctr;
	uintptr_t line, a, end;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = (uintptr_t)4 << ((ctr >> 16) & 0xfu); /* CTR_EL0.DminLine: log2(words) */
	a = (uintptr_t)va & ~(line - 1u);
	end = (uintptr_t)va + len;

	__asm__ volatile("dsb sy" ::: "memory");
	for (; a < end; a += line) {
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}


int rpivid_dma_alloc_cached(rpivid_dma_t *d, size_t size)
{
	int rc = dma_map(d, size, 0);

	if (rc == 0) {
		/* the zeroes reach memory before the block writes the buffer */
		dcache_civac(d->cpu, d->size);
	}
	return rc;
}


void rpivid_dma_sync_for_cpu(const rpivid_dma_t *d, size_t len)
{
	dcache_civac(d->cpu, (len < d->size) ? len : d->size);
}
#else
int rpivid_dma_alloc_cached(rpivid_dma_t *d, size_t size)
{
	return dma_map(d, size, MAP_UNCACHED);
}


void rpivid_dma_sync_for_cpu(const rpivid_dma_t *d, size_t len)
{
	(void)d;
	(void)len;
	rpivid_dma_fence();
}
#endif


int rpivid_dma_alloc(rpivid_dma_t *d, size_t size)
{
	return dma_map(d, size, MAP_UNCACHED);
}


void rpivid_dma_free(rpivid_dma_t *d)
{
	/* after the block stopped responding it may still hold any of these: left mapped (this
	 * happens once per boot, the block is not used again) */
	if ((d->cpu != NULL) && !hw_wedged) {
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
	char line[256];
	const char *e;
	int rc, dead;

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

	/* a block that stopped responding is left alone: no mailbox call, no register access */
	dead = dead_read(line, sizeof(line));
	if (dead != 0) {
		e = getenv("FFMPEG_RPIVID_RESET");
		if ((dead == 1) && (e != NULL) && (e[0] == '1')) {
			/* one try per boot: recorded before it starts */
			dead_write("reset-tried", "(trying the block again after a clock off/on)");
			(void)clock_off();
			usleep(1000);
			hw->resetting = 1;
			snprintf(hw->note, sizeof(hw->note), "clock switched off and on to try the block again (FFMPEG_RPIVID_RESET; it had stopped responding: %.80s)", line);
		}
		else {
			snprintf(why, whylen, "the block stopped responding earlier in this boot and is not used again (%s: %.110s)%s", RPIVID_DEAD_PATH, line,
				(dead == 1) ? "; FFMPEG_RPIVID_RESET=1 tries it once more" : "");
			rc = -EIO;
			goto fail;
		}
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
		st->stale += (uint32_t)clear_stale(hw, ACTIVE1_INT_SET);
		wr(hw->regs, RPI_CFBASE, RPI_VC_ADDR(hw->cmd.pa));
		if (wait_active(hw, ACTIVE1_INT_SET, P1_TIMEOUT_MS) < 0) {
			st->p1_runs = (uint32_t)runs;
			return block_dead(hw, 1, st);
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
	st->stale += (uint32_t)clear_stale(hw, ACTIVE2_INT_SET);
	wr(hw->regs, RPI_NUMROWS, j->ctb_rows);
	if (wait_active(hw, ACTIVE2_INT_SET, P2_TIMEOUT_MS) < 0) {
		return block_dead(hw, 2, st);
	}
	st->p2_ns = now_ns() - t1;
	if (hw->resetting) {
		/* the try after a clock reset worked: the block is usable again */
		hw->resetting = 0;
		(void)unlink(RPIVID_DEAD_PATH);
	}
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


const char *rpivid_hw_note(const rpivid_hw_t *hw)
{
	(void)hw;
	return "";
}


int rpivid_hw_dead(void)
{
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


int rpivid_dma_alloc_cached(rpivid_dma_t *d, size_t size)
{
	return rpivid_dma_alloc(d, size);
}


void rpivid_dma_sync_for_cpu(const rpivid_dma_t *d, size_t len)
{
	(void)d;
	(void)len;
}


void rpivid_dma_free(rpivid_dma_t *d)
{
	free(d->cpu);
	memset(d, 0, sizeof(*d));
}

#endif
