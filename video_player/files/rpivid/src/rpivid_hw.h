/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid HEVC decoder: hardware access
 *
 * A user process drives the block directly: its registers and interrupt controller are
 * mapped (MAP_DEVICE | MAP_PHYSMEM), its clock is switched on through the VideoCore
 * mailbox server (/dev/vcmbox), completion comes on GIC SPI 98, and every buffer it
 * reads or writes is contiguous, uncached memory (MAP_CONTIGUOUS | MAP_UNCACHED). One
 * process at a time may own the block (a lock file); inside a process, one decoder.
 *
 * Built for anything but Phoenix-RTOS the open call fails (ENOSYS) and the allocator
 * returns ordinary memory, so the decoder that uses it falls back to software.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_HW_H
#define RPIVID_HW_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	void *cpu;
	uint64_t pa;
	size_t size;
} rpivid_dma_t;

typedef struct rpivid_hw rpivid_hw_t;

/* One picture: the phase-1 command buffer and the phase-2 registers */
typedef struct {
	const uint64_t *cmd;
	uint32_t cmd_len;
	uint32_t width, height, ctb_rows;
	uint64_t out_y, out_c;
	uint32_t luma_stride, chroma_stride;
	uint64_t ref[16][2];           /* luma, chroma of each reference slot */
	uint32_t config2, currpoc;
	uint32_t colmv_stride;
	uint64_t mv, col;              /* this picture's / the collocated picture's MV buffer, 0 = none */
} rpivid_job_t;

typedef struct {
	uint64_t p1_ns, p2_ns;         /* phase durations */
	uint32_t p1_runs;              /* phase-1 runs (> 1: a PU/coefficient buffer was enlarged) */
	uint32_t status, cfstatus, cfnum;
} rpivid_hw_stat_t;

/* Take the block. On failure returns a negative errno and a reason in why. */
int rpivid_hw_open(rpivid_hw_t **out, char *why, size_t whylen);

void rpivid_hw_close(rpivid_hw_t *hw);

/* The clock the firmware set, Hz (0 off Phoenix) */
uint32_t rpivid_hw_clock(const rpivid_hw_t *hw);

/* Decode one picture. 0, -ETIMEDOUT (a phase did not finish: the block may be wedged and
 * should not be used again), -EIO (phase 1 rejected the stream), -ENOMEM. */
int rpivid_hw_decode(rpivid_hw_t *hw, const rpivid_job_t *job, rpivid_hw_stat_t *st);

/* Contiguous uncached memory the block can address (zeroed) */
int rpivid_dma_alloc(rpivid_dma_t *d, size_t size);
void rpivid_dma_free(rpivid_dma_t *d);

/* Order prior writes to uncached memory before the block reads it, and the block's
 * completion before the CPU reads its output */
void rpivid_dma_fence(void);

#endif
