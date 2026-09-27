/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: eventfd emulation (see sys/epoll.h). libwayland-server uses
 * one eventfd to wake its loop from wl_display_terminate() on another thread.
 * An emulated eventfd is one end of a socketpair; write() on it (-Wl,--wrap=write)
 * goes to the peer end, so the descriptor itself becomes readable, and read()
 * returns one 8-byte value per write -- NOT the summed counter of Linux (enough
 * for wake-up use; EFD_SEMAPHORE is accepted and has the same effect).
 */
#ifndef WLPHX_SYS_EVENTFD_H
#define WLPHX_SYS_EVENTFD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 0x0001
#define EFD_CLOEXEC   0x4000 /* = O_CLOEXEC */
#define EFD_NONBLOCK  0x8000 /* = SOCK_NONBLOCK */

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif
