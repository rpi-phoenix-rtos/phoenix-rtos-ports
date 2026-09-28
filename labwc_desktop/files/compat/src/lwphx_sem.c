/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: unnamed semaphores over a mutex and a condition variable
 * (see compat/include/semaphore.h). sem_post() signals every time the count is
 * raised (never only on 0 -> 1: that loses wake-ups with several waiters).
 */

#include <errno.h>
#include <limits.h>
#include <semaphore.h>

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
	if (pshared != 0) {
		errno = ENOSYS;
		return -1;
	}
	if (value > (unsigned int)INT_MAX) {
		errno = EINVAL;
		return -1;
	}
	if ((pthread_mutex_init(&sem->lock, NULL) != 0) || (pthread_cond_init(&sem->cond, NULL) != 0)) {
		errno = ENOMEM;
		return -1;
	}
	sem->value = value;
	return 0;
}


int sem_destroy(sem_t *sem)
{
	(void)pthread_cond_destroy(&sem->cond);
	(void)pthread_mutex_destroy(&sem->lock);
	return 0;
}


int sem_wait(sem_t *sem)
{
	pthread_mutex_lock(&sem->lock);
	while (sem->value == 0u) {
		pthread_cond_wait(&sem->cond, &sem->lock);
	}
	sem->value--;
	pthread_mutex_unlock(&sem->lock);
	return 0;
}


int sem_trywait(sem_t *sem)
{
	int rc = 0;

	pthread_mutex_lock(&sem->lock);
	if (sem->value == 0u) {
		rc = -1;
	}
	else {
		sem->value--;
	}
	pthread_mutex_unlock(&sem->lock);
	if (rc < 0) {
		errno = EAGAIN;
	}
	return rc;
}


int sem_post(sem_t *sem)
{
	pthread_mutex_lock(&sem->lock);
	if (sem->value == (unsigned int)INT_MAX) {
		pthread_mutex_unlock(&sem->lock);
		errno = EOVERFLOW;
		return -1;
	}
	sem->value++;
	pthread_cond_signal(&sem->cond);
	pthread_mutex_unlock(&sem->lock);
	return 0;
}


int sem_getvalue(sem_t *sem, int *sval)
{
	pthread_mutex_lock(&sem->lock);
	*sval = (int)sem->value;
	pthread_mutex_unlock(&sem->lock);
	return 0;
}
