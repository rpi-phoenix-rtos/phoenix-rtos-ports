#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="quakespasm"
	# QuakeSpasm's in-tree QUAKESPASM_VER_STRING at the pinned commit is 0.97.0.
	version="0.97.0"
	desc="QuakeSpasm (Quake I engine) — single static ELF on the ported SDL2 + Mesa/V3D GL stack"

	# QuakeSpasm's authoritative repo is sezero/quakespasm; it tags releases but this
	# port is pinned to a specific master commit (the one the tools/quakespasm-port
	# bring-up validated: QUAKESPASM_VERSION 0.97, 2026-03-11). GitHub serves the tree
	# as a content-addressed commit-archive at /archive/<sha>.tar.gz, so the fetched
	# bytes are reproducible (same pattern as the yquake2 / llama2 ports). Bump
	# `commit` + size/sha256 together if the pin moves.
	commit="6baceeac51cbbb368912267da071abe69b1f30de"
	source="https://github.com/sezero/quakespasm/archive"
	archive_filename="${commit}.tar.gz"
	src_path="quakespasm-${commit}/"

	size="11520209"
	sha256="c540596d464bb1bea1dfdb79dce0799233a0664c357bc16437f8f9e58ec9f8aa"

	license="GPL-2.0-or-later"
	license_file="LICENSE.txt"

	conflicts=""
	depends="sdl2"

	supports="phoenix>=3.3"
}

# Framework migration of tools/quakespasm-port/build-quakespasm-sdl-phoenix.py (the
# "real-SDL" QuakeSpasm variant). QuakeSpasm is already a single binary upstream (no
# .so game/renderer split), so the port compiles its GL renderer + engine core + the
# STOCK SDL2 video/input/audio backends (gl_vidsdl / in_sdl / snd_sdl) and links them
# against the ported SDL2 (depends="sdl2") + the Mesa/V3D GL stack into ONE static ELF
# installed as /usr/bin/quakespasm. A small Phoenix backend (glue/pl_phoenix_*) replaces
# main_sdl.c / sys_sdl_unix.c / pl_linux.c / net_bsd.c. The id1 game data (pak0.pak …)
# is a RUNTIME concern staged separately under /usr/share/quake; the port builds only
# the engine binary.
#
# The clone-local diagnostic scaffolding (the gl_screen.c deterministic frame-capture +
# TCP sink harness, the gl_rmain.c "#67 model gallery" test harness, and the
# r_alias_lerpmode bisection cvar) is intentionally NOT part of this port: those are
# visual-regression debugging aids, not engine behaviour. A clean upstream tarball is
# built with a single documented patch that carries ONLY the genuine Phoenix/V3D engine
# fixes (see patches/0001-quakespasm-phoenix-v3d-single-elf.patch):
#   * r_world.c   — draw brush surfaces from a bound element VBO (V3D cannot draw from a
#                   client-memory index array).
#   * r_alias.c / gl_mesh.c — single-pose alias de-alias (#67): at blend==0 don't bind
#                   the Pose2 byte attributes, and drop the duplicate single-pose VBO block.
#   * gl_warp.c   — default r_oldwater 1 (modern warpimage path needs glCopyTexSubImage2D,
#                   unimplemented on the ported v3d GL frontend).
#   * r_part.c    — default r_quadparticles 0 (GL_TRIANGLES; the GL_QUADS batch path wedges
#                   the V3D binner).
#   * net_udp.c   — replace the fatal ioctl(FIONREAD) accept-socket probe with a
#                   non-blocking MSG_PEEK (Phoenix lwIP has no FIONREAD).
#   * net_main.c  — connect directly to a specified host, skipping the LAN-discovery slist
#                   (which never clears slistInProgress on Phoenix).
#
# GPU coupling note (identical wart to the yquake2 port): libGL-phoenix.a / libv3d-phoenix.a
# (tools/.gpu-libs) and the Mesa headers (external/mesa) are prebuilt artifacts of the
# Mesa/V3D GL stack, which is not yet a framework port. Until it is, this recipe links
# those archives + the shared SDL2-GL glue (sources/phoenix-rtos-ports/sdl2/glue) by
# absolute path, anchored off PREFIX_PORT. Portifying that GL stack would remove this
# wart from all the SDL2 game ports at once.

# ---- Repository anchors (this port reaches artifacts outside the ports tree) ----
_qs_repo_root() { (cd "${PREFIX_PORT}/../../.." && pwd); }

p_prepare() {
	# Single documented patch: the genuine Phoenix/V3D engine fixes only (the
	# clone-local capture / model-gallery / lerpmode diagnostics are excluded).
	# The patch below is GENERATED from our fork (external/<game>, branch
	# phoenix-rpi4-port) by scripts/game-port-patch.sh in the coordination
	# repo. Do NOT hand-edit it: commit to the fork and re-run --regen, or
	# the two representations drift apart (they did -- see
	# docs/misc/2026-09-02-game-source-of-truth-audit.md).
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	local repo_root src glue_dir sdl2_glue gpu_libs mesa mcompat
	repo_root="$(_qs_repo_root)"
	src="${PREFIX_PORT_WORKDIR}/Quake"
	glue_dir="${PREFIX_PORT}/glue"
	sdl2_glue="$(cd "${PREFIX_PORT}/../sdl2/glue" && pwd)"
	gpu_libs="${repo_root}/tools/.gpu-libs"
	mesa="${repo_root}/external/mesa"
	mcompat="${repo_root}/sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/phoenix_mesa_compat.h"

	local sdllib="${PREFIX_A}/libSDL2.a"
	local sdlinc="${PREFIX_H}"
	local gllib="${gpu_libs}/libGL-phoenix.a"
	local v3dlib="${gpu_libs}/libv3d-phoenix.a"
	local glinc="${mesa}/include"

	# --- Prerequisite artifacts of the not-yet-portified GL stack (fail loud) ---
	local p missing=0
	for p in "${sdllib}" "${gllib}" "${v3dlib}" "${mcompat}" \
		"${sdl2_glue}/sdl_phoenix_glctx.c"; do
		[ -f "${p}" ] || { echo "quakespasm: MISSING prerequisite file: ${p}" >&2; missing=1; }
	done
	for p in "${mesa}/src" "${mesa}/include" "/tmp/mesa-v3d-build/src"; do
		[ -d "${p}" ] || { echo "quakespasm: MISSING prerequisite dir: ${p}" >&2; missing=1; }
	done
	if [ "${missing}" != 0 ]; then
		b_die "GL/V3D stack not present. Rebuild it first: sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/build-gl-phoenix.py (and build-v3d-phoenix.py); it stages libGL/libv3d in tools/.gpu-libs and /tmp/mesa-v3d-build."
	fi

	# --- Compile flags ---
	# Quake TUs + the stock SDL2 backends + the Phoenix shims. -DNO_SDL_CONFIG +
	# -DUSE_SDL2 select quakedef.h's branch that includes Mesa's <GL/gl.h> BEFORE the
	# real <SDL2/SDL.h> + <SDL2/SDL_opengl.h> (they share the __gl_h_ guard, so the Mesa
	# GL headers — which match libGL-phoenix — win and SDL's GL block is neutralised).
	# -I${sdlinc}/SDL2 resolves the bare "SDL.h" form a few TUs use.
	local qflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error -DUSE_SDL2 -DNO_SDL_CONFIG -I${src} -I${glinc} -I${sdlinc} -I${sdlinc}/SDL2"
	# SDL2 GL-context glue is compiled with Mesa's include/define set (winsys bridge).
	local mflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error -Wno-undef -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_STRUCT_TIMESPEC -include ${mcompat} -I${mesa}/src -I${mesa}/include -I${mesa}/src/mesa -I${mesa}/src/mapi -I${mesa}/src/compiler -I${mesa}/src/gallium/include -I${mesa}/src/gallium/auxiliary -I${mesa}/src/util -I/tmp/mesa-v3d-build/src -I${sdlinc}"

	# ---- TU lists, transcribed from the Makefile OBJS (paths relative to Quake/) ----
	# GL renderer + engine core. The SDL platform TUs Phoenix owns are omitted:
	# cd_sdl -> cd_null (portable), sys_sdl_unix -> pl_phoenix_sys, main_sdl ->
	# pl_phoenix_main (owns main()), pl_linux -> pl_phoenix_stubs, net_bsd ->
	# pl_phoenix_stubs (net driver tables). gl_vidsdl / in_sdl / snd_sdl ARE compiled.
	local globjs=(
		gl_refrag gl_rlight gl_rmain gl_fog gl_rmisc r_part r_world gl_screen gl_sky
		gl_warp gl_draw image gl_texmgr gl_mesh r_sprite r_alias r_brush gl_model
	)
	local core=(
		strlcat strlcpy net_dgrm net_loop net_main net_udp chase cl_demo cl_input
		cl_main cl_parse cl_tent console keys menu sbar view wad cmd common miniz crc
		cvar cfgfile host host_cmd mathlib pr_cmds pr_edict pr_exec sv_main sv_move
		sv_phys sv_user world zone snd_dma snd_mix snd_mem bgmusic cd_null snd_codec
	)
	local sdlbk=(gl_vidsdl in_sdl snd_sdl)
	# Phoenix backend (glue/) replacing main_sdl / sys_sdl_unix / pl_linux / net_bsd.
	local shims=(pl_phoenix_sys pl_phoenix_main pl_phoenix_stubs)

	local objdir="${PREFIX_PORT_WORKDIR}/_phoenix_obj"
	mkdir -p "${objdir}"

	# One gcc per TU, compiled in parallel; fail fast with the TU path.
	_qs_cc() {
		local kind="$1" unit="$2" base="$3" flags obj
		case "${kind}" in
			q) flags="${QS_QFLAGS}" ;;
			m) flags="${QS_MFLAGS}" ;;
		esac
		obj="${QS_OBJDIR}/${unit//\//_}.o"
		# shellcheck disable=2086
		if ! ${CC} ${flags} -o "${obj}" "${base}/${unit}.c"; then
			echo "!!! quakespasm: compile FAILED: ${base}/${unit}.c" >&2
			exit 1
		fi
	}
	export -f _qs_cc
	export CC QS_OBJDIR="${objdir}" QS_QFLAGS="${qflags}" QS_MFLAGS="${mflags}"

	{
		local u
		for u in "${globjs[@]}" "${core[@]}" "${sdlbk[@]}"; do
			printf 'q\t%s\t%s\n' "${u}" "${src}"
		done
		for u in "${shims[@]}"; do printf 'q\t%s\t%s\n' "${u}" "${glue_dir}"; done
		printf 'm\t%s\t%s\n' "sdl_phoenix_glctx" "${sdl2_glue}"
	} | xargs -P"$(nproc)" -L1 bash -c '_qs_cc "$@"' _

	# Deterministic object list (compile order above is parallel/non-deterministic).
	local objs=() u
	for u in "${globjs[@]}" "${core[@]}" "${sdlbk[@]}" "${shims[@]}" sdl_phoenix_glctx; do
		local o="${objdir}/${u//\//_}.o"
		[ -f "${o}" ] || b_die "quakespasm: object missing after compile: ${o}"
		objs+=("${o}")
	done

	# Circular refs (SDL <-> renderer <-> Mesa; libGL <-> libv3d) -> --start-group.
	# 32 MB stack: Quake's deep render/host call chains overflow the default (matches
	# the tools build's final choice).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086
	"${CC}" ${CFLAGS} ${LDFLAGS} "${objs[@]}" \
		-Wl,--start-group "${sdllib}" "${gllib}" "${v3dlib}" -Wl,--end-group \
		-lstdc++ -lm -Wl,-z,stack-size=33554432 \
		-o "${PREFIX_PROG}/quakespasm"

	"${STRIP}" -o "${PREFIX_PROG_STRIPPED}/quakespasm" "${PREFIX_PROG}/quakespasm"
	b_install "${PREFIX_PROG_TO_INSTALL}/quakespasm" /usr/bin
}
