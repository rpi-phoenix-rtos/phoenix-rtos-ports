/*
 * qs-drm -- launcher for quakespasm-drm (QuakeSpasm on SDL2 KMSDRM + Mesa GBM/EGL, the new GPU
 * lane). Installed as /bin/qs-drm and as /usr/bin/quakespasm; the engine is
 * /usr/bin/quakespasm-drm.
 *
 * It asks for the native 1080p fullscreen mode on the command line instead of relying on the
 * engine's id1/config.cfg: quakespasm writes config.cfg at a menu Quit, so one session in a
 * lower mode (e.g. `game-res qs 1280x720`) that quits normally would persist vid_width 1280 on
 * the root filesystem and every later default start would be 1280x720
 * (docs/gpu-new-lane/M9-scaled-fullscreen.md in the coordination repo). So:
 *
 *   -width 1920 -height 1080   only when the caller gives none of -width, -height, -current:
 *                              the engine takes the FIRST -width/-height (COM_CheckParm), so a
 *                              default placed before the caller's would win over it
 *   -fullscreen                always; a caller's -window still wins (VID_Init tests -window
 *                              first)
 *
 * Every other argument is passed on after these, unchanged.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef QSDRM_TARGET
#define QSDRM_TARGET "/usr/bin/quakespasm-drm"
#endif


static int caller_sets_size(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-width") == 0) || (strcmp(argv[i], "-height") == 0) || (strcmp(argv[i], "-current") == 0)) {
			return 1;
		}
	}
	return 0;
}


int main(int argc, char **argv)
{
	static char *size[] = { "-width", "1920", "-height", "1080" };
	char **a = calloc((size_t)argc + 8u, sizeof(char *));
	int i, n = 0;

	if (a == NULL) {
		fprintf(stderr, "qs-drm: out of memory\n");
		return 1;
	}
	a[n++] = QSDRM_TARGET;
	if (caller_sets_size(argc, argv) == 0) {
		for (i = 0; i < (int)(sizeof(size) / sizeof(size[0])); i++) {
			a[n++] = size[i];
		}
	}
	a[n++] = "-fullscreen";
	for (i = 1; i < argc; i++) {
		a[n++] = argv[i];
	}
	a[n] = NULL;

	fprintf(stderr, "qs-drm: exec");
	for (i = 0; i < n; i++) {
		fprintf(stderr, " %s", a[i]);
	}
	fprintf(stderr, "\n");
	execv(a[0], a);
	perror("qs-drm: exec " QSDRM_TARGET);
	return 1;
}
