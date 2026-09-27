/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * mesa-drm compat: libphoenix gap -- flock() is declared in <sys/file.h> but the
 * LOCK_* constants live only in <fcntl.h> (Mesa fossilize_db.c). Fixed on branch
 * gpu-lane/libc-gaps (1f5c7db).
 */
#include_next <sys/file.h>
#include <fcntl.h>
