/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: timerfd emulation (see sys/epoll.h). An emulated timer is
 * only observable through epoll_wait() of the same process (EPOLLIN while
 * expired and not re-armed) -- exactly how libwayland's timer heap uses it: it
 * never read()s the descriptor, it re-arms or disarms it after each dispatch.
 * read() on an emulated timer returns EAGAIN.
 */
#ifndef WLPHX_SYS_TIMERFD_H
#define WLPHX_SYS_TIMERFD_H

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libphoenix gap: <time.h> has no struct itimerspec (POSIX timers are absent). */
#ifndef WLPHX_HAVE_ITIMERSPEC
#define WLPHX_HAVE_ITIMERSPEC
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
#endif

#define TFD_TIMER_ABSTIME       (1 << 0)
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#define TFD_CLOEXEC             0x4000 /* = O_CLOEXEC */
#define TFD_NONBLOCK            0x8000 /* = SOCK_NONBLOCK; read() of an unexpired timer: EAGAIN */

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
/* read() of a timer (the uint64 expiration count), for a read() wrapper: returns 1 and
 * the result in *ret when fd is an emulated timer, 0 otherwise (wlphx_epoll.c) */
int wlphx_timer_read(int fd, void *buf, size_t n, ssize_t *ret);

#ifdef __cplusplus
}
#endif

#endif
