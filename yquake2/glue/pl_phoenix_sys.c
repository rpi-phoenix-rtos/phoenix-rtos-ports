/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2026 Phoenix Systems. Author: Witold Bołt.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * =======================================================================
 *
 * Phoenix-RTOS system backend (fork of src/backends/unix/system.c).
 *
 * Phoenix has no dlopen/dlsym: yQuake2's two dynamic-load seams (the game
 * DLL and the renderer DLL) are folded into ONE static ELF. The three
 * loader functions below resolve the compiled-in GetGameAPI / GetRefAPI
 * symbols directly instead of dlopen()ing a *.so. Everything else
 * (filesystem, time, console, directory walk) is kept verbatim from the
 * unix backend. The only other change is Sys_Realpath(), which uses a
 * caller-provided stack buffer rather than the glibc realpath(in, NULL)
 * allocation form that libphoenix does not honour.
 *
 * =======================================================================
 */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/select.h> /* for fd_set */
#ifndef FNDELAY
#define FNDELAY O_NDELAY
#endif

#include "common/header/common.h"
#include "common/header/glob.h"

/* Statically-linked entry points (single-ELF: no dlopen). Declared opaque
 * to avoid dragging ref.h / game.h into the backend; only their addresses
 * are taken (GetRefAPI) or forwarded (GetGameAPI), so the real signatures
 * (refexport_t GetRefAPI(refimport_t) / game_export_t *GetGameAPI(
 * game_import_t *)) are irrelevant to the linker. */
extern void *GetRefAPI();
/* Explicit param: under C23 (gcc-16 default) an empty () parameter list means
 * (void), which rejects the Sys_GetGameAPI(parms) forwarding call below. The
 * real signature returns/takes pointers, ABI-compatible with void*(void*). */
extern void *GetGameAPI(void *);

// Evil hack to determine if stdin is available
qboolean stdin_active = true;

// Terminal supports colors
static qboolean color_active = false;

// Console logfile
extern FILE	*logfile;

// Config dir name
char cfgdir[MAX_OSPATH] = CFGDIRNAME;
static qboolean user_cfgdir = false;

/* ================================================================ */

void setCustomCfgDir(const char* dir)
{
	Q_strlcpy(cfgdir, dir, MAX_OSPATH);
	user_cfgdir = true;
}

void
Sys_Error(const char *error, ...)
{
	va_list argptr;
	char string[1024];

	/* change stdin to blocking */
	if (fcntl(fileno(stdin), F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY))
	{
		Com_Printf("%s: change stdin to blocking %s\n",
			__func__, strerror(errno));
	}


#ifndef DEDICATED_ONLY
	CL_Shutdown();
#endif
	Qcommon_Shutdown();

	va_start(argptr, error);
	vsnprintf(string, 1024, error, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", string);

	exit(1);
}

void
Sys_Quit(void)
{
#ifndef DEDICATED_ONLY
	CL_Shutdown();
#endif

	if (logfile)
	{
		fclose(logfile);
		logfile = NULL;
	}

	Qcommon_Shutdown();
	if (fcntl(fileno(stdin), F_SETFL, fcntl(0, F_GETFL, 0) & ~FNDELAY))
	{
		Com_Printf("%s: change stdin to blocking %s\n",
			__func__, strerror(errno));
	}

	printf("------------------------------------\n");

	exit(0);
}

void
Sys_Init(void)
{
	char *envvar;

	envvar = getenv("TERM");
	if (envvar && strstr(envvar, "color"))
	{
		char buf[256];

		color_active = true;

		snprintf(buf, sizeof(buf),
			"\2Terminal supports colors: TERM='%s'\n", envvar);

		Sys_ConsoleOutput(buf);
		return;
	}

	envvar = getenv("COLORTERM");
	if (envvar && strlen(envvar))
	{
		char buf[256];
		color_active = true;

		snprintf(buf, sizeof(buf),
			"\2Terminal supports colors: COLORTERM='%s'\n", envvar);

		Sys_ConsoleOutput(buf);
		return;
	}

	Sys_ConsoleOutput("Terminal has no colors support.\n");

}

/* ================================================================ */

char *
Sys_ConsoleInput(void)
{
	static char text[256];
	int len;
	fd_set fdset;
	struct timeval timeout;

	if (!dedicated || !dedicated->value)
	{
		return NULL;
	}

	if (!stdin_active)
	{
		return NULL;
	}

	FD_ZERO(&fdset);
	FD_SET(0, &fdset); /* stdin */
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;

	if ((select(1, &fdset, NULL, NULL, &timeout) == -1) || !FD_ISSET(0, &fdset))
	{
		return NULL;
	}

	len = read(0, text, sizeof(text));

	if (len == 0)   /* eof! */
	{
		stdin_active = false;
		return NULL;
	}

	if (len < 1)
	{
		return NULL;
	}

	text[len - 1] = 0; /* rip off the /n and terminate */

	return text;
}

void
Sys_ConsoleOutput(char *string)
{
	if ((string[0] == 0x01) || (string[0] == 0x02))
	{
		if (color_active)
		{
			if (string[0] == 0x01)
			{
				/* red */
				fputs("\033[31;1m", stdout);
			}
			else
			{
				/* green */
				fputs("\033[32;1m", stdout);
			}

			fputs(string + 1, stdout);

			/* reset to default terminal settings */
			fputs("\033[0m", stdout);
			return;
		}
	}

	fputs(string, stdout);
}

/* ================================================================ */

long long
Sys_Microseconds(void)
{
	struct timespec now;
	static struct timespec first;
#ifdef _POSIX_MONOTONIC_CLOCK
	clock_gettime(CLOCK_MONOTONIC, &now);
#else
	clock_gettime(CLOCK_REALTIME, &now);
#endif

	if(first.tv_sec == 0)
	{
		long long nsec = now.tv_nsec;
		long long sec = now.tv_sec;
		// set back first by 1ms so neither this function nor Sys_Milliseconds()
		// (which calls this) will ever return 0
		nsec -= 1000000;
		if(nsec < 0)
		{
			nsec += 1000000000ll; // 1s in ns => definitely positive now
			--sec;
		}

		first.tv_sec = sec;
		first.tv_nsec = nsec;
	}

	long long sec = now.tv_sec - first.tv_sec;
	long long nsec = now.tv_nsec - first.tv_nsec;

	if(nsec < 0)
	{
		nsec += 1000000000ll; // 1s in ns
		--sec;
	}

	return sec*1000000ll + nsec/1000ll;
}

int
Sys_Milliseconds(void)
{
	return (int)(Sys_Microseconds()/1000ll);
}

void
Sys_Nanosleep(int nanosec)
{
	struct timespec t = {0, nanosec};
	nanosleep(&t, NULL);
}

/* ================================================================ */

/* The musthave and canhave arguments are unused in YQ2. We
   can't remove them since Sys_FindFirst() and Sys_FindNext()
   are defined in shared.h and may be used in custom game DLLs. */

static char findbase[MAX_OSPATH];
static char findpath[MAX_OSPATH];
static char findpattern[MAX_OSPATH];
static DIR *fdir;

char *
Sys_FindFirst(const char *path, unsigned musthave, unsigned canhave)
{
	struct dirent *d;
	char *p;

	if (fdir)
	{
		Sys_Error("Sys_BeginFind without close");
	}

	Q_strlcpy(findbase, path, sizeof(findbase));

	if ((p = strrchr(findbase, '/')) != NULL)
	{
		*p = 0;
		Q_strlcpy(findpattern, p + 1, sizeof(findpattern));
	}
	else
	{
		strcpy(findpattern, "*");
	}

	if (strcmp(findpattern, "*.*") == 0)
	{
		strcpy(findpattern, "*");
	}

	if ((fdir = opendir(findbase)) == NULL)
	{
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL)
	{
		if (!*findpattern || glob_match(findpattern, d->d_name))
		{
			if ((strcmp(d->d_name, ".") != 0) && (strcmp(d->d_name, "..") != 0))
			{
				Com_sprintf(findpath, sizeof(findpath), "%s/%s", findbase,
					d->d_name);
				return findpath;
			}
		}
	}

	return NULL;
}

char *
Sys_FindNext(unsigned musthave, unsigned canhave)
{
	struct dirent *d;

	if (fdir == NULL)
	{
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL)
	{
		if (!*findpattern || glob_match(findpattern, d->d_name))
		{
			if ((strcmp(d->d_name, ".") != 0) && (strcmp(d->d_name, "..") != 0))
			{
				Com_sprintf(findpath, sizeof(findpath), "%s/%s", findbase, d->d_name);
				return findpath;
			}
		}
	}

	return NULL;
}

void
Sys_FindClose(void)
{
	if (fdir != NULL)
	{
		closedir(fdir);
	}

	fdir = NULL;
}

/* ================================================================ */

void
Sys_UnloadGame(void)
{
	/* Game logic is statically linked: nothing to unload. */
}

void *
Sys_GetGameAPI(void *parms)
{
	/* Single-ELF: return the compiled-in game entry point directly
	 * instead of dlopen("game.so") + dlsym("GetGameAPI"). */
	return GetGameAPI(parms);
}

/* ================================================================ */

void
Sys_Mkdir(const char *path)
{
	if (!Sys_IsDir(path))
	{
		if (mkdir(path, 0755) != 0)
		{
			Com_Error(ERR_FATAL, "Couldn't create dir %s\n", path);
		}
	}
}

qboolean
Sys_IsDir(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISDIR(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

qboolean
Sys_IsFile(const char *path)
{
	struct stat sb;

	if (stat(path, &sb) != -1)
	{
		if (S_ISREG(sb.st_mode))
		{
			return true;
		}
	}

	return false;
}

char *
Sys_GetHomeDir()
{
	static char dir[MAX_OSPATH];

	if (!dir[0])
	{
		const char* home = getenv("HOME");

		if (!home) {
			// uh-oh
			return NULL;
		}

		if (user_cfgdir) {
			// custom cfgdir was set by the user: ~/{cfgdir}
			Com_sprintf(dir, MAX_OSPATH, "%s/%s/", home, cfgdir);
			Sys_Mkdir(dir);
			return dir;
		}

		// hidden dir: ~/.{CFGDIRNAME_SHORT}
		Com_sprintf(dir, MAX_OSPATH, "%s/.%s/", home, CFGDIRNAME_SHORT);
	}

	Sys_Mkdir(dir);
	return dir;
}

void
Sys_Remove(const char *path)
{
	if (remove(path) == -1 && errno != ENOENT)
	{
		Com_Printf("%s: remove %s: %s\n",
			__func__, path, strerror(errno));
	}
}

int
Sys_Rename(const char *from, const char *to)
{
	return rename(from, to);
}

void
Sys_RemoveDir(const char *path)
{
	char filepath[MAX_OSPATH];
	struct dirent *file;
	DIR *directory;

	if (Sys_IsDir(path))
	{
		directory = opendir(path);
		if (directory)
		{
			while ((file = readdir(directory)) != NULL)
			{
				snprintf(filepath, MAX_OSPATH, "%s/%s", path, file->d_name);
				Sys_Remove(filepath);
			}

			closedir(directory);
			Sys_Remove(path);
		}
	}
}

qboolean
Sys_Realpath(const char *in, char *out, size_t size)
{
	/* libphoenix's realpath() does not honour the glibc realpath(in, NULL)
	 * self-allocating form, so resolve into a caller-visible stack buffer. */
	char converted[PATH_MAX];

	if (realpath(in, converted) == NULL)
	{
		Com_Printf("Couldn't get realpath for %s\n", in);
		return false;
	}

	Q_strlcpy(out, converted, size);

	return true;
}

/* ================================================================ */

void *
Sys_GetProcAddress(void *handle, const char *sym)
{
	/* No dynamic symbol table on Phoenix. The GL renderer resolves its
	 * entry points through SDL_GL_GetProcAddress, not this hook. */
	(void)handle;
	(void)sym;
	return NULL;
}

void
Sys_FreeLibrary(void *handle)
{
	/* Statically linked: nothing to free. */
	(void)handle;
}

void *
Sys_LoadLibrary(const char *path, const char *sym, void **handle)
{
	/* Single-ELF: the only "library" the client loads is the renderer,
	 * via its GetRefAPI export. Hand back the compiled-in symbol. */
	(void)path;

	*handle = (void *)1; /* non-NULL sentinel; Sys_FreeLibrary ignores it */

	if (sym && !strcmp(sym, "GetRefAPI"))
	{
		return (void *)GetRefAPI;
	}

	return NULL;
}

/* ================================================================ */

void
Sys_GetWorkDir(char *buffer, size_t len)
{
	if (getcwd(buffer, len) != 0)
	{
		return;
	}

	buffer[0] = '\0';
}

qboolean
Sys_SetWorkDir(char *path)
{
	if (chdir(path) == 0)
	{
		return true;
	}

	return false;
}
