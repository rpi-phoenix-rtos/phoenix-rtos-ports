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
 * Phoenix-RTOS program entry point (fork of src/backends/unix/main.c).
 *
 * Kept almost verbatim; the two Unix setuid sanity checks (getuid()==0 and
 * getuid()!=geteuid()) are dropped — Phoenix is a single-user embedded
 * target where getuid() returns 0, which would make the stock check refuse
 * to launch. The setenv("LC_ALL", ...) locale pin is also dropped (no
 * locale subsystem on Phoenix).
 *
 * =======================================================================
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef FNDELAY
#define FNDELAY O_NDELAY
#endif

#include "common/header/common.h"

void registerHandler(void);       /* backends/unix/signalhandler.c */
void setCustomCfgDir(const char* dir); /* pl_phoenix_sys.c */

int
main(int argc, char **argv)
{
	// register signal handler
	registerHandler();

	// Setup FPU if necessary
	Sys_SetupFPU();

	// Implement command line options that the rather
	// crappy argument parser can't parse.
	for (int i = 0; i < argc; i++)
	{
		// Are we portable?
		if (strcmp(argv[i], "-portable") == 0)
		{
			is_portable = true;
		}

		// Inject a custom data dir.
		if (strcmp(argv[i], "-datadir") == 0)
		{
			// Mkay, did the user give us an argument?
			if (i != (argc - 1))
			{
				// Check if it exists.
				struct stat sb;

				if (stat(argv[i + 1], &sb) == 0)
				{
					if (!S_ISDIR(sb.st_mode))
					{
						printf("-datadir %s is not a directory\n", argv[i + 1]);
						return 1;
					}

					if(realpath(argv[i + 1], datadir) == NULL)
					{
						printf("realpath(datadir) failed: %s\n", strerror(errno));
						datadir[0] = '\0';
					}
				}
				else
				{
					printf("-datadir %s could not be found\n", argv[i + 1]);
					return 1;
				}
			}
			else
			{
				printf("-datadir needs an argument\n");
				return 1;
			}
		}

		// Inject a custom config dir.
		if (strcmp(argv[i], "-cfgdir") == 0)
		{
			// We need an argument.
			if (i != (argc - 1))
			{
				setCustomCfgDir(argv[i + 1]);
			}
			else
			{
				printf("-cfgdir needs an argument\n");
				return 1;
			}

		}
	}

	/// Do not delay reads on stdin
	if (fcntl(fileno(stdin), F_SETFL, fcntl(fileno(stdin), F_GETFL, NULL) | FNDELAY))
	{
		Com_Printf("%s: change stdin to nodeleay %s\n",
			__func__, strerror(errno));
	}

	// Initialize the game.
	// Never returns.
	Qcommon_Init(argc, argv);

	return 0;
}
