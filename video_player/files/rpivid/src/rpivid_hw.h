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

/* The block's DMA reach: it sits on the SCB bus, whose dma-ranges map bus addresses 1:1 onto
 * the first 16 GiB (Linux bcm2711-rpi-ds.dtsi; its hevc_dec driver sets a 36-bit DMA mask, and
 * an address register holds pa >> 6). All of a Pi 4's RAM is below it: the checks against it
 * are guards. */
#define RPIVID_DMA_LIMIT (1ull << 34)

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
	uint32_t stale;                /* completions already pending when a phase was started (cleared) */
	uint32_t status, cfstatus, cfnum;
	uint32_t timeout_phase;        /* -ETIMEDOUT: the phase that did not finish */
	uint32_t clock_off;            /* -ETIMEDOUT: 1 if the block's clock was then switched off */
} rpivid_hw_stat_t;

/* Take the block. On failure returns a negative errno and a reason in why.
 *
 * A block that stopped responding (a phase timed out) is not used again until reboot, by
 * any process: the process that saw it records it in /tmp/.rpivid.dead (/tmp is a RAM file
 * system made at boot) and switches the block's clock off; later opens fail at once (-EIO),
 * before touching the mailbox or a register. FFMPEG_RPIVID_RESET=1: one open per boot instead
 * switches the clock off and on and tries the block again (rpivid_hw_note says so); if that
 * also fails, the block stays off. */
int rpivid_hw_open(rpivid_hw_t **out, char *why, size_t whylen);

/* What the open did beyond the usual, for the log ("" if nothing) */
const char *rpivid_hw_note(const rpivid_hw_t *hw);

/* 1 once the block stopped responding in this process: the memory it was given may still be
 * written by it, so buffers it may write are not freed (rpivid_dma_free leaves them mapped) */
int rpivid_hw_dead(void);

void rpivid_hw_close(rpivid_hw_t *hw);

/* The clock the firmware set, Hz (0 off Phoenix) */
uint32_t rpivid_hw_clock(const rpivid_hw_t *hw);

/* 1: a decode sleeps on the block's interrupt; 0: it polls the interrupt controller */
int rpivid_hw_irq(const rpivid_hw_t *hw);

/* Decode one picture. 0, -ETIMEDOUT (a phase did not finish: the block is not used again,
 * see rpivid_hw_open), -EIO (phase 1 rejected the stream), -ENOMEM. */
int rpivid_hw_decode(rpivid_hw_t *hw, const rpivid_job_t *job, rpivid_hw_stat_t *st);

/* Contiguous uncached memory the block can address (zeroed; below RPIVID_DMA_LIMIT) */
int rpivid_dma_alloc(rpivid_dma_t *d, size_t size);
void rpivid_dma_free(rpivid_dma_t *d);

/* Contiguous cached memory for buffers the block writes and the CPU only reads (the
 * output pictures): zeroed and written back, so no dirty line can later be evicted over
 * what the block wrote. The CPU must not write it; call rpivid_dma_sync_for_cpu() after
 * every decode into it and before reading it. Freed with rpivid_dma_free(). */
int rpivid_dma_alloc_cached(rpivid_dma_t *d, size_t size);

/* Drop the CPU's cached copy of the first len bytes of a rpivid_dma_alloc_cached() buffer */
void rpivid_dma_sync_for_cpu(const rpivid_dma_t *d, size_t len);

/* Order prior writes to uncached memory before the block reads it, and the block's
 * completion before the CPU reads its output */
void rpivid_dma_fence(void);

#endif
