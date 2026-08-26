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
 * pl_phoenix_sys.c — Phoenix platform shim for vkQuake: replaces sys_sdl_unix.c
 * (+ the pl_linux.c platform odds-and-ends). Pure POSIX (Phoenix provides
 * open/read/write/lseek/close, clock_gettime, mkdir, usleep), no SDL.
 *
 * Adapted from quakespasm-port/platform/pl_phoenix_sys.c, but for vkQuake's newer sys.h:
 *   - the integer file-HANDLE functions (Sys_FileOpenRead/Write/Read/Write/Seek/Close +
 *     Sys_MemFileOpenRead + Sys_filelength) come from the PORTABLE upstream sys_sdl.c,
 *     which we add to the build (it has no SDL dependency). This shim provides only the
 *     genuinely non-portable pieces:
 *       Sys_Init, Sys_fseek/Sys_ftell (FILE* stdio helpers used by cl_demo/sys_sdl.c),
 *       Sys_FileType, Sys_mkdir, Sys_Printf/Error/Quit, Sys_DoubleTime, Sys_Sleep,
 *       Sys_ConsoleInput, Sys_SendKeyEvents, and the vkQuake-new thread-pin / debugger /
 *       stack-trace hooks (Sys_PinCurrentThread/StackTrace/IsInDebugger/DebugBreak) — all
 *       safe no-ops on Phoenix.
 *   - the platform odds-and-ends from pl_linux.c (PL_ErrorDialog, PL_GetClipboardData).
 */
#include "quakedef.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

qboolean isDedicated;          /* declared extern in quakedef.h */

void Sys_Init(void)
{
	/* quakespasm/vkQuake's sys_sdl_unix.c discovers basedir from argv[0] + cwd here.
	 * Our Phoenix entry point (pl_phoenix_main.c) sets host_parms->basedir directly
	 * (NFS rootfs path discovery), so there is nothing to do here. host_parms->userdir
	 * is also pre-set by main; leave it untouched. */
}

/* --- FILE* stdio helpers (cl_demo.c demo seeking; sys_sdl.c's Sys_filelength) --- */
int Sys_fseek(FILE *file, qfileofs_t ofs, int origin)
{
	return fseek(file, (long)ofs, origin);
}

qfileofs_t Sys_ftell(FILE *file)
{
	return (qfileofs_t)ftell(file);
}

int Sys_FileType(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return FS_ENT_NONE;
	if (S_ISDIR(st.st_mode))
		return FS_ENT_DIRECTORY;
	if (S_ISREG(st.st_mode))
		return FS_ENT_FILE;
	return FS_ENT_NONE;
}

void Sys_mkdir(const char *path)
{
	int rc = mkdir(path, 0777);
	if (rc != 0 && errno == EEXIST) {
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			rc = 0;
	}
	/* A failed mkdir on the (read-mostly) NFS rootfs is non-fatal for first-light: the
	 * data dir already exists and saves are best-effort. Do NOT Sys_Error like upstream
	 * (which would abort the boot on a read-only export). */
	(void)rc;
}

void Sys_Printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void Sys_Quit(void)
{
	/* Mirror the quakespasm-port rationale: hand the framebuffer + keyboard back so the
	 * fbcon console / psh become usable again, but skip the full Host_Shutdown() config
	 * write (the NFS large-write path can hang). The Vulkan vid shim owns VID_Shutdown;
	 * IN_Shutdown restores cooked /dev/kbd0. Everything else is reclaimed by exit(). */
	VID_Shutdown();
	IN_Shutdown();
	exit(0);
}

void Sys_Error(const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	fprintf(stderr, "\nvkQuake Error: ");
	vfprintf(stderr, error, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	/* Upstream sys_sdl_unix.c runs Host_Shutdown + a stack trace + an error dialog here;
	 * on first-light Phoenix we keep it minimal and just exit so the failure is visible
	 * on the UART without risking the NFS-write Host_Shutdown hang. */
	exit(1);
}

double Sys_DoubleTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char *Sys_ConsoleInput(void)
{
	/* No stdin console on the Pi4 boot path (UART is shared/non-interactive). */
	return NULL;
}

void Sys_Sleep(unsigned long msecs)
{
	usleep(msecs * 1000);
}

void Sys_SendKeyEvents(void)
{
	IN_Commands();
	IN_SendKeyEvents();
}

/* --- vkQuake-new platform hooks: thread affinity + debugger (no-ops on Phoenix) --- */

bool Sys_PinCurrentThread(int core_index)
{
	/* Phoenix Pi4 scheduler is cpu0-only (SMP findings); no per-thread affinity. */
	(void)core_index;
	return false;
}

const char *Sys_StackTrace(void)
{
	/* No backtrace()/symbolization on Phoenix; Sys_Error path tolerates a static string. */
	return "[no stack trace on Phoenix]";
}

bool Sys_IsInDebugger(void)
{
	return false;
}

void Sys_DebugBreak(void)
{
}

/* --- platform odds-and-ends (were in pl_linux.c, which this port excludes) --- */

void PL_ErrorDialog(const char *text)
{
	/* No GUI dialog; the message already went to the UART via Sys_Error/Sys_Printf. */
	(void)text;
}

char *PL_GetClipboardData(void)
{
	/* No clipboard on Phoenix Pi4. */
	return NULL;
}
