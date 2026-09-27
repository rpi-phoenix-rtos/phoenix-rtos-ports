/*
 * vkquake-drm: <execinfo.h> for Phoenix-RTOS, which has none (libphoenix gap). vkQuake's
 * sys_sdl_unix.c uses it only for the stack trace it appends to a Sys_Error report; with
 * zero frames that report says nothing more, as the vkquake port's Sys_StackTrace() does.
 * On the include path of vkquake-drm's engine TUs only (-idirafter, so a real one wins:
 * libphoenix branch feat/ipv6mreq-execinfo adds <execinfo.h>; delete this file after that
 * merge -- vkquake-drm is built -fno-omit-frame-pointer, so the real one walks every frame).
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VKQDRM_EXECINFO_H
#define VKQDRM_EXECINFO_H

#include <stddef.h>

static inline int backtrace(void **buffer, int size)
{
	(void)buffer;
	(void)size;
	return 0;
}

static inline char **backtrace_symbols(void *const *buffer, int size)
{
	(void)buffer;
	(void)size;
	return NULL;
}

#endif
