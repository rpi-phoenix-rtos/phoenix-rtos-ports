/*
 * stk-drm: process-level hooks of the SuperTuxKart clone on the full DRM stack
 * (SDL 2.30.12 KMSDRM + Mesa GBM/EGL/GLES + libdrm-phoenix). Nothing engine-side:
 * this file is linked only into supertuxkart-drm, never into libSDL2.a, so
 * quakespasm-drm and every other SDL user are unaffected.
 *
 * 1. One identifying line at process start (write(2), before any stdio), so a UART
 *    log names the lane: the shipped STK, stk-v3da and this clone print the same
 *    engine text otherwise.
 * 2. SDL's VIDEO and INPUT log categories at DEBUG, so KMSDRM reports its init steps
 *    (device, connector/CRTC counts, one "New DRM FB" per scan-out buffer) -- a
 *    dozen lines at start, nothing per frame (as quakespasm-drm).
 * 3. A frame counter: the program is linked with -Wl,--wrap=SDL_GL_SwapWindow, so
 *    Irrlicht's per-frame SDL_GL_SwapWindow() (COGLES2Driver::endScene) lands here
 *    first. Every V3D_FLIPSTAT_MS (default 5000) ms it prints
 *      stk-drm flipstat <N> frames in <T> ms = <X.XX> fps (total <M>)
 *    -- the old winsys' `flipstat ... (total ...)` shape, so
 *    scripts/flipstat-summary.sh reads it unchanged -- and a second line with the
 *    time spent inside the swap (KMSDRM: wait for the previous page flip +
 *    eglSwapBuffers + lock front buffer + drmModePageFlip), which separates
 *    present-path cost from render cost:
 *      stk-drm swapstat t=<ms since first swap> fr=<N> swap_us_avg=<a> swap_us_max=<m>
 *    V3D_FLIPSTAT=0 turns both off (same variable and meaning as the old winsys).
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

void __real_SDL_GL_SwapWindow(SDL_Window *window);
void __wrap_SDL_GL_SwapWindow(SDL_Window *window);

static const char stkdrm_banner[] =
	"stk-drm: new GPU lane -- SDL 2.30.12 KMSDRM + Mesa 26.2 GBM/EGL (GLES) + libdrm-phoenix "
	"-> rpi4-kms (card0) + rpi4-v3d-async (renderD128)\n";

static struct {
	int errfd;           /* private copy of fd 2, see stkdrm_start() */
	int state;           /* 0 = before the first swap, 1 = counting, 2 = off */
	unsigned window_ms;
	uint64_t start_us;   /* process start (constructor) */
	uint64_t first_us;   /* first swap */
	uint64_t win_t0;
	unsigned long total;
	unsigned long win_frames;
	uint64_t win_swap_us;
	uint64_t win_swap_max;
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


static void stkdrm_exit(void)
{
	if ((S.state == 1) && (S.total > 0u)) {
		out("stk-drm: exit after %lu swaps in %lu ms since the first swap\n", S.total,
			(unsigned long)((now_us() - S.first_us) / 1000u));
	}
}


__attribute__((constructor)) static void stkdrm_start(void)
{
	(void)write(1, stkdrm_banner, sizeof(stkdrm_banner) - 1u);
	/* STK's main() ends with fclose(stderr); fclose(stdout); -- descriptors 1 and 2
	 * are gone before the atexit handlers run, which silenced stkdrm_exit()'s line.
	 * Keep a private copy (above 2, close-on-exec) for everything out() prints. */
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
	S.first_us = t;
	S.win_t0 = t;
	if (window != NULL) {
		SDL_GetWindowSize(window, &w, &h);
		SDL_GL_GetDrawableSize(window, &dw, &dh);
	}
	out("stk-drm: first swap %lu ms after start: window %dx%d drawable %dx%d swap_interval %d flipstat %s\n",
		(unsigned long)((t - S.start_us) / 1000u), w, h, dw, dh, SDL_GL_GetSwapInterval(),
		(S.state == 1) ? "on" : "off");
	if (S.state == 1) {
		(void)atexit(stkdrm_exit);
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
	out("stk-drm flipstat %lu frames in %lu ms = %lu.%02lu fps (total %lu)\n", S.win_frames,
		(unsigned long)(dt / 1000u), cfps / 100u, cfps % 100u, S.total);
	out("stk-drm swapstat t=%lums fr=%lu swap_us_avg=%lu swap_us_max=%lu\n",
		(unsigned long)((t1 - S.first_us) / 1000u), S.win_frames,
		(unsigned long)(S.win_swap_us / S.win_frames), (unsigned long)S.win_swap_max);
	S.win_t0 = t1;
	S.win_frames = 0u;
	S.win_swap_us = 0u;
	S.win_swap_max = 0u;
}
