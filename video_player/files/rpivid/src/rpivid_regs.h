/*
 * Phoenix-RTOS
 *
 * BCM2711 rpivid (hevc_dec) HEVC decoder block: register map
 *
 * Byte offsets from the block base. Address registers take the bus address >> 6,
 * length/stride registers take (bytes + 63) >> 6. The map is the one the
 * coordination repository's tools/hevc-decode (hevc_regs.h) proved on the Pi 4;
 * register names follow the Raspberry Pi Linux hevc_dec driver so the two can be
 * read side by side.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_REGS_H
#define RPIVID_REGS_H

/* Block bases (ARM physical; device tree bus 0x7exxxxxx -> 0xfexxxxxx) */
#define RPIVID_HEVC_BASE 0xfeb00000u
#define RPIVID_HEVC_SIZE 0x10000u
#define RPIVID_INTC_BASE 0xfeb10000u
#define RPIVID_INTC_SIZE 0x1000u

#define RPI_VERSION         60u
#define RPIVID_EXPECT_VER   0x202u

/* Phase-1 registers: written through the command buffer (u64 entries off | data << 32) */
#define RPI_SPS0         0u
#define RPI_SPS1         4u
#define RPI_PPS          8u
#define RPI_SLICE        12u
#define RPI_TILESTART    16u
#define RPI_TILEEND      20u
#define RPI_SLICESTART   24u
#define RPI_MODE         28u
#define RPI_QP           48u
#define RPI_CONTROL      52u
#define RPI_STATUS       56u
#define RPI_BFBASE       64u
#define RPI_BFNUM        68u
#define RPI_BFCONTROL    72u
#define RPI_SLICECMDS    96u
#define RPI_BEGINTILEEND 100u
#define RPI_TRANSFER     104u
#define RPI_PROBBASE     0x1000u
#define RPI_SCALINGBASE  0x2000u
#define RPI_SLICEMSGBASE 0x4000u

#define RPI_MODE_TILE    0xffffu
#define RPI_MODE_WPP     1u
#define RPI_MODE_LASTCOL (1u << 17)
#define RPI_MODE_LASTROW (1u << 18)

#define RPI_BFCONTROL_STOP (1u << 7)
#define RPI_BFCONTROL_EMU  (1u << 6)

#define RPI_PROB_BACKUP ((20u << 12) | (20u << 6))
#define RPI_PROB_RELOAD ((20u << 12) | (20u << 0))

/* Phase-1 kick: direct register writes */
#define RPI_PUWBASE      80u
#define RPI_PUWSTRIDE    84u
#define RPI_COEFFWBASE   88u
#define RPI_COEFFWSTRIDE 92u
#define RPI_CFBASE       108u /* command buffer address >> 6: starts phase 1 */
#define RPI_CFNUM        112u /* number of u64 command entries */
#define RPI_CFSTATUS     116u /* phase 1 succeeded <=> CFSTATUS == CFNUM */

#define RPI_STATUS_COEFF_EXHAUSTED 8u
#define RPI_STATUS_PU_EXHAUSTED    16u

/* Phase-2 registers: direct register writes */
#define RPI_PURBASE      0x8000u
#define RPI_PURSTRIDE    0x8004u
#define RPI_COEFFRBASE   0x8008u
#define RPI_COEFFRSTRIDE 0x800cu
#define RPI_NUMROWS      0x8010u /* picture height in CTBs: starts phase 2 */
#define RPI_CONFIG2      0x8014u
#define RPI_OUTYBASE     0x8018u
#define RPI_OUTYSTRIDE   0x801cu
#define RPI_OUTCBASE     0x8020u
#define RPI_OUTCSTRIDE   0x8024u
#define RPI_FRAMESIZE    0x802cu
#define RPI_MVBASE       0x8030u
#define RPI_MVSTRIDE     0x8034u
#define RPI_COLBASE      0x8038u
#define RPI_COLSTRIDE    0x803cu
#define RPI_CURRPOC      0x8040u
#define RPI_REFBASE      0x9000u /* 16 slots of 16 bytes: Y base, Y stride, C base, C stride */
#define RPI_REFREGS_SIZE 16u

/* ARGON interrupt controller */
#define ARG_IC_ICTRL    0u
#define ACTIVE1_EN_SET  (1u << 2)
#define ACTIVE2_EN_SET  (1u << 6)
#define ACTIVE1_INT_SET (1u << 0)
#define ACTIVE2_INT_SET (1u << 4)
#define SET_ZERO_MASK   ((0xffu << 12) | (1u << 11))

/* GIC SPI 98 (device tree hevc_dec interrupts); SPIs start at 32 */
#define RPIVID_IRQ (32u + 98u)

#define RPI_VC_ADDR(pa) ((uint32_t)((uint64_t)(pa) >> 6))
#define RPI_VC_LEN(b)   ((uint32_t)(((uint64_t)(b) + 63u) >> 6))

#endif
