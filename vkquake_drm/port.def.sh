#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="vkquake_drm"
	version="1.34"
	desc="vkquake-drm + vkq-drm launcher: UPSTREAM vkQuake on the new GPU lane (SDL KMSDRM Vulkan + Mesa v3dv static ICD via phxvk)"

	# the vkquake port's pinned upstream commit (same archive, same sha256)
	commit="1aa13a56cdf1b8c18a556e8e48a71a559b925d5a"
	source="https://github.com/Novum/vkQuake/archive"
	archive_filename="${commit}.tar.gz"
	src_path="vkQuake-${commit}/"

	size="23941364"
	sha256="3e1a3bc4e8059cffc6246736e1d07f8656b8eba1619233e480010658ee3f3701"

	license="GPL-2.0-or-later"
	license_file="LICENSE.txt"

	# NEW GPU LANE: private prefix; /usr/bin/vkquake and the vkquake port are untouched. Not a
	# relink (unlike yquake2_drm/quake3_drm/supertuxkart_drm): the vkquake port replaces every
	# SDL/platform TU with a no-WSI /dev/fb0 shim, so this is upstream vkQuake with upstream's TU
	# list; the vkquake port is NOT a dependency (its SPIR-V arrays are read from its glue/).
	conflicts="vkquake_drm!=${version}"
	depends="sdl2_kmsdrm[vulkan] mesa_drm[vulkan] libdrm_phoenix zlib"

	# rootfs: install /usr/bin/vkquake-drm and its launcher /bin/vkq-drm into the image,
	# the launcher also as /usr/bin/vkquake
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build-vkquake-drm.sh (steps 2-9;
# its step 1, the SDL_VULKAN=ON SDL, is sdl2_kmsdrm's USE vulkan build):
#   patches/0001-0004  the vkquake port's engine fixes that do not concern video (cmdline on
#                      shareware, SV_LocalSound NULL guard, slurp-and-close reads, #29 texture
#                      copy extents); 0005 no timestamp query pool until the render server serves
#                      SUBMIT_CPU (gap G5); 0006 r_oit 0 and 0007 RGBA8 colour buffer (V3D
#                      performance defaults) -- = tools/gpu-lane/sdl2-drm/patches-vkquake/
#   glue/vkqdrm/       hooks (SDL_LoadObject/LoadFunction answered with phxvk; the flipstat /
#                      presentstat counter), the vk* trampoline generator, the launcher, and two
#                      libphoenix-gap bridges (vkqdrm_compat.h: <arm_neon.h> + struct ipv6_mreq;
#                      include/execinfo.h: zero-frame backtrace) that step aside by themselves
#                      once the sysroot has them -- = tools/gpu-lane/sdl2-drm/vkqdrm/
#   SPIR-V             the vkquake port's vendored glue/vkquake_shaders.c (this commit's shaders)
#   GL stubs           generated: SDL's KMSDRM GL half references gbm_*/egl*, which a Vulkan-only
#                      window never calls; linking Mesa's GL build too would put a second copy
#                      of Mesa's util/NIR/broadcom compiler next to the ICD's
# The embedded base pak (Misc/vq_pak) is built with the HOST compiler, as upstream does.
#
# PENDING (2026-09-27): tools/gpu-lane/sdl2-drm/patches-vkquake is about to get 0008 (raster
# warp) promoted and a new CPU-lightmap default. When they land there, copy them into patches/
# here (scripts/check-gpu-lane-ports-sync.sh in the coordination repo reports the drift until
# then); this recipe applies every patches/*.patch in order, so no recipe change is needed.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"
	nl_host_python "${PREFIX_PORT_BUILD}/nl"

	local target=/usr/bin/vkquake-drm
	local Q="${PREFIX_PORT_WORKDIR}" V="${PORT_DEP_mesa_drm}/vulkan" SVP="${PORT_DEP_sdl2_kmsdrm}/vulkan"
	local ICD="${V}/prefix/lib/libvulkan_broadcom.a" VKINC="${V}/prefix/include" PHXVK="${V}/phxvk"
	local COMPAT_A="${PORT_DEP_mesa_drm}/compat/libmesadrm-compat.a" SDL_A="${SVP}/lib/libSDL2.a"
	local VKQ_SHADERS_C="${PREFIX_PORT}/../vkquake/glue/vkquake_shaders.c" G="${PREFIX_PORT}/glue/vkqdrm"
	local GD="${PORT_DEP_sdl2_kmsdrm}/share/gamedrm" LDA="${PORT_DEP_libdrm_phoenix}/lib/libdrm.a"
	local p
	for p in "${ICD}" "${VKINC}/vulkan/vulkan_core.h" "${PHXVK}/phxvk_loader.c" "${COMPAT_A}" "${SDL_A}" \
			"${VKQ_SHADERS_C}" "${LDA}" "${PORT_DEP_zlib}/lib/libz.a"; do
		[ -e "${p}" ] || b_die "missing ${p}"
	done
	local cfg="${SVP}/include/SDL2/SDL_config.h" d
	for d in SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_VULKAN SDL_VIDEO_OPENGL_EGL SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX; do
		grep -qE "^#define ${d} +1" "${cfg}" || b_die "the Vulkan SDL's SDL_config.h lacks ${d} 1"
	done

	# --- the embedded base pak (gfx/maps/default.cfg), HOST compiler (Misc/vq_pak's HOST_CC) ----
	if [ ! -f "${Q}/Quake/embedded_pak.c" ]; then
		make -C "${Q}/Misc/vq_pak" || b_die "embedded pak generation failed"
	fi
	[ -f "${Q}/Quake/embedded_pak.c" ] || b_die "Quake/embedded_pak.c not generated"

	# --- compile: upstream meson.build `srcs` + its non-Windows block + embedded_pak + SPIR-V ----
	local engine=(bgmusic cd_null cfgfile chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar
		gl_draw gl_fog gl_heap gl_mesh gl_model gl_refrag gl_rlight gl_rmain gl_rmisc gl_screen gl_sky gl_texmgr
		gl_vidsdl gl_warp host host_cmd image json in_sdl in_sdl2 in_sdl3 keys main_sdl mathlib mdfour mem menu
		net_dgrm net_loop net_main palette pr_cmds pr_edict pr_exec pr_ext r_alias r_brush r_part r_part_fte
		r_sprite r_world sbar snd_codec snd_dma snd_mem snd_mix snd_sdl snd_sdl3 snd_umx snd_wave steam strlcat
		strlcpy sv_main sv_move sv_phys sv_user sys_sdl tasks view wad world hash_map
		net_bsd net_udp pl_linux sys_sdl_unix embedded_pak)
	# meson: c_std gnu11, -fno-omit-frame-pointer -fno-common, -D_FILE_OFFSET_BITS=64, USE_CODEC_WAVE
	# (the only default codec), release => NDEBUG; libphoenix's sched.h has no CPU_ZERO =>
	# TASK_AFFINITY_NOT_AVAILABLE (meson's own probe result).
	local QFLAGS=("${NL_TFLAGS[@]}" -std=gnu11 -O2 -g -fno-omit-frame-pointer -fno-common -Wno-trigraphs -D_FILE_OFFSET_BITS=64
		-DUSE_CODEC_WAVE -DNDEBUG -DTASK_AFFINITY_NOT_AVAILABLE -include "${G}/vkqdrm_compat.h"
		-I"${Q}/Quake" -I"${Q}/Quake/mimalloc" -I"${SVP}/include" -I"${SVP}/include/SDL2" -I"${VKINC}"
		-idirafter "${G}/include" -c)
	local OB="${PREFIX_PORT_BUILD}/obj" u o objs=() tus="${PREFIX_PORT_BUILD}/tus.txt"
	rm -rf "${OB}"
	mkdir -p "${OB}"
	{
		for u in "${engine[@]}"; do printf '%s\n' "${Q}/Quake/${u}.c"; done
		printf '%s\n' "${VKQ_SHADERS_C}"
	} > "${tus}"
	export OB
	vq_cc() { local f="$1" o; o="${OB}/$(basename "${f%.c}").o"; "${NL_CC}" "${@:2}" -o "${o}" "${f}" || { echo "compile FAILED: ${f}"; exit 1; }; }
	export -f vq_cc
	xargs -P"$(nproc)" -I{} bash -c 'vq_cc "$@"' _ {} "${QFLAGS[@]}" < "${tus}" || b_die "vkQuake compile failed"
	while IFS= read -r u; do
		o="${OB}/$(basename "${u%.c}").o"
		[ -f "${o}" ] || b_die "object missing: ${o}"
		objs+=("${o}")
	done < "${tus}"

	# --- the vk* trampolines (generated from the objects' own undefined vk* symbols) --------------
	local defined gen="${PREFIX_PORT_BUILD}/gen"
	mkdir -p "${gen}"
	defined="$(for o in "${objs[@]}"; do "${NL_NM}" -g --defined-only "${o}"; done | awk '{ print $3 }' | LC_ALL=C sort -u)"
	for o in "${objs[@]}"; do "${NL_NM}" -u "${o}"; done | awk '{ print $2 }' | grep -E '^vk[A-Z]' | LC_ALL=C sort -u \
		| LC_ALL=C comm -23 - <(printf '%s\n' "${defined}") > "${gen}/vk-direct-calls.txt"
	python3 "${G}/gen-vk-trampolines.py" "${VKINC}/vulkan/vulkan_core.h" "${gen}/vk-direct-calls.txt" \
		"${OB}/vkqdrm_vk_trampolines.c" || b_die "trampoline generation failed"

	# --- GL stubs for the KMSDRM GL half SDL references --------------------------------------------
	local sdl_defs n
	sdl_defs="$("${NL_NM}" -g --defined-only "${SDL_A}" 2>/dev/null | awk 'NF >= 3 { print $3 }' | LC_ALL=C sort -u)"
	"${NL_NM}" -u "${SDL_A}" 2>/dev/null | awk '{ print $2 }' | grep -E '^(gbm_|egl[A-Z])' | LC_ALL=C sort -u \
		| LC_ALL=C comm -23 - <(printf '%s\n' "${sdl_defs}") > "${gen}/gl-stub-names.txt"
	{
		cat <<'EOF'
/*
 * GENERATED by build-vkquake-drm.sh -- do not edit. The gbm_* / egl* functions that libSDL2.a's
 * KMSDRM GL path references (sdl2-drm patch 0006 binds them statically). vkquake-drm creates only
 * an SDL_WINDOW_VULKAN window, whose KMSDRM path uses neither; Mesa's GL build is therefore not
 * linked, and each of these prints its name once and fails (0 / NULL / EGL_FALSE / EGL_NO_*).
 */
#include <unistd.h>

static void vkqdrm_nogl(const char *name, unsigned long len)
{
	static const char msg[] = "vkquake-drm: GL path not linked in this binary, called: ";
	(void)write(2, msg, sizeof(msg) - 1u);
	(void)write(2, name, len);
	(void)write(2, "\n", 1u);
}
EOF
		while IFS= read -r n; do
			printf '\nlong %s(void);\nlong %s(void)\n{\n\tvkqdrm_nogl("%s", %d);\n\treturn 0;\n}\n' "$n" "$n" "$n" "${#n}"
		done < "${gen}/gl-stub-names.txt"
	} > "${OB}/vkqdrm_nogl.c"

	# --- the rest of the program: hooks, trampolines, stubs, phxvk --------------------------------
	local cc_strict=("${NL_CC}" -O2 -g -std=gnu17 -Wall -Wextra -Werror "${NL_TFLAGS[@]}")
	"${cc_strict[@]}" -I"${SVP}/include" -I"${PHXVK}" -I"${VKINC}" -c "${G}/vkqdrm_hooks.c" -o "${OB}/vkqdrm_hooks.o"
	"${cc_strict[@]}" -Wno-unused-parameter -I"${VKINC}" -c "${OB}/vkqdrm_vk_trampolines.c" -o "${OB}/vkqdrm_vk_trampolines.o"
	"${cc_strict[@]}" -c "${OB}/vkqdrm_nogl.c" -o "${OB}/vkqdrm_nogl.o"
	"${cc_strict[@]}" -I"${PHXVK}" -I"${VKINC}" -c "${PHXVK}/phxvk_loader.c" -o "${OB}/phxvk_loader.o"
	local extra=("${OB}/vkqdrm_hooks.o" "${OB}/vkqdrm_vk_trampolines.o" "${OB}/vkqdrm_nogl.o" "${OB}/phxvk_loader.o")

	# --- link: vkcube-drm's shape (C++ driver, -static, --gc-sections, 4 KiB pages, the ICD
	# whole-archive, --wrap=mmap/ioctl) + SDL's loadso wrapped for the Vulkan "library" + the
	# port's 32 MiB main stack (vkQuake runs Host_Frame on the main thread) ------------------------
	local out="${PREFIX_PORT_BUILD}/out" elf
	mkdir -p "${out}"
	elf="${out}/vkquake-drm"
	rm -f "${elf}"
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=mmap -Wl,--wrap=ioctl \
		-Wl,--wrap=SDL_LoadObject -Wl,--wrap=SDL_LoadFunction -Wl,--wrap=SDL_UnloadObject \
		-Wl,-z,stack-size=33554432 -Wl,-Map,"${elf}.map" -o "${elf}" "${objs[@]}" "${extra[@]}" \
		-Wl,--whole-archive "${ICD}" -Wl,--no-whole-archive \
		-Wl,--start-group "${SDL_A}" "${LDA}" "${COMPAT_A}" "${PORT_DEP_zlib}/lib/libz.a" -Wl,--end-group -lm \
		|| b_die "vkquake-drm link failed"
	"${NL_STRIP}" -o "${elf}.stripped" "${elf}"

	# --- verification ----------------------------------------------------------------------------
	nl_no_undefined "${elf}"
	local s bad=0 syms calls
	syms="$("${NL_NM}" "${elf}")"
	for s in KMSDRM_CreateDevice KMSDRM_Vulkan_LoadLibrary KMSDRM_Vulkan_CreateSurface KMSDRM_Vulkan_GetInstanceExtensions \
			SDL_Vulkan_CreateSurface SDL_PHOENIX_HID_Poll __wrap_SDL_LoadObject __wrap_SDL_LoadFunction \
			vkqdrm_GetInstanceProcAddr phxvk_GetInstanceProcAddr vk_icdGetInstanceProcAddr v3dv_CreateInstance \
			v3dv_queue_driver_submit wsi_CreateDisplayPlaneSurfaceKHR wsi_CreateSwapchainKHR wsi_QueuePresentKHR \
			vkCreateInstance vkQueueSubmit vkCmdDraw __wrap_mmap __wrap_ioctl drmPhoenixMmap drm_phoenix_ioctl \
			drmModeAtomicCommit Host_Init VID_Init; do
		grep -qE " [TtWw] ${s}\$" <<< "${syms}" || { echo "symbol ${s}: NO"; bad=1; }
	done
	# no Mesa GL in this binary (the stubs stand in), no old-lane glue
	for s in gbmint_get_backend kmsro_drm_screen_create v3d_drm_screen_create_renderonly _mesa_glapi_get_proc_address \
			PL_VkHostAllocator R_CreateBasicPipelines g_vk_device PHOENIX_bootstrap phoenix_v3d_ioctl winsys_init; do
		if grep -qE " [TWDBR] ${s}\$" <<< "${syms}"; then echo "unexpected global symbol ${s}: PRESENT"; bad=1; fi
	done
	calls="$("${NL_OBJDUMP}" -d --no-show-raw-insn "${elf}" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
		/\tbl?\t/ && / <(ioctl|mmap|SDL_LoadObject|SDL_LoadFunction|__wrap_SDL_LoadObject|__wrap_SDL_LoadFunction)>$/ {
			t = $NF; gsub(/[<>]/, "", t); print t, fn }' | sort | uniq -c)"
	printf '%s\n' "${calls}" > "${out}/call-sites.txt"
	awk '$2 == "ioctl" && $3 != "__wrap_ioctl" { b = 1 } END { exit b }' <<< "${calls}" \
		|| { echo "real ioctl() called from outside __wrap_ioctl"; bad=1; }
	awk '$2 == "mmap" && $3 != "__wrap_mmap" { b = 1 } END { exit b }' <<< "${calls}" \
		|| { echo "real mmap() called from outside __wrap_mmap"; bad=1; }
	if ! grep -qE ' __wrap_SDL_LoadObject KMSDRM_Vulkan_LoadLibrary$' <<< "${calls}" \
			|| ! grep -qE ' __wrap_SDL_LoadFunction KMSDRM_Vulkan_LoadLibrary$' <<< "${calls}"; then
		echo "KMSDRM_Vulkan_LoadLibrary does not reach the loadso wraps"; bad=1
	fi
	# the SDL lineage carries the frame-pacing order (patches/0009) -- linked, not used by vkQuake
	GAMEDRM_OBJDUMP="${NL_OBJDUMP}" bash "${GD}/check-swap-order.sh" "${elf}" || bad=1
	for s in 'KMS/DRM Video Driver' '/dev/dri/' 'libdrm-phoenix:' 'DRMPHX_TRACE' 'DRMPHX sync' '/dev/kbd0' '/dev/audio0' \
			'VK_KHR_display' 'VK_KHR_swapchain' 'V3D %d.%d.%d.%d' 'phxvk: new GPU lane' 'vkquake-drm: new GPU lane' \
			'vkquake-drm flipstat' 'vkquake-drm presentstat' "Vulkan couldn't find an appropriate plane" 'vkQuake'; do
		[ "$(nl_count_strings "${elf}.stripped" "${s}")" != 0 ] || { echo "string '${s}': 0"; bad=1; }
	done
	nl_forbid_old_lane "${elf}.stripped" V3DV_PHOENIX /dev/fb0 RPI4FB_GETMODE pl_phoenix PL_VkHostAllocator vkvid: \
		phoenix-map.cfg vktramp:
	# inverse control, when the image build installed the old-lane vkquake
	local shipped="${PREFIX_FS%/}/root/usr/bin/vkquake"
	if [ -f "${shipped}" ]; then
		for s in 'vkquake-drm: new GPU lane' 'KMS/DRM Video Driver' 'libdrm-phoenix:' 'phxvk: new GPU lane'; do
			if grep -aqF -- "${s}" "${shipped}"; then echo "the shipped vkquake carries '${s}'"; bad=1; fi
		done
	fi
	[ "${bad}" = 0 ] || b_die "vkquake-drm: verification failed (see above)"

	# --- launcher ---------------------------------------------------------------------------------
	"${NL_CC}" -O2 -static -Wall -Wextra -Werror --sysroot="${NL_SYSROOT}/" -B"${NL_SYSROOT}/lib/" -iprefix "${NL_SYSROOT}/" \
		-DVKQDRM_TARGET="\"${target}\"" -o "${out}/vkq-drm" "${G}/vkq-drm-launcher.c"
	nl_no_undefined "${out}/vkq-drm"
	grep -aqF "${target}" "${out}/vkq-drm" || b_die "launcher ELF lacks its exec target ${target}"

	local pp="${PREFIX_PORT_INSTALL}"
	mkdir -p "${pp}/bin" "${pp}/prog" "${pp}/share/vkquake-drm"
	install -m 755 "${elf}" "${pp}/prog/vkquake-drm"
	install -m 755 "${elf}.stripped" "${pp}/bin/vkquake-drm"
	install -m 755 "${out}/vkq-drm" "${pp}/bin/vkq-drm"
	install -m 644 "${elf}.map" "${out}/call-sites.txt" "${gen}/vk-direct-calls.txt" "${gen}/gl-stub-names.txt" \
		"${pp}/share/vkquake-drm/"
	if b_use rootfs; then
		b_install "${pp}/bin/vkquake-drm" /usr/bin
		b_install "${pp}/bin/vkq-drm" /bin
		# TODO(TD-26): the plain command name runs this program (GPU migration P1: the default
		# image). P4 gives the programs the plain names themselves.
		install -m 755 "${pp}/bin/vkq-drm" "${PREFIX_FS}/root/usr/bin/vkquake"
	fi
}
