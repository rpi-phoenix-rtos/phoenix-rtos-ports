/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * xfce-wayland compat: daemon() (libxfce4ui's xfce_spawn() detaches non-child
 * processes with it; compat/src/xfphx_misc.c).
 */
#include_next <unistd.h>

#ifndef XFPHX_UNISTD_H
#define XFPHX_UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif
int daemon(int nochdir, int noclose);
#ifdef __cplusplus
}
#endif

#endif
