/*
 * gamewl: process-level hooks of the WINDOWED SDL2 GL game clones (M8): quakespasm-wl,
 * quake2-wl (yquake2-wl), quake3-wl (quake3e-wl). SDL 2.30.12 with its Wayland video driver
 * (KMSDRM kept as the fallback) + Mesa 26.2 EGL on the wayland platform + libdrm-phoenix:
 * the game renders on rpi4-v3d-async (renderD128) into wl_buffers that the compositor
 * (labwc) composites or scans out. Nothing engine-side: linked only into those clones.
 * Build with -DGAMEWL_NAME='"quakespasm-wl"' -DGAMEWL_API='"desktop GL"' (or "GLES").
 *
 * gamedrm/gamedrm_hooks.c of tools/gpu-lane/sdl2-drm (the -drm clones) with the banner naming
 * the windowed stack and the first-swap line naming SDL's video driver:
 *
 * 1. One identifying line at process start (write(2), before any stdio), so a UART log
 *    names the build: the shipped engine and the clone print the same text otherwise.
 * 2. SDL's VIDEO and INPUT log categories at DEBUG, so the video driver reports its init
 *    steps (a few lines at start, nothing per frame).
 * 3. A frame counter: the clone is linked with -Wl,--wrap=SDL_GL_SwapWindow, so the
 *    engine's per-frame SDL_GL_SwapWindow() lands here first. The first swap prints
 *      <name>: first swap ... video_driver <wayland|KMSDRM> window WxH drawable WxH
 *              <windowed|fullscreen> context <GL|GLES> M.m swap_interval N flipstat on
 *    and every V3D_FLIPSTAT_MS (default 5000) ms
 *      <name> flipstat <N> frames in <T> ms = <X.XX> fps (total <M>)
 *    -- the shape scripts/run-showcase-gate.sh and scripts/flipstat-summary.sh read -- and
 *      <name> swapstat t=<ms since first swap> fr=<N> swap_us_avg=<a> swap_us_max=<m>
 *    (time inside the swap: on Wayland eglSwapBuffers + SDL's wait for the compositor's
 *    frame callback when swap_interval is 1). V3D_FLIPSTAT=0 turns both off.
 * 4. GAMEWL_EXIT_SECS=<N> (unset = never): at the first window boundary at least N s after
 *    the first swap, print `<name>: exit after ... (GAMEWL_EXIT_SECS=N)` and _exit(0).
 *    A clean quit is SIGTERM instead: SDL turns it into SDL_QUIT, which the engines
 *    handle with their own shutdown (quakespasm: Sys_Quit).
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL_log.h>
#include <SDL2/SDL_video.h>

#ifndef GAMEWL_NAME
#error "build with -DGAMEWL_NAME='\"<clone name>\"'"
#endif
#ifndef GAMEWL_API
#error "build with -DGAMEWL_API='\"GLES\"' or '\"desktop GL\"'"
#endif

void __real_SDL_GL_SwapWindow(SDL_Window *window);
void __wrap_SDL_GL_SwapWindow(SDL_Window *window);

static const char gamewl_banner[] =
	GAMEWL_NAME ": windowed GPU game -- SDL 2.30.12 Wayland (KMSDRM fallback) + Mesa 26.2 EGL wayland (" GAMEWL_API
	") + libdrm-phoenix -> rpi4-v3d-async (renderD128), presented by the Wayland compositor\n";

static struct {
	int errfd;           /* private copy of fd 2, see gamewl_start() */
	int state;           /* 0 = before the first swap, 1 = counting, 2 = off */
	unsigned window_ms;
	uint64_t start_us;   /* process start (constructor) */
	uint64_t first_us;   /* first swap */
	uint64_t win_t0;
	unsigned long total;
	unsigned long win_frames;
	uint64_t win_swap_us;
	uint64_t win_swap_max;
	unsigned exit_secs;  /* GAMEWL_EXIT_SECS, 0 = never */
} S;


static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(S.errfd, buf, ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1u);
	}
}


static void gamewl_exit(void)
{
	if ((S.state == 1) && (S.total > 0u)) {
		out(GAMEWL_NAME ": exit after %lu swaps in %lu ms since the first swap\n", S.total,
			(unsigned long)((now_us() - S.first_us) / 1000u));
	}
}


__attribute__((constructor)) static void gamewl_start(void)
{
	(void)write(1, gamewl_banner, sizeof(gamewl_banner) - 1u);
	/* A private close-on-exec copy of fd 2 (above 2) for everything out() prints, so the
	 * exit line survives an engine that closes its standard streams before exit. */
	S.errfd = fcntl(2, F_DUPFD_CLOEXEC, 3);
	if (S.errfd < 0) {
		S.errfd = 2;
	}
	SDL_LogSetPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_DEBUG);
	SDL_LogSetPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
	S.start_us = now_us();
}


static void first_swap(SDL_Window *window, uint64_t t)
{
	const char *e = getenv("V3D_FLIPSTAT"), *d;
	int w = 0, h = 0, dw = 0, dh = 0, prof = 0, maj = 0, mnr = 0;
	Uint32 fl = 0u;

	S.state = ((e != NULL) && (e[0] == '0')) ? 2 : 1;
	e = getenv("V3D_FLIPSTAT_MS");
	S.window_ms = ((e != NULL) && (atoi(e) > 0)) ? (unsigned)atoi(e) : 5000u;
	e = getenv("GAMEWL_EXIT_SECS");
	S.exit_secs = ((e != NULL) && (atoi(e) > 0)) ? (unsigned)atoi(e) : 0u;
	S.first_us = t;
	S.win_t0 = t;
	if (window != NULL) {
		SDL_GetWindowSize(window, &w, &h);
		SDL_GL_GetDrawableSize(window, &dw, &dh);
	}
	d = SDL_GetCurrentVideoDriver();
	if (window != NULL) {
		fl = SDL_GetWindowFlags(window);
	}
	(void)SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &prof);
	(void)SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &maj);
	(void)SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &mnr);
	out(GAMEWL_NAME ": first swap %lu ms after start: video_driver %s window %dx%d drawable %dx%d %s "
		"context %s %d.%d swap_interval %d flipstat %s\n",
		(unsigned long)((t - S.start_us) / 1000u), (d != NULL) ? d : "none", w, h, dw, dh,
		((fl & SDL_WINDOW_FULLSCREEN) != 0u) ? "fullscreen" : "windowed",
		(prof == SDL_GL_CONTEXT_PROFILE_ES) ? "GLES" : "GL", maj, mnr, SDL_GL_GetSwapInterval(),
		(S.state == 1) ? "on" : "off");
	if (S.state == 1) {
		(void)atexit(gamewl_exit);
	}
}


void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
	uint64_t t0 = now_us(), t1, dt, sw;
	unsigned long cfps;

	if (S.state == 0) {
		first_swap(window, t0);
	}
	__real_SDL_GL_SwapWindow(window);
	if (S.state != 1) {
		return;
	}

	t1 = now_us();
	sw = t1 - t0;
	S.total++;
	S.win_frames++;
	S.win_swap_us += sw;
	if (sw > S.win_swap_max) {
		S.win_swap_max = sw;
	}
	dt = t1 - S.win_t0;
	if (dt < (uint64_t)S.window_ms * 1000u) {
		return;
	}
	cfps = (unsigned long)((S.win_frames * 100000000ull + dt / 2u) / dt);
	out(GAMEWL_NAME " flipstat %lu frames in %lu ms = %lu.%02lu fps (total %lu)\n", S.win_frames,
		(unsigned long)(dt / 1000u), cfps / 100u, cfps % 100u, S.total);
	out(GAMEWL_NAME " swapstat t=%lums fr=%lu swap_us_avg=%lu swap_us_max=%lu\n",
		(unsigned long)((t1 - S.first_us) / 1000u), S.win_frames,
		(unsigned long)(S.win_swap_us / S.win_frames), (unsigned long)S.win_swap_max);
	S.win_t0 = t1;
	S.win_frames = 0u;
	S.win_swap_us = 0u;
	S.win_swap_max = 0u;
	if ((S.exit_secs != 0u) && ((t1 - S.first_us) >= (uint64_t)S.exit_secs * 1000000u)) {
		out(GAMEWL_NAME ": exit after %lu swaps in %lu ms since the first swap (GAMEWL_EXIT_SECS=%u)\n", S.total,
			(unsigned long)((t1 - S.first_us) / 1000u), S.exit_secs);
		_exit(0);
	}
}
