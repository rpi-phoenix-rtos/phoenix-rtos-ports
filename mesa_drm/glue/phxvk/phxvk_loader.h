/*
 * phxvk: a static stand-in for the Vulkan loader (new GPU lane, M5).
 * Use phxvk_GetInstanceProcAddr() wherever a program would take libvulkan's
 * vkGetInstanceProcAddr(); link the v3dv ICD archive (mesa-drm --vulkan).
 *
 * Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PHXVK_LOADER_H
#define PHXVK_LOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL phxvk_GetInstanceProcAddr(VkInstance instance, const char *name);

#ifdef __cplusplus
}
#endif

#endif
