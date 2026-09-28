/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: epoll / timerfd / signalfd emulated over poll() for
 * libwayland's event loop (see compat/include/sys/epoll.h for the contract).
 *
 * Every emulated descriptor is one end of an AF_UNIX socketpair (a real,
 * closable, pollable descriptor) plus a record in a per-process table indexed by
 * the descriptor number:
 *   epoll    the interest list; the socket itself never becomes readable
 *   timer    a deadline and a count of expirations not yet read; reported EPOLLIN by
 *            epoll_wait while the count is non-zero. read() of the count (Linux: an
 *            8-byte uint64) is wlphx_timer_read(), for a program that links a
 *            read() wrapper (labwc-drm's compat does: foot reads its timers)
 *   signal   the peer end receives one signalfd_siginfo per caught signal, so
 *            the descriptor is readable exactly when a signal is pending; the
 *            signals are deliverable only inside epoll_wait (see there)
 *   event    eventfd: write() on it (__wrap_write) goes to the peer end, so the
 *            descriptor itself turns readable; read() is the plain socket read
 * epoll_wait() builds a pollfd array from the interest list (timers are not
 * polled, they shorten the timeout), polls, and reports level-triggered events.
 *
 * On Phoenix poll() wakes at once for AF_UNIX sockets and for servers that opt
 * into the kernel's pollNotify (rpi4-kms does); other server-backed descriptors
 * are re-queried on the kernel's poll cycle (M3 G12).
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#define WLPHX_MAX_FDS 1024

enum wlphx_kind { WLPHX_NONE = 0, WLPHX_EPOLL, WLPHX_TIMER, WLPHX_SIGNAL, WLPHX_EVENT };

struct wlphx_item {
	int fd;
	uint32_t events;
	epoll_data_t data;
};

struct wlphx_epoll {
	struct wlphx_item *items;
	int n;
	int cap;
};

struct wlphx_timer {
	clockid_t clock;
	int armed;
	int nonblock;
	struct timespec deadline; /* absolute, on `clock` */
	struct timespec interval;
	uint64_t pending;         /* expirations since the last read()/settime() */
};

struct wlphx_signal {
	sigset_t mask;
	struct sigaction old[NSIG];
};

struct wlphx_slot {
	int kind;
	int peer; /* the other end of the socketpair */
	union {
		struct wlphx_epoll ep;
		struct wlphx_timer tm;
		struct wlphx_signal sg;
	} u;
};

static struct wlphx_slot *slots[WLPHX_MAX_FDS];
static pthread_mutex_t wlphx_lock = PTHREAD_MUTEX_INITIALIZER;

/* signal number -> write end of the signalfd that catches it (read by the handler) */
static volatile int sig_wfd[NSIG];

/* WLPHX_TRACE=1: tagged signal-path lines on stderr (set by the first signalfd()) */
static volatile int wlphx_trace;

int __real_close(int fd);
int __wrap_close(int fd);
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n);


/* One trace line (not for the signal handler, which formats its own). */
static void trace(const char *fmt, int a, int b)
{
	char line[96];
	int n;

	n = snprintf(line, sizeof(line), fmt, a, b);
	if (n > 0) {
		(void)__real_write(2, line, ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1);
	}
}


/* dst |= src (sigorset() is a GNU extension) */
static void sigset_merge(sigset_t *dst, const sigset_t *src)
{
	int sig;

	for (sig = 1; sig < NSIG; sig++) {
		if (sigismember(src, sig) == 1) {
			sigaddset(dst, sig);
		}
	}
}


static int slot_new(int kind, int flags_cloexec, int *out_fd)
{
	int sv[2];
	struct wlphx_slot *s;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		return -1;
	}
	if ((sv[0] >= WLPHX_MAX_FDS) || (sv[0] < 0)) {
		__real_close(sv[0]);
		__real_close(sv[1]);
		errno = EMFILE;
		return -1;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		__real_close(sv[0]);
		__real_close(sv[1]);
		errno = ENOMEM;
		return -1;
	}
	if (flags_cloexec) {
		(void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	}
	(void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(sv[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(sv[1], F_SETFL, O_NONBLOCK);
	s->kind = kind;
	s->peer = sv[1];
	pthread_mutex_lock(&wlphx_lock);
	slots[sv[0]] = s;
	pthread_mutex_unlock(&wlphx_lock);
	*out_fd = sv[0];
	return 0;
}


static struct wlphx_slot *slot_get(int fd, int kind)
{
	if ((fd < 0) || (fd >= WLPHX_MAX_FDS) || (slots[fd] == NULL) || (slots[fd]->kind != kind)) {
		return NULL;
	}
	return slots[fd];
}


/* ------------------------------------------------------------------------- */
/* time helpers                                                              */

static int ts_cmp(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec) {
		return (a->tv_sec < b->tv_sec) ? -1 : 1;
	}
	return (a->tv_nsec < b->tv_nsec) ? -1 : (a->tv_nsec > b->tv_nsec) ? 1 : 0;
}


static void ts_add(struct timespec *r, const struct timespec *a, const struct timespec *b)
{
	r->tv_sec = a->tv_sec + b->tv_sec;
	r->tv_nsec = a->tv_nsec + b->tv_nsec;
	if (r->tv_nsec >= 1000000000L) {
		r->tv_sec++;
		r->tv_nsec -= 1000000000L;
	}
}


static void ts_sub(struct timespec *r, const struct timespec *a, const struct timespec *b)
{
	r->tv_sec = a->tv_sec - b->tv_sec;
	r->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (r->tv_nsec < 0) {
		r->tv_sec--;
		r->tv_nsec += 1000000000L;
	}
}


static int ts_is_zero(const struct timespec *t)
{
	return (t->tv_sec == 0) && (t->tv_nsec == 0);
}


/* ------------------------------------------------------------------------- */
/* epoll                                                                     */

int epoll_create1(int flags)
{
	int fd;

	if ((flags & ~EPOLL_CLOEXEC) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (slot_new(WLPHX_EPOLL, (flags & EPOLL_CLOEXEC) != 0, &fd) < 0) {
		return -1;
	}
	return fd;
}


int epoll_create(int size)
{
	if (size <= 0) {
		errno = EINVAL;
		return -1;
	}
	return epoll_create1(0);
}


int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	struct wlphx_slot *s;
	struct wlphx_epoll *ep;
	int i, ret = 0;

	pthread_mutex_lock(&wlphx_lock);
	s = slot_get(epfd, WLPHX_EPOLL);
	if ((s == NULL) || (fd < 0) || (fd == epfd)) {
		pthread_mutex_unlock(&wlphx_lock);
		errno = (s == NULL) ? EBADF : EINVAL;
		return -1;
	}
	if ((op != EPOLL_CTL_DEL) && ((event == NULL) || ((event->events & (EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE)) != 0))) {
		/* level-triggered only: libwayland never asks for anything else */
		pthread_mutex_unlock(&wlphx_lock);
		errno = EINVAL;
		return -1;
	}
	ep = &s->u.ep;
	for (i = 0; i < ep->n; i++) {
		if (ep->items[i].fd == fd) {
			break;
		}
	}
	switch (op) {
		case EPOLL_CTL_ADD:
			if (i < ep->n) {
				errno = EEXIST;
				ret = -1;
				break;
			}
			if (ep->n == ep->cap) {
				int ncap = (ep->cap != 0) ? (2 * ep->cap) : 16;
				struct wlphx_item *n = realloc(ep->items, (size_t)ncap * sizeof(*n));
				if (n == NULL) {
					errno = ENOMEM;
					ret = -1;
					break;
				}
				ep->items = n;
				ep->cap = ncap;
			}
			ep->items[ep->n].fd = fd;
			ep->items[ep->n].events = event->events;
			ep->items[ep->n].data = event->data;
			ep->n++;
			break;
		case EPOLL_CTL_MOD:
			if (i == ep->n) {
				errno = ENOENT;
				ret = -1;
				break;
			}
			ep->items[i].events = event->events;
			ep->items[i].data = event->data;
			break;
		case EPOLL_CTL_DEL:
			if (i == ep->n) {
				errno = ENOENT;
				ret = -1;
				break;
			}
			ep->items[i] = ep->items[ep->n - 1];
			ep->n--;
			break;
		default:
			errno = EINVAL;
			ret = -1;
			break;
	}
	pthread_mutex_unlock(&wlphx_lock);
	return ret;
}


static short to_poll(uint32_t ev)
{
	short p = 0;

	if ((ev & (EPOLLIN | EPOLLRDNORM)) != 0) {
		p |= POLLIN;
	}
	if ((ev & EPOLLPRI) != 0) {
		p |= POLLPRI;
	}
	if ((ev & (EPOLLOUT | EPOLLWRNORM)) != 0) {
		p |= POLLOUT;
	}
	return p;
}


static uint32_t from_poll(short rev, uint32_t want)
{
	uint32_t e = 0;

	if ((rev & POLLIN) != 0) {
		e |= want & (EPOLLIN | EPOLLRDNORM);
	}
	if ((rev & POLLPRI) != 0) {
		e |= want & EPOLLPRI;
	}
	if ((rev & POLLOUT) != 0) {
		e |= want & (EPOLLOUT | EPOLLWRNORM);
	}
	if ((rev & POLLHUP) != 0) {
		e |= EPOLLHUP;
	}
	if ((rev & (POLLERR | POLLNVAL)) != 0) {
		e |= EPOLLERR;
	}
	return e;
}


/* Account expirations up to now (under the lock): a one-shot timer fires once and
 * disarms; a periodic one adds every interval that has passed and steps its deadline
 * past now. Returns the expirations not yet read (timerfd semantics: the descriptor
 * is readable while that count is non-zero). */
static uint64_t timer_update(struct wlphx_timer *t)
{
	struct timespec now;

	if (t->armed) {
		clock_gettime(t->clock, &now);
		if (ts_cmp(&now, &t->deadline) >= 0) {
			if (ts_is_zero(&t->interval)) {
				t->pending++;
				t->armed = 0;
			}
			else {
				while (ts_cmp(&now, &t->deadline) >= 0) {
					ts_add(&t->deadline, &t->deadline, &t->interval);
					t->pending++;
				}
			}
		}
	}
	return t->pending;
}


static int timer_expired(struct wlphx_timer *t)
{
	return timer_update(t) != 0u;
}


/* Milliseconds until the timer is readable (0 = now), or -1 when it never will be. */
static int timer_ms_left(struct wlphx_timer *t)
{
	struct timespec now, d;
	long long ms;

	if (timer_update(t) != 0u) {
		return 0;
	}
	if (!t->armed) {
		return -1;
	}
	clock_gettime(t->clock, &now);
	if (ts_cmp(&now, &t->deadline) >= 0) {
		return 0;
	}
	ts_sub(&d, &t->deadline, &now);
	ms = (long long)d.tv_sec * 1000 + (d.tv_nsec + 999999L) / 1000000L; /* round up: never early */
	return (ms > INT_MAX) ? INT_MAX : (int)ms;
}


int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	struct wlphx_slot *s;
	struct wlphx_item *items = NULL;
	struct pollfd *pfd = NULL;
	int *pidx = NULL;
	int n, i, np, count, wait_ms, left, r, e, nsig;
	struct timespec start, now, el;
	sigset_t sigs, saved;

	if ((events == NULL) || (maxevents <= 0)) {
		errno = EINVAL;
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		/* Snapshot the interest list: epoll_ctl may run on another thread while we poll. */
		pthread_mutex_lock(&wlphx_lock);
		s = slot_get(epfd, WLPHX_EPOLL);
		if (s == NULL) {
			pthread_mutex_unlock(&wlphx_lock);
			free(items);
			free(pfd);
			free(pidx);
			errno = EBADF;
			return -1;
		}
		n = s->u.ep.n;
		free(items);
		free(pfd);
		free(pidx);
		items = malloc(((size_t)n + 1) * sizeof(*items));
		pfd = malloc(((size_t)n + 1) * sizeof(*pfd));
		pidx = malloc(((size_t)n + 1) * sizeof(*pidx));
		if ((items == NULL) || (pfd == NULL) || (pidx == NULL)) {
			pthread_mutex_unlock(&wlphx_lock);
			free(items);
			free(pfd);
			free(pidx);
			errno = ENOMEM;
			return -1;
		}
		memcpy(items, s->u.ep.items, (size_t)n * sizeof(*items));

		/* Timers are not polled: they bound the wait. The epoll descriptor itself (a
		 * socket that never turns readable) is always in the set, so a list of only
		 * timers still sleeps in poll(): Phoenix's poll() with no descriptors returns
		 * at once for an infinite timeout. */
		wait_ms = timeout;
		count = 0;
		nsig = 0;
		sigemptyset(&sigs);
		pfd[0].fd = epfd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		pidx[0] = -1;
		np = 1;
		for (i = 0; i < n; i++) {
			struct wlphx_slot *t = slot_get(items[i].fd, WLPHX_TIMER);
			struct wlphx_slot *sg = slot_get(items[i].fd, WLPHX_SIGNAL);
			if (t != NULL) {
				left = timer_ms_left(&t->u.tm);
				if ((left >= 0) && ((wait_ms < 0) || (left < wait_ms))) {
					wait_ms = left;
				}
				continue;
			}
			if (sg != NULL) {
				sigset_merge(&sigs, &sg->u.sg.mask);
				nsig++;
			}
			pfd[np].fd = items[i].fd;
			pfd[np].events = to_poll(items[i].events);
			pfd[np].revents = 0;
			pidx[np] = i;
			np++;
		}
		pthread_mutex_unlock(&wlphx_lock);

		/* The signals of our signal descriptors are blocked by the caller, as a real
		 * signalfd wants (libwayland blocks them right after signalfd(), in every
		 * thread it goes on to create too), so on their own they would stay pending
		 * forever and never reach wlphx_sig_handler. Admit them while this thread
		 * sleeps: one already pending is taken on the unblock itself, one sent during
		 * poll() interrupts it, one sent after the re-block waits for the next call.
		 * No wake-up is lost, and the handler never interrupts code outside
		 * epoll_wait(). */
		if (nsig > 0) {
			(void)pthread_sigmask(SIG_UNBLOCK, &sigs, &saved);
		}
		r = poll(pfd, (nfds_t)np, wait_ms);
		e = errno;
		if (nsig > 0) {
			(void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
		}
		if (r < 0) {
			if ((e == EINTR) && wlphx_trace) {
				trace("WLPHX epoll_wait eintr fd=%d signal_fds=%d\n", epfd, nsig);
			}
			free(items);
			free(pfd);
			free(pidx);
			errno = e;
			return -1; /* EINTR included: libwayland retries, the socket is then readable */
		}

		for (i = 1; (i < np) && (count < maxevents); i++) {
			uint32_t ev = from_poll(pfd[i].revents, items[pidx[i]].events);
			if (ev != 0) {
				events[count].events = ev;
				events[count].data = items[pidx[i]].data;
				count++;
				if (wlphx_trace && (nsig > 0)) {
					pthread_mutex_lock(&wlphx_lock);
					if (slot_get(pfd[i].fd, WLPHX_SIGNAL) != NULL) {
						trace("WLPHX signalfd dispatched fd=%d revents=0x%x\n", pfd[i].fd, pfd[i].revents);
					}
					pthread_mutex_unlock(&wlphx_lock);
				}
			}
		}
		pthread_mutex_lock(&wlphx_lock);
		for (i = 0; (i < n) && (count < maxevents); i++) {
			struct wlphx_slot *t = slot_get(items[i].fd, WLPHX_TIMER);
			if ((t != NULL) && ((items[i].events & EPOLLIN) != 0) && timer_expired(&t->u.tm)) {
				/* level-triggered: readable until read() takes the count or
				 * timerfd_settime() resets it (libwayland re-arms, never reads) */
				events[count].events = EPOLLIN;
				events[count].data = items[i].data;
				count++;
			}
		}
		pthread_mutex_unlock(&wlphx_lock);

		if ((count > 0) || (timeout == 0)) {
			break;
		}
		if (timeout > 0) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			ts_sub(&el, &now, &start);
			if ((long long)el.tv_sec * 1000 + el.tv_nsec / 1000000L >= timeout) {
				break;
			}
		}
		/* woke for a timer that has not quite expired (poll's ms granularity) or a
		 * spurious wake-up: go round again with the remaining time */
	}
	free(items);
	free(pfd);
	free(pidx);
	return count;
}


/* ------------------------------------------------------------------------- */
/* timerfd                                                                   */

int timerfd_create(int clockid, int flags)
{
	int fd;
	struct wlphx_slot *s;

	if ((clockid != (int)CLOCK_MONOTONIC) && (clockid != (int)CLOCK_REALTIME)) {
		errno = EINVAL;
		return -1;
	}
	if ((flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (slot_new(WLPHX_TIMER, (flags & TFD_CLOEXEC) != 0, &fd) < 0) {
		return -1;
	}
	s = slots[fd];
	s->u.tm.clock = (clockid_t)clockid;
	s->u.tm.nonblock = ((flags & TFD_NONBLOCK) != 0);
	return fd;
}


int timerfd_settime(int fd, int flags, const struct itimerspec *nv, struct itimerspec *ov)
{
	struct wlphx_slot *s;
	struct timespec now;

	if (nv == NULL) {
		errno = EFAULT;
		return -1;
	}
	pthread_mutex_lock(&wlphx_lock);
	s = slot_get(fd, WLPHX_TIMER);
	if (s == NULL) {
		pthread_mutex_unlock(&wlphx_lock);
		errno = EBADF;
		return -1;
	}
	clock_gettime(s->u.tm.clock, &now);
	if (ov != NULL) {
		ov->it_interval = s->u.tm.interval;
		if (s->u.tm.armed && (ts_cmp(&s->u.tm.deadline, &now) > 0)) {
			ts_sub(&ov->it_value, &s->u.tm.deadline, &now);
		}
		else {
			ov->it_value.tv_sec = 0;
			ov->it_value.tv_nsec = 0;
		}
	}
	s->u.tm.interval = nv->it_interval;
	s->u.tm.pending = 0u; /* arming or disarming discards unread expirations */
	if (ts_is_zero(&nv->it_value)) {
		s->u.tm.armed = 0;
	}
	else {
		s->u.tm.armed = 1;
		if ((flags & TFD_TIMER_ABSTIME) != 0) {
			s->u.tm.deadline = nv->it_value;
		}
		else {
			ts_add(&s->u.tm.deadline, &now, &nv->it_value);
		}
	}
	pthread_mutex_unlock(&wlphx_lock);
	return 0;
}


int timerfd_gettime(int fd, struct itimerspec *cv)
{
	struct wlphx_slot *s;
	struct timespec now;

	pthread_mutex_lock(&wlphx_lock);
	s = slot_get(fd, WLPHX_TIMER);
	if (s == NULL) {
		pthread_mutex_unlock(&wlphx_lock);
		errno = EBADF;
		return -1;
	}
	memset(cv, 0, sizeof(*cv));
	cv->it_interval = s->u.tm.interval;
	clock_gettime(s->u.tm.clock, &now);
	if (s->u.tm.armed && (ts_cmp(&s->u.tm.deadline, &now) > 0)) {
		ts_sub(&cv->it_value, &s->u.tm.deadline, &now);
	}
	pthread_mutex_unlock(&wlphx_lock);
	return 0;
}


/* read() of a timer descriptor, for a read() wrapper: 1 when `fd` is an emulated
 * timer (the result is in *ret, errno set when it is -1), 0 otherwise. Linux
 * semantics: the buffer must hold a uint64_t; the call returns the expirations since
 * the last read and resets the count; with none, EAGAIN on a TFD_NONBLOCK timer,
 * otherwise it sleeps until the next expiration (a disarmed blocking timer blocks
 * forever, as on Linux). */
int wlphx_timer_read(int fd, void *buf, size_t n, ssize_t *ret)
{
	struct wlphx_slot *s;
	uint64_t v;
	int ms;

	/* every read() of the program comes here: leave non-emulated descriptors without
	 * taking the lock (slots[] is set before an emulated fd is ever returned) */
	if ((fd < 0) || (fd >= WLPHX_MAX_FDS) || (slots[fd] == NULL)) {
		return 0;
	}
	for (;;) {
		pthread_mutex_lock(&wlphx_lock);
		s = slot_get(fd, WLPHX_TIMER);
		if (s == NULL) {
			pthread_mutex_unlock(&wlphx_lock);
			return 0;
		}
		if (n < sizeof(v)) {
			pthread_mutex_unlock(&wlphx_lock);
			errno = EINVAL;
			*ret = -1;
			return 1;
		}
		v = timer_update(&s->u.tm);
		if (v != 0u) {
			s->u.tm.pending = 0u;
			pthread_mutex_unlock(&wlphx_lock);
			memcpy(buf, &v, sizeof(v));
			*ret = (ssize_t)sizeof(v);
			return 1;
		}
		ms = timer_ms_left(&s->u.tm);
		if (s->u.tm.nonblock) {
			pthread_mutex_unlock(&wlphx_lock);
			errno = EAGAIN;
			*ret = -1;
			return 1;
		}
		pthread_mutex_unlock(&wlphx_lock);
		{
			/* not poll(NULL, 0, ms): Phoenix's poll() without descriptors returns at once */
			struct timespec d;
			int w = (ms < 0) ? 1000 : ((ms == 0) ? 1 : ms);
			d.tv_sec = w / 1000;
			d.tv_nsec = (long)(w % 1000) * 1000000L;
			(void)nanosleep(&d, NULL);
		}
	}
}


/* ------------------------------------------------------------------------- */
/* signalfd                                                                  */

static void wlphx_sig_handler(int sig)
{
	struct signalfd_siginfo si;
	int saved = errno;
	int wfd;

	if ((sig <= 0) || (sig >= NSIG)) {
		return;
	}
	if (wlphx_trace) {
		/* "WLPHX sig=<n> caught\n", formatted by hand: only write(2) is async-signal-safe */
		char line[32] = "WLPHX sig=";
		size_t k = 10;
		if (sig >= 10) {
			line[k++] = (char)('0' + sig / 10);
		}
		line[k++] = (char)('0' + sig % 10);
		memcpy(&line[k], " caught\n", 8);
		(void)__real_write(2, line, k + 8);
	}
	wfd = sig_wfd[sig];
	if (wfd >= 0) {
		memset(&si, 0, sizeof(si));
		si.ssi_signo = (uint32_t)sig;
		(void)__real_write(wfd, &si, sizeof(si)); /* async-signal-safe; a full socket drops it */
	}
	errno = saved;
}


int signalfd(int fd, const sigset_t *mask, int flags)
{
	struct wlphx_slot *s;
	struct sigaction sa;
	int sig, nfd;
	static int init;

	if ((mask == NULL) || ((flags & ~(SFD_CLOEXEC | SFD_NONBLOCK)) != 0)) {
		errno = EINVAL;
		return -1;
	}
	if (fd != -1) {
		/* changing the mask of an existing signalfd: libwayland never does */
		errno = EINVAL;
		return -1;
	}
	if (!init) {
		const char *t = getenv("WLPHX_TRACE");
		wlphx_trace = (t != NULL) && (t[0] != '\0') && (strcmp(t, "0") != 0);
		for (sig = 0; sig < NSIG; sig++) {
			sig_wfd[sig] = -1;
		}
		init = 1;
	}
	if (slot_new(WLPHX_SIGNAL, (flags & SFD_CLOEXEC) != 0, &nfd) < 0) {
		return -1;
	}
	s = slots[nfd];
	s->u.sg.mask = *mask;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = wlphx_sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	for (sig = 1; sig < NSIG; sig++) {
		if (sigismember(mask, sig) == 1) {
			sig_wfd[sig] = s->peer;
			(void)sigaction(sig, &sa, &s->u.sg.old[sig]);
			if (wlphx_trace) {
				trace("WLPHX signalfd fd=%d sig=%d\n", nfd, sig);
			}
		}
	}
	/* The signal mask is the caller's: it blocks these signals (before or after this
	 * call -- libwayland does it after) and epoll_wait() admits them while it waits.
	 * Unblocking them here would not stick: libwayland's SIG_BLOCK right after this
	 * call undid it, and SIGTERM then stayed pending in every thread (M6 §14). */
	return nfd;
}


/* ------------------------------------------------------------------------- */
/* eventfd                                                                   */

int eventfd(unsigned int initval, int flags)
{
	int fd;
	eventfd_t v = initval;

	if ((flags & ~(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (slot_new(WLPHX_EVENT, (flags & EFD_CLOEXEC) != 0, &fd) < 0) {
		return -1;
	}
	if ((flags & EFD_NONBLOCK) == 0) {
		(void)fcntl(fd, F_SETFL, 0);
	}
	if ((initval != 0u) && (__wrap_write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))) {
		__wrap_close(fd);
		return -1;
	}
	return fd;
}


int eventfd_read(int fd, eventfd_t *value)
{
	return (read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value)) ? 0 : -1;
}


int eventfd_write(int fd, eventfd_t value)
{
	return (write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) ? 0 : -1;
}


ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
	int peer = -1;

	if ((fd >= 0) && (fd < WLPHX_MAX_FDS) && (slots[fd] != NULL)) {
		pthread_mutex_lock(&wlphx_lock);
		if ((slots[fd] != NULL) && (slots[fd]->kind == WLPHX_EVENT)) {
			peer = slots[fd]->peer;
		}
		pthread_mutex_unlock(&wlphx_lock);
	}
	if (peer >= 0) {
		if (n != sizeof(eventfd_t)) {
			errno = EINVAL;
			return -1;
		}
		return __real_write(peer, buf, n);
	}
	return __real_write(fd, buf, n);
}


/* ------------------------------------------------------------------------- */
/* ppoll                                                                     */

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo, const sigset_t *sigmask)
{
	sigset_t old;
	int ms = -1, r, e;

	if (tmo != NULL) {
		long long v = (long long)tmo->tv_sec * 1000 + (tmo->tv_nsec + 999999L) / 1000000L;
		ms = (v > INT_MAX) ? INT_MAX : (int)v;
	}
	if (sigmask != NULL) {
		(void)pthread_sigmask(SIG_SETMASK, sigmask, &old);
	}
	r = poll(fds, nfds, ms);
	e = errno;
	if (sigmask != NULL) {
		(void)pthread_sigmask(SIG_SETMASK, &old, NULL);
	}
	errno = e;
	return r;
}


/* ------------------------------------------------------------------------- */
/* close                                                                     */

int __wrap_close(int fd)
{
	struct wlphx_slot *s = NULL;
	int sig;

	if ((fd >= 0) && (fd < WLPHX_MAX_FDS)) {
		pthread_mutex_lock(&wlphx_lock);
		s = slots[fd];
		slots[fd] = NULL;
		pthread_mutex_unlock(&wlphx_lock);
	}
	if (s != NULL) {
		if (s->kind == WLPHX_SIGNAL) {
			for (sig = 1; sig < NSIG; sig++) {
				if ((sigismember(&s->u.sg.mask, sig) == 1) && (sig_wfd[sig] == s->peer)) {
					sig_wfd[sig] = -1;
					(void)sigaction(sig, &s->u.sg.old[sig], NULL);
				}
			}
		}
		else if (s->kind == WLPHX_EPOLL) {
			free(s->u.ep.items);
		}
		__real_close(s->peer);
		free(s);
	}
	return __real_close(fd);
}
