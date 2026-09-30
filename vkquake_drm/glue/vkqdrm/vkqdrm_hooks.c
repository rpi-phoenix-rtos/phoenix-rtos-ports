/*
 * vkquake-drm: process-level hooks of the vkQuake clone on the full DRM stack
 * (SDL 2.30.12 KMSDRM with its stock Vulkan path + Mesa 26.2 v3dv as a static ICD +
 * VK_KHR_display + libdrm-phoenix). Nothing engine-side: linked only into vkquake-drm.
 *
 * 1. One identifying line at process start (write(2), before any stdio), and SDL's VIDEO
 *    and INPUT log categories at DEBUG (KMSDRM init steps; nothing per frame).
 *
 * 2. The Vulkan "library" SDL loads. SDL_Vulkan_LoadLibrary() -> KMSDRM_Vulkan_LoadLibrary()
 *    does SDL_LoadObject("libvulkan.so.1") + SDL_LoadFunction("vkGetInstanceProcAddr"). Phoenix
 *    has no Vulkan loader and this SDL has only the dummy loadso (no dlopen), so the program is
 *    linked with -Wl,--wrap=SDL_LoadObject,--wrap=SDL_LoadFunction,--wrap=SDL_UnloadObject and
 *    answers those two calls itself: the "handle" is a sentinel, and vkGetInstanceProcAddr is
 *    vkqdrm_GetInstanceProcAddr below, i.e. phxvk (tools/gpu-lane/vulkan-drm/phxvk, the static
 *    loader stand-in in front of Mesa's vk_icdGetInstanceProcAddr). Every other name goes to
 *    SDL's own implementation. No SDL source change; vkQuake takes the same pointer back from
 *    SDL_Vulkan_GetVkGetInstanceProcAddr(), so SDL and the engine share one dispatch.
 *
 * 3. A frame counter in the old winsys' shape. vkQuake presents with vkQueuePresentKHR fetched
 *    through vkGetDeviceProcAddr; both lookups pass here, so the present is wrapped (on top of
 *    phxvk's own `phxvk: run presents=...` counter). Every V3D_FLIPSTAT_MS (default 5000) ms:
 *      vkquake-drm flipstat <N> frames in <T> ms = <X.XX> fps (total <M>)
 *      vkquake-drm presentstat t=<ms since first present> fr=<N> present_us_avg=<a> present_us_max=<m>
 *    The first line is what scripts/run-showcase-gate.sh reads for its `frames` column and
 *    scripts/flipstat-summary.sh for fps; vkQuake itself prints no fps on the console (only the
 *    on-screen scr_showfps). V3D_FLIPSTAT=0 turns both off.
 *
 * 4. Where the CPU waits. The four calls in which a frame can block on the GPU or the display
 *    (vkAcquireNextImageKHR, vkQueueSubmit, vkWaitForFences, vkWaitForPresent2KHR) are wrapped
 *    at the same two lookups and timed; with the lines of 3, once per window:
 *      vkquake-drm waitstat fr=<N> acquire=<calls>/<us_avg>/<us_max> submit=... fence=... pwait=...
 *    vkQuake runs its end-of-frame (acquire, submit, present) as a task, so these times are per
 *    call, not a partition of the frame (docs/gpu-new-lane/vkquake-perf.md).
 *
 * The vk* commands vkQuake calls as link symbols go through generated trampolines
 * (gen-vk-trampolines.py) that resolve with vkqdrm_GetInstanceProcAddr as well.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL_loadso.h>
#include <SDL2/SDL_log.h>

#include "phxvk_loader.h"

void *__real_SDL_LoadObject(const char *sofile);
void *__real_SDL_LoadFunction(void *handle, const char *name);
void __real_SDL_UnloadObject(void *handle);
void *__wrap_SDL_LoadObject(const char *sofile);
void *__wrap_SDL_LoadFunction(void *handle, const char *name);
void __wrap_SDL_UnloadObject(void *handle);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkqdrm_GetInstanceProcAddr(VkInstance instance, const char *name);

static const char vkqdrm_banner[] =
	"vkquake-drm: Phoenix-RTOS GPU stack -- SDL 2.30.12 KMSDRM (Vulkan, VK_KHR_display) + Mesa 26.2 v3dv (static ICD via phxvk) "
	"+ libdrm-phoenix -> rpi4-kms (card0) + rpi4-v3d-async (renderD128)\n";

static const char vkqdrm_vulkan_handle;   /* the sentinel "library handle" */

static struct {
	int errfd;
	int state;           /* 0 = before the first present, 1 = counting, 2 = off */
	unsigned window_ms;
	uint64_t start_us, first_us, win_t0;
	unsigned long total, win_frames;
	uint64_t win_us, win_max;
	PFN_vkGetDeviceProcAddr gdpa;   /* phxvk's */
	PFN_vkQueuePresentKHR present;  /* phxvk's (counting) wrapper of the ICD's */
	int said_loadso;
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


__attribute__((constructor)) static void vkqdrm_start(void)
{
	(void)write(1, vkqdrm_banner, sizeof(vkqdrm_banner) - 1u);
	S.errfd = fcntl(2, F_DUPFD_CLOEXEC, 3);
	if (S.errfd < 0) {
		S.errfd = 2;
	}
	SDL_LogSetPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_DEBUG);
	SDL_LogSetPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
	S.start_us = now_us();
}


/* --- 2. the Vulkan "library" ---------------------------------------------------------- */

static int is_vulkan_library(const char *sofile)
{
	return (sofile != NULL) && ((strcmp(sofile, "libvulkan.so.1") == 0) || (strcmp(sofile, "libvulkan.so") == 0));
}


void *__wrap_SDL_LoadObject(const char *sofile)
{
	if (is_vulkan_library(sofile)) {
		if (S.said_loadso == 0) {
			S.said_loadso = 1;
			out("vkquake-drm: SDL asked for %s -> the linked-in v3dv ICD (phxvk)\n", sofile);
		}
		return (void *)&vkqdrm_vulkan_handle;
	}
	return __real_SDL_LoadObject(sofile);
}


void *__wrap_SDL_LoadFunction(void *handle, const char *name)
{
	if (handle == (void *)&vkqdrm_vulkan_handle) {
		if ((name != NULL) && (strcmp(name, "vkGetInstanceProcAddr") == 0)) {
			return (void *)vkqdrm_GetInstanceProcAddr;
		}
		SDL_SetError("vkquake-drm: the linked-in Vulkan exports only vkGetInstanceProcAddr, not %s",
			(name != NULL) ? name : "(null)");
		return NULL;
	}
	return __real_SDL_LoadFunction(handle, name);
}


void __wrap_SDL_UnloadObject(void *handle)
{
	if (handle == (void *)&vkqdrm_vulkan_handle) {
		return;
	}
	__real_SDL_UnloadObject(handle);
}


/* --- 4. wait timing (declared before 3, which prints it) ------------------------------------ */

enum { W_ACQUIRE, W_SUBMIT, W_FENCE, W_PWAIT, W_N };

static const char *const w_name[W_N] = { "vkAcquireNextImageKHR", "vkQueueSubmit", "vkWaitForFences", "vkWaitForPresent2KHR" };
static const char *const w_tag[W_N] = { "acquire", "submit", "fence", "pwait" };

static struct {
	PFN_vkVoidFunction real;   /* the ICD's (through phxvk), one for every device */
	unsigned long n;
	uint64_t us, max;
} W[W_N];


static void w_add(int k, uint64_t t0)
{
	uint64_t d;

	if (S.state != 1) {
		return;
	}
	d = now_us() - t0;
	(void)__atomic_add_fetch(&W[k].n, 1u, __ATOMIC_RELAXED);
	(void)__atomic_add_fetch(&W[k].us, d, __ATOMIC_RELAXED);
	if (d > W[k].max) {
		W[k].max = d;   /* racy between threads: a maximum, not an exact one */
	}
}


static VKAPI_ATTR VkResult VKAPI_CALL vkqdrm_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
	uint64_t t0 = now_us();
	VkResult r = ((PFN_vkAcquireNextImageKHR)W[W_ACQUIRE].real)(device, swapchain, timeout, semaphore, fence, index);

	w_add(W_ACQUIRE, t0);
	return r;
}


static VKAPI_ATTR VkResult VKAPI_CALL vkqdrm_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
	uint64_t t0 = now_us();
	VkResult r = ((PFN_vkQueueSubmit)W[W_SUBMIT].real)(queue, count, submits, fence);

	w_add(W_SUBMIT, t0);
	return r;
}


static VKAPI_ATTR VkResult VKAPI_CALL vkqdrm_WaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all,
	uint64_t timeout)
{
	uint64_t t0 = now_us();
	VkResult r = ((PFN_vkWaitForFences)W[W_FENCE].real)(device, count, fences, all, timeout);

	w_add(W_FENCE, t0);
	return r;
}


static VKAPI_ATTR VkResult VKAPI_CALL vkqdrm_WaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain,
	const VkPresentWait2InfoKHR *info)
{
	uint64_t t0 = now_us();
	VkResult r = ((PFN_vkWaitForPresent2KHR)W[W_PWAIT].real)(device, swapchain, info);

	w_add(W_PWAIT, t0);
	return r;
}


static PFN_vkVoidFunction wrap_timed(const char *name, PFN_vkVoidFunction f)
{
	static const PFN_vkVoidFunction wrapper[W_N] = {
		(PFN_vkVoidFunction)vkqdrm_AcquireNextImageKHR, (PFN_vkVoidFunction)vkqdrm_QueueSubmit,
		(PFN_vkVoidFunction)vkqdrm_WaitForFences, (PFN_vkVoidFunction)vkqdrm_WaitForPresent2KHR,
	};
	int k;

	for (k = 0; k < W_N; k++) {
		if (strcmp(name, w_name[k]) == 0) {
			if (f == NULL) {
				return NULL;
			}
			W[k].real = f;
			return wrapper[k];
		}
	}
	return f;
}


static void waitstat_line(unsigned long frames)
{
	char buf[240];
	size_t len;
	int k, n;

	n = snprintf(buf, sizeof(buf), "vkquake-drm waitstat fr=%lu", frames);
	len = ((n > 0) && ((size_t)n < sizeof(buf))) ? (size_t)n : 0u;
	for (k = 0; k < W_N; k++) {
		unsigned long c = __atomic_exchange_n(&W[k].n, 0u, __ATOMIC_RELAXED);
		uint64_t us = __atomic_exchange_n(&W[k].us, 0u, __ATOMIC_RELAXED);

		n = snprintf(buf + len, sizeof(buf) - len, " %s=%lu/%lu/%lu", w_tag[k], c,
			(c != 0u) ? (unsigned long)(us / c) : 0ul, (unsigned long)W[k].max);
		W[k].max = 0u;
		if ((n > 0) && ((size_t)n < sizeof(buf) - len)) {
			len += (size_t)n;
		}
	}
	out("%s (calls/us_avg/us_max)\n", buf);
}


/* --- 3. the present counter -------------------------------------------------------------- */

static void vkqdrm_exit(void)
{
	if ((S.state == 1) && (S.total > 0u)) {
		out("vkquake-drm: exit after %lu presents in %lu ms since the first present\n", S.total,
			(unsigned long)((now_us() - S.first_us) / 1000u));
	}
}


static void first_present(uint64_t t)
{
	const char *e = getenv("V3D_FLIPSTAT");

	S.state = ((e != NULL) && (e[0] == '0')) ? 2 : 1;
	e = getenv("V3D_FLIPSTAT_MS");
	S.window_ms = ((e != NULL) && (atoi(e) > 0)) ? (unsigned)atoi(e) : 5000u;
	S.first_us = t;
	S.win_t0 = t;
	out("vkquake-drm: first present %lu ms after start, flipstat %s\n", (unsigned long)((t - S.start_us) / 1000u),
		(S.state == 1) ? "on" : "off");
	if (S.state == 1) {
		(void)atexit(vkqdrm_exit);
	}
}


static VKAPI_ATTR VkResult VKAPI_CALL vkqdrm_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	uint64_t t0 = now_us(), t1, dt, us;
	unsigned long cfps;
	VkResult r;

	if (S.state == 0) {
		first_present(t0);
	}
	r = S.present(queue, info);
	if (S.state != 1) {
		return r;
	}
	t1 = now_us();
	us = t1 - t0;
	S.total++;
	S.win_frames++;
	S.win_us += us;
	if (us > S.win_max) {
		S.win_max = us;
	}
	dt = t1 - S.win_t0;
	if (dt >= (uint64_t)S.window_ms * 1000u) {
		cfps = (unsigned long)((S.win_frames * 100000000ull + dt / 2u) / dt);
		out("vkquake-drm flipstat %lu frames in %lu ms = %lu.%02lu fps (total %lu)\n", S.win_frames,
			(unsigned long)(dt / 1000u), cfps / 100u, cfps % 100u, S.total);
		out("vkquake-drm presentstat t=%lums fr=%lu present_us_avg=%lu present_us_max=%lu\n",
			(unsigned long)((t1 - S.first_us) / 1000u), S.win_frames, (unsigned long)(S.win_us / S.win_frames),
			(unsigned long)S.win_max);
		waitstat_line(S.win_frames);
		S.win_t0 = t1;
		S.win_frames = 0u;
		S.win_us = 0u;
		S.win_max = 0u;
	}
	return r;
}


static PFN_vkVoidFunction wrap_present(PFN_vkVoidFunction phx)
{
	if (phx == NULL) {
		return NULL;
	}
	S.present = (PFN_vkQueuePresentKHR)phx;   /* one ICD, one phxvk wrapper for every device */
	return (PFN_vkVoidFunction)vkqdrm_QueuePresentKHR;
}


static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkqdrm_GetDeviceProcAddr(VkDevice device, const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)vkqdrm_GetDeviceProcAddr;
	}
	if (S.gdpa == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkQueuePresentKHR") == 0) {
		return wrap_present(S.gdpa(device, name));
	}
	return wrap_timed(name, S.gdpa(device, name));
}


VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkqdrm_GetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f;

	if (name == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		(void)phxvk_GetInstanceProcAddr(instance, name);   /* phxvk's banner + ICD negotiation, once */
		return (PFN_vkVoidFunction)vkqdrm_GetInstanceProcAddr;
	}
	f = phxvk_GetInstanceProcAddr(instance, name);
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		if (f == NULL) {
			return NULL;
		}
		S.gdpa = (PFN_vkGetDeviceProcAddr)f;
		return (PFN_vkVoidFunction)vkqdrm_GetDeviceProcAddr;
	}
	if (strcmp(name, "vkQueuePresentKHR") == 0) {
		return wrap_present(f);
	}
	return wrap_timed(name, f);
}
