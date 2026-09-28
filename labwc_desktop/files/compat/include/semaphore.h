/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: unnamed POSIX semaphores (libphoenix has no <semaphore.h>;
 * foot's render workers use sem_init/sem_wait/sem_post). A mutex, a condition
 * variable and a count: compat/src/lwphx_sem.c. Process-shared semaphores
 * (pshared != 0) and named ones are not supported.
 */
#ifndef LWPHX_SEMAPHORE_H
#define LWPHX_SEMAPHORE_H

#include <pthread.h>
#include <time.h>

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned int value;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

#ifdef __cplusplus
extern "C" {
#endif
int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *sval);
#ifdef __cplusplus
}
#endif

#endif
