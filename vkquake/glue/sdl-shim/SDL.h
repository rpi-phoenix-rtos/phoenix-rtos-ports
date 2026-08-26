/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for vkQuake (vkQuake is Copyright (C) id
 * Software, Inc. and the vkQuake developers, GPL-2.0-or-later). It implements
 * the vkQuake platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * SDL2-compat shim for the vkQuake -> Phoenix (V3DV) port.
 *
 * Larger than the quakespasm-port shim because vkQuake's portable engine TUs reference
 * more of SDL through quakedef.h's header chain:
 *   - SDL2 threading types (SDL_mutex / SDL_cond / SDL_sem / SDL_Thread) used by tasks.c
 *     and the snd_dma audio-lock path (quakedef.h provides SDL3->SDL2 compat #defines, so
 *     the SDL2 base names must exist as types here);
 *   - SDL_GetPrefPath (userdir resolution in host.c/cmd.c/menu.c/cfgfile.c/host_cmd.c);
 *   - SDL_RWsize + the SDL_RWops file-I/O surface (common.c localization loader);
 *   - SDL_PRIs64 / SDL_PRIu64 print macros (wad.c, common.c).
 *
 * The real SDL platform surface (window / Vulkan surface / input / audio / timer) is NOT
 * emulated here: those live in the EXCLUDED SDL platform TUs (gl_vidsdl, in_sdl*, snd_sdl*,
 * sys_sdl*, main_sdl), which are replaced wholesale by Phoenix platform shims
 * (pl_phoenix_*). This header only satisfies what leaks into PORTABLE engine TUs. The
 * function-bodied threading/path/RW shims map onto Phoenix sys/threads.h + stdio in
 * pl_phoenix_sdlcompat.c (STAGE 2 — link). Grow as compile errors demand.
 */
#ifndef PHOENIX_VKQ_SDL_SHIM_H
#define PHOENIX_VKQ_SDL_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <inttypes.h>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;
typedef int      SDL_bool;

#define SDL_FALSE 0
#define SDL_TRUE  1

#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_BYTEORDER  SDL_LIL_ENDIAN   /* aarch64 is little-endian */

#define SDLCALL

/* ---- print-format macros (wad.c / common.c) ---- */
#define SDL_PRIs64 PRIs64
#define SDL_PRIu64 PRIu64
#define SDL_PRIu32 PRIu32
#define SDL_PRIs32 PRId32
#ifndef PRIs64
#define PRIs64 PRId64
#endif

/* ---- threading (SDL2 names; quakedef.h aliases the SDL3 names onto these) ---- */
/* Opaque handles — the bodies live in pl_phoenix_sdlcompat.c over sys/threads.h. */
typedef struct SDL_mutex   SDL_mutex;
typedef struct SDL_cond    SDL_cond;
typedef struct SDL_sem     SDL_sem;
typedef struct SDL_Thread  SDL_Thread;
typedef int (SDLCALL *SDL_ThreadFunction)(void *data);

#define SDL_MUTEX_MAXWAIT (~(Uint32)0)   /* infinite wait sentinel (tasks.h uses it) */
#define SDL_MUTEX_TIMEDOUT 1

SDL_mutex *SDL_CreateMutex(void);
void       SDL_DestroyMutex(SDL_mutex *m);
int        SDL_LockMutex(SDL_mutex *m);
int        SDL_UnlockMutex(SDL_mutex *m);

SDL_cond  *SDL_CreateCond(void);
void       SDL_DestroyCond(SDL_cond *c);
int        SDL_CondSignal(SDL_cond *c);
int        SDL_CondBroadcast(SDL_cond *c);
int        SDL_CondWait(SDL_cond *c, SDL_mutex *m);
int        SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms);

SDL_sem   *SDL_CreateSemaphore(Uint32 initial);
void       SDL_DestroySemaphore(SDL_sem *s);
int        SDL_SemPost(SDL_sem *s);
int        SDL_SemWait(SDL_sem *s);
int        SDL_SemTryWait(SDL_sem *s);
int        SDL_SemWaitTimeout(SDL_sem *s, Uint32 ms);
Uint32     SDL_SemValue(SDL_sem *s);

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
void        SDL_WaitThread(SDL_Thread *t, int *status);
void        SDL_DetachThread(SDL_Thread *t);

int        SDL_GetCPUCount(void);
void       SDL_Delay(Uint32 ms);

/* menu.c reads the mouse directly through this even outside the input platform TU; the
 * body lives in the Phoenix input shim (pl_phoenix_in.c). */
Uint32     SDL_GetMouseState(int *x, int *y);   /* SDL2 form (USE_SDL2) */

/* ---- pref/user path (host.c, cmd.c, menu.c, cfgfile.c, host_cmd.c) ---- */
char      *SDL_GetPrefPath(const char *org, const char *app);
void       SDL_free(void *mem);

/* ---- SDL_RWops file I/O (common.c) ---- */
#define RW_SEEK_SET SEEK_SET
#define RW_SEEK_CUR SEEK_CUR
#define RW_SEEK_END SEEK_END

typedef FILE SDL_RWops;

static inline SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
	return fopen(file, mode);
}
static inline Sint64 SDL_RWseek(SDL_RWops *ctx, Sint64 offset, int whence)
{
	if (fseek((FILE *)ctx, (long)offset, whence) != 0)
		return -1;
	return (Sint64)ftell((FILE *)ctx);
}
static inline Sint64 SDL_RWtell(SDL_RWops *ctx)
{
	return (Sint64)ftell((FILE *)ctx);
}
static inline Sint64 SDL_RWsize(SDL_RWops *ctx)
{
	long cur = ftell((FILE *)ctx), end;
	if (cur < 0 || fseek((FILE *)ctx, 0, SEEK_END) != 0)
		return -1;
	end = ftell((FILE *)ctx);
	fseek((FILE *)ctx, cur, SEEK_SET);
	return (Sint64)end;
}
static inline size_t SDL_RWread(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum)
{
	return fread(ptr, size, maxnum, (FILE *)ctx);
}
static inline size_t SDL_RWwrite(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
	return fwrite(ptr, size, num, (FILE *)ctx);
}
static inline int SDL_RWclose(SDL_RWops *ctx)
{
	return fclose((FILE *)ctx);
}

#endif /* PHOENIX_VKQ_SDL_SHIM_H */
