/*
 * Phoenix-RTOS
 *
 * rpivid host test: a register-level stand-in for the BCM2711 rpivid block
 *
 * Serves the Phoenix-RTOS interfaces of shim/ (mmap of the block and of contiguous
 * memory, va2pa, the mailbox, interrupts) and records what a decoder programs, one
 * picture at a time, in a canonical form: the phase-1 command buffer (each slice's
 * bitstream address replaced by a CRC of the bytes it points at) and the phase-2
 * registers (picture buffers named by the POC last decoded into them). Two decoders
 * that drive the block identically produce identical logs, whatever their buffer
 * allocation; this is how the FFmpeg hwaccel is held to the HW-proven reference
 * player (tools/hevc-decode/hevc-m2.c).
 *
 * Environment: MOCK_LOG=<file> (the log; default stdout), MOCK_FAIL_AT=<n> (picture n
 * fails phase 1: CFSTATUS != CFNUM), MOCK_GOLDEN=<file> (phase 2 "decodes" picture POC p
 * by writing frame p of this raw yuv420p / yuv420p10le file, display order, into the
 * output buffers in the block's SAND layout: then the decoder's output path -- de-tiling,
 * frame order, cropping, threading -- can be checked bit-exact against the CPU decoder,
 * for closed-GOP clips: the display index is the POC plus the pictures decoded before the
 * last POC 0; MOCK_GOLDEN_SIZE=<w>x<h> when the file holds the cropped pictures of a
 * stream with a conformance window at the bottom/right; MOCK_GOLDEN_IDR=<i,j,...>: the display
 * indices of the stream's IDR pictures, in order -- the n-th POC 0 the block decodes is the n-th
 * of them -- for decoders that skip pictures (pictures dropped after leaving the block): else
 * the display index base of an IDR is the number of pictures the block decoded before it).
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "phoenix_shim.h"
#include "sys/interrupt.h"
#include "sys/msg.h"
#include "sys/threads.h"
#include "libvcmbox.h"
#include "rpivid_regs.h"

/* ---- contiguous memory with 32-bit-addressable fake bus addresses ---- */

typedef struct {
	uint64_t pa;
	uint8_t *cpu;
	size_t size;
} blk_t;

static blk_t blks[4096];
static int nblks;
static uint64_t next_pa = 0x10000000u;
static pthread_mutex_t memlock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t regs[RPIVID_HEVC_SIZE] __attribute__((aligned(64)));
static uint8_t intc[RPIVID_INTC_SIZE] __attribute__((aligned(64)));
static uint32_t ictrl;


void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	void *p;

	(void)addr;
	(void)prot;
	(void)fd;
	if (flags & MAP_PHYSMEM) {
		if ((uint64_t)off == RPIVID_HEVC_BASE) {
			return regs;
		}
		if ((uint64_t)off == RPIVID_INTC_BASE) {
			return intc;
		}
		return MAP_FAILED;
	}
	if (posix_memalign(&p, 4096, len) != 0) {
		return MAP_FAILED;
	}
	memset(p, 0, len);
	pthread_mutex_lock(&memlock);
	if (nblks == (int)(sizeof(blks) / sizeof(blks[0]))) {
		pthread_mutex_unlock(&memlock);
		free(p);
		return MAP_FAILED;
	}
	blks[nblks].pa = next_pa;
	blks[nblks].cpu = p;
	blks[nblks].size = len;
	nblks++;
	next_pa += (len + 4095u) & ~(size_t)4095u;
	pthread_mutex_unlock(&memlock);
	return p;
}


int mock_munmap(void *addr, size_t len)
{
	int i;

	(void)len;
	if ((addr == regs) || (addr == intc)) {
		return 0;
	}
	pthread_mutex_lock(&memlock);
	for (i = 0; i < nblks; i++) {
		if (blks[i].cpu == addr) {
			free(addr);
			blks[i] = blks[--nblks];
			break;
		}
	}
	pthread_mutex_unlock(&memlock);
	return 0;
}


addr_t va2pa(void *va)
{
	addr_t pa = 0;
	int i;

	pthread_mutex_lock(&memlock);
	for (i = 0; i < nblks; i++) {
		if (((uint8_t *)va >= blks[i].cpu) && ((uint8_t *)va < blks[i].cpu + blks[i].size)) {
			pa = blks[i].pa + (uint64_t)((uint8_t *)va - blks[i].cpu);
			break;
		}
	}
	pthread_mutex_unlock(&memlock);
	return pa;
}


static uint8_t *pa2va(uint64_t pa, size_t len)
{
	uint8_t *va = NULL;
	int i;

	pthread_mutex_lock(&memlock);
	for (i = 0; i < nblks; i++) {
		if ((pa >= blks[i].pa) && (pa + len <= blks[i].pa + blks[i].size)) {
			va = blks[i].cpu + (pa - blks[i].pa);
			break;
		}
	}
	pthread_mutex_unlock(&memlock);
	return va;
}


/* ---- the rest of the platform ---- */

int lookup(const char *name, oid_t *file, oid_t *dev)
{
	(void)name;
	if (file != NULL) {
		file->port = 1;
		file->id = 1;
	}
	if (dev != NULL) {
		dev->port = 1;
		dev->id = 1;
	}
	return 0;
}


int msgSend(uint32_t port, msg_t *m)
{
	uint32_t *out = (uint32_t *)(void *)m->o.raw;

	(void)port;
	/* vcmbox response: err 0, two words, word 1 a 500 MHz clock / "on" state */
	out[0] = 0;
	out[1] = 2;
	out[2] = ((const uint32_t *)(const void *)m->i.raw)[3];
	out[3] = 500000000u;
	return 0;
}


int vcmbox_call(uint32_t tag, uint32_t valBufSize, const uint32_t *in, uint32_t nIn, uint32_t *out, uint32_t nOut)
{
	(void)tag;
	(void)valBufSize;
	(void)nIn;
	if (nOut > 0u) {
		out[0] = in[0];
	}
	if (nOut > 1u) {
		out[1] = 500000000u;
	}
	return 0;
}


int interrupt(unsigned int n, int (*f)(unsigned int, void *), void *arg, handle_t queue, handle_t *handle)
{
	(void)n;
	(void)f;
	(void)arg;
	(void)queue;
	(void)handle;
	return -ENOSYS; /* the decoders poll the interrupt controller instead */
}


int mutexCreate(handle_t *h)
{
	*h = 1;
	return 0;
}


int mutexLock(handle_t h)
{
	(void)h;
	return 0;
}


int mutexUnlock(handle_t h)
{
	(void)h;
	return 0;
}


int condCreate(handle_t *h)
{
	*h = 2;
	return 0;
}


int condWait(handle_t h, handle_t m, time_t timeout)
{
	(void)h;
	(void)m;
	(void)timeout;
	return 0;
}


int condSignal(handle_t h)
{
	(void)h;
	return 0;
}


int resourceDestroy(handle_t h)
{
	(void)h;
	return 0;
}


/* ---- the block ---- */

static FILE *logf_;
static long pic_no;
static long fail_at = -1;
static int failing;
static FILE *golden;

/* the POC last decoded into each output / MV buffer */
#define MAXMAP 512
static struct {
	uint64_t pa;
	int32_t poc;
} ymap[MAXMAP], mvmap[MAXMAP];
static int nymap, nmvmap;


static uint32_t reg(uint32_t off)
{
	return *(uint32_t *)(void *)(regs + off);
}


static FILE *lg(void)
{
	if (logf_ == NULL) {
		const char *p = getenv("MOCK_LOG"), *f = getenv("MOCK_FAIL_AT");

		logf_ = (p != NULL) ? fopen(p, "w") : stdout;
		if (logf_ == NULL) {
			logf_ = stdout;
		}
		if (f != NULL) {
			fail_at = atol(f);
		}
		if ((f = getenv("MOCK_GOLDEN")) != NULL) {
			golden = fopen(f, "rb");
		}
	}
	return logf_;
}


static uint32_t crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	size_t i;
	int k;

	for (i = 0; i < n; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
		}
	}
	return ~c;
}


static void map_set(uint64_t pa, int32_t poc, int mv)
{
	int i, *n = mv ? &nmvmap : &nymap;
	__typeof__(ymap[0]) *m = mv ? mvmap : ymap;

	for (i = 0; i < *n; i++) {
		if (m[i].pa == pa) {
			m[i].poc = poc;
			return;
		}
	}
	if (*n < MAXMAP) {
		m[*n].pa = pa;
		m[*n].poc = poc;
		(*n)++;
	}
}


static void sym(char *out, size_t len, uint64_t pa, uint64_t cur, int mv)
{
	int i, n = mv ? nmvmap : nymap;
	const __typeof__(ymap[0]) *m = mv ? mvmap : ymap;

	if (pa == 0u) {
		snprintf(out, len, "0");
		return;
	}
	if (pa == cur) {
		snprintf(out, len, "CUR");
		return;
	}
	for (i = 0; i < n; i++) {
		if (m[i].pa == pa) {
			snprintf(out, len, "P%d", m[i].poc);
			return;
		}
	}
	snprintf(out, len, "?");
}


static void phase1(uint32_t cfbase)
{
	FILE *f = lg();
	uint32_t n = reg(RPI_CFNUM), i;
	const uint64_t *e = (const uint64_t *)(void *)pa2va((uint64_t)cfbase << 6, (size_t)n * 8u);

	fprintf(f, "PIC %ld\n", pic_no);
	fprintf(f, "K1 PUWSTRIDE=%u COEFFWSTRIDE=%u CFNUM=%u\n", reg(RPI_PUWSTRIDE), reg(RPI_COEFFWSTRIDE), n);
	if (e == NULL) {
		fprintf(f, "C ? command buffer not in mapped memory\n");
		return;
	}
	for (i = 0; i < n; i++) {
		uint32_t off = (uint32_t)e[i], v = (uint32_t)(e[i] >> 32);

		if ((off == RPI_BFBASE) && (i + 2u < n) && ((uint32_t)e[i + 1u] == RPI_BFNUM) && ((uint32_t)e[i + 2u] == RPI_BFCONTROL)) {
			uint32_t len = (uint32_t)(e[i + 1u] >> 32), lo = (uint32_t)(e[i + 2u] >> 32) & 63u;
			const uint8_t *d = pa2va(((uint64_t)v << 6) + lo, len);

			fprintf(f, "C %u BS:%08x:%u\n", off, (d != NULL) ? crc32(d, len) : 0u, len);
		}
		else {
			fprintf(f, "C %u %08x\n", off, v);
		}
	}
}


static void put_sample(uint8_t *plane, uint32_t stride, unsigned int bd, uint32_t x, uint32_t row, uint32_t v)
{
	if (bd == 8u) {
		plane[(size_t)(x / 128u) * stride + (size_t)row * 128u + x % 128u] = (uint8_t)v;
	}
	else {
		uint32_t s = x % 96u, *w = (uint32_t *)(void *)(plane + (size_t)(x / 96u) * stride + (size_t)row * 128u + (s / 3u) * 4u);

		*w = (*w & ~(0x3ffu << (10u * (s % 3u)))) | (v << (10u * (s % 3u)));
	}
}


/* write frame `poc` of the golden file into the output, SAND tiled */
static void golden_picture(uint8_t *py, uint8_t *pc, int32_t poc)
{
	uint32_t w = reg(RPI_FRAMESIZE) & 0xffffu, h = reg(RPI_FRAMESIZE) >> 16, bd = reg(RPI_CONFIG2) & 15u;
	const char *gs = getenv("MOCK_GOLDEN_SIZE");

	if ((gs != NULL) && (sscanf(gs, "%ux%u", &w, &h) != 2)) {
		w = reg(RPI_FRAMESIZE) & 0xffffu;
		h = reg(RPI_FRAMESIZE) >> 16;
	}
	uint32_t ys = reg(RPI_OUTYSTRIDE) << 6, cs = reg(RPI_OUTCSTRIDE) << 6, bps = (bd == 8u) ? 1u : 2u, x, y;
	uint32_t cw = (w + 1u) / 2u, ch = (h + 1u) / 2u;
	size_t fsize = ((size_t)w * h + 2u * (size_t)cw * ch) * bps;
	uint8_t *f = malloc(fsize), *u, *v;

	if ((f == NULL) || (py == NULL) || (pc == NULL) || (poc < 0) || (fseek(golden, (long)fsize * poc, SEEK_SET) != 0) ||
			(fread(f, 1, fsize, golden) != fsize)) {
		fprintf(lg(), "MOCK no golden frame for POC %d\n", poc);
		free(f);
		return;
	}
	u = f + (size_t)w * h * bps;
	v = u + (size_t)cw * ch * bps;
#define SMP(p, i) ((bps == 1u) ? (uint32_t)(p)[i] : ((uint32_t)(p)[2u * (i)] | ((uint32_t)(p)[2u * (i) + 1u] << 8)))
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			put_sample(py, ys, bd, x, y, SMP(f, (size_t)y * w + x));
		}
	}
	for (y = 0; y < ch; y++) {
		for (x = 0; x < cw; x++) {
			put_sample(pc, cs, bd, 2u * x, y, SMP(u, (size_t)y * cw + x));
			put_sample(pc, cs, bd, 2u * x + 1u, y, SMP(v, (size_t)y * cw + x));
		}
	}
#undef SMP
	free(f);
}


static void phase2(void)
{
	FILE *f = lg();
	uint64_t y = (uint64_t)reg(RPI_OUTYBASE) << 6, c = (uint64_t)reg(RPI_OUTCBASE) << 6;
	uint64_t mv = (uint64_t)reg(RPI_MVBASE) << 6, col = (uint64_t)reg(RPI_COLBASE) << 6;
	int32_t poc = (int32_t)reg(RPI_CURRPOC);
	char a[32], b[32];
	unsigned int i;

	fprintf(f, "K2 PURSTRIDE=%u COEFFRSTRIDE=%u OUTYSTRIDE=%u OUTCSTRIDE=%u CONFIG2=%08x FRAMESIZE=%08x CURRPOC=%d "
		"COLSTRIDE=%u MVSTRIDE=%u NUMROWS=%u\n", reg(RPI_PURSTRIDE), reg(RPI_COEFFRSTRIDE), reg(RPI_OUTYSTRIDE),
		reg(RPI_OUTCSTRIDE), reg(RPI_CONFIG2), reg(RPI_FRAMESIZE), poc, reg(RPI_COLSTRIDE), reg(RPI_MVSTRIDE), reg(RPI_NUMROWS));
	sym(b, sizeof(b), col, mv, 1);
	fprintf(f, "K2 MV=%s COL=%s\n", (mv != 0u) ? "CUR" : "0", b);
	fprintf(f, "K2 REF");
	for (i = 0; i < 16u; i++) {
		uint32_t r = RPI_REFBASE + i * RPI_REFREGS_SIZE;

		sym(a, sizeof(a), (uint64_t)reg(r) << 6, y, 0);
		sym(b, sizeof(b), (uint64_t)reg(r + 8u) << 6, c, 0);
		fprintf(f, " %s/%s", a, b);
		if ((reg(r + 4u) != reg(RPI_OUTYSTRIDE)) || (reg(r + 12u) != reg(RPI_OUTCSTRIDE))) {
			fprintf(f, "(stride!)");
		}
	}
	fprintf(f, "\n");
	fflush(f);

	if (golden != NULL) {
		static long gop_base;

		static long idr_no;
		const char *idrs = getenv("MOCK_GOLDEN_IDR");

		if ((poc == 0) && (idrs != NULL)) {
			/* the idr_no-th entry of the list (the last one past its end) */
			long k = 0;
			const char *q = idrs;

			while ((k < idr_no) && (strchr(q, ',') != NULL)) {
				q = strchr(q, ',') + 1;
				k++;
			}
			gop_base = atol(q);
			idr_no++;
		}
		else if ((poc == 0) && (pic_no > 0)) {
			gop_base = pic_no; /* an IDR: every earlier picture is displayed before it */
		}
		golden_picture(pa2va(y, 1), pa2va(c, 1), (int32_t)(gop_base + poc));
	}

	map_set(y, poc, 0);
	map_set(c, poc, 0);
	if (mv != 0u) {
		map_set(mv, poc, 1);
	}
	pic_no++;
}


static uint32_t mmio_rd(const volatile uint8_t *base, uint32_t off)
{
	if (base == intc) {
		return (off == ARG_IC_ICTRL) ? ictrl : 0u;
	}
	if (off == RPI_VERSION) {
		return RPIVID_EXPECT_VER;
	}
	if (off == RPI_CFSTATUS) {
		return failing ? reg(RPI_CFNUM) - 1u : reg(RPI_CFNUM);
	}
	return reg(off);
}


static void mmio_wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	if (base == intc) {
		if (off == ARG_IC_ICTRL) {
			ictrl &= ~(v & (ACTIVE1_INT_SET | ACTIVE2_INT_SET));
		}
		return;
	}
	*(uint32_t *)(void *)(regs + off) = v;
	if (off == RPI_CFBASE) {
		if (failing) {
			/* the failed picture never reached phase 2 */
			failing = 0;
			pic_no++;
		}
		phase1(v);
		ictrl |= ACTIVE1_INT_SET;
		if (pic_no == fail_at) {
			fprintf(lg(), "MOCK picture %ld fails phase 1\n", pic_no);
			failing = 1;
		}
	}
	else if (off == RPI_NUMROWS) {
		phase2();
		ictrl |= ACTIVE2_INT_SET;
	}
}


/* rpivid_hw.c (RPIVID_MMIO_HOOKS) */
uint32_t rd(const volatile uint8_t *base, uint32_t off);
void wr(volatile uint8_t *base, uint32_t off, uint32_t v);

uint32_t rd(const volatile uint8_t *base, uint32_t off)
{
	return mmio_rd(base, off);
}


void wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	mmio_wr(base, off, v);
}


/* hevc-m2.c (its rd/wr renamed by run.sh): plain addresses */
uint32_t m2_rd(const volatile void *a);
void m2_wr(volatile void *a, uint32_t v);

static const volatile uint8_t *region(const volatile void *a, uint32_t *off)
{
	const volatile uint8_t *p = a;

	if ((p >= intc) && (p < intc + sizeof(intc))) {
		*off = (uint32_t)(p - intc);
		return intc;
	}
	*off = (uint32_t)(p - regs);
	return regs;
}


uint32_t m2_rd(const volatile void *a)
{
	uint32_t off;
	const volatile uint8_t *b = region(a, &off);

	return mmio_rd(b, off);
}


void m2_wr(volatile void *a, uint32_t v)
{
	uint32_t off;
	const volatile uint8_t *b = region(a, &off);

	mmio_wr((volatile uint8_t *)b, off, v);
}
