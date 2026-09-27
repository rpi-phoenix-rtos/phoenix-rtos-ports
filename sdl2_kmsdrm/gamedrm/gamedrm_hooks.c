/*
 * gamedrm: process-level hooks of the SDL2 GL game clones on the full DRM stack
 * (SDL 2.30.12 KMSDRM + Mesa GBM/EGL + libdrm-phoenix): quake2-drm (yQuake2, GLES3),
 * quake3-drm (quake3e) and quakespasm-drm (desktop GL). Nothing engine-side: linked only
 * into those clones, never into libSDL2.a, so stk-drm (own hooks) is unaffected.
 * Build with -DGAMEDRM_NAME='"quake2-drm"' -DGAMEDRM_API='"GLES"' (or "desktop GL").
 *
 * The stk-drm hooks (stkdrm/stkdrm_hooks.c) with the name as a parameter:
 *
 * 1. One identifying line at process start (write(2), before any stdio), so a UART log
 *    names the lane: the shipped engine and the clone print the same text otherwise.
 * 2. SDL's VIDEO and INPUT log categories at DEBUG, so KMSDRM reports its init steps
 *    (a dozen lines at start, nothing per frame).
 * 3. A frame counter: the clone is linked with -Wl,--wrap=SDL_GL_SwapWindow, so the
 *    engine's per-frame SDL_GL_SwapWindow() lands here first. Every V3D_FLIPSTAT_MS
 *    (default 5000) ms it prints
 *      <name> flipstat <N> frames in <T> ms = <X.XX> fps (total <M>)
 *    -- the old winsys' `flipstat ... (total ...)` shape, which scripts/run-showcase-gate.sh
 *    reads for its `frames` column and scripts/flipstat-summary.sh for fps -- and
 *      <name> swapstat t=<ms since first swap> fr=<N> swap_us_avg=<a> swap_us_max=<m>
 *    (time inside the swap: KMSDRM's wait for the previous flip + eglSwapBuffers + lock
 *    front buffer + drmModePageFlip). V3D_FLIPSTAT=0 turns both off (as on the old lane).
 *    KMSDRM itself prints nothing per frame, and neither engine prints its fps outside
 *    `timedemo`, so without this the gate would have no frames figure for the clone.
 * 4. GAMEDRM_EXIT_SECS=<N> (unset = never): at the first window boundary at least N s after
 *    the first swap, print `<name>: exit after ... (GAMEDRM_EXIT_SECS=N)` and _exit(0). The
 *    engines never exit on their own (a demo loop; quakespasm stays at the console after a
 *    timedemo), so without it a psh cycle can run only one of them, last; with it, two
 *    binaries can be compared in one boot (frame-pacing.md). _exit(), not exit(): no engine
 *    or stdio teardown on the way out, only the kernel closing the process's descriptors.
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

#ifndef GAMEDRM_NAME
#error "build with -DGAMEDRM_NAME='\"<clone name>\"'"
#endif
#ifndef GAMEDRM_API
#error "build with -DGAMEDRM_API='\"GLES\"' or '\"desktop GL\"'"
#endif

void __real_SDL_GL_SwapWindow(SDL_Window *window);
void __wrap_SDL_GL_SwapWindow(SDL_Window *window);

static const char gamedrm_banner[] =
	GAMEDRM_NAME ": new GPU lane -- SDL 2.30.12 KMSDRM + Mesa 26.2 GBM/EGL (" GAMEDRM_API ") + libdrm-phoenix "
	"-> rpi4-kms (card0) + rpi4-v3d-async (renderD128)\n";

static struct {
	int errfd;           /* private copy of fd 2, see gamedrm_start() */
	int state;           /* 0 = before the first swap, 1 = counting, 2 = off */
	unsigned window_ms;
	uint64_t start_us;   /* process start (constructor) */
	uint64_t first_us;   /* first swap */
	uint64_t win_t0;
	unsigned long total;
	unsigned long win_frames;
	uint64_t win_swap_us;
	uint64_t win_swap_max;
	unsigned exit_secs;  /* GAMEDRM_EXIT_SECS, 0 = never */
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


static void gamedrm_exit(void)
{
	if ((S.state == 1) && (S.total > 0u)) {
		out(GAMEDRM_NAME ": exit after %lu swaps in %lu ms since the first swap\n", S.total,
			(unsigned long)((now_us() - S.first_us) / 1000u));
	}
}


__attribute__((constructor)) static void gamedrm_start(void)
{
	(void)write(1, gamedrm_banner, sizeof(gamedrm_banner) - 1u);
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
	const char *e = getenv("V3D_FLIPSTAT");
	int w = 0, h = 0, dw = 0, dh = 0;

	S.state = ((e != NULL) && (e[0] == '0')) ? 2 : 1;
	e = getenv("V3D_FLIPSTAT_MS");
	S.window_ms = ((e != NULL) && (atoi(e) > 0)) ? (unsigned)atoi(e) : 5000u;
	e = getenv("GAMEDRM_EXIT_SECS");
	S.exit_secs = ((e != NULL) && (atoi(e) > 0)) ? (unsigned)atoi(e) : 0u;
	S.first_us = t;
	S.win_t0 = t;
	if (window != NULL) {
		SDL_GetWindowSize(window, &w, &h);
		SDL_GL_GetDrawableSize(window, &dw, &dh);
	}
	out(GAMEDRM_NAME ": first swap %lu ms after start: window %dx%d drawable %dx%d swap_interval %d flipstat %s\n",
		(unsigned long)((t - S.start_us) / 1000u), w, h, dw, dh, SDL_GL_GetSwapInterval(),
		(S.state == 1) ? "on" : "off");
	if (S.state == 1) {
		(void)atexit(gamedrm_exit);
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
	out(GAMEDRM_NAME " flipstat %lu frames in %lu ms = %lu.%02lu fps (total %lu)\n", S.win_frames,
		(unsigned long)(dt / 1000u), cfps / 100u, cfps % 100u, S.total);
	out(GAMEDRM_NAME " swapstat t=%lums fr=%lu swap_us_avg=%lu swap_us_max=%lu\n",
		(unsigned long)((t1 - S.first_us) / 1000u), S.win_frames,
		(unsigned long)(S.win_swap_us / S.win_frames), (unsigned long)S.win_swap_max);
	S.win_t0 = t1;
	S.win_frames = 0u;
	S.win_swap_us = 0u;
	S.win_swap_max = 0u;
	if ((S.exit_secs != 0u) && ((t1 - S.first_us) >= (uint64_t)S.exit_secs * 1000000u)) {
		out(GAMEDRM_NAME ": exit after %lu swaps in %lu ms since the first swap (GAMEDRM_EXIT_SECS=%u)\n", S.total,
			(unsigned long)((t1 - S.first_us) / 1000u), S.exit_secs);
		_exit(0);
	}
}
