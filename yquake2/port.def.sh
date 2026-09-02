#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="yquake2"
	# Upstream YQ2VERSION at the pinned commit is 8.71pre; port_manager's resolver
	# wants a dotted/numeric version, so record the release-ish "8.71" here and keep
	# the authoritative provenance in `commit`/archive_filename/src_path below.
	version="8.71"
	desc="yQuake2 (Quake II engine) — single static ELF, ref_gl3/GLES3 on the ported SDL2 + Mesa/V3D GL stack"

	# yQuake2 tags its releases but this port is pinned to a specific master commit
	# (the one the tools/yquake2-port bring-up validated: YQ2VERSION 8.71pre,
	# 2026-08-01). GitHub serves the tree as a content-addressed commit-archive at
	# /archive/<sha>.tar.gz, so the fetched bytes are reproducible (same pattern as
	# the llama2 port). Bump `commit` + size/sha256 together if the pin moves.
	commit="e27fdcceb47769463b53b6d6f2e4c2ee572178b2"
	source="https://github.com/yquake2/yquake2/archive"
	archive_filename="${commit}.tar.gz"
	src_path="yquake2-${commit}/"

	size="2730313"
	sha256="e811f2b6b5659d401fbff3c22995ddb9827361e1b158cbd384fbccbfa4dd0106"

	license="GPL-2.0-or-later"
	license_file="LICENSE"

	conflicts=""
	depends="sdl2"

	supports="phoenix>=3.3"
}

# Framework migration of tools/yquake2-port/build-yquake2-phoenix.py (the C4 Quake II
# bring-up). Phoenix has no dlopen/dlsym, so yQuake2's two dynamic-load seams (the
# game DLL and the renderer DLL) are folded into ONE static ELF: client + integrated
# server + baseq2 game + one renderer (ref_gl3/GLES3 by default, ref_gl1 via
# YQ2_RENDERER=gl1) + a Phoenix backend (glue/pl_phoenix_*),
# linked against the ported SDL2 (depends="sdl2") + the Mesa/V3D GL stack. Installs
# the engine as /usr/bin/yquake2. The `quake2` launcher + `ram-stage-play` + the
# baseq2 game data (paks) are RUNTIME concerns staged separately (the port builds only
# the engine binary), matching how tools/yquake2-port/quake2-launcher.c execs it.
#
# The diagnostic capture harness (-DYQ2CAP_PHOENIX + the per-frame TGA/TCP capture
# hooks that live as local commits on the external/yquake2 clone) is intentionally
# NOT part of this port: it is a visual-regression debugging aid, not engine
# behaviour, so a clean upstream tarball + the single documented patch is built.
#
# GPU coupling note: libGL-phoenix.a / libv3d-phoenix.a (tools/.gpu-libs) and the
# Mesa headers (external/mesa) are prebuilt artifacts of the Mesa/V3D GL stack, which
# is not yet a framework port. Until it is, this recipe links those archives + the
# shared SDL2-GL glue (sources/phoenix-rtos-ports/sdl2/glue) by absolute path,
# anchored off PREFIX_PORT. Portifying that GL stack would remove this wart from all
# the SDL2 game ports at once.

# ---- Repository anchors (this port reaches artifacts outside the ports tree) ----
_yq2_repo_root() { (cd "${PREFIX_PORT}/../../.." && pwd); }

p_prepare() {
	# Single documented patch: fold the dlopen game/renderer seams and drop the
	# .so-era print forwarders so the client, ref_gl1 and baseq2 game link clean in
	# one binary (see patches/0001-single-elf-static-link.patch).
	# The patch below is GENERATED from our fork (external/<game>, branch
	# phoenix-rpi4-port) by scripts/game-port-patch.sh in the coordination
	# repo. Do NOT hand-edit it: commit to the fork and re-run --regen, or
	# the two representations drift apart (they did -- see
	# docs/misc/2026-09-02-game-source-of-truth-audit.md).
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	local repo_root src glue_dir sdl2_glue gpu_libs mesa mcompat compat
	repo_root="$(_yq2_repo_root)"
	src="${PREFIX_PORT_WORKDIR}/src"
	glue_dir="${PREFIX_PORT}/glue"
	sdl2_glue="$(cd "${PREFIX_PORT}/../sdl2/glue" && pwd)"
	gpu_libs="${repo_root}/tools/.gpu-libs"
	mesa="${repo_root}/external/mesa"
	mcompat="${repo_root}/sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/phoenix_mesa_compat.h"
	compat="${glue_dir}/pl_phoenix_compat.h"

	local sdllib="${PREFIX_A}/libSDL2.a"
	local sdlinc="${PREFIX_H}"
	local gllib="${gpu_libs}/libGL-phoenix.a"
	local v3dlib="${gpu_libs}/libv3d-phoenix.a"
	local glinc="${mesa}/include"

	# --- Prerequisite artifacts of the not-yet-portified GL stack (fail loud) ---
	local p missing=0
	for p in "${sdllib}" "${gllib}" "${v3dlib}" "${mcompat}" \
		"${sdl2_glue}/sdl_phoenix_glctx.c" "${sdl2_glue}/sdl_phoenix_glstubs.c"; do
		[ -f "${p}" ] || { echo "yquake2: MISSING prerequisite file: ${p}" >&2; missing=1; }
	done
	for p in "${mesa}/src" "${mesa}/include" "/tmp/mesa-v3d-build/src"; do
		[ -d "${p}" ] || { echo "yquake2: MISSING prerequisite dir: ${p}" >&2; missing=1; }
	done
	if [ "${missing}" != 0 ]; then
		b_die "GL/V3D stack not present. Rebuild it first: sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/build-gl-phoenix.py (and build-v3d-phoenix.py); it stages libGL/libv3d in tools/.gpu-libs and /tmp/mesa-v3d-build."
	fi

	# --- Compile flags (framework CFLAGS first so -mcpu/sysroot apply; port flags
	#     after so they win). -fcommon merges the tentative-definition cvar globals
	#     each .so declared independently; -include pulls the Phoenix compat shim. ---
	local base_cflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -fwrapv -fcommon -Wno-error -DNDEBUG -DYQ2OSTYPE=\"Phoenix\" -DYQ2ARCH=\"aarch64\" -DNOUNCRYPT -DIOAPI_NO_64 -include ${compat} -I${src} -I${sdlinc} -I${glinc}"
	# ref_gl1's initialized `modes` (texture-filter table) collides with the client's
	# initialized `modes` (video-mode menu); rename the renderer's across all gl1 TUs.
	local gl1_cflags="${base_cflags} -Dmodes=yq2_gl1_modes"
	# ref_gl3 (GLES3) variant: same initialized `modes` collision (gl3_image.c) needs
	# the same rename. YQ2_GL3_GLES3 takes the GLES3 code path (glad-gles3 loader,
	# <GLES3> types, ES context request in gl3_sdl.c); YQ2_GL3_GLES gates the parts
	# common to all GLES targets (the gladLoadGLES2Loader call, KHR-debug config) —
	# upstream defines BOTH for its ref_gles3 target (Makefile / CMakeLists.txt), and
	# gl3_sdl.c's ES loader path is gated on the shorter one. The extra -I lets the
	# glad-gles3 loader's `#include <glad/glad.h>` resolve.
	local gl3_cflags="${base_cflags} -DYQ2_GL3_GLES3 -DYQ2_GL3_GLES -Dmodes=yq2_gl3_modes -I${src}/client/refresh/gl3/glad-gles3/include"
	# SDL2 GL-context glue is compiled with Mesa's include/define set (winsys bridge).
	local mesa_cflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error -Wno-undef -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_STRUCT_TIMESPEC -include ${mcompat} -I${mesa}/src -I${mesa}/include -I${mesa}/src/mesa -I${mesa}/src/mapi -I${mesa}/src/compiler -I${mesa}/src/gallium/include -I${mesa}/src/gallium/auxiliary -I${mesa}/src/util -I/tmp/mesa-v3d-build/src -I${sdlinc}"

	# ---- TU lists, transcribed from CMakeLists.txt (paths relative to src/) ----
	# Client-Source (already includes the integrated server sv_*.c).
	local client=(
		client/cl_cin client/cl_console client/cl_download client/cl_effects
		client/cl_entities client/cl_input client/cl_image client/cl_inventory
		client/cl_keyboard client/cl_lights client/cl_main client/cl_network
		client/cl_parse client/cl_particles client/cl_prediction client/cl_screen
		client/cl_tempentities client/cl_view
		client/curl/download client/curl/qcurl
		client/input/gyro
		client/menu/menu client/menu/qmenu client/menu/videomenu
		client/sound/ogg client/sound/openal client/sound/qal client/sound/sdl
		client/sound/sound client/sound/wave
		client/vid/vid
		common/argproc common/clientserver common/collision common/crc
		common/cmdparser common/cvar common/filesystem common/glob common/md4
		common/movemsg common/frame common/netchan common/pmove common/szone
		common/zone
		common/shared/flash common/shared/rand common/shared/shared
		common/unzip/ioapi common/unzip/unzip common/unzip/miniz/miniz
		common/unzip/miniz/miniz_tdef common/unzip/miniz/miniz_tinfl
		server/sv_cmd server/sv_conless server/sv_entities server/sv_game
		server/sv_init server/sv_main server/sv_save server/sv_send
		server/sv_user server/sv_world
	)
	local client_sdl=(client/input/sdl2 client/vid/glimp_sdl2)
	local generic=(backends/generic/misc)
	local unix_keep=(backends/unix/network backends/unix/signalhandler)
	local game=(
		game/g_ai game/g_chase game/g_cmds game/g_combat game/g_func
		game/g_items game/g_main game/g_misc game/g_monster game/g_phys
		game/g_spawn game/g_svcmds game/g_target game/g_trigger game/g_turret
		game/g_utils game/g_weapon
		game/monster/berserker/berserker game/monster/boss2/boss2
		game/monster/boss3/boss3 game/monster/boss3/boss31 game/monster/boss3/boss32
		game/monster/brain/brain game/monster/chick/chick game/monster/flipper/flipper
		game/monster/float/float game/monster/flyer/flyer game/monster/gladiator/gladiator
		game/monster/gunner/gunner game/monster/hover/hover game/monster/infantry/infantry
		game/monster/insane/insane game/monster/medic/medic game/monster/misc/move
		game/monster/mutant/mutant game/monster/parasite/parasite game/monster/soldier/soldier
		game/monster/supertank/supertank game/monster/tank/tank
		game/player/client game/player/hud game/player/trail game/player/view
		game/player/weapon
		game/savegame/savegame
	)
	# GL1-Source — ref_gl1 ONLY, minus shared.c + md4.c (compiled once in client).
	local gl1=(
		client/refresh/gl1/qgl client/refresh/gl1/gl1_draw client/refresh/gl1/gl1_image
		client/refresh/gl1/gl1_light client/refresh/gl1/gl1_lightmap
		client/refresh/gl1/gl1_main client/refresh/gl1/gl1_mesh client/refresh/gl1/gl1_misc
		client/refresh/gl1/gl1_model client/refresh/gl1/gl1_scrap client/refresh/gl1/gl1_surf
		client/refresh/gl1/gl1_warp client/refresh/gl1/gl1_sdl client/refresh/gl1/gl1_buffer
		client/refresh/files/common client/refresh/files/models client/refresh/files/pcx
		client/refresh/files/stb client/refresh/files/surf client/refresh/files/wal
		client/refresh/files/pvs
	)
	# GL3-Source — ref_gl3 (built as GLES3), mirroring the gl1 list: the 12 gl3_*.c
	# TUs + the glad-gles3 loader + the SAME shared client/refresh/files/*.c (compiled
	# once, whichever renderer is selected). Selected instead of gl1 when
	# YQ2_RENDERER=gl3; never built alongside gl1 (only one GetRefAPI per binary).
	local gl3=(
		client/refresh/gl3/gl3_draw client/refresh/gl3/gl3_image client/refresh/gl3/gl3_light
		client/refresh/gl3/gl3_lightmap client/refresh/gl3/gl3_main client/refresh/gl3/gl3_mesh
		client/refresh/gl3/gl3_misc client/refresh/gl3/gl3_model client/refresh/gl3/gl3_sdl
		client/refresh/gl3/gl3_shaders client/refresh/gl3/gl3_surf client/refresh/gl3/gl3_warp
		client/refresh/gl3/glad-gles3/src/glad
		client/refresh/files/common client/refresh/files/models client/refresh/files/pcx
		client/refresh/files/stb client/refresh/files/surf client/refresh/files/wal
		client/refresh/files/pvs
	)
	# Phoenix backend (glue/) replacing backends/unix/{system,main,shared/hunk}.c.
	local phoenix=(pl_phoenix_sys pl_phoenix_main pl_phoenix_hunk)

	# Renderer selection: default gl3 (GLES3). That is the configuration the owner
	# has actually seen render a textured 3D frame on the Pi 4's V3D 4.2, and it is
	# the build that has been staged on the netboot root all along; the patch's
	# gl3_image.c hunk additionally defaults glGenerateMipmap OFF (set
	# YQ2_GL3_MIPMAP=1 to re-enable), which is what makes it load in reasonable
	# time. gl1 stays available via YQ2_RENDERER=gl1 but has never been confirmed to
	# reach the 3D view here, so it must not be what an image build ships by default.
	local renderer_kind="${YQ2_RENDERER:-gl3}"
	local renderer=()
	case "${renderer_kind}" in
		gl1) renderer=("${gl1[@]}") ;;
		gl3) renderer=("${gl3[@]}") ;;
		*) b_die "yquake2: unknown YQ2_RENDERER='${renderer_kind}' (want gl1 or gl3)" ;;
	esac

	local objdir="${PREFIX_PORT_WORKDIR}/_phoenix_obj"
	mkdir -p "${objdir}"

	# One gcc per TU, compiled in parallel; fail fast with the TU path. Bypasses
	# MAKEFLAGS (no make here), so the pool concurrency is explicit via xargs -P.
	_yq2_cc() {
		local kind="$1" unit="$2" base="$3" flags obj
		case "${kind}" in
			base) flags="${YQ2_BASE_CFLAGS}" ;;
			gl1) flags="${YQ2_GL1_CFLAGS}" ;;
			gl3) flags="${YQ2_GL3_CFLAGS}" ;;
			mesa) flags="${YQ2_MESA_CFLAGS}" ;;
		esac
		obj="${YQ2_OBJDIR}/${unit//\//_}.o"
		# shellcheck disable=2086
		if ! ${CC} ${flags} -o "${obj}" "${base}/${unit}.c"; then
			echo "!!! yquake2: compile FAILED: ${base}/${unit}.c" >&2
			exit 1
		fi
	}
	export -f _yq2_cc
	export CC YQ2_OBJDIR="${objdir}" \
		YQ2_BASE_CFLAGS="${base_cflags}" YQ2_GL1_CFLAGS="${gl1_cflags}" \
		YQ2_GL3_CFLAGS="${gl3_cflags}" YQ2_MESA_CFLAGS="${mesa_cflags}"

	{
		local u
		for u in "${client[@]}" "${client_sdl[@]}" "${generic[@]}" "${unix_keep[@]}" "${game[@]}"; do
			printf 'base\t%s\t%s\n' "${u}" "${src}"
		done
		for u in "${renderer[@]}"; do printf '%s\t%s\t%s\n' "${renderer_kind}" "${u}" "${src}"; done
		for u in "${phoenix[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${glue_dir}"; done
		printf 'mesa\t%s\t%s\n' "sdl_phoenix_glctx" "${sdl2_glue}"
		printf 'base\t%s\t%s\n' "sdl_phoenix_glstubs" "${sdl2_glue}"
	} | xargs -P"$(nproc)" -L1 bash -c '_yq2_cc "$@"' _

	# Deterministic object list (compile order above is parallel/non-deterministic).
	local objs=() u
	for u in "${client[@]}" "${client_sdl[@]}" "${generic[@]}" "${unix_keep[@]}" \
		"${game[@]}" "${renderer[@]}" "${phoenix[@]}" sdl_phoenix_glctx sdl_phoenix_glstubs; do
		local o="${objdir}/${u//\//_}.o"
		[ -f "${o}" ] || b_die "yquake2: object missing after compile: ${o}"
		objs+=("${o}")
	done

	# Circular refs (SDL <-> renderer <-> Mesa; libGL <-> libv3d) -> --start-group.
	# 4 MB stack: Quake2 uses a few MB; a smaller committed PT_GNU_STACK trims the
	# exec-time eager-commit footprint (matches the tools build's final choice).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086
	"${CC}" ${CFLAGS} ${LDFLAGS} "${objs[@]}" \
		-Wl,--start-group "${sdllib}" "${gllib}" "${v3dlib}" -Wl,--end-group \
		-lstdc++ -lm -Wl,-z,stack-size=4194304 \
		-o "${PREFIX_PROG}/yquake2"

	"${STRIP}" -o "${PREFIX_PROG_STRIPPED}/yquake2" "${PREFIX_PROG}/yquake2"
	b_install "${PREFIX_PROG_TO_INSTALL}/yquake2" /usr/bin
}
