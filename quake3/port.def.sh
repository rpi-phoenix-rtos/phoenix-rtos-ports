#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="quake3"
	# quake3e's in-tree Q3_VERSION at the pinned commit is "Q3 1.32e"; port_manager's
	# resolver wants a dotted/numeric version, so record "1.32" here and keep the
	# authoritative provenance in `commit`/archive_filename/src_path below.
	version="1.32"
	desc="quake3e (Quake III Arena engine) — single static ELF on the ported SDL2 + Mesa/V3D GL stack"

	# quake3e's authoritative repo is ec-/quake3e; it does not tag releases, so this
	# port is pinned to the specific master commit the tools/quake3-port bring-up
	# validated (Q3_VERSION "Q3 1.32e", 2026-08). GitHub serves the tree as a
	# content-addressed commit-archive at /archive/<sha>.tar.gz, so the fetched bytes
	# are reproducible (same pattern as the yquake2 / quakespasm / llama2 ports). The
	# archive's top-level directory is capital-Q "Quake3e-<sha>/". Bump `commit` +
	# size/sha256 together if the pin moves.
	commit="623982900a132e5c812dcb5231a430f28fafabeb"
	source="https://github.com/ec-/quake3e/archive"
	archive_filename="${commit}.tar.gz"
	src_path="Quake3e-${commit}/"

	size="18298807"
	sha256="2c8d8fddb1dff42a0e3b51dcf013109fe8aacfec4c695716ebf3a47e216be288"

	license="GPL-2.0-or-later"
	license_file="COPYING.txt"

	conflicts=""
	depends="sdl2"

	supports="phoenix>=3.3"
}

# Framework migration of tools/quake3-port/build-quake3e-phoenix.py (the quake3
# bring-up). quake3e is structurally simpler to fold into one ELF than yQuake2: the
# game / cgame / ui modules are interpreted QVM bytecode shipped in the pak (data,
# not C we compile or link), so there is NO GetGameAPI game-DLL fold and NO game-TU
# list. The renderer is compiled in directly (USE_RENDERER_DLOPEN=0 -> the engine
# calls the renderer's GetRefAPI at link time). So the client + integrated server +
# botlib + opengl1 renderer + code/sdl backend + bundled libjpeg + a Phoenix backend
# (glue/pl_phoenix_*) link into ONE static ELF installed as /usr/bin/quake3e, against
# the ported SDL2 (depends="sdl2") + the Mesa/V3D GL stack.
#
# VM config: the aarch64 QVM JIT (code/qcommon/vm_aarch64.c) IS compiled in, with
# vm_interpreted.c as the fallback. The single documented patch makes the JIT viable
# on Phoenix — the kernel honours PROT_EXEC at mmap time (vm/map.c) but mprotect
# cannot ADD PROT_EXEC to an existing mapping, so the code buffer is allocated RWX up
# front and a later mprotect(RX) failure is non-fatal. (This differs from the stale
# NO_VM_COMPILED/interpreter-only note in the old python docstring; the actual flags
# never defined NO_VM_COMPILED and the TU list has always compiled vm_aarch64.)
#
# The `quake3` launcher (tools/quake3-port/quake3-launcher.c: ram-stage-play then
# exec /usr/bin/quake3e) and the demoq3 game data (pak0.pk3 …) are RUNTIME concerns
# staged separately under /usr/share/quake3; the port builds only the engine binary.
#
# The diagnostic capture harness — the -DQ3CAP_PHOENIX per-frame readback hook in
# tr_init.c, which lives as a clone-local commit on the external/quake3e clone (on
# top of the pinned upstream commit) — is intentionally NOT part of this port: it is
# a visual-regression debugging aid, not engine behaviour. A clean upstream tarball
# is built with only the single documented patch (patches/), so the renderer uses the
# stock glReadPixels path. Same exclusion the yquake2 (YQ2CAP) and quakespasm (QSS)
# ports made.
#
# The single patch (patches/0001-quake3e-phoenix-single-elf.patch) carries ONLY the
# genuine Phoenix engine fixes:
#   * q_platform.h — add the __phoenix__ OS identity + ID_INLINE.
#   * qgl.h        — pull GL types from the ported Mesa headers, exclude the X11/GLX
#                    proc block (SDL owns the GL context; Phoenix has no GLX).
#   * vm_aarch64.c — allocate the JIT code buffer RWX; tolerate mprotect(RX) failure.
#   * huffman.c    — rename the file-local `send` to `Huff_send` so it no longer
#                    shadows POSIX send() (the compat shim force-includes the socket
#                    headers, which declare send()).
# The msg_t type clash (Phoenix SysV-IPC msg_t vs Q3 network msg_t) is defused with
# ZERO Q3-source edits by the force-included glue/pl_phoenix_compat.h shim.
#
# GPU coupling note (identical wart to the yquake2 / quakespasm ports): libGL-phoenix.a
# / libv3d-phoenix.a (tools/.gpu-libs) and the Mesa headers (external/mesa) are prebuilt
# artifacts of the Mesa/V3D GL stack, which is not yet a framework port. Until it is,
# this recipe links those archives + the shared SDL2-GL glue
# (sources/phoenix-rtos-ports/sdl2/glue) by absolute path, anchored off PREFIX_PORT.
# Portifying that GL stack would remove this wart from all the SDL2 game ports at once.

# ---- Repository anchors (this port reaches artifacts outside the ports tree) ----
_q3_repo_root() { (cd "${PREFIX_PORT}/../../.." && pwd); }

p_prepare() {
	# Single documented patch: the genuine Phoenix/V3D engine fixes only (the
	# clone-local -DQ3CAP_PHOENIX capture hook is excluded — see header note).
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	local repo_root src glue_dir sdl2_glue gpu_libs mesa mcompat compat
	repo_root="$(_q3_repo_root)"
	src="${PREFIX_PORT_WORKDIR}/code"
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
		[ -f "${p}" ] || { echo "quake3: MISSING prerequisite file: ${p}" >&2; missing=1; }
	done
	for p in "${mesa}/src" "${mesa}/include" "/tmp/mesa-v3d-build/src"; do
		[ -d "${p}" ] || { echo "quake3: MISSING prerequisite dir: ${p}" >&2; missing=1; }
	done
	if [ "${missing}" != 0 ]; then
		b_die "GL/V3D stack not present. Rebuild it first: sources/phoenix-rtos-devices/gpu/rpi4-v3d/mesa/build-gl-phoenix.py (and build-v3d-phoenix.py); it stages libGL/libv3d in tools/.gpu-libs and /tmp/mesa-v3d-build."
	fi

	# --- Compile flags (framework CFLAGS first so -mcpu/sysroot apply; port flags
	#     after so they win). -fcommon merges the tentative-definition cvar globals
	#     the .so-era build gave several TUs; -include pulls the Phoenix compat shim
	#     (msg_t defuse + __clear_cache builtin for the JIT). -DUSE_OPENGL_API +
	#     -DUSE_LOCAL_HEADERS=1 match the opengl1 / local-SDL-header config. ---
	local base_cflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -fwrapv -fcommon -Wno-error -DNDEBUG -DUSE_OPENGL_API -DUSE_LOCAL_HEADERS=1 -include ${compat} -I${src} -I${src}/qcommon -I${src}/renderercommon -I${src}/renderer -I${src}/client -I${src}/libjpeg -I${sdlinc}/SDL2 -I${sdlinc} -I${glinc}"
	# botlib TUs select their engine-build include set with -DBOTLIB (mirrors the
	# Makefile's do_cc_botlib rule); without it source_t/punctuation_t/fielddef_t are
	# undefined.
	local botlib_cflags="${base_cflags} -DBOTLIB"
	# SDL2 GL-context glue is compiled with Mesa's include/define set (winsys bridge).
	local mesa_cflags="${CFLAGS} -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error -Wno-undef -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_STRUCT_TIMESPEC -include ${mcompat} -I${mesa}/src -I${mesa}/include -I${mesa}/src/mesa -I${mesa}/src/mapi -I${mesa}/src/compiler -I${mesa}/src/gallium/include -I${mesa}/src/gallium/auxiliary -I${mesa}/src/util -I/tmp/mesa-v3d-build/src -I${sdlinc}"

	# ---- TU lists, transcribed from the Makefile (Q3OBJ/Q3REND1OBJ/JPGOBJ) ----
	# qcommon core (incl. the aarch64 JIT + interpreter fallback, unzip, net_ip).
	local qcommon=(
		cm_load cm_patch cm_polylib cm_test cm_trace
		cmd common cvar files history keys md4 md5 msg
		net_chan net_ip huffman huffman_static q_math q_shared
		unzip puff
		vm vm_interpreted vm_aarch64
	)
	local client=(
		cl_cgame cl_cin cl_console cl_input cl_keys cl_main
		cl_net_chan cl_parse cl_scrn cl_ui cl_avi cl_jpeg
		snd_adpcm snd_dma snd_mem snd_mix snd_wavelet
		snd_main snd_codec snd_codec_wav
	)
	local server=(
		sv_bot sv_ccmds sv_client sv_filter sv_game sv_init
		sv_main sv_net_chan sv_snapshot sv_world
	)
	local botlib=(
		be_aas_bspq3 be_aas_cluster be_aas_debug be_aas_entity
		be_aas_file be_aas_main be_aas_move be_aas_optimize
		be_aas_reach be_aas_route be_aas_routealt be_aas_sample
		be_ai_char be_ai_chat be_ai_gen be_ai_goal be_ai_move
		be_ai_weap be_ai_weight be_ea be_interface
		l_crc l_libvar l_log l_memory l_precomp l_script l_struct
	)
	# Bundled libjpeg (USE_SYSTEM_JPEG off) -- JPGOBJ.
	local jpeg=(
		jaricom jcapimin jcapistd jcarith jccoefct jccolor
		jcdctmgr jchuff jcinit jcmainct jcmarker jcmaster
		jcomapi jcparam jcprepct jcsample jctrans jdapimin
		jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor
		jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster
		jdmerge jdpostct jdsample jdtrans jerror jfdctflt
		jfdctfst jfdctint jidctflt jidctfst jidctint jmemmgr
		jmemnobs jquant1 jquant2 jutils
	)
	# renderercommon TUs used by Q3REND1OBJ.
	local rendcommon=(
		tr_font tr_image_png tr_image_jpg tr_image_bmp tr_image_tga
		tr_image_pcx tr_noise
	)
	# opengl1 renderer (Q3REND1OBJ). Compiled on plain base CFLAGS — NOT the
	# diagnostic -DQ3CAP_PHOENIX capture path (excluded; see header note).
	local rend1=(
		tr_animation tr_arb tr_backend tr_bsp tr_cmds tr_curve
		tr_flares tr_image tr_init tr_light tr_main tr_marks
		tr_mesh tr_model tr_model_iqm tr_scene tr_shade
		tr_shade_calc tr_shader tr_shadows tr_sky tr_surface
		tr_vbo tr_world
	)
	# SDL2 client backend (USE_SDL=1 branch of Q3OBJ).
	local sdlbk=(sdl_glimp sdl_gamma sdl_input sdl_snd)
	# Unix backend TU kept verbatim (no dlopen; pure POSIX signal handling).
	local unix_keep=(linux_signals)
	# Phoenix backend (glue/): forks of unix_main.c + unix_shared.c with the dlopen
	# seam stubbed. The libc/Mesa gap-filler (pthread_getcpuclockid) is the shared
	# SDL2-port Zlib glstubs, not a per-port copy.
	local phoenix=(pl_phoenix_main pl_phoenix_sys)

	local objdir="${PREFIX_PORT_WORKDIR}/_phoenix_obj"
	mkdir -p "${objdir}"

	# One gcc per TU, compiled in parallel; fail fast with the TU path. Bypasses
	# MAKEFLAGS (no make here), so the pool concurrency is explicit via xargs -P.
	_q3_cc() {
		local kind="$1" unit="$2" base="$3" flags obj
		case "${kind}" in
			base) flags="${Q3_BASE_CFLAGS}" ;;
			botlib) flags="${Q3_BOTLIB_CFLAGS}" ;;
			mesa) flags="${Q3_MESA_CFLAGS}" ;;
		esac
		obj="${Q3_OBJDIR}/${unit//\//_}.o"
		# shellcheck disable=2086
		if ! ${CC} ${flags} -o "${obj}" "${base}/${unit}.c"; then
			echo "!!! quake3: compile FAILED: ${base}/${unit}.c" >&2
			exit 1
		fi
	}
	export -f _q3_cc
	export CC Q3_OBJDIR="${objdir}" \
		Q3_BASE_CFLAGS="${base_cflags}" Q3_BOTLIB_CFLAGS="${botlib_cflags}" Q3_MESA_CFLAGS="${mesa_cflags}"

	{
		local u
		for u in "${qcommon[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/qcommon"; done
		for u in "${client[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/client"; done
		for u in "${server[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/server"; done
		for u in "${botlib[@]}"; do printf 'botlib\t%s\t%s\n' "${u}" "${src}/botlib"; done
		for u in "${jpeg[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/libjpeg"; done
		for u in "${rendcommon[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/renderercommon"; done
		for u in "${rend1[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/renderer"; done
		for u in "${sdlbk[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/sdl"; done
		for u in "${unix_keep[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${src}/unix"; done
		for u in "${phoenix[@]}"; do printf 'base\t%s\t%s\n' "${u}" "${glue_dir}"; done
		printf 'mesa\t%s\t%s\n' "sdl_phoenix_glctx" "${sdl2_glue}"
		printf 'base\t%s\t%s\n' "sdl_phoenix_glstubs" "${sdl2_glue}"
	} | xargs -P"$(nproc)" -L1 bash -c '_q3_cc "$@"' _

	# Deterministic object list (compile order above is parallel/non-deterministic).
	local objs=() u
	for u in "${qcommon[@]}" "${client[@]}" "${server[@]}" "${botlib[@]}" "${jpeg[@]}" \
		"${rendcommon[@]}" "${rend1[@]}" "${sdlbk[@]}" "${unix_keep[@]}" "${phoenix[@]}" \
		sdl_phoenix_glctx sdl_phoenix_glstubs; do
		local o="${objdir}/${u//\//_}.o"
		[ -f "${o}" ] || b_die "quake3: object missing after compile: ${o}"
		objs+=("${o}")
	done

	# Circular refs (SDL <-> renderer <-> Mesa; libGL <-> libv3d) -> --start-group.
	# 4 MB stack matches the tools build's final choice (Phoenix commits PT_GNU_STACK
	# eagerly at exec; keep the exec footprint modest).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	# shellcheck disable=2086
	"${CC}" ${CFLAGS} ${LDFLAGS} "${objs[@]}" \
		-Wl,--start-group "${sdllib}" "${gllib}" "${v3dlib}" -Wl,--end-group \
		-lstdc++ -lm -Wl,-z,stack-size=4194304 \
		-o "${PREFIX_PROG}/quake3e"

	"${STRIP}" -o "${PREFIX_PROG_STRIPPED}/quake3e" "${PREFIX_PROG}/quake3e"
	b_install "${PREFIX_PROG_TO_INSTALL}/quake3e" /usr/bin
}
