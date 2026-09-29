#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="quakespasm_drm"
	version="0.97.0"
	desc="quakespasm-drm: QuakeSpasm on the new GPU lane (SDL KMSDRM + Mesa GBM/EGL desktop GL + libdrm-phoenix)"

	# the quakespasm port's pinned upstream commit (same archive, same sha256)
	commit="f5fe17864918239d443fe4c0d6bfb980e44d19e6"
	source="https://github.com/sezero/quakespasm/archive"
	archive_filename="${commit}.tar.gz"
	src_path="quakespasm-${commit}/"

	size="11519219"
	sha256="1ca8ad2f190f9ab0aecd06d854c92637b62a3bd3f298dd877799dee0a300abd4"

	license="GPL-2.0-or-later"
	license_file="LICENSE.txt"

	# NEW GPU LANE clone of the quakespasm port: private prefix; /usr/bin/quakespasm and the
	# quakespasm port are untouched.
	conflicts="quakespasm_drm!=${version}"
	depends="sdl2_kmsdrm mesa_drm[opengl] libdrm_phoenix zlib wayland? ( sdl2_kmsdrm[wayland] )"

	# rootfs: install /usr/bin/quakespasm-drm and its launcher /bin/qs-drm into the image,
	# the launcher also as /usr/bin/quakespasm
	# wayland: ALSO build the WINDOWED clone for the Wayland desktop (M8): /usr/bin/quakespasm-wl
	# and (with rootfs) the XFCE menu entry "Quake (window)" = /bin/game-window.sh quakespasm
	# (sdl2_kmsdrm USE rootfs); no launcher: game-window.sh passes -window -width -height
	iuse="rootfs wayland"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build.sh step 4. The engine is
# the quakespasm port's: its patch (patches/0001, a copy of quakespasm/patches/0001) and its
# Phoenix platform glue (glue/pl_phoenix_{sys,main,stubs}.c, copies of quakespasm/glue/), with
# the same TU lists as its p_build -- minus the old lane's SDL-GL context glue
# (sdl2/glue/sdl_phoenix_glctx.c): SDL's KMSDRM/EGL owns the context. GL headers come from the
# new lane's Mesa source (the Mesa the binary links), never external/mesa.
# The clone links the shared gamedrm hooks (banner, SDL VIDEO/INPUT at DEBUG, and the
# `quakespasm-drm flipstat ... (total N)` + `swapstat` counter on GL_EndRendering's
# SDL_GL_SwapWindow, behind -Wl,--wrap=SDL_GL_SwapWindow) that the migration gate reads.
# The copies of the quakespasm port's patch and glue are kept identical to it by the
# coordination repo's scripts/check-gpu-lane-ports-sync.sh.
# The launcher (launcher/qs-drm-launcher.c, this port's own file, not a copy) passes the native
# 1920x1080 fullscreen mode on the command line, so a lower mode that a session persisted in
# id1/config.cfg never becomes the default (GPU migration P1 + M9).
# USE wayland: tools/gpu-lane/sdl2-wl/build.sh steps 4-5 (quakespasm-wl) -- the same TUs
# compiled against sdl2_kmsdrm's Wayland SDL headers and linked with its Wayland link group
# (wayland/link-inputs.txt, desktop GL half) and the gamewl hooks (share/gamewl/).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local qs_name=quakespasm-drm
	local M="${PORT_DEP_mesa_drm}/gl" SP="${PORT_DEP_sdl2_kmsdrm}"
	local MB="${M}/mesa-build" Q="${PREFIX_PORT_WORKDIR}/Quake" G="${PREFIX_PORT}/glue"
	local GD="${PORT_DEP_sdl2_kmsdrm}/share/gamedrm"
	local SDL_A="${SP}/lib/libSDL2.a" GL_BRIDGE="${MB}/src/mesa/glapi/glapi/libglapi_bridge.a"
	grep -qx 'opengl=true' "${M}/opengl.txt" 2>/dev/null || b_die "${M} is not a desktop-GL Mesa build"
	[ -f "${GL_BRIDGE}" ] || b_die "missing ${GL_BRIDGE}"

	# TU lists = the quakespasm port's p_build (the SDL platform TUs the Phoenix glue replaces
	# are omitted exactly as there)
	local globjs=(gl_refrag gl_rlight gl_rmain gl_fog gl_rmisc r_part r_world gl_screen gl_sky
		gl_warp gl_draw image gl_texmgr gl_mesh r_sprite r_alias r_brush gl_model)
	local core=(strlcat strlcpy net_dgrm net_loop net_main net_udp chase cl_demo cl_input
		cl_main cl_parse cl_tent console keys menu sbar view wad cmd common miniz crc
		cvar cfgfile host host_cmd mathlib pr_cmds pr_edict pr_exec sv_main sv_move
		sv_phys sv_user world zone snd_dma snd_mix snd_mem bgmusic cd_null snd_codec)
	local sdlbk=(gl_vidsdl in_sdl snd_sdl)
	local shims=(pl_phoenix_sys pl_phoenix_main pl_phoenix_stubs)
	# GL headers: the include/ of the Mesa source the binary links (tools: <mesa-gl>/mesa-src/include)
	local QFLAGS=("${NL_TFLAGS[@]}" -fomit-frame-pointer -std=gnu17 -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error
		-DUSE_SDL2 -DNO_SDL_CONFIG -I"${Q}" -I"${PORT_DEP_mesa_drm}/src-include" -I"${SP}/include" -I"${SP}/include/SDL2")
	[ -f "${PORT_DEP_mesa_drm}/src-include/GL/gl.h" ] || b_die "no ${PORT_DEP_mesa_drm}/src-include/GL/gl.h"
	local QO="${PREFIX_PORT_BUILD}/qs-obj" u o objs=()
	rm -rf "${QO}"
	mkdir -p "${QO}"
	{
		for u in "${globjs[@]}" "${core[@]}" "${sdlbk[@]}"; do printf '%s\n' "${Q}/${u}.c"; done
		for u in "${shims[@]}"; do printf '%s\n' "${G}/${u}.c"; done
	} > "${PREFIX_PORT_BUILD}/qs-tus.txt"
	export QO
	qs_cc() { local f="$1" o; o="${QO}/$(basename "${f%.c}").o"; "${NL_CC}" "${@:2}" -o "${o}" "${f}" || { echo "compile FAILED: ${f}"; exit 1; }; }
	export -f qs_cc
	xargs -P"$(nproc)" -I{} bash -c 'qs_cc "$@"' _ {} "${QFLAGS[@]}" < "${PREFIX_PORT_BUILD}/qs-tus.txt" \
		|| b_die "quakespasm compile failed"
	while IFS= read -r u; do
		o="${QO}/$(basename "${u%.c}").o"
		[ -f "${o}" ] || b_die "object missing: ${o}"
		objs+=("${o}")
	done < "${PREFIX_PORT_BUILD}/qs-tus.txt"
	# the clone's process hooks (built with their own flags, -Werror)
	"${NL_CC}" -O2 -g -std=gnu17 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${SP}/include" \
		-DGAMEDRM_NAME="\"${qs_name}\"" -DGAMEDRM_API='"desktop GL"' -c "${GD}/gamedrm_hooks.c" -o "${QO}/gamedrm_hooks.o"
	objs+=("${QO}/gamedrm_hooks.o")

	# Link = mesa_drm's kmscube shape (C++ driver; -static; --gc-sections; 4 KiB pages;
	# --wrap=mmap/ioctl for libdrm-phoenix; libgallium whole-archive) plus libglapi_bridge.a
	# (desktop gl* instead of libGLESv2.a), libSDL2.a in the group, the port's 32 MiB main stack
	# and --wrap=SDL_GL_SwapWindow for the frame counter. The archive list is mesa_drm's
	# gl/link-gl.txt (tools: sdl2-drm build.sh A=(...)).
	local gallium="" MA=() l
	while IFS= read -r l; do
		case "${l}" in "--whole-archive "*) gallium="${l#--whole-archive }" ;; *) MA+=("${l}") ;; esac
	done < "${M}/link-gl.txt"
	[ -f "${gallium}" ] || b_die "no libgallium in ${M}/link-gl.txt"
	local out="${PREFIX_PORT_BUILD}/out" QS
	mkdir -p "${out}"
	QS="${out}/${qs_name}"
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=mmap \
		-Wl,--wrap=SDL_GL_SwapWindow -Wl,-z,stack-size=33554432 -Wl,-Map,"${QS}.map" -o "${QS}" "${objs[@]}" \
		-Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${SDL_A}" "${MA[@]}" -Wl,--end-group -lm
	"${NL_STRIP}" -o "${QS}.stripped" "${QS}"

	# --- verification (tools step 5) ---------------------------------------------------------
	nl_no_undefined "${QS}"
	local s bad=0 syms
	syms="$("${NL_NM}" "${QS}")"
	for s in KMSDRM_CreateDevice KMSDRM_GLES_SwapWindow SDL_EGL_LoadLibrary SDL_PHOENIX_HID_Poll \
			__wrap_mmap drmPhoenixMmap drm_phoenix_ioctl gbmint_get_backend kmsro_drm_screen_create \
			v3d_drm_screen_create_renderonly glBegin eglGetPlatformDisplayEXT Host_Init __wrap_SDL_GL_SwapWindow; do
		grep -qE " [TtWw] ${s}\$" <<< "${syms}" || { echo "symbol ${s}: NO"; bad=1; }
	done
	for s in PHOENIX_bootstrap PHOENIX_PumpEvents phxgl_init phoenix_v3d_ioctl winsys_init v3da_connect; do
		if grep -qE " [TtWwDdBb] ${s}\$" <<< "${syms}"; then echo "forbidden symbol ${s}: PRESENT"; bad=1; fi
	done
	for s in 'KMS/DRM Video Driver' '/dev/dri/' 'libdrm-phoenix:' '/dev/kbd0' '/dev/audio0' 'EGL_KHR_platform_gbm' \
			"${qs_name}:" "${qs_name} flipstat" "${qs_name} swapstat" 'V3D 4.2' 'kmsro'; do
		[ "$(nl_count_strings "${QS}.stripped" "${s}")" != 0 ] || { echo "string '${s}': 0"; bad=1; }
	done
	nl_forbid_old_lane "${QS}.stripped" phxgl /dev/fb0 RPI4FB_GETMODE
	# The counter must sit in the engine's swap path: GL_EndRendering -> __wrap_SDL_GL_SwapWindow
	# -> SDL_GL_SwapWindow, and no direct engine call may bypass the wrapper (a call is `bl` or,
	# as GL_EndRendering's last statement, a tail-call `b`).
	local dis ger wrap_body direct
	dis="$("${NL_OBJDUMP}" -d --no-show-raw-insn "${QS}")"
	ger="$(awk '/^[0-9a-f]+ <GL_EndRendering>:$/{f=1; next} f && /^$/{exit} f' <<< "${dis}")"
	grep -qE '\sbl?\s+[0-9a-f]+ <__wrap_SDL_GL_SwapWindow>$' <<< "${ger}" \
		|| { echo "GL_EndRendering -> __wrap_SDL_GL_SwapWindow: NO"; bad=1; }
	wrap_body="$(awk '/^[0-9a-f]+ <__wrap_SDL_GL_SwapWindow>:$/{f=1; next} f && /^$/{exit} f' <<< "${dis}")"
	grep -qE '\sbl?\s+[0-9a-f]+ <SDL_GL_SwapWindow>$' <<< "${wrap_body}" \
		|| { echo "__wrap_SDL_GL_SwapWindow -> SDL_GL_SwapWindow: NO"; bad=1; }
	direct="$(grep -cE '\sbl?\s+[0-9a-f]+ <SDL_GL_SwapWindow>$' <<< "${dis}" || true)"
	[ "${direct}" = 1 ] || { echo "direct calls of the real SDL_GL_SwapWindow: ${direct} (expected 1)"; bad=1; }
	# the frame-pacing order of KMSDRM_GLES_SwapWindow (sdl2_kmsdrm patches/0009)
	GAMEDRM_OBJDUMP="${NL_OBJDUMP}" bash "${GD}/check-swap-order.sh" "${QS}" || bad=1
	[ "${bad}" = 0 ] || b_die "quakespasm-drm: verification failed (see above)"

	# --- launcher ---------------------------------------------------------------------------------
	local target="/usr/bin/${qs_name}"
	"${NL_CC}" -O2 -static -Wall -Wextra -Werror --sysroot="${NL_SYSROOT}/" -B"${NL_SYSROOT}/lib/" -iprefix "${NL_SYSROOT}/" \
		-DQSDRM_TARGET="\"${target}\"" -o "${out}/qs-drm" "${PREFIX_PORT}/launcher/qs-drm-launcher.c"
	nl_no_undefined "${out}/qs-drm"
	grep -aqF "${target}" "${out}/qs-drm" || b_die "launcher ELF lacks its exec target ${target}"

	local p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${p}/bin" "${p}/prog" "${p}/share/quakespasm-drm"
	install -m 755 "${QS}" "${p}/prog/${qs_name}"
	install -m 755 "${QS}.stripped" "${p}/bin/${qs_name}"
	install -m 755 "${out}/qs-drm" "${p}/bin/qs-drm"
	install -m 644 "${QS}.map" "${p}/share/quakespasm-drm/"
	if b_use rootfs; then
		b_install "${p}/bin/${qs_name}" /usr/bin
		b_install "${p}/bin/qs-drm" /bin
		# TODO(TD-26): the plain command name runs this program (GPU migration P1: the default
		# image). P4 gives the programs the plain names themselves.
		install -m 755 "${p}/bin/qs-drm" "${PREFIX_FS}/root/usr/bin/quakespasm"
	fi

	if b_use wayland; then _quakespasm_wl; fi
}

# USE wayland: quakespasm-wl (tools/gpu-lane/sdl2-wl/build.sh steps 4-5)
_quakespasm_wl() {
	local qs_name=quakespasm-wl
	local SP="${PORT_DEP_sdl2_kmsdrm}/wayland" Q="${PREFIX_PORT_WORKDIR}/Quake" G="${PREFIX_PORT}/glue"
	local LI="${PORT_DEP_sdl2_kmsdrm}/wayland/link-inputs.txt" GW="${PORT_DEP_sdl2_kmsdrm}/share/gamewl"
	[ -f "${LI}" ] || b_die "missing ${LI} (sdl2_kmsdrm built without USE wayland?)"
	local kind item GALLIUM_A="" SDL_A="" MESA_GL=() TAILL=() WRAPS=()
	while read -r kind item; do
		case "${kind}" in
			gallium) GALLIUM_A="${item}" ;;
			sdl) SDL_A="${item}" ;;
			mesa-gl) MESA_GL+=("${item}") ;;   # desktop GL: libglapi_bridge.a, no libGLESv2.a
			mesa-es) ;;
			tail) TAILL+=("${item}") ;;
			flag) WRAPS+=("${item}") ;;
			*) b_die "unknown line in ${LI}: ${kind}" ;;
		esac
	done < "${LI}"
	[ -f "${GALLIUM_A}" ] && [ -f "${SDL_A}" ] && [ "${#MESA_GL[@]}" -gt 10 ] || b_die "${LI} is incomplete"

	# the same TU lists and flags as quakespasm-drm, the Wayland SDL's headers
	local globjs=(gl_refrag gl_rlight gl_rmain gl_fog gl_rmisc r_part r_world gl_screen gl_sky
		gl_warp gl_draw image gl_texmgr gl_mesh r_sprite r_alias r_brush gl_model)
	local core=(strlcat strlcpy net_dgrm net_loop net_main net_udp chase cl_demo cl_input
		cl_main cl_parse cl_tent console keys menu sbar view wad cmd common miniz crc
		cvar cfgfile host host_cmd mathlib pr_cmds pr_edict pr_exec sv_main sv_move
		sv_phys sv_user world zone snd_dma snd_mix snd_mem bgmusic cd_null snd_codec)
	local sdlbk=(gl_vidsdl in_sdl snd_sdl)
	local shims=(pl_phoenix_sys pl_phoenix_main pl_phoenix_stubs)
	local QFLAGS=("${NL_TFLAGS[@]}" -fomit-frame-pointer -std=gnu17 -c -O2 -g -ffreestanding -fno-strict-aliasing -Wno-error
		-DUSE_SDL2 -DNO_SDL_CONFIG -I"${Q}" -I"${PORT_DEP_mesa_drm}/src-include" -I"${SP}/include" -I"${SP}/include/SDL2")
	local QO="${PREFIX_PORT_BUILD}/qs-wl-obj" u o objs=()
	rm -rf "${QO}"
	mkdir -p "${QO}"
	{
		for u in "${globjs[@]}" "${core[@]}" "${sdlbk[@]}"; do printf '%s\n' "${Q}/${u}.c"; done
		for u in "${shims[@]}"; do printf '%s\n' "${G}/${u}.c"; done
	} > "${PREFIX_PORT_BUILD}/qs-wl-tus.txt"
	export QO
	qs_cc() { local f="$1" o; o="${QO}/$(basename "${f%.c}").o"; "${NL_CC}" "${@:2}" -o "${o}" "${f}" || { echo "compile FAILED: ${f}"; exit 1; }; }
	export -f qs_cc
	xargs -P"$(nproc)" -I{} bash -c 'qs_cc "$@"' _ {} "${QFLAGS[@]}" < "${PREFIX_PORT_BUILD}/qs-wl-tus.txt" \
		|| b_die "quakespasm-wl compile failed"
	while IFS= read -r u; do
		o="${QO}/$(basename "${u%.c}").o"
		[ -f "${o}" ] || b_die "object missing: ${o}"
		objs+=("${o}")
	done < "${PREFIX_PORT_BUILD}/qs-wl-tus.txt"
	"${NL_CC}" -O2 -g -std=gnu17 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${SP}/include" \
		-DGAMEWL_NAME="\"${qs_name}\"" -DGAMEWL_API='"desktop GL"' -c "${GW}/gamewl_hooks.c" -o "${QO}/gamewl_hooks.o"
	objs+=("${QO}/gamewl_hooks.o")

	# the tools link: C++ driver, -static, --gc-sections, 4 KiB pages, the group's wraps,
	# --wrap=SDL_GL_SwapWindow (frame counter), the port's 32 MiB main stack
	local out="${PREFIX_PORT_BUILD}/out" QS
	mkdir -p "${out}"
	QS="${out}/${qs_name}"
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 "${WRAPS[@]}" \
		-Wl,--wrap=SDL_GL_SwapWindow -Wl,-z,stack-size=33554432 -Wl,-Map,"${QS}.map" -o "${QS}" "${objs[@]}" \
		-Wl,--whole-archive "${GALLIUM_A}" -Wl,--no-whole-archive \
		-Wl,--start-group "${SDL_A}" "${MESA_GL[@]}" "${TAILL[@]}" -Wl,--end-group -lm
	"${NL_STRIP}" -o "${QS}.stripped" "${QS}"

	# --- verification (tools step 5) ---------------------------------------------------------
	nl_no_undefined "${QS}"
	local s bad=0 syms
	syms="$("${NL_NM}" "${QS}")"
	for s in Wayland_CreateDevice Wayland_GLES_SwapWindow Wayland_GLES_CreateContext Wayland_PumpEvents \
			KMSDRM_CreateDevice SDL_EGL_LoadLibrary wl_display_connect wl_egl_window_create wl_cursor_theme_load \
			xkb_keymap_new_from_string xkb_state_update_mask dri2_initialize_wayland __wrap_mmap __wrap_ioctl \
			__wrap_close __wrap_write memfd_create pipe2 drm_phoenix_ioctl kmsro_drm_screen_create \
			v3d_drm_screen_create_renderonly glBegin eglGetPlatformDisplayEXT os_create_anonymous_file \
			wlcursor_os_create_anonymous_file Host_Init __wrap_SDL_GL_SwapWindow; do
		grep -qE " [TtWw] ${s}\$" <<< "${syms}" || { echo "symbol ${s}: NO"; bad=1; }
	done
	for s in PHOENIX_bootstrap PHOENIX_PumpEvents phxgl_init phoenix_v3d_ioctl winsys_init v3da_connect libdecor_new; do
		if grep -qE " [TtWwDdBb] ${s}\$" <<< "${syms}"; then echo "forbidden symbol ${s}: PRESENT"; bad=1; fi
	done
	for s in 'SDL Wayland video driver' 'KMS/DRM Video Driver' 'xdg_wm_base' 'zxdg_decoration_manager_v1' \
			'zwp_relative_pointer_manager_v1' 'zwp_pointer_constraints_v1' 'zwp_linux_dmabuf_v1' 'wl_seat' \
			'WAYLAND_DISPLAY' 'XDG_RUNTIME_DIR' 'libdrm-phoenix:' '/dev/dri/' 'V3D 4.2' 'kmsro' \
			"${qs_name}: windowed GPU game" "${qs_name} flipstat" "${qs_name} swapstat" 'video_driver'; do
		[ "$(nl_count_strings "${QS}.stripped" "${s}")" != 0 ] || { echo "string '${s}': 0"; bad=1; }
	done
	nl_forbid_old_lane "${QS}.stripped" phxgl /dev/fb0 RPI4FB_GETMODE libdecor-
	# the counter sits in the engine's swap path: GL_EndRendering -> __wrap_SDL_GL_SwapWindow
	# -> SDL_GL_SwapWindow, no direct engine call bypasses the wrapper; only the wrappers call
	# the real ioctl() and mmap()
	local calls
	calls="$("${NL_OBJDUMP}" -d --no-show-raw-insn "${QS}" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
		/\tbl?\t/ && / <(ioctl|mmap|__wrap_SDL_GL_SwapWindow|SDL_GL_SwapWindow)>$/ {
			t = $NF; gsub(/[<>]/, "", t); print t, fn }' | sort | uniq -c)"
	printf '%s\n' "${calls}" > "${out}/${qs_name}-call-sites.txt"
	if ! grep -qE ' __wrap_SDL_GL_SwapWindow GL_EndRendering$' <<< "${calls}" \
			|| ! grep -qE ' SDL_GL_SwapWindow __wrap_SDL_GL_SwapWindow$' <<< "${calls}" \
			|| awk '$2 == "SDL_GL_SwapWindow" && $3 != "__wrap_SDL_GL_SwapWindow" { f = 1 } END { exit !f }' <<< "${calls}"; then
		echo "the SDL_GL_SwapWindow wrap is NOT in the engine's swap path:"; printf '%s\n' "${calls}"; bad=1
	fi
	awk '$2 == "ioctl" && $3 != "__wrap_ioctl" { b = 1 } $2 == "mmap" && $3 != "__wrap_mmap" { b = 1 } END { exit b }' <<< "${calls}" \
		|| { echo "real ioctl()/mmap() called from outside the wrappers:"; printf '%s\n' "${calls}"; bad=1; }
	[ "${bad}" = 0 ] || b_die "quakespasm-wl: verification failed (see above)"

	local p="${PREFIX_PORT_INSTALL}"
	mkdir -p "${p}/bin" "${p}/prog" "${p}/share/quakespasm-wl"
	install -m 755 "${QS}" "${p}/prog/${qs_name}"
	install -m 755 "${QS}.stripped" "${p}/bin/${qs_name}"
	install -m 644 "${QS}.map" "${out}/${qs_name}-call-sites.txt" "${p}/share/quakespasm-wl/"
	if b_use rootfs; then
		b_install "${p}/bin/${qs_name}" /usr/bin
	fi
	# shellcheck disable=SC1091
	. "${GW}/relink-sdl-gl-game-wl.subr"
	gamewl_desktop_entry quakespasm "Quake (window)" "QuakeSpasm in a window on the desktop"
}
