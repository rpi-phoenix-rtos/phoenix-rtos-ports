/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * <semaphore.h>: unnamed POSIX semaphores over a pthread mutex + condition variable.
 *
 * TODO(browser-B1): libphoenix has no <semaphore.h> (docs/research/2026-10-01-web-browser-options.md
 * section 2e). Delete this file and phx_sem_* in ../phoenix-jsc-compat.c when it does.
 *
 * WTF uses one semaphore, in Thread::suspend()/resume() (wtf/posix/ThreadingPOSIX.cpp), where
 * sem_post() runs in a signal handler. This sem_post() takes a mutex, so it is NOT
 * async-signal-safe. That path runs only when a thread suspends ANOTHER thread (conservative scan
 * of a second thread sharing one VM, the sampling profiler); the jsc shell with concurrent GC off
 * and the sampling profiler compiled out does not reach it. A real implementation (atomic counter
 * + futex-like wait) belongs in libphoenix together with B4's SA_SIGINFO work.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHOENIX_JSC_COMPAT_SEMAPHORE_H
#define PHOENIX_JSC_COMPAT_SEMAPHORE_H

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned int value;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_timedwait(sem_t *sem, const struct timespec *abstime);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);

#ifdef __cplusplus
}
#endif

#endif
