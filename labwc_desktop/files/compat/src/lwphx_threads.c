/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: C11 threads over pthreads (see compat/include/threads.h).
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

struct lwphx_thrd_start {
	thrd_start_t func;
	void *arg;
};


static int lwphx_thrd_rc(int err)
{
	return (err == 0) ? thrd_success : ((err == ENOMEM) ? thrd_nomem : ((err == EBUSY) ? thrd_busy : thrd_error));
}


static void *lwphx_thrd_trampoline(void *p)
{
	struct lwphx_thrd_start s = *(struct lwphx_thrd_start *)p;

	free(p);
	return (void *)(intptr_t)s.func(s.arg);
}


int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
	struct lwphx_thrd_start *s = malloc(sizeof(*s));
	int err;

	if (s == NULL) {
		return thrd_nomem;
	}
	s->func = func;
	s->arg = arg;
	err = pthread_create(thr, NULL, lwphx_thrd_trampoline, s);
	if (err != 0) {
		free(s);
	}
	return lwphx_thrd_rc(err);
}


int thrd_join(thrd_t thr, int *res)
{
	void *r;
	int err = pthread_join(thr, &r);

	if ((err == 0) && (res != NULL)) {
		*res = (int)(intptr_t)r;
	}
	return lwphx_thrd_rc(err);
}


thrd_t thrd_current(void)
{
	return pthread_self();
}


int thrd_equal(thrd_t a, thrd_t b)
{
	return pthread_equal(a, b);
}


void thrd_exit(int res)
{
	pthread_exit((void *)(intptr_t)res);
	__builtin_unreachable(); /* libphoenix does not declare pthread_exit() noreturn */
}


int mtx_init(mtx_t *mtx, int type)
{
	pthread_mutexattr_t a;
	int err;

	if ((type & ~(mtx_recursive | mtx_timed)) != 0) {
		return thrd_error;
	}
	if ((type & mtx_recursive) == 0) {
		return lwphx_thrd_rc(pthread_mutex_init(mtx, NULL));
	}
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	err = pthread_mutex_init(mtx, &a);
	pthread_mutexattr_destroy(&a);
	return lwphx_thrd_rc(err);
}


int mtx_lock(mtx_t *mtx)
{
	return lwphx_thrd_rc(pthread_mutex_lock(mtx));
}


int mtx_trylock(mtx_t *mtx)
{
	return lwphx_thrd_rc(pthread_mutex_trylock(mtx));
}


int mtx_unlock(mtx_t *mtx)
{
	return lwphx_thrd_rc(pthread_mutex_unlock(mtx));
}


void mtx_destroy(mtx_t *mtx)
{
	(void)pthread_mutex_destroy(mtx);
}


int cnd_init(cnd_t *cond)
{
	return lwphx_thrd_rc(pthread_cond_init(cond, NULL));
}


int cnd_signal(cnd_t *cond)
{
	return lwphx_thrd_rc(pthread_cond_signal(cond));
}


int cnd_broadcast(cnd_t *cond)
{
	return lwphx_thrd_rc(pthread_cond_broadcast(cond));
}


int cnd_wait(cnd_t *cond, mtx_t *mtx)
{
	return lwphx_thrd_rc(pthread_cond_wait(cond, mtx));
}


void cnd_destroy(cnd_t *cond)
{
	(void)pthread_cond_destroy(cond);
}


void call_once(once_flag *flag, void (*func)(void))
{
	(void)pthread_once(flag, func);
}
