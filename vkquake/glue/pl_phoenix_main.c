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
 * pl_phoenix_main.c — Phoenix entry point + host loop for vkQuake: replaces main_sdl.c.
 *
 * No SDL init. Just COM_InitArgv -> Sys_Init -> Host_Init, then the Host_Frame loop
 * driven by Sys_DoubleTime. Adapted from quakespasm-port/platform/pl_phoenix_main.c.
 *
 * KEY DIFFERENCE vs. the quakespasm port: vkQuake's quakeparms_t has NO membase/memsize
 * fields — the engine uses its own mem.c (mimalloc-style) allocator, not the classic
 * Quake hunk. So this main does NOT pre-allocate/commit a heap; it just discovers basedir
 * and runs the host loop. (The upstream main_sdl.c host loop calls VID_HasMouseOrInputFocus
 * / VID_IsMinimized / `listening` for input-focus sleeps; those belong to the not-yet-
 * written Vulkan vid shim, so we use a plain fixed-cadence loop here to avoid pulling new
 * undefined symbols into the link. The vid shim can reinstate the focus-aware sleeps later.)
 */
#include "quakedef.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static quakeparms_t parms;      /* host_parms (the pointer) is owned by host.c */

/* Discovered basedir (set by wait_for_gamedata): "<basedir>/id1/pak0.pak" is the data. */
static const char *g_basedir = "/usr/share/quake";

/* Wait (bounded) for the game data to be reachable before Host_Init, and discover WHERE
 * it lives. The data is installed FHS-style under /usr/share/quake (#46), same as the
 * quakespasm port. Probe a few standard locations each poll and adopt whichever first
 * exposes id1/pak0.pak (the bounded retry also absorbs a syspage process racing the
 * nfs-fs takeover + the libnfs first-read dircache ENOENT, #156). */
static void wait_for_gamedata(void)
{
	/* RAM-staged tmpfs paths first (the load-time workaround): if id1/ is staged into a RAM
	 * fs — e.g. `ram-stage-play /usr/share/quake/id1 /tmp/quake/id1 /usr/bin/vkquake` — asset
	 * reads are ~20x faster than over NFS (measured; RAM-staging is proven on Q1/Q2/Q3). Falls
	 * through to the FHS NFS dir when nothing is staged (transparent + backward-compatible).
	 * vkQuake picks the level from id1/phoenix-map.cfg not argv (#I2), so RAM preference is via
	 * this candidate list rather than a -basedir override. */
	static const char *cands[] = { "/ramtmp/quake", "/tmp/quake", "/usr/share/quake", "/opt/quake", "/" };
	char path[80];
	int i, c;
	for (i = 0; i < 360; i++) {     /* ~180 s — NFS mount + DHCP can be slow/variable (#156) */
		for (c = 0; c < (int)(sizeof(cands) / sizeof(cands[0])); c++) {
			FILE *f;
			snprintf(path, sizeof(path), "%s/id1/pak0.pak", cands[c]);
			f = fopen(path, "rb");
			if (f) {
				fclose(f);
				g_basedir = cands[c];
				Sys_Printf("vkquake: found %s after %d tries (basedir=%s)\n",
				           path, i + 1, g_basedir);
				return;
			}
		}
		usleep(500000);
	}
	Sys_Printf("vkquake: pak0.pak not found after wait (continuing; Host_Init will report)\n");
}

/* Startup map: this port has no console/argv path to pick a level (the classic engine takes
 * `+map <name>`, but Phoenix argv marshalling doesn't surface it — #I2), so historically the
 * boot map was HARDCODED to "start". Instead, read the map name from an optional one-line file
 * `<basedir>/id1/phoenix-map.cfg` (first whitespace-delimited token). Absent/empty -> "start"
 * (unchanged default). This lets a tester boot any level (e2m1, a water map, a DM map, ...) by
 * dropping a file into the game dir — no rebuild — which is also how the HDMI render pipeline can
 * exercise maps beyond start. Only [A-Za-z0-9_-] are accepted (it is pasted into a `map` command). */
static void read_boot_map(const char *basedir, char *out, size_t n)
{
	char path[96];
	FILE *f;
	size_t i;
	out[0] = '\0';
	snprintf(path, sizeof(path), "%s/id1/phoenix-map.cfg", basedir);
	f = fopen(path, "r");
	if (f != NULL) {
		if (fgets(out, (int)n, f) == NULL)
			out[0] = '\0';
		fclose(f);
	}
	/* keep only a leading run of safe map-name chars (strips newline/space/anything odd) */
	for (i = 0; out[i] != '\0'; i++) {
		char ch = out[i];
		int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		         (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
		if (!ok) {
			out[i] = '\0';
			break;
		}
	}
	if (out[0] == '\0')
		snprintf(out, n, "start");
}

int main(int argc, char *argv[])
{
	double time, oldtime, newtime;

	/* LINE-buffered stdout so each printf line reaches the shared UART console in one
	 * write() (otherwise our log interleaves char-by-char with the concurrent lwip
	 * process). stderr unbuffered so a fault's diagnostics reach the UART immediately. */
	static char vkq_stdout_buf[2048];
	setvbuf(stdout, vkq_stdout_buf, _IOLBF, sizeof(vkq_stdout_buf));
	setvbuf(stderr, NULL, _IONBF, 0);
	printf("vkquake: main() entered (argc=%d)\n", argc);

	host_parms = &parms;
	parms.basedir = "/usr/share/quake";    /* FHS data dir; wait_for_gamedata() refines it (#46) */
	parms.userdir = "/usr/share/quake";    /* DO_USERDIRS disabled -> userdir == basedir */
	parms.argc = argc;
	parms.argv = argv;
	parms.errstate = 0;

	COM_InitArgv(parms.argc, parms.argv);

	isDedicated = (COM_CheckParm("-dedicated") != 0);

	Sys_Init();

	Sys_Printf("Detected %d CPUs.\n", SDL_GetCPUCount());
	Sys_Printf("Initializing vkQuake (Phoenix/V3DV)\n");

	wait_for_gamedata();
	parms.basedir = g_basedir;      /* whichever path exposed id1/pak0.pak */
	parms.userdir = g_basedir;

	Sys_Printf("Host_Init\n");
	Host_Init();

	/* Boot straight into a lit 3D world via the GPU-COMPUTE lightmap path (r_gpulightmapupdate=1).
	 * This is the modern vkQuake architecture: the update_lightmap compute shader builds/updates
	 * lightmaps on the GPU each frame (+ the indirect-draw compute), instead of the slow CPU
	 * R_BuildLightMap path. It works now that the winsys implements CSD compute dispatch
	 * (v3d_phoenix_winsys.c ioc_submit_csd) — before that, DRM_V3D_SUBMIT_CSD was a no-op so no
	 * compute ran. RT shadows are off (Cvar r_rtshadows=0): V3D 4.2 has no ray-query hardware, so
	 * the update_lightmap_rt path / TLAS build must not run. Verified: map start + full maps (e1m2)
	 * render correctly lit with the GPU lightmap. Measured render perf (2026-08-06, HW, map start,
	 * 1920x1080): ~30 fps, render-bound at ~33 ms/frame, stable (few >50 ms stalls after the initial
	 * lightmap build). (An earlier "~150 fps" note here was an unverified estimate; the direct
	 * host-loop measurement is ~30 fps at 1080p — the render is fill/submit-bound, a perf lead.) */
	{
		extern cvar_t r_gpulightmapupdate, r_rtshadows;
		char bootmap[64];
		char mapcmd[80];
		Cvar_SetValueQuick(&r_rtshadows, 0.0f);
		Cvar_SetValueQuick(&r_gpulightmapupdate, 1.0f);
		read_boot_map(g_basedir, bootmap, sizeof(bootmap));
		snprintf(mapcmd, sizeof(mapcmd), "map %s\n", bootmap);
		Cbuf_AddText(mapcmd);
		Cbuf_Execute();
		Sys_Printf("vkquake: loading 'map %s' (GPU-compute lightmap path; boot map from id1/phoenix-map.cfg, default start)\n", bootmap);
	}

	/* TEXTURE-STAGING FLUSH (hygiene; HW: textured 2D samples 0 = upload gap). conchars + the
	 * other init-time textures (whitetexture, ...) record their vkCmdCopyBufferToImage + the
	 * SHADER_READ_ONLY transition into a STAGING command buffer during Host_Init (TexMgr_LoadImage
	 * -> R_StagingAllocate). The shim only flushes staging per-frame inside GL_EndRendering, which
	 * starts at frame 1 — so force the pending init-time uploads onto the GPU here, before the first
	 * SCR_UpdateScreen, and wait the device idle so the copies+transitions COMPLETE before any
	 * textured draw samples the image. (Note: per-frame submit+device-wait would also eventually
	 * land these, so if textures stay black AFTER this the cause is the copy/sample path, not submit
	 * timing — a marker disambiguates.) TODO(vkquake-port): keep if it proves load-bearing. */
	{
		extern void R_SubmitStagingBuffers(void);
		extern void GL_WaitForDeviceIdle(void);
		R_SubmitStagingBuffers();
		GL_WaitForDeviceIdle();
		Sys_Printf("vkquake: init-texture staging flushed + device idle (conchars/whitetexture uploads forced to GPU)\n");
	}

	oldtime = Sys_DoubleTime();
	{
		/* BRING-UP heartbeat: an UNCONDITIONAL, flushed marker bracketing each Host_Frame for
		 * the first frames. The earlier "present 1 then nothing" grabs were a pre-instrumentation
		 * %30-present-gate artifact (frames 2..29 simply weren't logged) AND the process was alive
		 * at +15s, so it was NOT a hang. This heartbeat removes the ambiguity: "loop N enter/exit"
		 * climbing => the loop iterates (a missing present is then a per-frame render gate, not a
		 * blocked loop); enter-without-exit => Host_Frame itself blocks on frame N (the real hang).
		 * TODO(vkquake-port): remove once the sustained frame loop is proven on HW. */
		unsigned long loopn = 0;
		while (1) {
			newtime = Sys_DoubleTime();
			time = newtime - oldtime;
			/* Clamp the per-frame delta so a single slow frame (e.g. the >15 s CPU lightmap
			 * build on a full-map load) does NOT fast-forward game/demo time. Without this the
			 * huge dt makes CL_GetDemoMessage consume the whole demo in one step (attract loop
			 * blows past the level to the menu) and makes live play jump. Matches classic Quake's
			 * host_frametime cap. TODO(vkquake-port): GPU-compute lightmaps will make that first
			 * frame fast; the clamp stays correct regardless. */
			if (time > 0.1)
				time = 0.1;
			if (loopn < 8) {
				printf("vkquake: loop %lu enter\n", loopn);
				fflush(stdout);
			}
			Host_Frame(time);
			if (loopn < 8) {
				printf("vkquake: loop %lu exit\n", loopn);
				fflush(stdout);
			}
			loopn++;
			oldtime = newtime;
			usleep(1000);
		}
	}
	return 0;
}
