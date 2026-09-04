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
 * pl_phoenix_vk_vid.c — Phoenix-RTOS / V3DV Vulkan video shim for vkQuake.
 *
 * Replaces upstream gl_vidsdl.c (which is SDL_Vulkan + VK_KHR_swapchain WSI). On
 * Phoenix the V3DV ICD is loader-less and has NO WSI/swapchain extension, so this shim
 * brings Vulkan up the same way the HW-proven v3dv_harness.c does:
 *
 *   vkCreateInstance  -> publish g_vk_instance   (load-bearing for vk_trampolines.c)
 *   vkEnumeratePhysicalDevices / vkCreateDevice -> publish g_vk_device + vulkan_globals.device
 *   vkGetDeviceQueue  -> vulkan_globals.queue
 *
 * and presents to /dev/fb0 via the v3d winsys scanout (v3d_phoenix_set_scanout), NOT a
 * VkSwapchainKHR — exactly the Tier-4a/4b path the harness proved lands pixels on HDMI.
 * That keeps the link closed: pulling in gl_vidsdl's present chain would reintroduce
 * vkAcquireNextImageKHR / vkQueuePresentKHR (WSI the ICD lacks) as fresh undefineds.
 *
 * SCOPE (this session, host-side link): this shim DEFINES every vid/render symbol the
 * engine references at link (the 26 of bucket C), publishes the global instance/device
 * for the trampolines, and populates the vulkan_globals fields the renderer reads, so
 * the link closes to 0 undefined and the struct is runtime-plausible for the main agent.
 * The per-frame GL_BeginRendering/GL_EndRendering are kept MINIMAL-but-real (command
 * buffer begin -> submit -> scanout). The full render-target/render-pass/pipeline build
 * lives in the engine's own gl_rmisc.c R_Create* path (already compiled); this shim only
 * supplies device/queue/formats/dispatch-pointers it keys off.
 *
 * NOT YET RUNTIME-COMPLETE (flagged for the main agent's on-HW Tier-1 work):
 *   - the swapchain images vulkan_globals expects are faked to a single fb0-backed image;
 *     the renderer's NUM_COLOR_BUFFERS/double-buffer assumptions may need adjustment.
 *   - V3DV-feature steering: r_usesops / screen_effects_sops / ray_query are forced off
 *     (V3D has no subgroup-size-control / RT); compute-lightmap path left to the engine's
 *     own fallback. These are runtime knobs, not link symbols.
 *
 * Copyright 2026 Phoenix Systems  %LICENSE%
 */
#include "quakedef.h"
#include "glquake.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <phoenix/fbcon.h> /* FBCONSETMODE / FBCON_DISABLED|ENABLED — silence the HDMI text console */

/* ---- the global instance/device the trampoline layer (vk_trampolines.c) resolves
 *      every direct vk* call against. Published right after create, as the harness does. */
extern VkInstance g_vk_instance;
extern VkDevice	  g_vk_device;

/* The one ICD entry the loader-less client needs; vk_icd_link.c aliases it to
 * v3dv_GetInstanceProcAddr. Global cmds: NULL instance. Instance cmds: g_vk_instance.
 * Device cmds: vkGetDeviceProcAddr(g_vk_device, ...). (vk_trampolines.c enforces this.) */
extern PFN_vkVoidFunction vkGetInstanceProcAddr (VkInstance instance, const char *pName);

/* v3d winsys scanout (libv3d-phoenix.a) — the fb0 present path the harness proved. */
extern void v3d_phoenix_set_scanout (uint32_t pa, uint32_t bytes);
extern void v3d_phoenix_set_next_scanout (void);

/* ============================ globals the engine references ============================ */
/* (a) GENUINELY UNDEFINED — this shim must DEFINE them (verified vs undefined-symbols.txt) */
viddef_t	  vid;					   /* global video state (vid.h) */
modestate_t	  modestate = MS_UNINIT;
task_handle_t prev_end_rendering_task = INVALID_TASK_HANDLE;

/* vid_* cvars — copied verbatim (names + defaults + flags) from gl_vidsdl.c so the engine's
 * Cvar_RegisterVariable(&vid_gamma) etc. (in gl_rmisc.c / view.c) bind the same objects.
 * NOTE the cvar *names* "gamma"/"contrast" differ from the C identifiers (upstream quirk). */
cvar_t vid_palettize   = {"vid_palettize", "0", CVAR_ARCHIVE};
cvar_t vid_filter	   = {"vid_filter", "0", CVAR_ARCHIVE};
cvar_t vid_anisotropic = {"vid_anisotropic", "0", CVAR_ARCHIVE};
cvar_t vid_fsaa		   = {"vid_fsaa", "0", CVAR_ARCHIVE};
cvar_t vid_fsaamode	   = {"vid_fsaamode", "0", CVAR_ARCHIVE};
cvar_t vid_gamma	   = {"gamma", "0.9", CVAR_ARCHIVE};
cvar_t vid_contrast	   = {"contrast", "1.4", CVAR_ARCHIVE};
cvar_t r_usesops	   = {"r_usesops", "1", CVAR_ARCHIVE};

/* ================================ local Vulkan handles ================================ */
static VkPhysicalDevice phys_device = VK_NULL_HANDLE;
static VkQueue			gfx_queue	= VK_NULL_HANDLE;

/* fb0 scanout geometry (discovered in VID_Init). */
typedef struct {
	unsigned short	   width, height, bpp, pitch;
	unsigned long long smemlen, framebuffer;
} rpi4fb_mode_t;
static rpi4fb_mode_t fb_mode;
static int			 fb_ready = 0;

/* ---- no-WSI render resources (this shim's GL_CreateRenderResources equivalent) ----
 * vkQuake's upstream render path is a WSI swapchain with double-buffered primary + a dozen
 * secondary command-buffer contexts, OIT/WBOIT/post-process render passes, etc. The V3DV ICD
 * has no WSI; we render into the SINGLE fb0-backed scanout VkImage (the Tier-4b harness proved
 * loadOp=CLEAR -> storeOp=STORE into a LINEAR scanout image lands on HDMI). So this collapses the
 * whole machinery to: one primary command buffer + one single-attachment UI render pass + one
 * framebuffer wrapping the fb0 scanout image. SCR_DrawGUI records the 2D/console draws straight
 * into that primary cb (no secondary cbs, no vkCmdExecuteCommands), and the "present" is the
 * scanout image itself. World/3D rendering (V_RenderView) is the documented next blocker. */
static VkCommandPool   gfx_command_pool	  = VK_NULL_HANDLE;
static VkCommandBuffer frame_cb			  = VK_NULL_HANDLE;
/* vkQuake records into PCBX_NUM PRIMARY command-buffer contexts per frame (accel / lightmap /
 * warp / render-passes) and submits them together, RENDER_PASSES last, in one vkQueueSubmit — the
 * in-submit order IS the cross-pass sync. We allocate all PCBX_NUM primaries; frame_cb aliases the
 * RENDER_PASSES primary (the one carrying the color+depth render pass + the 2D/world draws). The
 * PCBX_UPDATE_WARP primary now carries the real turbulent-surface (water/lava/slime/teleporter)
 * compute warp — R_UpdateWarpTextures dispatches cs_tex_warp into it each frame. The remaining
 * primaries (accel / GPU lightmap) stay empty for now (GPU lightmap/accel off via
 * r_gpulightmapupdate=0) but MUST still be non-NULL command buffers, because R_UpdateWarpTextures
 * issues an UNCONDITIONAL vkCmdPipelineBarrier on primary_cb_contexts[PCBX_UPDATE_WARP].cb — a NULL
 * cb there was the world-render SIGSEGV. */
static VkCommandBuffer pcbx_cb[PCBX_NUM]  = { VK_NULL_HANDLE };
static VkImage		   scanout_image	  = VK_NULL_HANDLE;
static VkDeviceMemory  scanout_memory	  = VK_NULL_HANDLE;
static VkImageView	   scanout_view		  = VK_NULL_HANDLE;
static VkImage		   depth_image		  = VK_NULL_HANDLE;   /* 3D: D32 depth buffer */
static VkDeviceMemory  depth_memory		  = VK_NULL_HANDLE;
static VkImageView	   depth_view		  = VK_NULL_HANDLE;
static VkRenderPass	   ui_render_pass	  = VK_NULL_HANDLE;   /* the single color+depth pass (world+2D) */
static VkFramebuffer   ui_framebuffer	  = VK_NULL_HANDLE;
static int			   have_depth		  = 0;   /* 1 once the color+depth pass/world path is wired */
static int			   render_resources_created = 0;
static int			   frame_recording	  = 0; /* set between GL_BeginRendering and GL_EndRendering */
static unsigned long   present_count	  = 0; /* frames submitted+presented to the fb0 scanout */

#define GIPA(inst, name) ((PFN_##name) vkGetInstanceProcAddr ((inst), #name))

static PFN_vkVoidFunction gdpa (const char *name)
{
	static PFN_vkGetDeviceProcAddr fp = NULL;
	if (!fp)
		fp = (PFN_vkGetDeviceProcAddr) vkGetInstanceProcAddr (g_vk_instance, "vkGetDeviceProcAddr");
	return fp ? fp (g_vk_device, name) : NULL;
}

/* ============================== instance / device bring-up ============================== */
static qboolean create_instance (void)
{
	PFN_vkCreateInstance pCreateInstance = GIPA (NULL, vkCreateInstance);
	if (!pCreateInstance) {
		Sys_Printf ("vkvid: vkGetInstanceProcAddr(vkCreateInstance) NULL\n");
		return false;
	}
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkQuake-phoenix",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 0, /* no WSI/surface ext: fb0 scanout, not a swapchain */
	};
	VkInstance inst = VK_NULL_HANDLE;
	VkResult r = pCreateInstance (&ici, NULL, &inst);
	Sys_Printf ("vkvid: vkCreateInstance -> %d\n", (int)r);
	if (r != VK_SUCCESS)
		return false;
	g_vk_instance = inst; /* publish for the trampolines */
	return true;
}

static qboolean create_device (void)
{
	PFN_vkEnumeratePhysicalDevices pEnum = GIPA (g_vk_instance, vkEnumeratePhysicalDevices);
	PFN_vkCreateDevice			   pCreateDevice = GIPA (g_vk_instance, vkCreateDevice);
	if (!pEnum || !pCreateDevice) {
		Sys_Printf ("vkvid: missing enum/createdevice proc\n");
		return false;
	}

	uint32_t n = 1;
	VkResult r = pEnum (g_vk_instance, &n, &phys_device);
	Sys_Printf ("vkvid: vkEnumeratePhysicalDevices -> %d count=%u\n", (int)r, n);
	if (r < 0 || phys_device == VK_NULL_HANDLE)
		return false;

	/* Memory/feature/queue properties the renderer reads from vulkan_globals. Resolve the
	 * phys-device queries via instance-proc-addr (they dispatch to the instance table). */
	PFN_vkGetPhysicalDeviceMemoryProperties pMem =
		GIPA (g_vk_instance, vkGetPhysicalDeviceMemoryProperties);
	PFN_vkGetPhysicalDeviceFeatures pFeat = GIPA (g_vk_instance, vkGetPhysicalDeviceFeatures);
	PFN_vkGetPhysicalDeviceProperties pProps = GIPA (g_vk_instance, vkGetPhysicalDeviceProperties);
	if (pMem)
		pMem (phys_device, &vulkan_globals.memory_properties);
	if (pFeat)
		pFeat (phys_device, &vulkan_globals.device_features);
	/* CRITICAL (2026-07-30): upstream gl_vidsdl.c queries this in GL_InitDevice; the shim
	 * omitted it, so vulkan_globals.device_properties was ALL-ZERO. That makes
	 * limits.min{Uniform,Storage}BufferOffsetAlignment == 0, and R_DynBufferAllocate's
	 * q_align(size, 0) returns 0 -> every dynamic UBO/SSBO sub-allocation collapses onto
	 * offset 0 (last-writer-wins garbage): the alias-model/viewmodel UBOs and per-draw
	 * uniforms alias each other. This is the missed-SDL-init class of bug. */
	if (pProps)
		pProps (phys_device, &vulkan_globals.device_properties);
	else
		Sys_Printf ("vkvid: WARNING vkGetPhysicalDeviceProperties MISSING -> device_properties ZERO (buffer alignments collapse!)\n");
	Sys_Printf ("vkvid: device_properties minUBOalign=%llu minSSBOalign=%llu maxAniso=%.1f\n",
		(unsigned long long)vulkan_globals.device_properties.limits.minUniformBufferOffsetAlignment,
		(unsigned long long)vulkan_globals.device_properties.limits.minStorageBufferOffsetAlignment,
		(double)vulkan_globals.device_properties.limits.maxSamplerAnisotropy);
	/* The V3D exposes one universal (graphics+compute+transfer) queue family at index 0. */
	vulkan_globals.gfx_queue_family_index = 0;

	float				  prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = vulkan_globals.gfx_queue_family_index,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	/* Enable the same device features upstream GL_InitDevice enables (gl_vidsdl.c:1229-1255),
	 * gated on device support (device_features queried above) so vkCreateDevice can't fail on
	 * an unsupported feature. The shim previously passed pEnabledFeatures=NULL (all features
	 * off), leaving samplerAnisotropy etc. disabled — a latent correctness gap. */
	VkPhysicalDeviceFeatures enabled = { 0 };
	enabled.shaderStorageImageExtendedFormats = vulkan_globals.device_features.shaderStorageImageExtendedFormats;
	enabled.independentBlend                  = vulkan_globals.device_features.independentBlend;
	enabled.samplerAnisotropy                 = vulkan_globals.device_features.samplerAnisotropy;
	enabled.sampleRateShading                 = vulkan_globals.device_features.sampleRateShading;
	enabled.fillModeNonSolid                  = vulkan_globals.device_features.fillModeNonSolid;
	enabled.multiDrawIndirect                 = vulkan_globals.device_features.multiDrawIndirect;
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.pEnabledFeatures = &enabled,
	};
	VkDevice dev = VK_NULL_HANDLE;
	r = pCreateDevice (phys_device, &dci, NULL, &dev);
	Sys_Printf ("vkvid: vkCreateDevice -> %d\n", (int)r);
	if (r != VK_SUCCESS)
		return false;

	g_vk_device			   = dev; /* publish for the trampolines (device cmds) */
	vulkan_globals.device  = dev;

	PFN_vkGetDeviceQueue pGetQueue = (PFN_vkGetDeviceQueue) gdpa ("vkGetDeviceQueue");
	if (pGetQueue)
		pGetQueue (dev, vulkan_globals.gfx_queue_family_index, 0, &gfx_queue);
	vulkan_globals.queue = gfx_queue;

	/* TODO(vkquake-port): drop once R_CreateShaderModules is proven on HW.
	 * Discriminator for the Instruction-Abort pc=0 inside R_CreateShaderModules:
	 * resolve vkCreateShaderModule via the device proc-addr ONCE here and print the
	 * returned pointer. A NULL/garbage value means GetDeviceProcAddr handed back a bad
	 * fp (dispatch-table slot unpopulated); a sane value means the NULL fptr is reached
	 * INSIDE the resolved entrypoint (e.g. device->alloc.pfnAllocation). */
	{
		PFN_vkVoidFunction pCSM = gdpa ("vkCreateShaderModule");
		Sys_Printf ("vkvid: gdpa(vkCreateShaderModule) -> %p\n", (const void *)pCSM);
	}
	return true;
}

/* ===================== BRING-UP draw/push-constant counting wrappers =====================
 * The frame loop sustains and the magenta clear reaches fb0, but the 2D draws (console/menu)
 * don't show. To split "draws never recorded into the submitted cb" from "draws recorded but
 * rasterize nothing", install counting wrappers as the dispatch-table draw entrypoints (the
 * renderer calls vulkan_globals.vk_cmd_draw* directly). Since V_RenderView early-returns at the
 * menu (con_forcedup), every counted draw is a 2D/GUI draw into frame_cb.
 *
 * Also wrap vk_cmd_push_constants to catch a NULL pipeline layout: GL_SetCanvas -> GL_OrthoMatrix
 * pushes the 2D projection matrix via cbx->current_pipeline.layout.handle, which is reset to 0 at
 * frame start — if the canvas runs before any pipeline bind, the projection push targets a NULL
 * layout and the matrix is lost -> all 2D geometry transforms off-screen (would look like bare
 * clear). The counter pins which case it is. TODO(vkquake-port): remove once 2D lands. */
static PFN_vkCmdDraw		 real_vk_cmd_draw		   = NULL;
static PFN_vkCmdDrawIndexed	 real_vk_cmd_draw_indexed  = NULL;
static PFN_vkCmdDrawIndexedIndirect real_vk_cmd_draw_indexed_indirect = NULL;
static PFN_vkCmdPushConstants real_vk_cmd_push_constants = NULL;
static unsigned long g_draw_calls	   = 0; /* vkCmdDraw this frame */
static unsigned long g_draw_idx_calls  = 0; /* vkCmdDrawIndexed this frame */
static unsigned long g_draw_ind_calls  = 0; /* vkCmdDrawIndexedIndirect this frame (WORLD brush geom) */
static unsigned long g_pc_null_layout  = 0; /* push-constant calls with NULL layout this frame */
/* Render-pass nesting probe: the world (R_RenderView) records into frame_cb via the inline SCBX
 * short-circuit; if those draws land OUTSIDE our begun UI render pass they rasterize nothing on V3D
 * (clear survives -> blue) even though the draw counter increments. rp_active is set only while our
 * render pass is begun on frame_cb. */
static int rp_active = 0;
static unsigned long g_draws_outside_rp = 0; /* indexed+indirect draws recorded while rp_active==0 */

static void VKAPI_PTR count_vk_cmd_draw (VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
	g_draw_calls++;
	if (real_vk_cmd_draw)
		real_vk_cmd_draw (cb, vc, ic, fv, fi);
}

static void VKAPI_PTR count_vk_cmd_draw_indexed (VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fia)
{
	g_draw_idx_calls++;
	if (!rp_active)
		g_draws_outside_rp++;
	if (real_vk_cmd_draw_indexed)
		real_vk_cmd_draw_indexed (cb, ic, inst, fi, vo, fia);
}

/* The WORLD brush geometry draws via vkCmdDrawIndexedIndirect (r_brush.c indirect=true), so it is
 * invisible to the plain draw/drawIndexed counters. Count it separately to answer "is the world
 * actually recording draws?" without inferring from con_forcedup. */
static void VKAPI_PTR count_vk_cmd_draw_indexed_indirect (VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t count, uint32_t stride)
{
	g_draw_ind_calls++;
	if (!rp_active)
		g_draws_outside_rp++;
	if (real_vk_cmd_draw_indexed_indirect)
		real_vk_cmd_draw_indexed_indirect (cb, buf, off, count, stride);
}

static void VKAPI_PTR count_vk_cmd_push_constants (VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags sf,
                                                   uint32_t off, uint32_t sz, const void *vals)
{
	if (layout == VK_NULL_HANDLE) {
		g_pc_null_layout++;
		return; /* a NULL-layout push is invalid; skip it (it would be dropped/UB anyway) */
	}
	if (real_vk_cmd_push_constants)
		real_vk_cmd_push_constants (cb, layout, sf, off, sz, vals);
}

/* Populate the dispatch-table function pointers vulkan_globals exposes (the renderer calls
 * through vulkan_globals.vk_cmd_* directly in the hot path). Resolve via vkGetDeviceProcAddr.
 * Unsupported-on-V3D entries (push-descriptor / buffer-device-address / accel-structure) are
 * left NULL — the renderer feature-gates them, and we force those features off below. */
static void populate_dispatch_table (void)
{
	vulkan_globals.vk_cmd_bind_pipeline = (PFN_vkCmdBindPipeline) gdpa ("vkCmdBindPipeline");
	real_vk_cmd_push_constants = (PFN_vkCmdPushConstants) gdpa ("vkCmdPushConstants");
	vulkan_globals.vk_cmd_push_constants = count_vk_cmd_push_constants;
	vulkan_globals.vk_cmd_bind_descriptor_sets =
		(PFN_vkCmdBindDescriptorSets) gdpa ("vkCmdBindDescriptorSets");
	vulkan_globals.vk_cmd_bind_index_buffer =
		(PFN_vkCmdBindIndexBuffer) gdpa ("vkCmdBindIndexBuffer");
	vulkan_globals.vk_cmd_bind_vertex_buffers =
		(PFN_vkCmdBindVertexBuffers) gdpa ("vkCmdBindVertexBuffers");
	real_vk_cmd_draw = (PFN_vkCmdDraw) gdpa ("vkCmdDraw");
	vulkan_globals.vk_cmd_draw = count_vk_cmd_draw;
	real_vk_cmd_draw_indexed = (PFN_vkCmdDrawIndexed) gdpa ("vkCmdDrawIndexed");
	vulkan_globals.vk_cmd_draw_indexed = count_vk_cmd_draw_indexed;
	real_vk_cmd_draw_indexed_indirect =
		(PFN_vkCmdDrawIndexedIndirect) gdpa ("vkCmdDrawIndexedIndirect");
	vulkan_globals.vk_cmd_draw_indexed_indirect = count_vk_cmd_draw_indexed_indirect;
	vulkan_globals.vk_cmd_pipeline_barrier =
		(PFN_vkCmdPipelineBarrier) gdpa ("vkCmdPipelineBarrier");
	vulkan_globals.vk_cmd_copy_buffer_to_image =
		(PFN_vkCmdCopyBufferToImage) gdpa ("vkCmdCopyBufferToImage");
	vulkan_globals.vk_cmd_dispatch = (PFN_vkCmdDispatch) gdpa ("vkCmdDispatch");
	/* push-descriptor / buffer-device-address / accel-structure: V3DV-unsupported -> NULL. */
}

/* =========================== mmap-backed VkAllocationCallbacks =========================== */
/* WHY: stock vkCreateShaderModule (vk_common_CreateShaderModule -> vk_alloc2 with
 * pAllocator==NULL -> libphoenix malloc) crashes for any single shader-module allocation
 * whose codeSize exceeds one page (4096 B): every module <= 3624 B succeeds; md5_vert
 * (5228) + screen_effects_8bit_comp (8844) fault. The trigger is a >1-page, 8-byte-aligned
 * allocation on this process's libphoenix heap (root cause established 2026-06-25,
 * docs/inprogress/2026-06-25-vkquake-hw-tier1-result.md "DECISIVE root-cause").
 *
 * Interim unblock (no core change): supply these callbacks as the pAllocator argument to the
 * shader-module create/destroy calls. They back every allocation with mmap(MAP_ANONYMOUS) — a
 * SEPARATE kernel mapping that does NOT touch the failing malloc heap (the same primitive
 * libphoenix's own malloc_dl.c uses to grow its heap, so it is a proven userspace path here).
 *
 * Layout: mmap returns a page-aligned base (>= 4096-aligned, so any Vulkan alignment <= page is
 * trivially honored). A header { base, maplen, size } (three pointer-width fields) is written
 * immediately before the user pointer so pfnFree/pfnReallocation can recover the mapping and the
 * original size (Vulkan's free/realloc callbacks receive only the pointer). The driver does NOT
 * remember which allocator
 * created an object, so the SAME callbacks MUST be passed to vkDestroyShaderModule — see the
 * DESTROY_SHADER_MODULE macro (gl_rmisc.c), which is patched to pass PL_VkHostAllocator(). */

typedef struct {
	void  *base;   /* mmap() base to hand back to munmap() */
	size_t maplen; /* total mapped length (for munmap + realloc copy bound) */
	size_t size;   /* user-requested size (for realloc copy) */
} pl_vkalloc_hdr_t;

#ifndef PAGE_SIZE
#define PL_VKALLOC_PAGE 4096u
#else
#define PL_VKALLOC_PAGE PAGE_SIZE
#endif

static size_t pl_round_up (size_t v, size_t a)
{
	return (v + (a - 1)) & ~(a - 1);
}

static void *VKAPI_PTR pl_vk_alloc (void *pUserData, size_t size, size_t alignment,
                                    VkSystemAllocationScope scope)
{
	(void)pUserData;
	(void)scope;
	if (size == 0)
		return NULL;
	if (alignment < 8)
		alignment = 8;

	/* Reserve room for the header AND for aligning the user pointer up from the page base. */
	size_t hdr	 = pl_round_up (sizeof (pl_vkalloc_hdr_t), alignment);
	size_t maplen = pl_round_up (hdr + size, PL_VKALLOC_PAGE);

	void *base = mmap (NULL, maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;

	/* base is page-aligned; hdr is a multiple of alignment, so the user pointer is aligned. */
	void			 *user = (char *)base + hdr;
	pl_vkalloc_hdr_t *h	   = (pl_vkalloc_hdr_t *)((char *)user - sizeof (pl_vkalloc_hdr_t));
	h->base	  = base;
	h->maplen = maplen;
	h->size	  = size;
	return user;
}

static void VKAPI_PTR pl_vk_free (void *pUserData, void *pMemory)
{
	(void)pUserData;
	if (pMemory == NULL)
		return;
	pl_vkalloc_hdr_t *h = (pl_vkalloc_hdr_t *)((char *)pMemory - sizeof (pl_vkalloc_hdr_t));
	munmap (h->base, h->maplen);
}

static void *VKAPI_PTR pl_vk_realloc (void *pUserData, void *pOriginal, size_t size,
                                      size_t alignment, VkSystemAllocationScope scope)
{
	if (pOriginal == NULL)
		return pl_vk_alloc (pUserData, size, alignment, scope);
	if (size == 0) {
		pl_vk_free (pUserData, pOriginal);
		return NULL;
	}
	pl_vkalloc_hdr_t *oh	   = (pl_vkalloc_hdr_t *)((char *)pOriginal - sizeof (pl_vkalloc_hdr_t));
	size_t			  old_size = oh->size;
	void			 *nptr	   = pl_vk_alloc (pUserData, size, alignment, scope);
	if (nptr == NULL)
		return NULL;
	memcpy (nptr, pOriginal, old_size < size ? old_size : size);
	pl_vk_free (pUserData, pOriginal);
	return nptr;
}

static const VkAllocationCallbacks pl_vk_host_allocator = {
	.pUserData			   = NULL,
	.pfnAllocation		   = pl_vk_alloc,
	.pfnReallocation	   = pl_vk_realloc,
	.pfnFree			   = pl_vk_free,
	.pfnInternalAllocation = NULL,
	.pfnInternalFree	   = NULL,
};

/* Exposed to the engine (gl_rmisc.c R_CreateShaderModule / DESTROY_SHADER_MODULE) so the >4KB
 * shader-module allocations bypass the failing libphoenix malloc path. */
const VkAllocationCallbacks *PL_VkHostAllocator (void)
{
	return &pl_vk_host_allocator;
}

/* ============================== HDMI display ownership ============================== */
/* The HDMI text console (pl011-tty fbcon + the kernel klog mirror) keeps drawing to /dev/fb0.
 * Our render-pass storeOp=STORE writes the rendered frame straight into the fb0 scanout BO (the
 * winsys backs the scanout VkImage's memory with the fb0 physical pages — see v3d_phoenix_winsys.c),
 * so while the console still owns the framebuffer it overdraws every vkQuake frame between/over
 * presents. So we tell the console to stop drawing (FBCON_DISABLED) while vkQuake owns the display,
 * and restore it (FBCON_ENABLED) on clean shutdown. Mirrors the GL flagship's
 * pl_phoenix_vid.c::console_setmode and the X DDX's fbdevConsoleSetMode. This only touches the HDMI
 * fbcon, NOT the UART — vkvid: prints keep flowing on UART regardless. */
static int console_ttyfd = -1;

static void console_setmode (int mode)
{
	if (console_ttyfd < 0) {
		console_ttyfd = open ("/dev/tty0", O_RDWR);
		if (console_ttyfd < 0)
			console_ttyfd = open ("/dev/console", O_RDWR);
	}
	if (console_ttyfd >= 0) {
		if (ioctl (console_ttyfd, FBCONSETMODE, mode) == 0)
			Sys_Printf ("vkvid: HDMI console fbcon mode -> %d\n", mode);
		else
			Sys_Printf ("vkvid: FBCONSETMODE(%d) failed (console may overdraw)\n", mode);
	} else {
		Sys_Printf ("vkvid: no /dev/tty0|console to switch fbcon mode\n");
	}
}

/* ============================== fb0 scanout discovery ============================== */
static void discover_fb0 (void)
{
	int fd = open ("/dev/fb0", O_RDWR);
	memset (&fb_mode, 0, sizeof (fb_mode));
	if (fd >= 0 && ioctl (fd, _IOR ('g', 1, rpi4fb_mode_t), &fb_mode) == 0 && fb_mode.framebuffer != 0) {
		Sys_Printf ("vkvid: fb0 %ux%u pitch=%u pa=0x%llx size=%llu\n", fb_mode.width, fb_mode.height,
		            fb_mode.pitch, (unsigned long long)fb_mode.framebuffer,
		            (unsigned long long)fb_mode.smemlen);
		v3d_phoenix_set_scanout ((uint32_t)fb_mode.framebuffer, (uint32_t)fb_mode.smemlen);
		fb_ready = 1;
	} else {
		Sys_Printf ("vkvid: fb0 GETMODE unavailable (offscreen only)\n");
	}
	if (fd >= 0)
		close (fd);
}

/* ============================ no-WSI render-resource bring-up ============================ */
/* Pick the first memory type the image's requirements allow (the V3DV scanout BO is type 0,
 * HOST_VISIBLE + uncached — the same one the Tier-4b harness selected). */
static uint32_t pick_memory_type (uint32_t type_bits)
{
	for (uint32_t i = 0; i < 32; ++i)
		if (type_bits & (1u << i))
			return i;
	return 0;
}

/* Create the single fb0-backed scanout VkImage + view, the UI render pass + framebuffer, and
 * the basic UI pipelines (reusing the engine's R_CreateBasicPipelines, gated to the UI variant).
 * Returns 1 on success. Mirrors the proven harness present path. Called once. */
static int create_render_resources (void)
{
	if (render_resources_created)
		return 1;
	if (!fb_ready) {
		Sys_Printf ("vkvid: create_render_resources: no fb0 scanout, cannot render\n");
		return 0;
	}

	PFN_vkCreateImage				 pCreateImage	   = (PFN_vkCreateImage) gdpa ("vkCreateImage");
	PFN_vkGetImageMemoryRequirements pGetImageMemReq   = (PFN_vkGetImageMemoryRequirements) gdpa ("vkGetImageMemoryRequirements");
	PFN_vkAllocateMemory			 pAllocateMemory   = (PFN_vkAllocateMemory) gdpa ("vkAllocateMemory");
	PFN_vkBindImageMemory			 pBindImageMemory  = (PFN_vkBindImageMemory) gdpa ("vkBindImageMemory");
	PFN_vkCreateImageView			 pCreateImageView  = (PFN_vkCreateImageView) gdpa ("vkCreateImageView");
	PFN_vkCreateRenderPass			 pCreateRenderPass = (PFN_vkCreateRenderPass) gdpa ("vkCreateRenderPass");
	PFN_vkCreateFramebuffer			 pCreateFramebuffer = (PFN_vkCreateFramebuffer) gdpa ("vkCreateFramebuffer");
	PFN_vkCreateCommandPool			 pCreateCommandPool = (PFN_vkCreateCommandPool) gdpa ("vkCreateCommandPool");
	PFN_vkAllocateCommandBuffers	 pAllocCmdBuffers  = (PFN_vkAllocateCommandBuffers) gdpa ("vkAllocateCommandBuffers");
	if (!pCreateImage || !pGetImageMemReq || !pAllocateMemory || !pBindImageMemory || !pCreateImageView || !pCreateRenderPass ||
	    !pCreateFramebuffer || !pCreateCommandPool || !pAllocCmdBuffers) {
		Sys_Printf ("vkvid: create_render_resources: missing device proc\n");
		return 0;
	}

	VkResult err;

	/* (1) Scanout image: LINEAR R8G8B8A8, color-attachment usage, backed by the fb0 BO. The
	 * winsys binds the NEXT allocated BO to the live scanout, so set_next_scanout() must be
	 * called right before this image's memory is allocated (the harness ordering). */
	{
		VkImageCreateInfo ici = {
			.sType		   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType	   = VK_IMAGE_TYPE_2D,
			.format		   = vulkan_globals.color_format,
			.extent		   = {fb_mode.width, fb_mode.height, 1},
			.mipLevels	   = 1,
			.arrayLayers   = 1,
			.samples	   = VK_SAMPLE_COUNT_1_BIT,
			.tiling		   = VK_IMAGE_TILING_LINEAR,
			.usage		   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		err = pCreateImage (g_vk_device, &ici, NULL, &scanout_image);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkCreateImage(scanout) -> %d\n", (int)err);
			return 0;
		}

		VkMemoryRequirements mreq;
		memset (&mreq, 0, sizeof (mreq));
		pGetImageMemReq (g_vk_device, scanout_image, &mreq);

		VkMemoryAllocateInfo mai = {
			.sType			 = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize	 = mreq.size ? mreq.size : (VkDeviceSize)fb_mode.smemlen,
			.memoryTypeIndex = pick_memory_type (mreq.memoryTypeBits),
		};
		v3d_phoenix_set_next_scanout (); /* the next BO (this image's memory) backs the live scanout */
		err = pAllocateMemory (g_vk_device, &mai, NULL, &scanout_memory);
		if (err == VK_SUCCESS)
			err = pBindImageMemory (g_vk_device, scanout_image, scanout_memory, 0);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: scanout image alloc/bind -> %d\n", (int)err);
			return 0;
		}
		Sys_Printf ("vkvid: scanout image %ux%u bound (mem=%u)\n", fb_mode.width, fb_mode.height, (unsigned)mreq.size);
	}

	/* (2) Image view over the scanout image. */
	{
		VkImageViewCreateInfo vci = {
			.sType			  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image			  = scanout_image,
			.viewType		  = VK_IMAGE_VIEW_TYPE_2D,
			.format			  = vulkan_globals.color_format,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		err = pCreateImageView (g_vk_device, &vci, NULL, &scanout_view);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkCreateImageView -> %d\n", (int)err);
			return 0;
		}
		Sys_Printf ("vkvid: rr: imageview ok\n");
	}

	/* (2b) 3D: D32_SFLOAT depth buffer (image + memory + view), OPTIMAL tiling, depth-stencil
	 * attachment usage. Enables the color+depth MAIN render pass so V_RenderView's world draws
	 * depth-test. Failure here is non-fatal: fall back to the color-only (2D) pass. */
	{
		VkImageCreateInfo dci = {
			.sType		   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType	   = VK_IMAGE_TYPE_2D,
			.format		   = vulkan_globals.depth_format,
			.extent		   = {fb_mode.width, fb_mode.height, 1},
			.mipLevels	   = 1,
			.arrayLayers   = 1,
			.samples	   = VK_SAMPLE_COUNT_1_BIT,
			.tiling		   = VK_IMAGE_TILING_OPTIMAL,
			.usage		   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VkMemoryRequirements dmreq; memset (&dmreq, 0, sizeof (dmreq));
		if (pCreateImage (g_vk_device, &dci, NULL, &depth_image) == VK_SUCCESS) {
			pGetImageMemReq (g_vk_device, depth_image, &dmreq);
			VkMemoryAllocateInfo dmai = {
				.sType			 = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize	 = dmreq.size,
				.memoryTypeIndex = pick_memory_type (dmreq.memoryTypeBits),
			};
			if (pAllocateMemory (g_vk_device, &dmai, NULL, &depth_memory) == VK_SUCCESS &&
			    pBindImageMemory (g_vk_device, depth_image, depth_memory, 0) == VK_SUCCESS) {
				VkImageViewCreateInfo dvci = {
					.sType			  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image			  = depth_image,
					.viewType		  = VK_IMAGE_VIEW_TYPE_2D,
					.format			  = vulkan_globals.depth_format,
					.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
				};
				if (pCreateImageView (g_vk_device, &dvci, NULL, &depth_view) == VK_SUCCESS) {
					have_depth = 1;
					Sys_Printf ("vkvid: rr: depth buffer ok (%ux%u D32, mem=%u)\n",
						fb_mode.width, fb_mode.height, (unsigned)dmreq.size);
				}
			}
		}
		if (!have_depth)
			Sys_Printf ("vkvid: rr: depth buffer setup FAILED -> 2D-only (color) pass\n");
	}

	/* (3) UI render pass: ONE color attachment, ONE subpass, samples=1, color_format. This is
	 * the exact shape R_CreateBasicPipelines expects for RENDER_PASS_INDEX_UI (attachment_count=1,
	 * subpass=0, samples=1), so the pipelines it builds are render-pass-compatible with the
	 * draw-time vkCmdBeginRenderPass below. loadOp=CLEAR -> storeOp=STORE is the harness's proven
	 * scanout-paint path. finalLayout=GENERAL matches the host-mapped scanout BO. */
	{
		VkAttachmentDescription att[2] = {
			{	/* 0: color = scanout */
				.format			= vulkan_globals.color_format,
				.samples		= VK_SAMPLE_COUNT_1_BIT,
				.loadOp			= VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp		= VK_ATTACHMENT_STORE_OP_STORE,
				.stencilLoadOp	= VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout	= VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout	= VK_IMAGE_LAYOUT_GENERAL,
			},
			{	/* 1: depth (only attached when have_depth) */
				.format			= vulkan_globals.depth_format,
				.samples		= VK_SAMPLE_COUNT_1_BIT,
				.loadOp			= VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp		= VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.stencilLoadOp	= VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout	= VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout	= VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			},
		};
		VkAttachmentReference attref = {
			.attachment = 0,
			.layout		= VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};
		VkAttachmentReference depthref = {
			.attachment = 1,
			.layout		= VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};
		VkSubpassDescription sub = {
			.pipelineBindPoint		 = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount	 = 1,
			.pColorAttachments		 = &attref,
			.pDepthStencilAttachment = have_depth ? &depthref : NULL,
		};
		VkRenderPassCreateInfo rpci = {
			.sType			 = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = have_depth ? 2u : 1u,
			.pAttachments	 = att,
			.subpassCount	 = 1,
			.pSubpasses		 = &sub,
		};
		err = pCreateRenderPass (g_vk_device, &rpci, NULL, &ui_render_pass);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkCreateRenderPass -> %d\n", (int)err);
			return 0;
		}
		/* Publish as BOTH the MAIN (world) and UI pass so R_CreatePipelines builds the world
		 * pipelines against it (the skip-guard only drops NULL-render-pass variants). 2D pipelines
		 * are render-pass-compatible with the extra depth attachment (they just don't depth-test). */
		if (have_depth) {
			vulkan_globals.main_render_pass[MAIN_RENDER_PASS_STANDARD][MAIN_RENDER_PASS_STENCIL_CLEAR] = ui_render_pass;
			vulkan_globals.main_render_pass[MAIN_RENDER_PASS_STANDARD][MAIN_RENDER_PASS_NO_STENCIL]	 = ui_render_pass;
		}
		Sys_Printf ("vkvid: rr: renderpass ok (depth=%d)\n", have_depth);
	}

	/* (4) Framebuffer wrapping the scanout view. */
	{
		VkImageView fb_atts[2] = { scanout_view, depth_view };
		VkFramebufferCreateInfo fbci = {
			.sType			 = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass		 = ui_render_pass,
			.attachmentCount = have_depth ? 2u : 1u,
			.pAttachments	 = fb_atts,
			.width			 = fb_mode.width,
			.height			 = fb_mode.height,
			.layers			 = 1,
		};
		err = pCreateFramebuffer (g_vk_device, &fbci, NULL, &ui_framebuffer);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkCreateFramebuffer(ui) -> %d\n", (int)err);
			return 0;
		}
		Sys_Printf ("vkvid: rr: framebuffer ok\n");
	}

	/* (5) Command pool + one PRIMARY command buffer for the per-frame record/submit. */
	{
		VkCommandPoolCreateInfo cpci = {
			.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags			  = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = vulkan_globals.gfx_queue_family_index,
		};
		err = pCreateCommandPool (g_vk_device, &cpci, NULL, &gfx_command_pool);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkCreateCommandPool(gfx) -> %d\n", (int)err);
			return 0;
		}
		Sys_Printf ("vkvid: rr: cmdpool ok\n");
		VkCommandBufferAllocateInfo cbai = {
			.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool		= gfx_command_pool,
			.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = PCBX_NUM,
		};
		err = pAllocCmdBuffers (g_vk_device, &cbai, pcbx_cb);
		if (err != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkAllocateCommandBuffers(gfx) -> %d\n", (int)err);
			return 0;
		}
		/* frame_cb == the RENDER_PASSES primary: the render pass + all world/2D draws record here.
		 * Wire every PCBX primary context to its own cb so the engine's warp/lightmap/accel phases
		 * have a valid (if, in bring-up, empty) command buffer instead of a NULL one. */
		frame_cb = pcbx_cb[PCBX_RENDER_PASSES];
		for (int p = 0; p < PCBX_NUM; p++) {
			cb_context_t *pcbx = &vulkan_globals.primary_cb_contexts[p];
			memset (pcbx, 0, sizeof (*pcbx));
			pcbx->cb			  = pcbx_cb[p];
			pcbx->current_canvas  = CANVAS_INVALID;
			pcbx->render_pass	  = ui_render_pass;
			pcbx->render_pass_index = RENDER_PASS_INDEX_UI;
			pcbx->subpass		  = 0;
		}
		Sys_Printf ("vkvid: rr: %d primary cb contexts allocated (frame_cb=RENDER_PASSES)\n", PCBX_NUM);
	}

	/* (6) The single SCBX_GUI command-buffer context the 2D draw path (SCR_DrawGUI) records into.
	 * Upstream allocates a *pointer* per SCBX index and uses a SECONDARY cb executed into the
	 * primary; here it points at the PRIMARY frame_cb (inline subpass), and shares ui_render_pass /
	 * RENDER_PASS_INDEX_UI with the pipeline build so everything is render-pass-compatible. */
	{
		/* Wire EVERY secondary cb-context (world + 2D) to the single primary frame_cb + the one
		 * color+depth pass. Single-threaded (r_tasks=0) records them inline in order. Scene
		 * contexts (SCBX_WORLD..just-before-GUI) use RENDER_PASS_INDEX_MAIN; GUI/POST use UI.
		 * Both indices map to the same VkRenderPass here, so pipelines from either are compatible. */
		for (int scbx = 0; scbx < SCBX_NUM; scbx++) {
			int mult = SECONDARY_CB_MULTIPLICITY[scbx];
			if (mult < 1) mult = 1;
			cb_context_t *ctxs = (cb_context_t *)Mem_Alloc ((size_t)mult * sizeof (cb_context_t));
			int rpi = (scbx < SCBX_GUI && have_depth) ? RENDER_PASS_INDEX_MAIN : RENDER_PASS_INDEX_UI;
			for (int i = 0; i < mult; i++) {
				memset (&ctxs[i], 0, sizeof (cb_context_t));
				ctxs[i].cb				 = frame_cb;
				ctxs[i].current_canvas	 = CANVAS_INVALID;
				ctxs[i].render_pass		 = ui_render_pass;
				ctxs[i].render_pass_index = rpi;
				ctxs[i].subpass			 = 0;
			}
			vulkan_globals.secondary_cb_contexts[scbx] = ctxs;
		}
		Sys_Printf ("vkvid: rr: %d scbx contexts wired to frame_cb (world+2D, single color+depth pass)\n", SCBX_NUM);
	}

	/* (7) Build the basic UI pipelines by reusing the engine's own R_CreateBasicPipelines (now
	 * un-static + guarded to skip the MAIN/OIT/WBOIT variants whose render pass is NULL). It reads
	 * secondary_cb_contexts[SCBX_GUI]->render_pass (set above) for the UI variant. This reuses the
	 * exact vertex-input / blend / push-constant state the draw path expects — hand-authoring it
	 * would risk a silent GPU hang. Requires basic_pipeline_layout (R_CreatePipelineLayouts, done
	 * in VID_Init) + shader modules (created here, destroyed after). */
	Sys_Printf ("vkvid: rr: calling R_CreateShaderModules\n");
	R_CreateShaderModules ();
	Sys_Printf ("vkvid: rr: shadermodules ok\n");
	R_InitVertexAttributes ();
	Sys_Printf ("vkvid: rr: vertexattrs ok\n");
	if (have_depth) {
		/* Full pipeline set: R_CreatePipelines() builds the basic (UI) pipelines PLUS the world/
		 * alias/brush/sky/particle MAIN pipelines against main_render_pass (now non-NULL). The
		 * engine's skip-guard drops the OIT/WBOIT variants (their render pass stays NULL). */
		extern void R_CreatePipelines (void);
		R_CreatePipelines ();
		Sys_Printf ("vkvid: rr: FULL pipelines ok (world+2D)\n");
	} else {
		R_CreateBasicPipelines ();
		Sys_Printf ("vkvid: rr: basic (UI-only) pipelines ok\n");
	}
	R_DestroyShaderModules ();
	Sys_Printf ("vkvid: rr: shadermodules destroyed\n");

	/* Clear color = BLACK. (Was diagnostic-magenta to prove the no-WSI present reached fb0 —
	 * confirmed on HW: 99.9% magenta every frame.) Now that the 2D-projection fix lets the
	 * console/menu draws land, black is the correct backdrop so the actual 2D content shows
	 * (the Quake console background + text overdraw the cleared frame). If the screen comes up
	 * solid black, the per-frame `2D draws=N nullLayoutPC=K` marker disambiguates a present
	 * regression from a remaining 2D-recording gap (it should read draws>0, nullLayoutPC=0). */
	vulkan_globals.color_clear_value.color.float32[0] = 0.0f;
	vulkan_globals.color_clear_value.color.float32[1] = 0.0f;
	vulkan_globals.color_clear_value.color.float32[2] = 0.0f;
	vulkan_globals.color_clear_value.color.float32[3] = 1.0f;

	render_resources_created = 1;
	Sys_Printf ("vkvid: render resources created (UI render pass + fb0 framebuffer + basic pipelines)\n");
	return 1;
}

/* ================================ VID_* public surface ================================ */
void VID_Init (void)
{
	Sys_Printf ("vkvid: VID_Init (Phoenix/V3DV fb0 scanout, no WSI)\n");

	/* The vid_* cvars are registered by the engine (gl_rmisc.c / view.c). Bring Vulkan up. */
	if (!create_instance () || !create_device ()) {
		Sys_Error ("VID_Init: Vulkan bring-up failed");
		return;
	}
	populate_dispatch_table ();
	discover_fb0 ();

	/* Take HDMI display ownership: stop the text console overdrawing vkQuake's fb0 scanout. Only
	 * meaningful when we actually drive fb0; if fb0 was unavailable we render offscreen and leave
	 * the console alone. (Restored to FBCON_ENABLED in VID_Shutdown's clean-exit path.) */
	if (fb_ready)
		console_setmode (FBCON_DISABLED);

	/* Steer onto a V3DV-supported feature path (no subgroup-size-control, no RT). These are
	 * the runtime gates the renderer keys off; force them off so it picks the classic raster
	 * + non-sops compute path. (Cvar values; the engine re-reads them.) */
	vulkan_globals.screen_effects_sops = false;
	vulkan_globals.ray_query		   = false;
	vulkan_globals.non_solid_fill	   = false;
	vulkan_globals.multi_draw_indirect = false;
	Cvar_SetValueQuick (&r_usesops, 0.0f);

	/* Force the SINGLE-THREADED render path. host.c calls SCR_UpdateScreen(true), and with the
	 * task path SCR_DrawGUI runs as a worker task while this shim's inline GL_EndRendering would
	 * end the render pass + submit frame_cb before the draws are recorded -> empty/garbage frame.
	 * Our synchronous single-command-buffer present REQUIRES the non-task path. r_tasks=0 makes
	 * SCR_UpdateScreen's use_tasks gate false unconditionally (independent of worker count). */
	{
		extern cvar_t r_tasks;
		Cvar_SetValueQuick (&r_tasks, 0.0f);
	}

	/* Formats + sample count the renderer's render-pass/pipeline build reads. R8G8B8A8 is the
	 * fb0 scanout format the harness rendered into; single-sample (no MSAA on first light). */
	vulkan_globals.swap_chain_format = VK_FORMAT_R8G8B8A8_UNORM;
	vulkan_globals.color_format		 = VK_FORMAT_R8G8B8A8_UNORM;
	vulkan_globals.depth_format		 = VK_FORMAT_D32_SFLOAT;
	vulkan_globals.sample_count		 = VK_SAMPLE_COUNT_1_BIT;
	vulkan_globals.supersampling	 = false;
	vulkan_globals.device_idle		 = true;

	/* Video geometry from fb0 (fallback 1024x768 if fb0 absent — same default the V3D scout
	 * used). The engine recomputes refdef off vid.width/height. */
	vid.width	   = fb_ready ? fb_mode.width : 1024;
	vid.height	   = fb_ready ? fb_mode.height : 768;
	vid.conwidth   = vid.width;
	vid.conheight  = vid.height;
	vid.rowbytes   = fb_ready ? fb_mode.pitch : (vid.width * 4);
	vid.aspect	   = (float)vid.width / (float)vid.height;
	vid.recalc_refdef = 1;
	vid.restart_next_frame = false;
	modestate = MS_FULLSCREEN;

	/* Renderer-resource init — restored from upstream gl_vidsdl.c VID_Init (4108-4116).
	 * This shim REPLACES gl_vidsdl.c, and the original drop of this block was the bug:
	 * the heaps/buffers/layouts these create (most visibly mesh_buffer_heap from
	 * R_InitMeshHeap) were never allocated, so the first alias-model mesh upload hit a
	 * NULL heap in GL_HeapAllocate. These functions live in the COMPILED engine files
	 * (gl_rmisc.c / gl_mesh.c / gl_texmgr.c), so calling them here is the faithful
	 * upstream ordering fix, not a workaround.
	 *
	 * Deliberately NOT called (all static-linkage inside the excluded gl_vidsdl.c, and
	 * only reached via the screen-effects / GL_CreateRenderResources path this shim does
	 * not run — the upstream line 4118 `GL_CreateRenderResources()` is itself commented):
	 *   R_CreatePaletteOctreeBuffers()  -> palette_octree_buffer (screen-effects only)
	 *   VID_Gamma_Init() / VID_Menu_Init()  -> gamma ramp + video menu
	 * If the screen-effects post-process or the video menu are later enabled, those will
	 * need un-static-ing in external/vkquake (and tracking in the .patch). */
	vulkan_globals.staging_buffer_size = INITIAL_STAGING_BUFFER_SIZE_KB * 1024;
	R_InitStagingBuffers ();
	R_CreateDescriptorSetLayouts ();
	R_CreateDescriptorPool ();
	R_InitGPUBuffers ();
	R_InitMeshHeap ();
	TexMgr_InitHeap ();
	R_InitSamplers ();
	R_CreatePipelineLayouts ();

	Sys_Printf ("vkvid: VID_Init done (%dx%d, device=%p queue=%p)\n", vid.width, vid.height,
	            (void *)g_vk_device, (void *)gfx_queue);
}

void VID_Shutdown (void)
{
	GL_WaitForDeviceIdle ();
	/* Hand the framebuffer back to the text console (reappears with everything the console
	 * accumulated off-screen while vkQuake owned the display). */
	if (fb_ready) {
		console_setmode (FBCON_ENABLED);
		if (console_ttyfd >= 0) {
			close (console_ttyfd);
			console_ttyfd = -1;
		}
	}
	/* Device/instance teardown deliberately omitted: the V3DV ICD destroy paths are not
	 * exercised here and the process exits anyway. (Main agent: wire vkDestroyDevice /
	 * vkDestroyInstance if a clean restart is needed.) */
	Sys_Printf ("vkvid: VID_Shutdown\n");
}

void VID_Restart (qboolean set_mode)
{
	(void)set_mode;
	/* No mode switching on the fixed fb0 scanout; a real restart would rebuild render
	 * resources. Synchronize any in-flight rendering so the engine's invariant holds. */
	GL_SynchronizeEndRenderingTask ();
	GL_WaitForDeviceIdle ();
	Sys_Printf ("vkvid: VID_Restart (no-op on fixed fb0 scanout)\n");
}

void VID_Toggle (void)
{
	/* Fullscreen-only on HDMI scanout; nothing to toggle. */
}

void VID_Lock (void) {}

/* VID_HasMouseOrInputFocus / VID_IsMinimized / VID_GetWindow are referenced by the SDL host
 * loop only; pl_phoenix_main.c uses a fixed-cadence loop, so they are not link symbols here. */

/* ================================ GL_* render surface ================================ */
void GL_SetObjectName (uint64_t object, VkObjectType object_type, const char *name)
{
	/* Debug-utils object naming: no-op (VK_EXT_debug_utils not enabled). */
	(void)object;
	(void)object_type;
	(void)name;
}

void GL_WaitForDeviceIdle (void)
{
	GL_SynchronizeEndRenderingTask ();
	PFN_vkDeviceWaitIdle pWait = (PFN_vkDeviceWaitIdle) gdpa ("vkDeviceWaitIdle");
	if (pWait && g_vk_device)
		pWait (g_vk_device);
	vulkan_globals.device_idle = true;
}

void GL_SynchronizeEndRenderingTask (void)
{
	/* The end-of-frame task is run inline (single submit path below), so there is never an
	 * outstanding task to join; just clear the handle. (Main agent: if tasks.c offloads the
	 * end-rendering work, join prev_end_rendering_task here.) */
	if (prev_end_rendering_task != INVALID_TASK_HANDLE) {
		Task_Join (prev_end_rendering_task, 0);
		prev_end_rendering_task = INVALID_TASK_HANDLE;
	}
}

void GL_UpdateDescriptorSets (void)
{
	/* Deferred descriptor-set writes: the engine batches vkUpdateDescriptorSets calls here.
	 * Minimal shim flushes nothing extra (the renderer issues its writes directly); kept as a
	 * defined symbol + device-idle barrier so the engine's ordering assumption holds. */
}

qboolean GL_BeginRendering (qboolean use_tasks, task_handle_t *begin_rendering_task, int *width,
                            int *height)
{
	(void)use_tasks;
	if (vid.restart_next_frame) {
		VID_Restart (false);
		vid.restart_next_frame = false;
	}

	if (!render_resources_created)
		Sys_Printf ("vkvid: GL_BeginRendering: first frame, creating render resources\n");

	/* Create render resources lazily on the first frame (matches upstream GL_BeginRendering, which
	 * defers GL_CreateRenderResources to the first frame so the device/heaps are fully up). */
	if (!render_resources_created && !create_render_resources ())
		return false;

	*width	= vid.width;
	*height = vid.height;
	if (begin_rendering_task)
		*begin_rendering_task = INVALID_TASK_HANDLE; /* inline path: no async begin task */

	/* Begin the per-frame primary command buffer, then begin the UI render pass INLINE on the fb0
	 * framebuffer. SCR_DrawGUI then records its 2D draws straight into frame_cb (it reads the
	 * SCBX_GUI cb_context we point at frame_cb). Reset the canvas + pipeline so GL_SetCanvas /
	 * R_BindPipeline re-emit their state this frame. */
	PFN_vkResetCommandBuffer pReset = (PFN_vkResetCommandBuffer) gdpa ("vkResetCommandBuffer");
	PFN_vkBeginCommandBuffer pBegin = (PFN_vkBeginCommandBuffer) gdpa ("vkBeginCommandBuffer");
	PFN_vkCmdBeginRenderPass pBeginRP = (PFN_vkCmdBeginRenderPass) gdpa ("vkCmdBeginRenderPass");
	if (!pBegin || !pBeginRP) {
		Sys_Printf ("vkvid: GL_BeginRendering: missing device proc\n");
		return false;
	}

	/* Reset + begin EVERY PCBX primary this frame (warp/lightmap/accel + render-passes). They are
	 * all submitted together in GL_EndRendering; the non-render-pass primaries stay empty in the
	 * bring-up path but must be validly begun. No render pass is begun on them here — the warp
	 * primary opens its own warp render pass internally (when warp is enabled); the render pass
	 * below is begun only on frame_cb (== PCBX_RENDER_PASSES). */
	VkCommandBufferBeginInfo bbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	for (int p = 0; p < PCBX_NUM; p++) {
		if (pReset)
			pReset (pcbx_cb[p], 0);
		vulkan_globals.primary_cb_contexts[p].current_canvas = CANVAS_INVALID;
		/* Reset the pipeline cache too: vkResetCommandBuffer above clears the driver's recording
		 * state (state.gfx/compute.pipeline -> NULL), so R_BindPipeline's cbx->current_pipeline
		 * cache is now stale. Without this, R_BindPipeline sees the cached handle still matching
		 * (e.g. cs_tex_warp bound last frame), SKIPS the vkCmdBindPipeline, and the freshly-reset cb
		 * dispatches/draws with no pipeline bound -> V3DV derefs a NULL state.compute.pipeline
		 * (cmd_buffer_create_csd_job) -> EL0 Data Abort. Matches the GUI context reset below. */
		memset (&vulkan_globals.primary_cb_contexts[p].current_pipeline, 0,
		        sizeof (vulkan_globals.primary_cb_contexts[p].current_pipeline));
		VkResult perr = pBegin (pcbx_cb[p], &bbi);
		if (perr != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkBeginCommandBuffer(pcbx %d) -> %d\n", p, (int)perr);
			return false;
		}
	}

	cb_context_t *gui = vulkan_globals.secondary_cb_contexts[SCBX_GUI];
	gui->cb				= frame_cb;
	gui->current_canvas = CANVAS_INVALID;
	memset (&gui->current_pipeline, 0, sizeof (gui->current_pipeline));

	/* FIX (2D-recording gap): seed current_pipeline.LAYOUT (not .handle) with the basic pipeline
	 * layout so the FIRST GL_SetCanvas -> GL_OrthoMatrix -> R_PushConstants pushes the 2D ortho
	 * matrix against a VALID layout. SCR_DrawGUI calls GL_SetCanvas before its first R_BindPipeline,
	 * and R_PushConstants pushes via cbx->current_pipeline.layout.handle; with the handle reset to 0
	 * that push had a NULL layout and was dropped (nullLayoutPC=1 on HW) -> the projection matrix was
	 * lost -> all 2D geometry transformed off-screen (bare clear). The basic layout's push range is
	 * {offset 0, size 21 floats, ALL_GRAPHICS}, which covers the 16-float ortho push, so it is
	 * push-constant-compatible with every 2D draw. Leaving .handle == 0 means the first real
	 * R_BindPipeline still fires (its handle != 0), so binding is unaffected. */
	gui->current_pipeline.layout = vulkan_globals.basic_pipeline_layout;

	VkClearValue clears[2];
	clears[0] = vulkan_globals.color_clear_value;   /* color (engine default) */
	/* vkQuake uses REVERSED-Z: pipelines compare with VK_COMPARE_OP_GREATER_OR_EQUAL
	 * (gl_rmisc.c R_InitDefaultStates), so the far plane is depth 0.0 and near is 1.0. Clearing to
	 * 1.0 (normal-Z far) made every world fragment (depth < 1.0) FAIL the GEQUAL test -> zero world
	 * pixels (blue clear showed through). Clear to 0.0 so fragments pass and the world rasterizes. */
	clears[1].depthStencil.depth = 0.0f;			/* reversed-Z far */
	clears[1].depthStencil.stencil = 0;
	VkRenderPassBeginInfo rpbi = {
		.sType			 = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass		 = ui_render_pass,
		.framebuffer	 = ui_framebuffer,
		.renderArea		 = {{0, 0}, {(uint32_t)vid.width, (uint32_t)vid.height}},
		.clearValueCount = have_depth ? 2u : 1u,
		.pClearValues	 = clears,
	};
	pBeginRP (frame_cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	rp_active = 1;

	/* The viewport/scissor are dynamic state set per-canvas by GL_SetCanvas -> GL_Viewport. Seed
	 * a full-frame scissor/viewport so a draw before the first GL_SetCanvas has valid state. */
	PFN_vkCmdSetViewport pSetVp = (PFN_vkCmdSetViewport) gdpa ("vkCmdSetViewport");
	PFN_vkCmdSetScissor	 pSetSc = (PFN_vkCmdSetScissor) gdpa ("vkCmdSetScissor");
	if (pSetVp) {
		VkViewport vp = {0.0f, 0.0f, (float)vid.width, (float)vid.height, 0.0f, 1.0f};
		pSetVp (frame_cb, 0, 1, &vp);
	}
	if (pSetSc) {
		VkRect2D sc = {{0, 0}, {(uint32_t)vid.width, (uint32_t)vid.height}};
		pSetSc (frame_cb, 0, 1, &sc);
	}

	/* New frame: cycle the dynamic vertex/index/uniform buffers (resets per-frame alloc offsets).
	 * The 2D draw path (R_VertexAllocate / R_IndexAllocate in gl_draw.c) writes into these; without
	 * the swap they'd accumulate across frames and overflow. Mirrors GL_BeginRenderingTask's tail. */
	R_SwapDynamicBuffers ();

	/* Reset the per-frame bring-up counters (draws + NULL-layout push constants). */
	g_draw_calls = g_draw_idx_calls = g_draw_ind_calls = g_pc_null_layout = g_draws_outside_rp = 0;

	vulkan_globals.device_idle = false;
	frame_recording = 1;
	return true;
}

task_handle_t GL_EndRendering (qboolean use_tasks, qboolean use_swapchain)
{
	(void)use_tasks;
	(void)use_swapchain;

	if (!frame_recording)
		return INVALID_TASK_HANDLE;
	frame_recording = 0;

	/* Flush staging (texture uploads) + dynamic-buffer mapped ranges so the GPU sees the 2D vertex/
	 * index/uniform data recorded this frame. R_SubmitStagingBuffers runs its own staging cb+fence;
	 * R_FlushDynamicBuffers flushes the non-coherent mapped dynamic buffers. Mirrors the head of
	 * GL_EndRenderingTask. Must happen before the queue submit below. */
	R_SubmitStagingBuffers ();
	R_FlushDynamicBuffers ();

	/* End the UI render pass + command buffer, submit on the graphics queue, and wait. The
	 * render pass's storeOp=STORE writes the rendered tiles to the fb0 scanout BO -> the frame
	 * is visible on HDMI. This single-image scanout IS the "present" (no WSI swapchain, no
	 * vkQueuePresentKHR). Synchronous submit + wait mirrors the proven Tier-4b harness. */
	PFN_vkCmdEndRenderPass pEndRP	= (PFN_vkCmdEndRenderPass) gdpa ("vkCmdEndRenderPass");
	PFN_vkEndCommandBuffer pEnd		= (PFN_vkEndCommandBuffer) gdpa ("vkEndCommandBuffer");
	PFN_vkQueueSubmit	   pSubmit	= (PFN_vkQueueSubmit) gdpa ("vkQueueSubmit");
	if (!pEndRP || !pEnd || !pSubmit) {
		Sys_Printf ("vkvid: GL_EndRendering: missing device proc\n");
		return INVALID_TASK_HANDLE;
	}

	/* BRING-UP: report the 2D draw activity recorded into THIS submitted cb before ending the
	 * render pass. draws>0 => 2D geometry IS in the presented cb (the gap is then rasterization:
	 * geometry/scissor/projection), draws==0 => the Draw_* calls never reached frame_cb (skipped
	 * early / null pics / wrong cb). null_layout_pc>0 flags the projection-matrix-lost ordering bug
	 * (GL_SetCanvas pushing before any pipeline bind). Logged for the first frames only. */
	if (present_count < 30 || (present_count % 60) == 0) {
		Sys_Printf ("vkvid: 2D draws=%lu drawIdx=%lu drawIndirect=%lu outsideRP=%lu nullLayoutPC=%lu | vrect=%dx%d@%d,%d\n",
		            g_draw_calls, g_draw_idx_calls, g_draw_ind_calls, g_draws_outside_rp, g_pc_null_layout,
		            r_refdef.vrect.width, r_refdef.vrect.height, r_refdef.vrect.x, r_refdef.vrect.y);
		fflush (stdout);
	}

	rp_active = 0;
	pEndRP (frame_cb);

	/* End every PCBX primary, then submit them together in PCBX order (RENDER_PASSES last) in a
	 * single vkQueueSubmit — that in-submit ordering is the sync vkQuake relies on so the warp/
	 * lightmap primaries complete before the render pass that samples their results. */
	VkResult err = VK_SUCCESS;
	for (int p = 0; p < PCBX_NUM; p++) {
		VkResult perr = pEnd (pcbx_cb[p]);
		if (perr != VK_SUCCESS) {
			Sys_Printf ("vkvid: vkEndCommandBuffer(pcbx %d) -> %d\n", p, (int)perr);
			return INVALID_TASK_HANDLE;
		}
	}

	VkSubmitInfo si = {
		.sType				= VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = PCBX_NUM,
		.pCommandBuffers	= pcbx_cb,
	};
	/* BRING-UP: unconditional flushed brackets around submit + device-wait for the first
	 * frames, so a grab can tell a SLOW frame (submit ok, wait returns late) from a true block
	 * (wait never returns -> "submitted N" with no "waited N"). Synchronous wait-per-frame is
	 * by design here (no WSI), so a slow caches-off GPU frame looks like a stall but isn't a hang.
	 * TODO(vkquake-port): remove with the other bring-up markers once frames sustain on HW. */
	if (present_count < 8) {
		Sys_Printf ("vkvid: submitting %lu\n", present_count + 1);
		fflush (stdout);
	}
	err = pSubmit (gfx_queue, 1, &si, VK_NULL_HANDLE);
	if (err != VK_SUCCESS)
		Sys_Printf ("vkvid: vkQueueSubmit -> %d\n", (int)err);
	if (present_count < 8) {
		Sys_Printf ("vkvid: submitted %lu, waiting idle\n", present_count + 1);
		fflush (stdout);
	}

	GL_WaitForDeviceIdle ();

	if (present_count < 8) {
		Sys_Printf ("vkvid: waited %lu\n", present_count + 1);
		fflush (stdout);
	}

	/* Per-frame present heartbeat (UART). storeOp=STORE just wrote this frame's tiles to the fb0
	 * scanout BO = the displayed surface, so a climbing count means frames ARE reaching HDMI.
	 * First frame logged unconditionally (so "any frame at all?" shows immediately), then every
	 * 30th to avoid flooding — caches-off vkQuake renders only a few frames/min, so a pure %30
	 * could hide the signal for minutes. Absent on the next boot => still loading game data /
	 * never reached the first SCR_UpdateScreen; climbing => rendering+presenting. */
	if (err == VK_SUCCESS) {
		present_count++;
		/* BRING-UP: log EVERY frame for the first 60 so the orchestrator can tell "stalled at
		 * 1" from "climbing" in a single grab (the old %30 gating hid frames 2..29). After 60,
		 * fall back to every 30th to avoid flooding the lwip-shared UART. */
		if (present_count <= 60 || (present_count % 30) == 0)
			Sys_Printf ("vkvid: present %lu\n", present_count);
	}
	return INVALID_TASK_HANDLE; /* inline: no async end task to chain */
}

/* ================================ menu + screenshot stubs ================================ */
void M_Menu_Video_f (void)
{
	/* Video options menu: no modes to pick on the fixed fb0 scanout. Stubbed. */
}

void M_Video_Draw (cb_context_t *cbx)
{
	(void)cbx;
}

void M_Video_Key (int key)
{
	(void)key;
}

void SCR_ScreenShot_f (void)
{
	/* Screenshot: would read back the scanout BO + write a PNG (lodepng is linked). Stubbed
	 * for first-light; the harness already proves scanout readback works. */
	Sys_Printf ("vkvid: screenshot not implemented\n");
}

/* Cursor shape for the menu's hover states (upstream 2026-09-04). The fb0 path
 * draws its own software pointer and has no window-system cursor to swap. */
void VID_SetMouseCursor(mousecursor_t cursor)
{
	(void)cursor;
}
