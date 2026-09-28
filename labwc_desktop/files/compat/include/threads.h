/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: C11 <threads.h> over pthreads (libphoenix has none; fcft and
 * foot use mutexes, condition variables and threads). compat/src/lwphx_threads.c.
 * Not provided: tss_*, mtx_timedlock, cnd_timedwait, thrd_sleep/yield/detach.
 */
#ifndef LWPHX_THREADS_H
#define LWPHX_THREADS_H

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	thrd_success = 0,
	thrd_busy = 1,
	thrd_error = 2,
	thrd_nomem = 3,
	thrd_timedout = 4
};

enum {
	mtx_plain = 0,
	mtx_recursive = 1,
	mtx_timed = 2
};

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef int (*thrd_start_t)(void *);
typedef pthread_once_t once_flag;
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int thrd_join(thrd_t thr, int *res);
thrd_t thrd_current(void);
int thrd_equal(thrd_t a, thrd_t b);
void thrd_exit(int res) __attribute__((noreturn));

int mtx_init(mtx_t *mtx, int type);
int mtx_lock(mtx_t *mtx);
int mtx_trylock(mtx_t *mtx);
int mtx_unlock(mtx_t *mtx);
void mtx_destroy(mtx_t *mtx);

int cnd_init(cnd_t *cond);
int cnd_signal(cnd_t *cond);
int cnd_broadcast(cnd_t *cond);
int cnd_wait(cnd_t *cond, mtx_t *mtx);
void cnd_destroy(cnd_t *cond);

void call_once(once_flag *flag, void (*func)(void));

#ifdef __cplusplus
}
#endif

#endif
