/*
 * quake3 — launcher for the quake3e engine on Phoenix-RTOS / RPi4.
 *
 * Demo data lives on the slow NFS root at /usr/share/quake3; loading over NFS is
 * slow (owner HW test). RAM-stage first via ram-stage-play (copy the asset tree
 * into the /tmp tmpfs, then exec the engine reading from RAM) so load is fast.
 * fs_basepath points at the RAM copy; fs_game=demoq3 (the demo paks). Forwards
 * extra user args. Install as /usr/bin/quake3.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	static char *base[] = {
		"ram-stage-play", "/usr/share/quake3", "/tmp/quake3",
		"/usr/bin/quake3e", "+set", "fs_basepath", "/tmp/quake3", "+set", "fs_game", "demoq3",
		/* r_mergeLightmaps: quake3e's default (1, merged POT lightmap atlas) is now used.
		 * The old r_mergeLightmaps=0 workaround (individual 128x128 lightmaps) is REMOVED:
		 * the underlying V3D lightmap-tiling bug is fixed (external/mesa 4363822955b excludes
		 * SAMPLER_VIEW from the should_tile gate so the 1024² atlas is UIF_XOR not RASTER).
		 * NOTE (2026-08-22): q3dm7 also exhibits an INTERMITTENT (~50% of boots) V3D GPU wedge
		 * (`v3d-winsys: BIN/RENDER TIMEOUT ... mmu_ill=0x8000886x ... GPU wedged`) — this is the
		 * KNOWN winsys flush-completion race (v3d_phoenix_winsys.c l2t_flush_wait: back-to-back
		 * L2T flushes with no wait), NOT the merged atlas: r_mergeLightmaps 0 does NOT reliably
		 * avoid it (wedged 1 of 2 boots too). The real fix belongs in the winsys submit path;
		 * tracked in memory project_quake3_lightmap_uif_xor. Do NOT re-add r_mergeLightmaps 0 as
		 * a "fix" — a single clean boot is a false metric for an intermittent race.
		 * See docs .../2026-08-21-v3d-uif-xor-1024-redirect.md. */
	};
	const int nbase = (int)(sizeof(base) / sizeof(base[0]));
	char **a = calloc((size_t)(nbase + argc + 1), sizeof(char *));
	int i, n = 0;

	if (a == NULL) { fprintf(stderr, "quake3: out of memory\n"); return 1; }
	for (i = 0; i < nbase; i++) { a[n++] = base[i]; }
	for (i = 1; i < argc; i++) { a[n++] = argv[i]; }
	a[n] = NULL;

	execvp("ram-stage-play", a);  /* PATH search: it lives in /bin */
	perror("quake3: exec /usr/bin/ram-stage-play");
	return 1;
}
