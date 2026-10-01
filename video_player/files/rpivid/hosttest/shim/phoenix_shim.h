/*
 * Phoenix-RTOS
 *
 * rpivid host test: the Phoenix-RTOS interfaces the decoder's hardware layer uses,
 * served by mock.c (forced include for the host build of the hardware layer and of
 * the reference player, tools/hevc-decode/hevc-m2.c)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RPIVID_PHOENIX_SHIM_H
#define RPIVID_PHOENIX_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/mman.h>

typedef uint64_t addr_t;
typedef int handle_t;

#define MAP_UNCACHED   0x01000000
#define MAP_DEVICE     0x02000000
#define MAP_PHYSMEM    0x04000000
#define MAP_CONTIGUOUS 0x08000000

void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int mock_munmap(void *addr, size_t len);
#define mmap   mock_mmap
#define munmap mock_munmap

addr_t va2pa(void *va);

#endif
