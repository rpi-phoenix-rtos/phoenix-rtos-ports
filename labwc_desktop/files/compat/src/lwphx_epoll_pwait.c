/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: epoll_pwait() = epoll_wait() with the signal mask replaced
 * around it (foot's event loop keeps its signals blocked and admits them only
 * while it waits). Not atomic, like the M6 ppoll() stand-in: a signal arriving
 * after the mask is restored is seen at the next wake-up.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask)
{
	sigset_t old;
	int r, e;

	if ((sigmask != NULL) && (pthread_sigmask(SIG_SETMASK, sigmask, &old) != 0)) {
		return -1;
	}
	r = epoll_wait(epfd, events, maxevents, timeout);
	e = errno;
	if (sigmask != NULL) {
		(void)pthread_sigmask(SIG_SETMASK, &old, NULL);
	}
	errno = e;
	return r;
}
