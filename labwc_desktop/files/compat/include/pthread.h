/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: pthread_setname_np() (foot names its render threads).
 * Phoenix-RTOS threads have no names: compat/src/lwphx_misc.c accepts and drops it.
 */
#include_next <pthread.h>

#ifndef LWPHX_PTHREAD_H
#define LWPHX_PTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif
int pthread_setname_np(pthread_t thread, const char *name);
#ifdef __cplusplus
}
#endif

#endif
