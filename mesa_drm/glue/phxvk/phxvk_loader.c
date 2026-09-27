/*
 * phxvk: a static stand-in for the Vulkan loader (new GPU lane, M5).
 *
 * Phoenix-RTOS has no Vulkan loader and cannot dlopen() an ICD (libphoenix is not
 * PIC; its dlopen resolves no TLS relocations), so a Vulkan program links Mesa's
 * v3dv ICD statically (mesa-drm patch 0010) and asks this file for its entry
 * points where it would ask libvulkan's vkGetInstanceProcAddr(). With exactly one
 * driver there is nothing to dispatch between: every name resolves in the ICD
 * through its loader-interface entry point vk_icdGetInstanceProcAddr(), and Mesa's
 * handles are used directly (they already carry the loader magic a real loader
 * would check). What a loader would add and this does:
 *
 *  - the ICD interface negotiation, once, as a loader does before the first call
 *    (vk_icdNegotiateLoaderICDInterfaceVersion; Mesa accepts up to version 7);
 *  - vkGetInstanceProcAddr / vkGetDeviceProcAddr resolve to this file, so every
 *    later lookup passes here too.
 *
 * And, for the Pi logs (the UART is the only observer):
 *
 *  - one banner line naming the lane at the first call;
 *  - vkQueuePresentKHR is counted: a "phxvk: presents=N secs=S fps=F" line every
 *    2 s of presenting and one at exit, plus the first non-success VkResult of a
 *    present (a display WSI swapchain reports SUBOPTIMAL/OUT_OF_DATE/SURFACE_LOST
 *    there). Nothing else is intercepted; the count costs one clock read per
 *    present.
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "phxvk_loader.h"

/* The ICD's loader interface (Mesa: src/vulkan/runtime + src/broadcom/vulkan). */
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);

#define PHXVK_REPORT_NS 2000000000ull
#define PHXVK_ICD_INTERFACE_VERSION 7u

static struct {
	int started;
	uint32_t icd_version;
	PFN_vkGetDeviceProcAddr icd_get_device_proc_addr;
	PFN_vkQueuePresentKHR icd_queue_present;
	uint64_t presents, first_ns, last_report_ns;
	int reported_result;
} phxvk;


static void phxvk_say(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(1, line, ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1u);
	}
}


static uint64_t phxvk_now_ns(void)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}


static void phxvk_report(const char *when, uint64_t now)
{
	uint64_t ns = now - phxvk.first_ns;
	unsigned long long fps100 = (ns != 0u) ? (unsigned long long)(phxvk.presents * 100u * 1000000000ull / ns) : 0u;

	phxvk_say("phxvk: %s presents=%llu secs=%llu.%02llu fps=%llu.%02llu\n", when, (unsigned long long)phxvk.presents,
		(unsigned long long)(ns / 1000000000ull), (unsigned long long)(ns % 1000000000ull / 10000000ull), fps100 / 100u,
		fps100 % 100u);
}


static void phxvk_at_exit(void)
{
	if (phxvk.presents != 0u) {
		phxvk_report("exit", phxvk_now_ns());
	}
}


static VKAPI_ATTR VkResult VKAPI_CALL phxvk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	VkResult r = phxvk.icd_queue_present(queue, info);
	uint64_t now = phxvk_now_ns();

	if (phxvk.presents++ == 0u) {
		phxvk.first_ns = now;
		phxvk.last_report_ns = now;
		phxvk_say("phxvk: first present result=%d\n", (int)r);
	}
	else if (now - phxvk.last_report_ns >= PHXVK_REPORT_NS) {
		phxvk_report("run", now);
		phxvk.last_report_ns = now;
	}
	if ((r != VK_SUCCESS) && (phxvk.reported_result == 0)) {
		phxvk.reported_result = 1;
		phxvk_say("phxvk: present n=%llu result=%d (first non-success)\n", (unsigned long long)phxvk.presents, (int)r);
	}
	return r;
}


static PFN_vkVoidFunction phxvk_wrap_present(PFN_vkVoidFunction icd)
{
	if (icd == NULL) {
		return NULL;
	}
	phxvk.icd_queue_present = (PFN_vkQueuePresentKHR)icd;   /* one ICD: the same trampoline for every device */
	return (PFN_vkVoidFunction)phxvk_QueuePresentKHR;
}


static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL phxvk_GetDeviceProcAddr(VkDevice device, const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)phxvk_GetDeviceProcAddr;
	}
	if (phxvk.icd_get_device_proc_addr == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkQueuePresentKHR") == 0) {
		return phxvk_wrap_present(phxvk.icd_get_device_proc_addr(device, name));
	}
	return phxvk.icd_get_device_proc_addr(device, name);
}


static void phxvk_start(void)
{
	uint32_t v = PHXVK_ICD_INTERFACE_VERSION;
	VkResult r;

	phxvk.started = 1;
	phxvk_say("phxvk: new GPU lane -- Mesa 26.2 v3dv (static ICD) + VK_KHR_display + libdrm-phoenix "
		"-> rpi4-kms (card0) + rpi4-v3d-async (renderD128/card1)\n");
	r = vk_icdNegotiateLoaderICDInterfaceVersion(&v);
	phxvk.icd_version = v;
	phxvk_say("phxvk: ICD interface version %u (negotiate result=%d)\n", (unsigned)v, (int)r);
	(void)atexit(phxvk_at_exit);
}


VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL phxvk_GetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f;

	if (phxvk.started == 0) {
		phxvk_start();
	}
	if (name == NULL) {
		return NULL;
	}
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)phxvk_GetInstanceProcAddr;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		f = vk_icdGetInstanceProcAddr(instance, name);
		if (f == NULL) {
			return NULL;
		}
		phxvk.icd_get_device_proc_addr = (PFN_vkGetDeviceProcAddr)f;
		return (PFN_vkVoidFunction)phxvk_GetDeviceProcAddr;
	}
	f = vk_icdGetInstanceProcAddr(instance, name);
	if (strcmp(name, "vkQueuePresentKHR") == 0) {
		return phxvk_wrap_present(f);
	}
	return f;
}
