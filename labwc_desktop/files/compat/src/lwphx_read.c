/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: read() of an emulated timerfd. The M6 timers are table entries
 * over a socketpair that never carries data (weston-drm compat/src/wlphx_epoll.c);
 * libwayland never reads them, but foot and fuzzel do: they read the uint64
 * expiration count after epoll reports a timer and render only when it is > 0.
 * Without this every read() was EAGAIN and foot never drew its first frame
 * (m7b-foot, 2026-09-27). Programs link -Wl,--wrap=read.
 */

#include <sys/timerfd.h>
#include <unistd.h>

ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n);

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
	ssize_t r;

	if (wlphx_timer_read(fd, buf, n, &r) != 0) {
		return r;
	}
	return __real_read(fd, buf, n);
}
