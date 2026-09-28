/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix has no epoll(7). libwayland's event loop
 * (src/event-loop.c) is written against epoll + timerfd + signalfd, as on Linux;
 * FreeBSD links the same code against epoll-shim (a kqueue emulation). Phoenix has
 * no kqueue either, so compat/src/wlphx_epoll.c emulates the three on top of
 * poll(): level-triggered only (all libwayland uses), EPOLLET/EPOLLONESHOT are
 * refused with EINVAL. The values are Linux's.
 *
 * Link programs with -Wl,--wrap=close: the emulated descriptors are ordinary
 * socket descriptors plus a per-descriptor record, and __wrap_close drops the
 * record when the descriptor number is closed (a recycled number must not keep
 * its old meaning).
 */
#ifndef WLPHX_SYS_EPOLL_H
#define WLPHX_SYS_EPOLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLLIN      0x001u
#define EPOLLPRI     0x002u
#define EPOLLOUT     0x004u
#define EPOLLERR     0x008u
#define EPOLLHUP     0x010u
#define EPOLLRDNORM  0x040u
#define EPOLLRDBAND  0x080u
#define EPOLLWRNORM  0x100u
#define EPOLLWRBAND  0x200u
#define EPOLLMSG     0x400u
#define EPOLLRDHUP   0x2000u
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP  (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 0x4000 /* = O_CLOEXEC on Phoenix; only its presence matters */

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

#endif
