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
 * It starts the first demo level (+map demo1). A caller's own level or demo replaces it:
 * `quake2 +map base1`, or the recorded demo of the pak, `quake2 +demomap q2demo1.dm2`.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* True when the caller names a level or a demo itself (then +map demo1 is left out). */
static int caller_loads(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "+map") == 0 || strcmp(argv[i], "+demomap") == 0 ||
				strcmp(argv[i], "+gamemap") == 0) {
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	/* ram-stage-play <src> <dst> <exec> [exec-args...]
	 *
	 * Video args: the display's native custom mode (r_mode -1 + r_customwidth/height,
	 * 1920x1080) full screen, and boot straight into the demo level so 3D renders
	 * immediately. Any user args are appended after and win (yquake2 runs every +set
	 * before the first frame); a user +map/+demomap replaces the launcher's. */
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
	/* the last two words are the +map demo1 */
	const int nbase = (int)(sizeof(base) / sizeof(base[0])) - (caller_loads(argc, argv) ? 2 : 0);
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
