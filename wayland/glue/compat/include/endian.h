/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: libphoenix <endian.h> defines only the glibc-internal
 * __BYTE_ORDER/__LITTLE_ENDIAN/__BIG_ENDIAN. The BSD/glibc public names
 * (BYTE_ORDER, LITTLE_ENDIAN, BIG_ENDIAN) are tested by Weston's GL renderer;
 * undefined, "BYTE_ORDER == BIG_ENDIAN" reads 0 == 0, i.e. big endian.
 */
#include_next <endian.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER__
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
