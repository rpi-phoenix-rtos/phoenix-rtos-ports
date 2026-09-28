/*
 * quake2 — launcher for yQuake2 on Phoenix-RTOS / RPi4.
 *
 * The baseq2 assets live on the (slow) NFS root at /usr/share/quake2; loading
 * textures directly over NFS is so slow the demo never paints (owner HW test:
 * black screen). So we RAM-stage first: exec ram-stage-play, which copies the
 * asset tree into the /tmp tmpfs (RAM) and then execs the engine reading from
 * RAM — the demo then renders in full textured 3D on the V3D GPU (HW-verified).
 * Forwards any extra user args. Install as /usr/bin/quake2.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	/* ram-stage-play <src> <dst> <exec> [exec-args...]
	 *
	 * Video args are REQUIRED: the Phoenix /dev/fb0 is 1920x1080-only, but yquake2
	 * defaults to r_mode 4 (640x480) which SDL SetVideoMode rejects on this fb
	 * ("Unknown pixel format") -> no visible image. Force the fb-native custom mode
	 * (r_mode -1 + r_customwidth/height) with fullscreen + the working ref_gl1
	 * renderer, and boot straight into the demo so 3D renders immediately. Any user
	 * args are appended after and win (e.g. `quake2 +map base1`). */
	static char *base[] = {
		"ram-stage-play", "/usr/share/quake2", "/tmp/quake2",
		"/usr/bin/yquake2", "-datadir", "/tmp/quake2",
		"+set", "vid_renderer", "gl1",
		"+set", "r_mode", "-1",
		"+set", "r_customwidth", "1920",
		"+set", "r_customheight", "1080",
		"+set", "vid_fullscreen", "2",
		"+map", "demo1",
	};
	const int nbase = (int)(sizeof(base) / sizeof(base[0]));
	char **a = calloc((size_t)(nbase + argc + 1), sizeof(char *));
	int i, n = 0;

	if (a == NULL) { fprintf(stderr, "quake2: out of memory\n"); return 1; }
	for (i = 0; i < nbase; i++) { a[n++] = base[i]; }
	for (i = 1; i < argc; i++) { a[n++] = argv[i]; }
	a[n] = NULL;

	execvp("ram-stage-play", a);  /* PATH search: it lives in /bin */
	perror("quake2: exec /usr/bin/ram-stage-play");
	return 1;
}
