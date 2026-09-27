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
 * pl_phoenix_main.c — Phoenix entry point + host loop for Quakespasm: replaces
 * main_sdl.c. No SDL init; just COM_InitArgv -> Sys_Init -> Host_Init, then the
 * Host_Frame loop driven by Sys_DoubleTime.
 *
 * USE_SDL2 build (build-quakespasm-sdl-phoenix.py, the real-SDL variant) ONLY:
 * this file is shared with the proven flagship build, which does NOT define
 * USE_SDL2 and is therefore completely unaffected by every USE_SDL2 block below.
 * SDL_MAIN_HANDLED is defined before any SDL header is pulled in (quakedef.h
 * includes <SDL2/SDL.h> under -DNO_SDL_CONFIG -DUSE_SDL2) so SDL never rewrites
 * our main() to SDL_main. main() then does the global SDL_Init(0) that stock
 * main_sdl.c's Sys_InitSDL did; the compiled-in gl_vidsdl/in_sdl/snd_sdl call
 * SDL_InitSubSystem for their own subsystems as upstream. PL_VID_Shutdown /
 * PL_SetWindowIcon (stock hooks gl_vidsdl.c references, normally in the excluded
 * pl_linux.c) are provided here as no-ops.
 */
#ifdef USE_SDL2
#define SDL_MAIN_HANDLED
#endif

#include "quakedef.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* Shareware Quake needs far less than the upstream 256 MB default; use a modest
 * heap that Phoenix can reliably back, and memset it after malloc to force every
 * page committed/mapped up front (the hunk faulted at a low offset during BSP load,
 * i.e. untouched malloc pages weren't mapped). */
#define DEFAULT_MEMORY (96 * 1024 * 1024)

/* The host runs on the MAIN thread (not a pthread). Quake has large stack frames +
 * recursive renderers, so the main-thread stack is enlarged via PT_GNU_STACK in the
 * link (-z stack-size=... in phoenix-rtos-project/_user/rpi4-quake/Makefile). Running
 * on the main thread is deliberate: Mesa's glapi dispatch is TLS and the kernel sets up
 * TLS for the main thread (process.c), whereas GL on a libphoenix pthread faulted in the
 * dispatch (far=0x100030428) — identical GL ran on the main thread fine. */

static quakeparms_t parms;      /* host_parms (the pointer) is owned by host.c */

/* Discovered basedir (set by wait_for_gamedata): "<basedir>/id1/pak0.pak" is the data. */
static const char *g_basedir = "/usr/share/quake";

/* Wait (bounded) for the game data to be reachable before Host_Init, and discover WHERE
 * it lives. The data (id1/pak0.pak) is installed FHS-style under /usr/share/quake on the
 * NFS root (#46). Probe a few standard locations each poll and adopt whichever first
 * exposes id1/pak0.pak. The bounded retry also covers a syspage-launched process racing
 * the nfs-fs takeover (the root briefly resolves to the pre-takeover dummyfs) and the
 * libnfs first-read dircache ENOENT (#156): a later retry succeeds. */
static void wait_for_gamedata(void)
{
	/* RAM-staged tmpfs paths are probed FIRST (the load-time workaround): if id1/ has been
	 * staged into a RAM fs — e.g. `nfs-read-bench <nfs>/id1/pak0.pak stage /ramtmp/quake/id1/
	 * pak0.pak` — reads are ~20x faster than over NFS (measured: NFS random 4KiB 1.46 ms/read
	 * vs tmpfs 0.07 ms/read). Falls through to the FHS NFS dir when nothing is staged, so this
	 * is transparent + backward-compatible. */
	static const char *dflt[] = { "/ramtmp/quake", "/tmp/quake", "/usr/share/quake", "/opt/quake", "/" };
	const char *forced[1];
	const char **cands = dflt;
	int ncands = (int)(sizeof(dflt) / sizeof(dflt[0]));
	char path[80];
	int i, c, bp;

	/* An explicit `-basedir <dir>` wins over the search (point at a specific RAM stage, or
	 * force the NFS path for an A/B load benchmark). */
	bp = COM_CheckParm("-basedir");
	if (bp != 0 && bp + 1 < com_argc) {
		forced[0] = com_argv[bp + 1];
		cands = forced;
		ncands = 1;
	}
	for (i = 0; i < 360; i++) {     /* ~180 s — NFS mount + DHCP can be slow/variable (#156) */
		for (c = 0; c < ncands; c++) {
			FILE *f;
			snprintf(path, sizeof(path), "%s/id1/pak0.pak", cands[c]);
			f = fopen(path, "rb");
			if (f) {
				fclose(f);
				g_basedir = cands[c];
				Sys_Printf("quakespasm: found %s after %d tries (basedir=%s)\n",
				           path, i + 1, g_basedir);
				return;
			}
		}
		usleep(500000);
	}
	Sys_Printf("quakespasm: pak0.pak not found after wait (continuing; Host_Init will report)\n");
}

#ifdef USE_SDL2
/* Stock platform hooks gl_vidsdl.c calls (upstream live in pl_linux.c, which this
 * build excludes). No-ops: this port owns no SDL window icon and needs no extra
 * VID teardown beyond gl_vidsdl.c's own SDL_GL_DeleteContext/DestroyWindow. */
void PL_VID_Shutdown(void) {}
void PL_SetWindowIcon(void) {}
#endif

int main(int argc, char *argv[])
{
	double time, oldtime, newtime;
	struct timespec qs_t0;

	/* LINE-buffered stdout: each printf line is written to the shared UART console in one
	 * write() instead of per character, so our log lines no longer interleave character-by-
	 * character with the concurrently-running lwip process's output (which made boot messages
	 * like "Initializing QuakeSpasm" unreadable, one fragment per line). stderr stays unbuffered
	 * so crash/wedge diagnostics reach the UART immediately even on an early fault. */
	static char qs_stdout_buf[2048];
	setvbuf(stdout, qs_stdout_buf, _IOLBF, sizeof(qs_stdout_buf));
	setvbuf(stderr, NULL, _IONBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &qs_t0);
	printf("quakespasm: main() entered (argc=%d)\n", argc);

	host_parms = &parms;
	parms.basedir = "/usr/share/quake";    /* FHS data dir; wait_for_gamedata() refines it (#46) */
	parms.argc = argc;
	parms.argv = argv;
	parms.errstate = 0;

	COM_InitArgv(parms.argc, parms.argv);

	isDedicated = (COM_CheckParm("-dedicated") != 0);

#ifdef USE_SDL2
	/* Global SDL init, matching stock main_sdl.c's Sys_InitSDL (SDL_Init(0) with no
	 * subsystems; gl_vidsdl/in_sdl/snd_sdl SDL_InitSubSystem their own). SDL_SetMainReady
	 * tells SDL this app owns main() (paired with SDL_MAIN_HANDLED at the top). */
	SDL_SetMainReady();
	if (SDL_Init(0) < 0)
		Sys_Error("Couldn't init SDL: %s", SDL_GetError());
	atexit(SDL_Quit);
#endif

	Sys_Init();

	Sys_Printf("Initializing QuakeSpasm (Phoenix/V3D)\n");

	parms.memsize = DEFAULT_MEMORY;
	parms.membase = malloc(parms.memsize);
	if (!parms.membase)
		Sys_Error("Not enough memory free; check disk space\n");
	/* Touch every page so the whole hunk is committed/mapped (Phoenix does not
	 * demand-page large anonymous malloc: untouched pages translation-fault). */
	memset(parms.membase, 0, parms.memsize);
	Sys_Printf("quakespasm: heap %d MB committed at %p\n",
	           (int)(parms.memsize >> 20), parms.membase);

	wait_for_gamedata();
	parms.basedir = g_basedir;      /* whichever path exposed id1/pak0.pak */

	Sys_Printf("Host_Init\n");
	Host_Init();

	/* Load-time telemetry (main entry -> game data fully loaded), and an A/B benchmark hook:
	 * with `-loadbench` the process exits right after load instead of entering the render loop,
	 * so a single Pi cycle can time the load from two basedirs back-to-back (e.g. a RAM stage vs
	 * the NFS path) without a rendering game blocking the second run. */
	{
		struct timespec qs_t1;
		clock_gettime(CLOCK_MONOTONIC, &qs_t1);
		double load_s = (double)(qs_t1.tv_sec - qs_t0.tv_sec) + (double)(qs_t1.tv_nsec - qs_t0.tv_nsec) / 1e9;
		Sys_Printf("quakespasm: LOAD-TIME main->Host_Init = %.3f s (basedir=%s)\n", load_s, g_basedir);
		if (COM_CheckParm("-loadbench") != 0) {
			Sys_Printf("quakespasm: -loadbench set -> exiting after load\n");
			exit(0);
		}
	}

	/* Force the classic per-vertex water warp. r_oldwater defaults to 1 in this port
	 * (the modern warpimage path needs glCopyTexSubImage2D, unimplemented on V3D ->
	 * water samples RGB noise), but config.cfg is CVAR_ARCHIVE and a saved one on the
	 * rootfs sets r_oldwater "0" — which execs AFTER our default. Re-assert it here,
	 * after Host_Init has queued the config exec, so it wins regardless of the config. */
	Cbuf_AddText("r_oldwater 1\n");

	/* MP (#68): if id1/phoenix-connect.cfg exists (one line = a dedicated-server
	 * host/IP), connect to it at boot instead of the demo loop. Absent -> the
	 * unchanged attract loop. Lets the netboot harness drive a multiplayer join
	 * to the host server (scripts/quake-mp-server.sh) for diagnosing #68. */
	{
		char cpath[256], line[80];
		FILE *cf;
		snprintf(cpath, sizeof(cpath), "%s/id1/phoenix-connect.cfg", g_basedir);
		cf = fopen(cpath, "r");
		if (cf != NULL) {
			if (fgets(line, sizeof(line), cf) != NULL) {
				int n;
				for (n = 0; line[n] != '\0'; n++) {
					char c = line[n];
					if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
							(c >= 'A' && c <= 'Z') || c == '.' || c == '-')) {
						line[n] = '\0';
						break;
					}
				}
				if (line[0] != '\0') {
					char cmd[96];
					snprintf(cmd, sizeof(cmd), "connect %s\n", line);
					Sys_Printf("phoenix: boot connect -> %s\n", line);
					Cbuf_AddText(cmd);
				}
			}
			fclose(cf);
		}
	}

	/* SP (#68 diag / general): if id1/phoenix-map.cfg exists (one line = a map
	 * name, e.g. "start"), boot a single-player loopback map load instead of the
	 * demo loop. Used to compare the SP loopback map load against the MP join —
	 * both exercise CL_ParseServerInfo + Mod_ForName(maps/<name>.bsp). */
	{
		char mpath[256], line[80];
		FILE *mf;
		snprintf(mpath, sizeof(mpath), "%s/id1/phoenix-map.cfg", g_basedir);
		mf = fopen(mpath, "r");
		if (mf != NULL) {
			if (fgets(line, sizeof(line), mf) != NULL) {
				int n;
				for (n = 0; line[n] != '\0'; n++) {
					char c = line[n];
					if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
							(c >= 'A' && c <= 'Z') || c == '_')) {
						line[n] = '\0';
						break;
					}
				}
				if (line[0] != '\0') {
					char cmd[96];
					snprintf(cmd, sizeof(cmd), "map %s\n", line);
					Sys_Printf("phoenix: boot map (SP) -> %s\n", line);
					Cbuf_AddText(cmd);
				}
			}
			fclose(mf);
		}
	}

	/* Boot into the attract demo loop (cl_startdemos default = 1 -> demo1.dem, a recorded
	 * E1M3 walkthrough) as the no-input attract mode. Full single-player "map" loading also
	 * works now (server + QuakeC VM + loopback connect, see pl_phoenix_stubs.c net_drivers)
	 * — once /dev/kbd0 input lands, the menu's New Game path is functional. With the MMU
	 * TLB-flush fix the 3D frames render to the V3D, and with the 1MB NFS readmax the pak0
	 * load is faster. (The BSP/lightmap build is still CPU-bound with caches off (TD-16) —
	 * that is the remaining wall for fast 3D load.) */

	oldtime = Sys_DoubleTime();
	while (1) {
		newtime = Sys_DoubleTime();
		time = newtime - oldtime;
		Host_Frame(time);
		oldtime = newtime;
		usleep(1000);
	}
	return 0;
}
