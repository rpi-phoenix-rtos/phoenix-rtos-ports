#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="vkquake"
	# vkQuake's in-tree VKQUAKE_VERSION at the pinned commit is 1.34 (Quake/quakever.h).
	version="1.34"
	desc="vkQuake (Vulkan Quake I engine) — single static ELF on the ported V3DV Vulkan ICD + Mesa/V3D back-end"

	# vkQuake's authoritative repo is Novum/vkQuake; this port is pinned to the specific
	# upstream master commit the tools/vkquake-port bring-up validated (VKQUAKE_VERSION 1.34,
	# the last upstream commit before the Phoenix bring-up work — vsonnier "Missing Clang
	# format", 2026-05-29). GitHub serves the tree as a content-addressed commit-archive at
	# /archive/<sha>.tar.gz, so the fetched bytes are reproducible (same pattern as the
	# quake3 / quakespasm / yquake2 ports). The archive's top-level directory is the
	# repo-name-cased "vkQuake-<sha>/". Bump `commit` + size/sha256 together if the pin moves.
	commit="9be3a5addeb3023396299efd588627e39345f451"
	source="https://github.com/Novum/vkQuake/archive"
	archive_filename="${commit}.tar.gz"
	src_path="vkQuake-${commit}/"

	size="22885227"
	sha256="8b913a17066d85904c53788bbdfd681f626b235caa27711a315f56356b7895f8"

	license="GPL-2.0-or-later"
	license_file="LICENSE.txt"

	conflicts=""
	# NO sdl2 dependency (unlike the GL game ports): vkQuake's SDL usage is shimmed
	# ENTIRELY — a header shim (glue/sdl-shim/SDL.h) plus the SDL2 threading/path bodies
	# in glue/pl_phoenix_sdlcompat.c — so no libSDL2.a is linked. See the header note.
	depends=""

	supports="phoenix>=3.3"
}

# Framework migration of tools/vkquake-port/build-vkquake-phoenix.py (the vkQuake
# bring-up). vkQuake is a single binary upstream (no .so game/renderer split), so the
# port compiles its Vulkan renderer + engine core + a Phoenix platform backend
# (glue/pl_phoenix_*) into ONE static ELF installed as /usr/bin/vkquake. The id1 game
# data (pak0.pak …) is a RUNTIME concern staged separately under /usr/share/quake; the
# port builds only the engine binary (with a small base pak embedded so it boots).
#
# SHIPPING STATE: this port is registered `if: true` in the rpi4b project's ports.yaml,
# so an image build installs /usr/bin/vkquake INTO THE ROOTFS and it ships on the SD
# image ALONGSIDE the GL engines — the old "loader.disk fits only one large GL/VK
# binary, so ship either quakespasm or vkquake" constraint is gone, together with the
# _user/rpi4-vkquake wrapper and the ad-hoc libvkquake.a archive step.
#
# HOW vkQuake DIFFERS from the GL game ports (quake3 / quakespasm / yquake2):
#   * It renders via VULKAN on the ported V3DV ICD (SPIR-V -> NIR -> QPU), NOT the
#     Mesa/V3D *GL* stack. So it links libv3dv-phoenix.a (the V3DV Vulkan ICD, which
#     bundles the Mesa Vulkan runtime layer) + libv3d-phoenix.a (the V3D back-end) —
#     NOT libGL-phoenix.a. There is no SDL2 dependency and no libstdc++.
#   * The engine calls core Vulkan commands DIRECTLY as link symbols, but the ICD does
#     not export the public vk* symbols. A generated trampoline layer
#     (glue/vk_trampolines.c) forwards each directly-called core command through
#     vkGetInstanceProcAddr/vkGetDeviceProcAddr (aliased to v3dv_* inside the ICD).
#   * There is no window-system integration (no WSI/swapchain): the Vulkan vid shim
#     (glue/pl_phoenix_vk_vid.c) renders into a LINEAR VkImage mapped onto /dev/fb0.
#     For that no-WSI fb0 path, four renderer helpers that are file-static upstream
#     (R_CreateShaderModules / R_DestroyShaderModules / R_InitVertexAttributes /
#     R_CreateBasicPipelines) are exposed so the shim can drive them — this is the
#     load-bearing part of the single documented patch (see below).
#
# The clone-local diagnostic scaffolding (the SCR_DrawGUI 2D bisector harness, the
# per-shader-module and gate-trace Sys_Printf traces, the tools/dbg-probe watchdog wired
# into main) is intentionally NOT part of this port: those are visual-regression /
# bring-up debugging aids, not engine behaviour. A clean upstream tarball is built with a
# single documented patch (patches/0001-vkquake-phoenix-v3dv-single-elf.patch) that
# carries ONLY the genuine Phoenix/V3DV engine fixes:
#   * gl_rmisc.c / glquake.h — expose the four formerly-static renderer helpers for the
#                              no-WSI fb0 path; route shader-module create/destroy through
#                              the mmap-backed PL_VkHostAllocator() (a >page libphoenix
#                              malloc faults); back-face cull ON for 3D (upstream) but NONE
#                              for the basic 2D/UI pipelines; skip pipeline variants whose
#                              render pass is VK_NULL_HANDLE; re-enable md5_vert.
#   * gl_texmgr.c — #29 fix: re-derive each copy region's imageExtent from the glt dims at
#                   point-of-use (the built extent reaches the aarch64 driver as 0x1x1).
#   * gl_screen.c — drop the SCR_DrawGUI GL_SetCanvas/R_BindPipeline (the fb0 vid shim owns
#                   the 2D canvas + pipeline bind).
#   * r_alias.c / Shaders/alias_common.inc — present opaque alias models with alpha=1 (the
#                   no-WSI fb0 scanout uses the color-buffer alpha).
#   * host_cmd.c — skip the auto-demo world-load; go straight to the menu (the world is
#                  loaded later inside the frame loop). TODO(vkquake-port): restore the
#                  demo loop once the world-render path is proven on HW.
#   * sv_main.c — guard SV_LocalSound against a NULL client (start-up NULL-deref crash).
#   * sys_sdl.c — slurp files into RAM and close immediately (NFS perf + demo-playback fix;
#                 mirrors the quakespasm-port file layer).
# Same clone-local-diagnostic exclusion the quake3 (Q3CAP) / quakespasm (QSS) / yquake2
# (YQ2CAP) ports made.
#
# GENERATED build-infra vendored into glue/ (these are generated artifacts, committed the
# same way tools/vkquake-port commits them; regenerate after the noted inputs change):
#   * glue/vk_trampolines.c   — the public-vk*-command trampolines. Regenerate via
#                               tools/vkquake-port/gen-vk-trampolines.py after a Vulkan
#                               header bump.
#   * glue/vkquake_shaders.c  — the embedded-SPIR-V arrays (*_spv / *_spv_size), REAL
#                               bytes (glslangValidator + spirv-opt). Regenerate via
#                               tools/vkquake-port/gen-vkquake-shaders.py after a Shaders/
#                               change (e.g. the alias_common.inc patch above).
# The base pak (embedded_pak.c: gfx/maps/default.cfg compressed + bin2c'd) is NOT vendored
# — it is GENERATED from the upstream tarball at build time (`make -C Misc/vq_pak`, which
# uses its own HOST_CC), keeping id game-data out of the ports tree.
#
# GPU coupling note (V3DV analogue of the GL ports' wart): libv3dv-phoenix.a /
# libv3d-phoenix.a (tools/.gpu-libs) and the Vulkan headers (external/mesa/include) are
# prebuilt artifacts of the Mesa/V3DV stack, which is not yet a framework port. Until it
# is, this recipe links those archives + includes those headers by absolute path, anchored
# off PREFIX_PORT. Portifying that GPU stack would remove this wart from all the game ports.

# ---- Repository anchors (this port reaches artifacts outside the ports tree) ----
_vkq_repo_root() { (cd "${PREFIX_PORT}/../../.." && pwd); }

p_prepare() {
	# Single documented patch: the genuine Phoenix/V3DV engine fixes only (the clone-local
	# 2D bisector / trace / watchdog diagnostics are excluded — see header note).
	# The patch below is GENERATED from our fork (external/<game>, branch
	# phoenix-rpi4-port) by scripts/game-port-patch.sh in the coordination
	# repo. Do NOT hand-edit it: commit to the fork and re-run --regen, or
	# the two representations drift apart (they did -- see
	# docs/misc/2026-09-02-game-source-of-truth-audit.md).
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	local repo_root src shaders_src glue_dir gpu_libs vkinc compat
	repo_root="$(_vkq_repo_root)"
	src="${PREFIX_PORT_WORKDIR}/Quake"
	shaders_src="${PREFIX_PORT_WORKDIR}/Shaders"
	glue_dir="${PREFIX_PORT}/glue"
	gpu_libs="${repo_root}/tools/.gpu-libs"
	vkinc="${repo_root}/external/mesa/include"     # <vulkan/vulkan_core.h>
	compat="${glue_dir}/vkq_phoenix_compat.h"

	local v3dvlib="${gpu_libs}/libv3dv-phoenix.a"
	local v3dlib="${gpu_libs}/libv3d-phoenix.a"

	# --- Prerequisite artifacts of the not-yet-portified V3DV/Vulkan stack (fail loud) ---
	local p missing=0
	for p in "${v3dvlib}" "${v3dlib}"; do
		[ -f "${p}" ] || { echo "vkquake: MISSING prerequisite file: ${p}" >&2; missing=1; }
	done
	[ -f "${vkinc}/vulkan/vulkan_core.h" ] || { echo "vkquake: MISSING Vulkan header: ${vkinc}/vulkan/vulkan_core.h" >&2; missing=1; }
	if [ "${missing}" != 0 ]; then
		b_die "V3DV/Vulkan stack not present. Rebuild it first: sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/build-v3dv-phoenix.py (and build-v3d-phoenix.py); it stages libv3dv/libv3d in tools/.gpu-libs. Vulkan headers come from external/mesa/include."
	fi

	# --- Generate the embedded base pak (embedded_pak.c) from the upstream tarball. The
	#     Makefile uses its own HOST_CC=gcc (immune to the exported cross ${CC}); it builds
	#     mkpak + bintoc on the host, packs Misc/vq_pak/{gfx,maps,default.cfg}, and writes
	#     Quake/embedded_pak.c (which defines vkquake_pak / _size / _decompressed_size that
	#     common.c externs). ---
	make -C "${PREFIX_PORT_WORKDIR}/Misc/vq_pak"
	[ -f "${src}/embedded_pak.c" ] || b_die "vkquake: embedded_pak.c not generated (make -C Misc/vq_pak failed)"

	# --- Compile flags. quakedef.h pulls <vulkan/vulkan_core.h> + vid.h, so vkinc and the
	#     SDL header shim are both on the path. -include pulls the force-included compat shim
	#     (arm_neon + libphoenix math-gap decls + ipv6_mreq). -DUSE_SDL2 keeps the SDL2
	#     codepaths (SDL is shimmed). Framework CFLAGS first so -mcpu/sysroot apply; port
	#     flags after so they win. -fno-omit-frame-pointer keeps the x29 chain walkable. ---
	local cflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -fno-omit-frame-pointer -Wno-error -DLINUX -D_GNU_SOURCE -DUSE_SDL2 -include ${compat} -I${glue_dir}/sdl-shim -I${src} -I${vkinc} -I${shaders_src}"

	# ---- TU lists, transcribed from external/vkquake/meson.build `srcs` minus the SDL /
	#      platform TUs Phoenix owns (gl_vidsdl / in_sdl* / snd_sdl* / sys_sdl_{unix,win} /
	#      main_sdl / pl_linux / net_bsd / cd_sdl and the optional codec backends). sys_sdl.c
	#      is PORTABLE (the integer file-handle layer, no SDL dep) so it stays in ENGINE. ----
	local engine=(
		bgmusic cd_null cfgfile chase cl_demo cl_input cl_main
		cl_parse cl_tent cmd common console crc cvar
		gl_draw gl_fog gl_heap gl_mesh gl_model gl_refrag gl_rlight
		gl_rmain gl_rmisc gl_screen gl_sky gl_texmgr gl_warp
		host host_cmd image keys mathlib mdfour mem menu miniz
		net_dgrm net_loop net_main net_udp palette pr_cmds pr_edict
		pr_exec pr_ext r_alias r_brush r_part r_part_fte r_sprite
		r_world sbar snd_codec snd_dma snd_mem snd_mix snd_umx
		snd_wave strlcat strlcpy sv_main sv_move sv_phys sv_user
		tasks view wad world hash_map quakedef lodepng
		sys_sdl
		embedded_pak
	)
	# Phoenix platform shims (glue/): replace the EXCLUDEd SDL/platform TUs. The tools build's
	# dbg-probe watchdog (tools/dbg-probe/dbg.c) is a diagnostic and is NOT compiled in.
	local shims=(
		pl_phoenix_sdlcompat pl_phoenix_sys pl_phoenix_in pl_phoenix_snd
		pl_phoenix_main pl_phoenix_stubs pl_phoenix_vk_vid
	)
	# Generated build-infra (vendored into glue/): the vk* trampolines + embedded SPIR-V.
	local generated=(vk_trampolines vkquake_shaders)

	local objdir="${PREFIX_PORT_WORKDIR}/_phoenix_obj"
	mkdir -p "${objdir}"

	# One gcc per TU, compiled in parallel; fail fast with the TU path.
	_vkq_cc() {
		local unit="$1" base="$2" obj
		obj="${VKQ_OBJDIR}/${unit//\//_}.o"
		# shellcheck disable=2086
		if ! ${CC} ${VKQ_CFLAGS} -o "${obj}" "${base}/${unit}.c"; then
			echo "!!! vkquake: compile FAILED: ${base}/${unit}.c" >&2
			exit 1
		fi
	}
	export -f _vkq_cc
	export CC VKQ_OBJDIR="${objdir}" VKQ_CFLAGS="${cflags}"

	{
		local u
		for u in "${engine[@]}"; do printf '%s\t%s\n' "${u}" "${src}"; done
		for u in "${shims[@]}"; do printf '%s\t%s\n' "${u}" "${glue_dir}"; done
		for u in "${generated[@]}"; do printf '%s\t%s\n' "${u}" "${glue_dir}"; done
	} | xargs -P"$(nproc)" -L1 bash -c '_vkq_cc "$@"' _

	# Deterministic object list (compile order above is parallel/non-deterministic).
	local objs=() u
	for u in "${engine[@]}" "${shims[@]}" "${generated[@]}"; do
		local o="${objdir}/${u//\//_}.o"
		[ -f "${o}" ] || b_die "vkquake: object missing after compile: ${o}"
		objs+=("${o}")
	done

	# Archive every engine/shim/trampoline/generated object, then whole-archive-link against
	# the V3DV ICD + V3D back-end (mirrors tools/vkquake-port/build-vkquake-phoenix.py --link).
	#
	# whole-archive on libvkquake.a forces every engine TU into the link so its real Vulkan
	# references enter the closure (a bare-archive link only pulls what crt0's main reaches).
	# whole-archive on libv3dv is ALSO load-bearing: the Mesa Vulkan runtime layer bundled in
	# it exposes vk_common_* fallback entrypoints reachable only through WEAK relocs in the
	# generated dispatch tables; weak refs do NOT pull archive members, so without whole-archive
	# those dispatch slots resolve to NULL -> pc=0 abort at runtime (HW-proven). The V3D back-end
	# stays in the normal group. --allow-multiple-definition tolerates the ICD/runtime overlaps.
	local lib="${objdir}/libvkquake.a"
	rm -f "${lib}"
	"${AR}" rcs "${lib}" "${objs[@]}"

	# --build-id: V3DV's init_uuids() walks the ELF for a build-id note to derive the
	# pipeline-cache UUID; without the note create_physical_device fails with
	# "Failed to find build-id" and no Vulkan device is created (HW-proven via the
	# _user/rpi4-vkquake link this recipe replaces).
	# -z stack-size=32 MiB: PT_GNU_STACK p_memsz is what the kernel (process.c) uses
	# for the MAIN-thread stack, and vkQuake runs Host_Init + the Host_Frame loop on
	# the main thread (glue/pl_phoenix_main.c) with deep recursive render frames.
	# Both flags were present in the HW-verified _user/rpi4-vkquake link and were
	# missing here, so a ports-built vkquake had no build-id note and no PT_GNU_STACK
	# at all (verified with readelf on 2026-09-03).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086
	"${CC}" ${CFLAGS} ${LDFLAGS} \
		-Wl,--build-id -Wl,--allow-multiple-definition \
		-Wl,--whole-archive "${lib}" "${v3dvlib}" -Wl,--no-whole-archive \
		-Wl,--start-group "${v3dvlib}" "${v3dlib}" -Wl,--end-group -lm \
		-Wl,-z,stack-size=33554432 \
		-o "${PREFIX_PROG}/vkquake"

	"${STRIP}" -o "${PREFIX_PROG_STRIPPED}/vkquake" "${PREFIX_PROG}/vkquake"
	b_install "${PREFIX_PROG_TO_INSTALL}/vkquake" /usr/bin
}
