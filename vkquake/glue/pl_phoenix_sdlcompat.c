/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_sdlcompat.c — Phoenix bodies for the SDL2 threading/path shim that
 * vkQuake's portable engine TUs reference (tasks.c job system, snd_dma audio lock,
 * cfgfile/host/menu pref-path). Net-new vs. quakespasm-port: quakespasm used pthreads
 * directly for its one audio feeder thread, whereas vkQuake's tasks.c is built around
 * SDL2 mutex/cond/semaphore/thread primitives (the SDL2 names; quakedef.h aliases the
 * SDL3 spellings onto them). We map those onto Phoenix native sys/threads.h primitives.
 *
 * Threading model on Phoenix (sys/threads.h, verified against libphoenix's own
 * pthread/semaphore wrappers):
 *   - mutexCreate(handle_t*)            -> recursive-free mutex handle
 *   - condCreate(handle_t*) + condWait(cond, mutex, deadline_us) where deadline_us is an
 *     ABSOLUTE microsecond timestamp (built from gettime()), 0 == wait forever, and the
 *     call returns -ETIME on timeout. (Mirrors libphoenix pthread_cond_timedwait /
 *     semaphoreDown.)
 *   - semaphore_t (mutex+cond+count) with semaphoreDown(s, timeout_REL_us) — RELATIVE
 *     timeout, 0 == forever, -ETIME on timeout — and semaphoreUp(s).
 *   - beginthreadex(start, prio, stack, stacksz, arg, &handle): caller owns the stack;
 *     start is void(*)(void*) and must call endthread() to exit. For vkQuake's worker
 *     threads (detached, live for the whole process) we malloc a stack once and never
 *     free it.
 *
 * The libc gap `copysign` lives here too (libphoenix math/common.c is the upstream home;
 * see the note in the port status doc — editing sources/libphoenix would need a
 * toolchain re-sync to affect this host link, so we provide it locally for now).
 */
#include <sys/threads.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "SDL.h"

/* ------------------------------------------------------------------ mutex --- */

/* SDL mutexes are RECURSIVE (a thread may re-lock a mutex it already holds); the Phoenix
 * `mutexLock` primitive is NOT. vkQuake relies on the SDL semantics (e.g. S_StopAllSounds holds
 * snd_mutex then calls S_ClearBuffer which re-locks it) — without recursion that self-deadlocks
 * (observed: map load hung in phMutexLock <- S_ClearBuffer <- S_StopAllSounds <- Host_Map_f).
 * Emulate recursion with an owner-thread id + recursion count on top of the Phoenix mutex. */
struct SDL_mutex {
	handle_t  h;
	pthread_t owner;
	int       owned;   /* nonzero while held */
	unsigned  count;   /* recursion depth */
};

SDL_mutex *SDL_CreateMutex(void)
{
	SDL_mutex *m = (SDL_mutex *)malloc(sizeof(*m));
	if (m == NULL)
		return NULL;
	if (mutexCreate(&m->h) < 0) {
		free(m);
		return NULL;
	}
	m->owned = 0;
	m->count = 0;
	return m;
}

void SDL_DestroyMutex(SDL_mutex *m)
{
	if (m == NULL)
		return;
	resourceDestroy(m->h);
	free(m);
}

int SDL_LockMutex(SDL_mutex *m)
{
	pthread_t self;
	if (m == NULL)
		return -1;
	self = pthread_self();
	/* Already owned by THIS thread -> recurse (only the owner can observe owned+owner==self;
	 * any other thread sees a different owner and blocks in mutexLock below). */
	if (m->owned && pthread_equal(m->owner, self)) {
		m->count++;
		return 0;
	}
	if (mutexLock(m->h) < 0)
		return -1;
	m->owner = self;
	m->owned = 1;
	m->count = 1;
	return 0;
}

int SDL_UnlockMutex(SDL_mutex *m)
{
	if (m == NULL)
		return -1;
	if (m->count > 1) {
		m->count--;
		return 0;
	}
	m->owned = 0;
	m->count = 0;
	return mutexUnlock(m->h) < 0 ? -1 : 0;
}

/* Internal: the Phoenix condWait releases + reacquires the underlying mutex EXACTLY ONCE, so the
 * caller must hold the SDL mutex non-recursively (count==1). Clear our bookkeeping across the wait
 * (the Phoenix mutex is genuinely released) and restore it after condWait reacquires it. */
static int sdlcompat_cond_wait(handle_t cond, SDL_mutex *m, time_t deadline)
{
	int r;
	m->owned = 0;
	m->count = 0;
	r = condWait(cond, m->h, deadline);
	m->owner = pthread_self();
	m->owned = 1;
	m->count = 1;
	return r;
}

/* ------------------------------------------------------------------- cond --- */

struct SDL_cond {
	handle_t h;
};

SDL_cond *SDL_CreateCond(void)
{
	SDL_cond *c = (SDL_cond *)malloc(sizeof(*c));
	if (c == NULL)
		return NULL;
	if (condCreate(&c->h) < 0) {
		free(c);
		return NULL;
	}
	return c;
}

void SDL_DestroyCond(SDL_cond *c)
{
	if (c == NULL)
		return;
	resourceDestroy(c->h);
	free(c);
}

int SDL_CondSignal(SDL_cond *c)
{
	if (c == NULL)
		return -1;
	return condSignal(c->h) < 0 ? -1 : 0;
}

int SDL_CondBroadcast(SDL_cond *c)
{
	if (c == NULL)
		return -1;
	return condBroadcast(c->h) < 0 ? -1 : 0;
}

int SDL_CondWait(SDL_cond *c, SDL_mutex *m)
{
	if (c == NULL || m == NULL)
		return -1;
	/* deadline 0 == wait forever */
	return sdlcompat_cond_wait(c->h, m, 0) < 0 ? -1 : 0;
}

/*
 * SDL_CondWaitTimeout: 0 on signal, nonzero on timeout (quakedef.h wraps this as
 * SDL_WaitConditionTimeout(...) == 0, and tasks.c does `if (!...)`). condWait wants an
 * ABSOLUTE microsecond deadline; build it from gettime() exactly like libphoenix's own
 * pthread_cond_timedwait, and avoid deadline==0 (== forever) for a real finite wait.
 */
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms)
{
	time_t now = 0, deadline;
	int err;

	if (c == NULL || m == NULL)
		return -1;
	if (ms == SDL_MUTEX_MAXWAIT)
		return sdlcompat_cond_wait(c->h, m, 0) < 0 ? -1 : 0;   /* infinite */

	gettime(&now, NULL);                                   /* microseconds */
	deadline = now + (time_t)ms * 1000;
	if (deadline == 0)
		deadline = 1;                                  /* never the "forever" sentinel */

	err = sdlcompat_cond_wait(c->h, m, deadline);
	if (err == -ETIME)
		return SDL_MUTEX_TIMEDOUT;                     /* timed out */
	return err < 0 ? -1 : 0;                               /* 0 == signaled */
}

/* -------------------------------------------------------------- semaphore --- */

struct SDL_sem {
	semaphore_t s;
};

SDL_sem *SDL_CreateSemaphore(Uint32 initial)
{
	SDL_sem *s = (SDL_sem *)malloc(sizeof(*s));
	if (s == NULL)
		return NULL;
	if (semaphoreCreate(&s->s, initial) < 0) {
		free(s);
		return NULL;
	}
	return s;
}

void SDL_DestroySemaphore(SDL_sem *s)
{
	if (s == NULL)
		return;
	semaphoreDone(&s->s);
	free(s);
}

int SDL_SemPost(SDL_sem *s)
{
	if (s == NULL)
		return -1;
	return semaphoreUp(&s->s) < 0 ? -1 : 0;
}

int SDL_SemWait(SDL_sem *s)
{
	if (s == NULL)
		return -1;
	return semaphoreDown(&s->s, 0) < 0 ? -1 : 0;           /* 0 == block forever */
}

/* 0 on acquire; nonzero if it would block. semaphoreDown has no native trywait, so use
 * the shortest finite RELATIVE timeout (1 us) and treat -ETIME as "would block". */
int SDL_SemTryWait(SDL_sem *s)
{
	int err;
	if (s == NULL)
		return -1;
	err = semaphoreDown(&s->s, 1);
	if (err == -ETIME)
		return SDL_MUTEX_TIMEDOUT;
	return err < 0 ? -1 : 0;
}

int SDL_SemWaitTimeout(SDL_sem *s, Uint32 ms)
{
	int err;
	if (s == NULL)
		return -1;
	if (ms == SDL_MUTEX_MAXWAIT)
		return semaphoreDown(&s->s, 0) < 0 ? -1 : 0;
	err = semaphoreDown(&s->s, (time_t)ms * 1000);         /* RELATIVE microseconds */
	if (err == -ETIME)
		return SDL_MUTEX_TIMEDOUT;
	return err < 0 ? -1 : 0;
}

Uint32 SDL_SemValue(SDL_sem *s)
{
	return s != NULL ? (Uint32)s->s.v : 0;
}

/* ----------------------------------------------------------------- thread --- */

/* vkQuake worker threads are detached and live for the whole process, so a malloc'd
 * stack that is never reclaimed is fine. We trampoline the SDL int(*)(void*) entry
 * through a void(*)(void*) Phoenix entry and discard the int return (SDL detaches them).
 */
/* vkQuake's Task_Worker runs the full render/BSP/lightmap/alias call chains on these
 * shim threads. 512 KiB was too tight for the deepest workloads: a worker overflowed its
 * stack into adjacent heap, corrupting the stdout FILE's bufpos, which then crashed the
 * main thread in a memmove overrun during Con_Printf of a centerprint (owner HW crash
 * 2026-08-22, artifacts/rpi4b-uart/20260822-221322-live-test.log). The main thread itself
 * links a 32 MiB stack; give the workers generous headroom too. HYPOTHESIS-driven — pending
 * Pi re-validation that the crash is gone. */
#define PL_THREAD_STACKSZ (8 * 1024 * 1024)
#define PL_THREAD_PRIO    4

struct SDL_Thread {
	handle_t h;
	void *stack;
	SDL_ThreadFunction fn;
	void *arg;
};

static void pl_thread_trampoline(void *p)
{
	SDL_Thread *t = (SDL_Thread *)p;
	t->fn(t->arg);
	endthread();
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
	SDL_Thread *t;
	(void)name;

	t = (SDL_Thread *)malloc(sizeof(*t));
	if (t == NULL)
		return NULL;
	t->stack = malloc(PL_THREAD_STACKSZ);
	if (t->stack == NULL) {
		free(t);
		return NULL;
	}
	t->fn = fn;
	t->arg = data;
	if (beginthreadex(pl_thread_trampoline, PL_THREAD_PRIO, t->stack,
	                  PL_THREAD_STACKSZ, t, &t->h) < 0) {
		free(t->stack);
		free(t);
		return NULL;
	}
	return t;
}

void SDL_WaitThread(SDL_Thread *t, int *status)
{
	if (t == NULL)
		return;
	threadJoin(t->h, 0);
	if (status != NULL)
		*status = 0;
	free(t->stack);
	free(t);
}

/* Detached: vkQuake never joins workers (tasks.c:439 does
 * SDL_DetachThread(SDL_CreateThread(...))). We must NOT free t here: the just-spawned
 * worker still has to read t->fn/t->arg in pl_thread_trampoline, and on the cpu0-only
 * scheduler it almost certainly has not run yet when main calls Detach — freeing t now
 * is a use-after-free (the trampoline would call through a garbage fn pointer). The
 * trampoline dereferences t exactly once at startup and Task_Worker never returns, so
 * leaking the ~32-byte handle (alongside the deliberately-leaked lifetime stack) is the
 * correct, race-free behaviour. */
void SDL_DetachThread(SDL_Thread *t)
{
	(void)t;
}

/* ------------------------------------------------------------- misc / time --- */

int SDL_GetCPUCount(void)
{
	/* Phoenix Pi4 scheduler is cpu0-only (SMP findings); report 1 logical core. The
	 * main agent should note that tasks.c sizing semaphores at (cores-1) with 1 core
	 * is a runtime concern (Task_Worker count), out of scope for the host link. */
	return 1;
}

void SDL_Delay(Uint32 ms)
{
	usleep((useconds_t)ms * 1000);
}

void SDL_free(void *mem)
{
	free(mem);
}

/*
 * SDL_GetPrefPath: SDL contract is a malloc'd, '/'-terminated, writable user directory
 * (the caller frees it with SDL_free). vkQuake uses it for config.cfg + savegames. On
 * Phoenix we have a single (NFS/dummyfs) rootfs; return "<basedir>/" under /. We keep it
 * org/app-namespaced to match upstream's <pref>/<app>/ layout so save paths stay stable.
 */
char *SDL_GetPrefPath(const char *org, const char *app)
{
	const char *base = "/";
	size_t n;
	char *out;

	if (org == NULL)
		org = "";
	if (app == NULL)
		app = "";

	/* "<base><org>/<app>/" — collapse empty org to avoid a leading "//". */
	n = strlen(base) + strlen(org) + 1 + strlen(app) + 1 + 1;
	out = (char *)malloc(n);
	if (out == NULL)
		return NULL;

	if (org[0] != '\0')
		snprintf(out, n, "%s%s/%s/", base, org, app);
	else
		snprintf(out, n, "%s%s/", base, app);
	return out;
}

/* ------------------------------------------------------------------- libc --- */

/*
 * pthread_mutex_timedlock: absent from libphoenix. Reached transitively via the link
 * closure (not by vkQuake's own .c — no direct caller in external/vkquake). Implement
 * over the existing pthread_mutex_trylock + a short polling sleep until abstime. abstime
 * is an absolute CLOCK_REALTIME timespec per POSIX.
 */
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
	int rc;

	for (;;) {
		rc = pthread_mutex_trylock(mutex);
		if (rc != EBUSY)
			return rc;                             /* 0 == locked, or a real error */

		if (abstime != NULL) {
			struct timespec now;
			clock_gettime(CLOCK_REALTIME, &now);
			if (now.tv_sec > abstime->tv_sec ||
			    (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
				return ETIMEDOUT;
		}
		usleep(1000);
	}
}

/*
 * copysign: libphoenix math/common.c provides fabs but not copysign. Use signbit (both
 * exist) rather than __builtin_copysign (which can self-recurse to this symbol).
 * NOTE TO MAIN AGENT: the upstreamable home is sources/libphoenix/math/common.c (add
 * copysign alongside fabs); doing it there needs a libphoenix rebuild + toolchain
 * re-sync to affect this host link, so it is provided here for now.
 */
double copysign(double x, double y)
{
	double a = fabs(x);
	return signbit(y) ? -a : a;
}
