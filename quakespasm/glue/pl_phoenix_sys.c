/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * Phoenix-RTOS platform backend for QuakeSpasm (QuakeSpasm is Copyright (C) id
 * Software, Inc. and the QuakeSpasm developers, GPL-2.0-or-later). It implements
 * the QuakeSpasm platform interface and is distributed under the same license as
 * the program it is built into; see COPYING in this directory.
 */
/*
 * pl_phoenix_sys.c — Phoenix platform shim for Quakespasm: replaces sys_sdl_unix.c
 * + pl_linux.c. Pure POSIX (Phoenix provides open/read/write/lseek/close,
 * clock_gettime, mkdir, usleep), no SDL.
 */
#include "quakedef.h"

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

#define MAX_HANDLES 64
/* Read handles slurp the whole file into RAM on open (one sequential read) and serve
 * reads/seeks from the buffer — pak0.pak over NFS is otherwise read per-lump (hundreds
 * of slow random reads), which dominates map-load time. Write handles use a FILE*. */
typedef struct {
	int used;
	FILE *fp;            /* write handles */
	unsigned char *buf;  /* read handles: whole-file cache */
	long size, pos;
} qfh_t;
static qfh_t sys_handles[MAX_HANDLES];

static int findhandle(void)
{
	int i;
	for (i = 1; i < MAX_HANDLES; i++)
		if (!sys_handles[i].used)
			return i;
	Sys_Error("out of file handles");
	return -1;
}

void Sys_Init(void)
{
}

int Sys_FileOpenRead(const char *path, int *hndl)
{
	int i = findhandle();
	FILE *f = fopen(path, "rb");
	long len;
	unsigned char *buf = NULL;

	if (!f) {
		*hndl = -1;
		return -1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len > 0) {
		buf = (unsigned char *)malloc((size_t)len);
		if (buf != NULL && fread(buf, 1, (size_t)len, f) != (size_t)len) {
			free(buf);
			buf = NULL;
			len = -1;
		}
	}
	fclose(f);
	if (len < 0) { *hndl = -1; return -1; }

	sys_handles[i].used = 1;
	sys_handles[i].fp = NULL;
	sys_handles[i].buf = buf;
	sys_handles[i].size = len;
	sys_handles[i].pos = 0;
	*hndl = i;
	return (int)len;
}

int Sys_FileOpenWrite(const char *path)
{
	int i = findhandle();
	FILE *f = fopen(path, "wb");
	if (!f)
		Sys_Error("Error opening %s: %s", path, strerror(errno));
	sys_handles[i].used = 1;
	sys_handles[i].fp = f;
	sys_handles[i].buf = NULL;
	sys_handles[i].size = 0;
	sys_handles[i].pos = 0;
	return i;
}

void Sys_FileClose(int handle)
{
	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle].used)
		return;
	if (sys_handles[handle].fp != NULL)
		fclose(sys_handles[handle].fp);
	free(sys_handles[handle].buf);
	memset(&sys_handles[handle], 0, sizeof(sys_handles[handle]));
}

void Sys_FileSeek(int handle, int position)
{
	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle].used)
		return;
	if (sys_handles[handle].buf != NULL)
		sys_handles[handle].pos = position;
	else if (sys_handles[handle].fp != NULL)
		fseek(sys_handles[handle].fp, position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
	qfh_t *h;
	long avail;

	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle].used)
		return 0;
	h = &sys_handles[handle];
	if (h->buf != NULL) {
		avail = h->size - h->pos;
		if ((long)count > avail)
			count = (int)avail;
		if (count <= 0)
			return 0;
		memcpy(dest, h->buf + h->pos, (size_t)count);
		h->pos += count;
		return count;
	}
	if (h->fp != NULL)
		return (int)fread(dest, 1, count, h->fp);
	return 0;
}

int Sys_FileWrite(int handle, const void *data, int count)
{
	if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle].used)
		return 0;
	if (sys_handles[handle].fp != NULL)
		return (int)fwrite(data, 1, count, sys_handles[handle].fp);
	return 0;
}

int Sys_FileType(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;                       /* FS_ENT_NONE */
	if (S_ISDIR(st.st_mode))
		return 2;                       /* FS_ENT_DIRECTORY */
	return 1;                               /* FS_ENT_FILE */
}

void Sys_mkdir(const char *path)
{
	(void)mkdir(path, 0777);
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
	/* Restore the system-wide device state our port grabbed, before exiting. Our Sys_Quit had
	 * dropped this, so "quit" left the screen frozen on the last rendered frame and psh
	 * non-interactive:
	 *   - VID_Shutdown(): pans the display off the last page-flipped GPU buffer and hands the
	 *     framebuffer back to the fbcon text console.
	 *   - IN_Shutdown():  restores cooked mode + closes /dev/kbd0 so the pl011-tty bridge
	 *     reacquires it and psh becomes interactive again.
	 * We deliberately do NOT call the full Host_Shutdown() here: it runs Host_WriteConfiguration()
	 * which writes config.cfg to the (NFS) rootfs, and the NFS large-write path can hang — which
	 * would prevent the display/input restore below from ever running. Everything else
	 * (sockets, heap, audio) is reclaimed by the kernel on exit(). */
	VID_Shutdown();
	IN_Shutdown();
	exit(0);
}

void Sys_Error(const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	fprintf(stderr, "\nQuake Error: ");
	vfprintf(stderr, error, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

double Sys_DoubleTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void Sys_Sleep(unsigned long msecs)
{
	usleep(msecs * 1000);
}

void Sys_SendKeyEvents(void)
{
	IN_SendKeyEvents();
}
