/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: SO_DOMAIN (Linux's number) for foot's check of a socket
 * passed by a service manager; Phoenix-RTOS getsockopt() refuses it, so such a
 * socket is reported as unusable (foot --server with socket activation only).
 */
#include_next <sys/socket.h>

#ifndef LWPHX_SYS_SOCKET_H
#define LWPHX_SYS_SOCKET_H

#ifndef SO_DOMAIN
#define SO_DOMAIN 39
#endif

#endif
