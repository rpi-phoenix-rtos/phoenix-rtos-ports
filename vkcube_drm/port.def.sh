#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="vkcube_drm"
	version="1.4.350"
	desc="vkcube-drm: upstream vkcube (Vulkan-Tools) on the new GPU lane -- Mesa v3dv static ICD + phxvk, VK_KHR_display"

	# Vulkan-Tools tag vulkan-sdk-1.4.350.0 = 1cb3a319969cf0d3e2315b0a87a27447f55b4167 (checked
	# identical to `git archive` of that commit); Vulkan headers 1.4.350 <= Mesa 26.2's 1.4.354
	tag="vulkan-sdk-1.4.350.0"
	source="https://github.com/KhronosGroup/Vulkan-Tools/archive/refs/tags"
	archive_filename=("Vulkan-Tools-${tag}.tar.gz" "${tag}.tar.gz")
	src_path="Vulkan-Tools-${tag}/"

	size="811196"
	sha256="3079796d51b29ce49dc7b7c7e243df93b343d54c3be9d4a8292c3231b9698deb"

	license="Apache-2.0"
	license_file="LICENSE.txt"

	conflicts="vkcube_drm!=${version}"
	depends="mesa_drm[vulkan] libdrm_phoenix zlib"

	# rootfs: install /bin/vkcube-drm into the image
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/vulkan-drm/build.sh: Mesa 26.2 v3dv as a
# STATIC ICD (mesa_drm[vulkan]) + phxvk (the static Vulkan-loader stand-in, installed by
# mesa_drm's vulkan build) + upstream vkcube with its VK_KHR_display WSI only (patch 0001: take
# vkGetInstanceProcAddr from the linked-in ICD) + libdrm-phoenix, linked -Wl,--wrap=mmap
# -Wl,--wrap=ioctl (BO maps; sync_file ioctls) -> one static binary.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local V="${PORT_DEP_mesa_drm}/vulkan" vt="${PREFIX_PORT_WORKDIR}" obj="${PREFIX_PORT_BUILD}/obj"
	local ICD="${V}/prefix/lib/libvulkan_broadcom.a" VKINC="${V}/prefix/include" PHXVK="${V}/phxvk"
	local p
	for p in "${ICD}" "${VKINC}/vulkan/vulkan.h" "${PHXVK}/phxvk_loader.c" "${PORT_DEP_mesa_drm}/compat/libmesadrm-compat.a"; do
		[ -e "${p}" ] || b_die "missing ${p} (mesa_drm built without USE vulkan?)"
	done
	rm -rf "${obj}"
	mkdir -p "${obj}"
	# vkcube: display WSI only (no xcb/xlib/wayland); SPIR-V shaders are the committed .inc files
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wno-unused-function "${NL_TFLAGS[@]}" -DVK_USE_PLATFORM_DISPLAY_KHR \
		-I"${vt}/cube" -I"${VKINC}" -c "${vt}/cube/cube.c" -o "${obj}/cube.o"
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${PHXVK}" -I"${VKINC}" \
		-c "${PHXVK}/phxvk_loader.c" -o "${obj}/phxvk_loader.o"

	# The ICD archive goes in WHOLE: Mesa's generated dispatch tables reference the driver's
	# entry points (v3dv_*, wsi_*, vk_common_*) as WEAK symbols, and a weak reference never pulls
	# an archive member -- linked normally, every entry point whose object nothing else references
	# would silently resolve to NULL. --gc-sections then drops what the tables do not reach.
	# -Wl,--wrap=mmap: v3dv maps BOs with mmap(render_fd, MMAP_BO token); -Wl,--wrap=ioctl: v3dv
	# merges its per-queue fences with libsync's raw ioctl(SYNC_IOC_MERGE) on every signalling
	# vkQueueSubmit, which libdrm-phoenix's __wrap_ioctl answers in-process (M5 section 9).
	local out="${PREFIX_PORT_BUILD}/out" whole="" L=() l
	while IFS= read -r l; do
		case "${l}" in "--whole-archive "*) whole="${l#--whole-archive }" ;; *) L+=("${l}") ;; esac
	done < "${V}/link.txt"
	[ "${whole}" -ef "${ICD}" ] || b_die "${V}/link.txt does not start with the ICD"
	mkdir -p "${out}"
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=mmap -Wl,--wrap=ioctl \
		-Wl,-Map,"${out}/vkcube-drm.map" -o "${out}/vkcube-drm" "${obj}/cube.o" "${obj}/phxvk_loader.o" \
		-Wl,--whole-archive "${ICD}" -Wl,--no-whole-archive \
		-Wl,--start-group "${L[@]}" -Wl,--end-group -lm
	"${NL_STRIP}" -o "${out}/vkcube-drm.stripped" "${out}/vkcube-drm"

	# --- verification ------------------------------------------------------------------------
	nl_no_undefined "${out}/vkcube-drm"
	local s bad=0 dis direct
	for s in phxvk_GetInstanceProcAddr vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion \
			v3dv_CreateInstance vk_common_QueueSubmit2 v3dv_queue_driver_submit v3dv_CmdDraw v3dv_CreateGraphicsPipelines \
			v3dv_AllocateMemory v3dv_GetMemoryFdKHR wsi_CreateDisplayPlaneSurfaceKHR wsi_GetPhysicalDeviceDisplayPropertiesKHR \
			wsi_CreateSwapchainKHR wsi_QueuePresentKHR wsi_AcquireNextImage2KHR \
			__wrap_mmap __wrap_ioctl drmPhoenixMmap drm_phoenix_ioctl drmModeAtomicCommit drmCrtcQueueSequence; do
		nl_has_sym "${out}/vkcube-drm" "${s}" || { echo "symbol ${s}: NO"; bad=1; }
	done
	for s in 'phxvk: new GPU lane' 'V3D %d.%d.%d.%d' VK_KHR_display VK_KHR_swapchain 'libdrm-phoenix:' DRMPHX_TRACE \
			/dev/dri/card0 /dev/dri/renderD128 /dev/dri/card1 /kmsbuf 'brcm,2711-v3d'; do
		echo "vkcube-drm strings '${s}': $(nl_count_strings "${out}/vkcube-drm.stripped" "${s}")"
	done
	nl_forbid_old_lane "${out}/vkcube-drm.stripped" V3DV_PHOENIX /dev/fb0 RPI4FB_GETMODE pl_phoenix PL_VkHostAllocator vkquake
	# every raw ioctl() must go through __wrap_ioctl (the sync_file emulation)
	dis="$("${NL_OBJDUMP}" -d --no-show-raw-insn "${out}/vkcube-drm")"
	direct="$(awk '/^[0-9a-f]+ <[^>]+>:$/ {fn=$2} /\tbl\t[0-9a-f]+ <ioctl>$/ {print fn}' <<< "${dis}" | sort -u)"
	if [ -n "${direct}" ] && grep -qvx '<__wrap_ioctl>:' <<< "${direct}"; then
		echo "ioctl() called outside __wrap_ioctl: ${direct}"; bad=1
	fi
	[ "${bad}" = 0 ] || b_die "vkcube-drm: verification failed"

	local p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${p}/bin" "${p}/prog"
	install -m 755 "${out}/vkcube-drm" "${p}/prog/vkcube-drm"
	install -m 755 "${out}/vkcube-drm.stripped" "${p}/bin/vkcube-drm"
	if b_use rootfs; then
		b_install "${p}/bin/vkcube-drm" /bin
	fi
}
