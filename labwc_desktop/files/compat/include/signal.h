/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: Phoenix-RTOS has no real-time signals. Programs that size
 * per-signal tables with SIGRTMAX (foot) get NSIG, one past the last signal.
 */
#include_next <signal.h>

#ifndef LWPHX_SIGNAL_H
#define LWPHX_SIGNAL_H

#ifndef SIGRTMAX
#define SIGRTMIN NSIG
#define SIGRTMAX NSIG
#endif

#endif
