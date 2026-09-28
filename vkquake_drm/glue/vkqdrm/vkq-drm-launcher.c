/*
 * vkq-drm -- launcher for vkquake-drm (vkQuake on SDL2 KMSDRM + Vulkan VK_KHR_display, the
 * new GPU lane). Install as /bin/vkq-drm; the engine is /usr/bin/vkquake-drm.
 *
 * The shipped /usr/bin/vkquake is launched bare by the showcase gate: its Phoenix main()
 * (ports/vkquake glue) finds the data dir itself (/usr/share/quake unless a RAM copy exists),
 * forces r_rtshadows 0 / r_gpulightmapupdate 1 and boots `map start` from id1/phoenix-map.cfg,
 * because its video shim had no argv path. vkquake-drm is upstream vkQuake with upstream
 * main_sdl.c, so the same workload is spelled on the command line instead:
 *
 *   -basedir /usr/share/quake       the same data (over NFS, no RAM staging, as the gate ran it)
 *   -width 1920 -height 1080 -fullscreen
 *                                   the 1080p mode: SDL's KMSDRM Vulkan surface must match a
 *                                   display mode exactly (it cannot create one on this display)
 *   +r_rtshadows 0                  as the port's main() (V3D 4.2 has no ray queries)
 *   +map start                      the fixed viewpoint the #67 torch ROI check scores
 *
 * (`+map` takes effect on the shareware pak only with vkquake-drm's patch 0001, which
 * publishes the command line there too.) Extra arguments are appended and win.
 *
 * The engine path is VKQDRM_TARGET (build-vkquake-drm.sh passes VKQDRM_TARGET from its
 * environment), so a variant build is staged next to the default one under its own name.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef VKQDRM_TARGET
#define VKQDRM_TARGET "/usr/bin/vkquake-drm"
#endif

int main(int argc, char **argv)
{
	static char *base[] = {
		VKQDRM_TARGET, "-basedir", "/usr/share/quake",
		"-width", "1920", "-height", "1080", "-fullscreen",
		"+r_rtshadows", "0",
		"+map", "start",
	};
	const int nbase = (int)(sizeof(base) / sizeof(base[0]));
	char **a = calloc((size_t)(nbase + argc + 1), sizeof(char *));
	int i, n = 0;

	if (a == NULL) {
		fprintf(stderr, "vkq-drm: out of memory\n");
		return 1;
	}
	for (i = 0; i < nbase; i++) {
		a[n++] = base[i];
	}
	for (i = 1; i < argc; i++) {
		a[n++] = argv[i];
	}
	a[n] = NULL;

	fprintf(stderr, "vkq-drm: exec " VKQDRM_TARGET " -basedir /usr/share/quake -width 1920 -height 1080 -fullscreen +r_rtshadows 0 +map start");
	for (i = 1; i < argc; i++) {
		fprintf(stderr, " %s", argv[i]);
	}
	fprintf(stderr, "\n");
	execv(base[0], a);
	perror("vkq-drm: exec " VKQDRM_TARGET);
	return 1;
}
