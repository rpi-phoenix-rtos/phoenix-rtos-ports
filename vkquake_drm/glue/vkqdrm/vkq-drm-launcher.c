/*
 * vkquake -- launcher for vkquake-drm (vkQuake on SDL2 KMSDRM + Vulkan VK_KHR_display).
 * Install as /usr/bin/vkquake; the engine is /usr/bin/vkquake-drm (upstream vkQuake with
 * upstream main_sdl.c), started with:
 *
 *   -basedir /usr/share/quake       the same data (over NFS, no RAM staging, as the gate ran it)
 *   -width 1920 -height 1080 -fullscreen
 *                                   the 1080p mode: SDL's KMSDRM Vulkan surface must match a
 *                                   display mode exactly (it cannot create one on this display)
 *   +r_rtshadows 0                  as the port's main() (V3D 4.2 has no ray queries)
 *   +map start                      the fixed viewpoint the #67 torch ROI check scores;
 *                                   left out when the caller gives +map, +playdemo or
 *                                   +timedemo: `vkquake +playdemo demo1` plays a recorded demo
 *                                   of the pak, `vkquake +timedemo demo1` benchmarks it
 *
 * (`+map` takes effect on the shareware pak only with vkquake-drm's patch 0001, which
 * publishes the command line there too.) Extra arguments are appended. vkQuake takes the FIRST
 * -width/-height/-basedir (COM_CheckParm), so an appended one would lose to the defaults above:
 * when the caller gives -width, -height or -current the launcher leaves out its -width 1920
 * -height 1080, and when the caller gives -basedir it leaves out its own (a caller's -window
 * wins over -fullscreen anyway). With the M9 scaled modes of rpi4-kms, `vkquake -width 1280
 * -height 720` is therefore a 1280x720 fullscreen mode, scaled to the screen.
 *
 * The first start on a system compiles every Vulkan pipeline into Mesa's shader cache
 * ($HOME/.cache/mesa_shader_cache): about 75 s of black screen before the first frame. Later
 * starts read the cache and take a few seconds. So that the wait is not mistaken for a hang,
 * the launcher says so on the console and pauses briefly before the display is taken, the first
 * time it runs (until $HOME/.cache/vkquake-first-start exists; a freshly flashed card or a
 * fresh NFS export has neither the marker nor the cache).
 *
 * The engine path is VKQDRM_TARGET (the vkquake_drm port passes it).
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef VKQDRM_TARGET
#define VKQDRM_TARGET "/usr/bin/vkquake-drm"
#endif


/* Tell the user about the one-time shader compile, the first time only. */
static void first_start_notice(void)
{
	const char *home = getenv("HOME");
	char marker[256];
	int fd;

	if ((home == NULL) || (home[0] == '\0')) {
		home = "/";
	}
	(void)snprintf(marker, sizeof(marker), "%s%s.cache/vkquake-first-start", home,
		(home[strlen(home) - 1] == '/') ? "" : "/");
	if (access(marker, F_OK) == 0) {
		fprintf(stderr, "vkquake: starting (shaders come from the cache; a first start after an update compiles them again)\n");
		return;
	}

	fprintf(stderr,
		"\n"
		"  vkQuake: FIRST START on this system.\n"
		"  The Vulkan shaders are compiled now and kept in the shader cache. This takes about\n"
		"  1-1.5 minutes, with a black screen. It has not hung: please wait for the first frame.\n"
		"  Later starts take a few seconds.\n"
		"\n");
	(void)sleep(5);

	/* best effort: without a writable $HOME/.cache the notice simply shows again */
	(void)snprintf(marker, sizeof(marker), "%s%s.cache", home, (home[strlen(home) - 1] == '/') ? "" : "/");
	(void)mkdir(marker, 0755);
	(void)strncat(marker, "/vkquake-first-start", sizeof(marker) - strlen(marker) - 1);
	fd = open(marker, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) {
		(void)close(fd);
	}
}


static int caller_gives(int argc, char **argv, const char *opt)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], opt) == 0) {
			return 1;
		}
	}
	return 0;
}


int main(int argc, char **argv)
{
	static char *basedir[] = { "-basedir", "/usr/share/quake" };
	static char *size[] = { "-width", "1920", "-height", "1080" };
	static char *rest[] = { "-fullscreen", "+r_rtshadows", "0", "+map", "start" };
	/* the last two words are the +map start */
	const int nrest = (int)(sizeof(rest) / sizeof(rest[0])) -
		((caller_gives(argc, argv, "+map") || caller_gives(argc, argv, "+playdemo") ||
			caller_gives(argc, argv, "+timedemo")) ? 2 : 0);
	char **a = calloc((size_t)argc + 16u, sizeof(char *));
	int i, n = 0;

	if (a == NULL) {
		fprintf(stderr, "vkquake: out of memory\n");
		return 1;
	}
	a[n++] = VKQDRM_TARGET;
	if (caller_gives(argc, argv, "-basedir") == 0) {
		for (i = 0; i < (int)(sizeof(basedir) / sizeof(basedir[0])); i++) {
			a[n++] = basedir[i];
		}
	}
	if ((caller_gives(argc, argv, "-width") == 0) && (caller_gives(argc, argv, "-height") == 0) &&
			(caller_gives(argc, argv, "-current") == 0)) {
		for (i = 0; i < (int)(sizeof(size) / sizeof(size[0])); i++) {
			a[n++] = size[i];
		}
	}
	for (i = 0; i < nrest; i++) {
		a[n++] = rest[i];
	}
	for (i = 1; i < argc; i++) {
		a[n++] = argv[i];
	}
	a[n] = NULL;

	first_start_notice();

	fprintf(stderr, "vkquake: exec");
	for (i = 0; i < n; i++) {
		fprintf(stderr, " %s", a[i]);
	}
	fprintf(stderr, "\n");
	execv(a[0], a);
	perror("vkquake: exec " VKQDRM_TARGET);
	return 1;
}
