#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="sdl2_kmsdrm"
	version="2.30.12"
	desc="SDL 2.30.12 with its stock KMSDRM and Wayland video drivers on Mesa GBM/EGL + Phoenix HID input and audio"

	# the SDL 2.30.12 release tarball
	source="https://github.com/libsdl-org/SDL/releases/download/release-${version}"
	archive_filename="SDL2-${version}.tar.gz"
	src_path="SDL2-${version}/"

	size="7588596"
	sha256="ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea"

	license="Zlib"
	license_file="LICENSE.txt"

	# Private install prefix: nothing of SDL reaches the shared ports prefix (on an older
	# buildroot that prefix may still hold the deleted sdl2 port's headers and archive).
	conflicts="sdl2_kmsdrm!=${version}"
	# SDL is configured against the desktop-GL Mesa build (SDL_OPENGL + SDL_OPENGLES), which has
	# the EGL platforms of both SDL video drivers: GBM (KMSDRM) and Wayland. The Wayland client
	# stack is wayland_phoenix's: libwayland + wlphx-compat from its libwayland/ view (the one
	# Mesa's wayland platform is built on), libxkbcommon + <linux/input.h> from its prefix/.
	depends="libdrm_phoenix mesa_drm[opengl] zlib wayland_phoenix"

	# vulkan: ALSO build the SDL_VULKAN=ON variant (patches/vulkan/) into vulkan/ -- SDL's
	# stock KMSDRM Vulkan code (VK_KHR_display) for vkquake_drm (KMSDRM only: a vkQuake window
	# on Wayland needs the V3DV Wayland WSI, which the static ICD does not have).
	#
	# rootfs: install the desktop's game launcher into the image: /bin/game-window.sh <game>
	# (one game in a window of the running desktop, the XFCE menu entries' command),
	# game-window-autostart.sh, game-window-quit.sh and the labwc configuration of the games
	# session /etc/xdg/labwc-xfce-games/ (`export CONF_DIR=/etc/xdg/labwc-xfce-games` before
	# /bin/xfce-session: the games of GAME_LIST start by themselves, one after another).
	iuse="vulkan rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-wl/build.sh (steps 1-3: the SDL
# build with the Wayland AND the KMSDRM video drivers, and its link group; step 4, the game,
# is the quakespasm_drm port) -- itself tools/gpu-lane/sdl2-drm/build.sh's SDL plus the
# Wayland driver -- and build-vkquake-drm.sh step 1 (the Vulkan variant). Same patches,
# overlay, cmake options and flags. ONE libSDL2.a serves full screen and windowed: SDL tries
# its Wayland driver first and falls through to KMSDRM when there is no compositor socket
# (SDL_VIDEODRIVER picks one explicitly), so a program runs full screen from psh and in a
# window inside the desktop.
#
#   patches/0001-0004   Phoenix cmake branch + pthread detection, dynapi off, thread priority no-op
#   patches/0005        cmake: the Phoenix audio driver (overlay/src/audio/phoenix)
#   patches/0006        KMSDRM: bind GBM + EGL statically (no dlopen)
#   patches/0007        KMSDRM: Phoenix HID input (overlay/src/core/phoenix: /dev/kbd0, /dev/mouse0)
#   patches/0008        KMSDRM: XRGB8888 scan-out
#   patches/0009        KMSDRM: submit the frame before waiting for the previous flip
#                       (frame pacing: quake2-drm 30.00 -> 60.00 fps)
#   patches/0010        KMSDRM: release the locked GBM buffers before destroying the EGL surface
#                       (upstream 9cc2f248f5; exit use-after-free in Mesa's release_buffer)
#   patches/0011        thread: condition-variable timeouts on the monotonic clock
#   patches/wayland/0101-0103 (= tools/gpu-lane/sdl2-wl/patches) the Phoenix Wayland video
#                       driver in cmake, a clipboard pipe without sigtimedwait, fractional-scale
#                       uint32_t (applied after the vulkan copy: that variant has no Wayland)
#   patches/vulkan/0001 (USE vulkan) PHOENIX in SDL_VULKAN's condition + SDL_VIDEO_VULKAN
#
# Installs: include/SDL2, lib/libSDL2.a (+ libSDL2main.a), vulkan/{include,lib} (USE vulkan),
# link-inputs.txt, and share/gamedrm/ -- the games' shared hooks and checks (gamedrm_hooks.c,
# check-swap-order.sh, relink-sdl-gl-game.subr): installed here so that a change to them
# rebuilds the games (they depend on this port).
# link-inputs.txt (tools: sdl2-wl/build-out/link-inputs.txt) is the link group of an SDL GL
#   program, one item per line in link order: "gallium <libgallium>" (whole-archive),
#   "sdl <libSDL2.a>", "mesa-gl <a>" (the desktop-GL archives, libglapi_bridge.a first),
#   "mesa-es <a>" (the GLES archives, with libGLESv2.a), "tail <a>" (libwayland-client/-egl/
#   -cursor, libxkbcommon, wlphx-compat, libffi, libdrm, the Mesa compat shim, zlib) and
#   "flag <ld flag>" (--wrap=mmap/ioctl for libdrm-phoenix, --wrap=close/write + -u for the
#   compat layer's emulated descriptors).
# libwayland-cursor's os_create_anonymous_file() clashes with Mesa's (util/anon_file.c, another
# signature; hidden from each other as shared libraries): the group links a private copy of
# libwayland-cursor.a with its copy renamed (the tools build renamed Mesa's copies instead).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	# whole new files (Phoenix HID input + audio driver), copied every prepare
	cp -a "${PREFIX_PORT}/overlay/." "${PREFIX_PORT_WORKDIR}/"
	if b_use vulkan; then
		local vs="${PREFIX_PORT_BUILD}/sdl-vk-src"
		[ -d "${vs}" ] || cp -a "${PREFIX_PORT_WORKDIR}" "${vs}"
		b_port_apply_patches "${vs}" vulkan
	fi
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" wayland
}

# _sdl2_kmsdrm_cmake <src> <build dir> <install prefix> <SDL_VULKAN ON|OFF>
#                    [<pkg-config> <extra C flags> <SDL_WAYLAND ON|OFF>]
_sdl2_kmsdrm_cmake() {
	local src="$1" bd="$2" ip="$3" vk="$4" pkgc="${5:-${PREFIX_PORT_BUILD}/nl/pkg-config-sdl}" xcf="${6:-}" wl="${7:-OFF}"
	local wl_opts=()
	[ "${wl}" = ON ] && wl_opts=(-DSDL_WAYLAND_SHARED=OFF -DSDL_WAYLAND_LIBDECOR=OFF -DSDL_WAYLAND_QT_TOUCH=OFF)
	if [ ! -f "${bd}/Makefile" ]; then
		mkdir -p "${bd}"
		(cd "${bd}" && PKG_CONFIG="${pkgc}" cmake "${src}" \
			-DCMAKE_INSTALL_PREFIX="${ip}" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_SYSTEM_NAME=Generic \
			-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${NL_CC}" \
			-DCMAKE_CXX_COMPILER="${NL_CXX}" \
			-DCMAKE_AR="${NL_AR}" \
			-DCMAKE_RANLIB="${NL_RANLIB}" \
			-DCMAKE_C_FLAGS="${NL_TFLAGS[*]} -O2 -g -std=gnu17${xcf:+ ${xcf}}" \
			-DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" \
			-DCMAKE_EXE_LINKER_FLAGS="${NL_TFLAGS[*]} -Wl,-z,max-page-size=0x1000" \
			-DPKG_CONFIG_EXECUTABLE="${pkgc}" \
			-DPHOENIX=ON \
			-DSDL_LIBC=ON \
			-DSDL_PTHREADS=ON \
			-DSDL_CLOCK_GETTIME=ON \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_TEST=OFF \
			-DSDL_X11=OFF \
			-DSDL_WAYLAND="${wl}" "${wl_opts[@]}" \
			-DSDL_KMSDRM=ON \
			-DSDL_KMSDRM_SHARED=OFF \
			-DSDL_OPENGL=ON \
			-DSDL_OPENGLES=ON \
			-DSDL_VULKAN="${vk}" \
			-DSDL_HIDAPI=OFF \
			-DSDL_LIBUDEV=OFF \
			-DSDL_DBUS=OFF \
			-DSDL_IBUS=OFF \
			-DSDL_PULSEAUDIO=OFF \
			-DSDL_ALSA=OFF \
			-DSDL_PIPEWIRE=OFF \
			-DSDL_JACK=OFF \
			-DSDL_OSS=OFF \
			-DSDL_SNDIO=OFF \
			-DSDL_ESD=OFF \
			-DSDL_NAS=OFF \
			-DSDL_ARTS=OFF \
			-DSDL_DIRECTFB=OFF \
			-DSDL_RPI=OFF \
			-DSDL_VIVANTE=OFF \
			-DSDL_OFFSCREEN=OFF)
	fi
	make -C "${bd}" -j"$(nproc)" install
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local M="${PORT_DEP_mesa_drm}/gl" WO="${PORT_DEP_wayland_phoenix:?}/libwayland" WX="${PORT_DEP_wayland_phoenix:?}/prefix"
	local LDP="${PORT_DEP_libdrm_phoenix}" nl="${PREFIX_PORT_BUILD}/nl" p
	command -v wayland-scanner > /dev/null || b_die "host wayland-scanner not found"
	grep -qx 'opengl=true' "${M}/opengl.txt" 2>/dev/null || b_die "${M} is not a desktop-GL Mesa build"
	for p in "${M}/link-gl.txt" "${M}/link-gles.txt" "${WO}/lib/libwayland-client.a" "${WO}/lib/libwayland-egl.a" \
			"${WO}/lib/libwayland-cursor.a" "${WO}/lib/libwlphx-compat.a" "${WO}/deps/libffi/lib/libffi.a" \
			"${WX}/share/wayland-phoenix/compat/include/sys/mman.h" "${WX}/lib/libxkbcommon.a" "${WX}/lib/pkgconfig/xkbcommon.pc" \
			"${WX}/include/xkbcommon/xkbcommon.h" "${WX}/include/linux/input.h" "${WX}/include/evdev/input-event-codes.h"; do
		[ -e "${p}" ] || b_die "missing ${p}"
	done

	# <linux/input.h> (BTN_* / KEY_* for SDL_waylandevents.c): wayland_phoenix's shim over the
	# evdev codes, alone -- the rest of its linux/ shims (types.h, ioctl.h...) stays off SDL's path
	local wi="${PREFIX_PORT_BUILD}/wl-include"
	rm -rf "${wi}"
	mkdir -p "${wi}/linux" "${wi}/evdev"
	cp "${WX}/include/linux/input.h" "${wi}/linux/"
	cp "${WX}/include/evdev/input-event-codes.h" "${wi}/evdev/"
	# libxkbcommon through a private view (only its own headers; wayland_phoenix's prefix/include
	# also holds the libinput/libudev/linux shims)
	local xv="${PREFIX_PORT_BUILD}/xkbcommon-view" xver
	xver="$(sed -n 's/^Version: //p' "${WX}/lib/pkgconfig/xkbcommon.pc")"
	rm -rf "${xv}"
	mkdir -p "${xv}/include" "${xv}/lib/pkgconfig"
	cp -a "${WX}/include/xkbcommon" "${xv}/include/"
	printf '%s\n' "prefix=${xv}" "Name: xkbcommon" "Description: libxkbcommon from wayland_phoenix (private view)" \
		"Version: ${xver}" "Libs: ${WX}/lib/libxkbcommon.a" "Cflags: -I\${prefix}/include" > "${xv}/lib/pkgconfig/xkbcommon.pc"

	# pkg-config sees ONLY: Mesa's desktop-GL prefix (egl, gbm; egl.pc requires wayland-client,
	# -server and wayland-egl-backend), libdrm-phoenix, Mesa's private zlib view, the Wayland
	# client stack + its libffi view, and the xkbcommon view; -pthread removed (Mesa's .pc)
	nl_pkgconfig "${nl}/pkg-config-sdl" \
		"${M}/prefix/lib/pkgconfig:${LDP}/lib/pkgconfig:${PORT_DEP_mesa_drm}/zlib-prefix/lib/pkgconfig:${WO}/lib/pkgconfig:${WO}/share/pkgconfig:${WO}/deps/libffi/lib/pkgconfig:${xv}/lib/pkgconfig" \
		strip-pthread

	_sdl2_kmsdrm_cmake "${PREFIX_PORT_WORKDIR}" "${PREFIX_PORT_BUILD}/sdl-build" "${PREFIX_PORT_INSTALL}" OFF \
		"${nl}/pkg-config-sdl" "-I${WX}/share/wayland-phoenix/compat/include -I${wi}" ON

	# The configuration must be what this port is about -- fail loudly otherwise.
	local cfg="${PREFIX_PORT_INSTALL}/include/SDL2/SDL_config.h" d
	for d in SDL_VIDEO_DRIVER_WAYLAND SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_OPENGL_EGL SDL_VIDEO_OPENGL SDL_VIDEO_OPENGL_ES2 \
			SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX SDL_THREAD_PTHREAD SDL_TIMER_UNIX HAVE_MEMFD_CREATE; do
		grep -qE "^#define ${d} +1" "${cfg}" || b_die "SDL_config.h lacks ${d} 1"
	done
	for d in SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_EGL SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_CURSOR \
			SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_XKBCOMMON SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR HAVE_LIBDECOR_H \
			SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC SDL_VIDEO_DRIVER_PHOENIX SDL_VIDEO_DRIVER_X11 SDL_INPUT_LINUXEV \
			SDL_LOADSO_DLOPEN SDL_VIDEO_VULKAN; do
		if grep -qE "^#define ${d}( |$)" "${cfg}"; then b_die "SDL_config.h defines ${d}"; fi
	done
	for d in Wayland_CreateDevice KMSDRM_CreateDevice; do
		nl_has_sym "${PREFIX_PORT_INSTALL}/lib/libSDL2.a" "${d}" || b_die "libSDL2.a has no ${d}"
	done

	_sdl2_kmsdrm_link_inputs

	if b_use vulkan; then
		local vp="${PREFIX_PORT_INSTALL}/vulkan"
		_sdl2_kmsdrm_cmake "${PREFIX_PORT_BUILD}/sdl-vk-src" "${PREFIX_PORT_BUILD}/sdl-vk-build" "${vp}" ON "${nl}/pkg-config-sdl"
		cfg="${vp}/include/SDL2/SDL_config.h"
		for d in SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_VULKAN SDL_VIDEO_OPENGL_EGL SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX \
				SDL_THREAD_PTHREAD SDL_TIMER_UNIX; do
			grep -qE "^#define ${d} +1" "${cfg}" || b_die "vulkan: SDL_config.h lacks ${d} 1"
		done
		for d in SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC SDL_VIDEO_DRIVER_PHOENIX SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_WAYLAND \
				SDL_INPUT_LINUXEV SDL_LOADSO_DLOPEN; do
			if grep -qE "^#define ${d}( |$)" "${cfg}"; then b_die "vulkan: SDL_config.h defines ${d}"; fi
		done
		nl_has_sym "${vp}/lib/libSDL2.a" KMSDRM_Vulkan_CreateSurface || b_die "vulkan: libSDL2.a has no KMSDRM_Vulkan_CreateSurface"
	fi

	local g="${PREFIX_PORT_INSTALL}/share/gamedrm"
	mkdir -p "${g}"
	install -m 644 "${PREFIX_PORT}/gamedrm/gamedrm_hooks.c" "${PREFIX_PORT}/gamedrm/relink-sdl-gl-game.subr" "${g}/"
	install -m 755 "${PREFIX_PORT}/gamedrm/check-swap-order.sh" "${g}/"

	if b_use rootfs; then _sdl2_kmsdrm_stage_session; fi
}

# link-inputs.txt (see the header), from Mesa's gl/ lists and the Wayland client stack
_sdl2_kmsdrm_link_inputs() {
	local M="${PORT_DEP_mesa_drm}/gl" WO="${PORT_DEP_wayland_phoenix}/libwayland" WX="${PORT_DEP_wayland_phoenix}/prefix"
	local LDP="${PORT_DEP_libdrm_phoenix}"
	# libwayland-cursor with its os_create_anonymous_file() renamed (see the header)
	local cur="${PREFIX_PORT_INSTALL}/lib/libwayland-cursor-phx.a"
	nl_has_sym "${WO}/lib/libwayland-cursor.a" os_create_anonymous_file \
		|| b_die "${WO}/lib/libwayland-cursor.a defines no os_create_anonymous_file (the rename is stale)"
	"${NL_OBJCOPY}" --redefine-sym os_create_anonymous_file=wlcursor_os_create_anonymous_file \
		"${WO}/lib/libwayland-cursor.a" "${cur}"
	nl_has_sym "${cur}" wlcursor_os_create_anonymous_file || b_die "the libwayland-cursor rename failed"
	if nl_has_sym "${cur}" os_create_anonymous_file; then b_die "${cur} still defines os_create_anonymous_file"; fi

	# --- the link group (tools: sdl2-wl/build-out/link-inputs.txt) ------------------------------
	# Mesa's two lists: "--whole-archive <libgallium>", its archives, then libdrm.a / the compat
	# shim / libz.a (Mesa's tail_libs), which go to the end of the group here.
	local tail_mesa=("${LDP}/lib/libdrm.a" "${PORT_DEP_mesa_drm}/compat/libmesadrm-compat.a" "${PORT_DEP_zlib}/lib/libz.a")
	local li="${PREFIX_PORT_INSTALL}/link-inputs.txt" l k gallium="" t
	: > "${li}.tmp"
	for k in gl gles; do
		while IFS= read -r l; do
			case "${l}" in
				"--whole-archive "*) gallium="${l#--whole-archive }" ;;
				*) for t in "${tail_mesa[@]}"; do [ "${l}" -ef "${t}" ] && continue 2; done   # (-ef: PREFIX_BUILD may end in /)
					[ -f "${l}" ] || b_die "${M}/link-${k}.txt names a missing ${l}"
					if [ "${k}" = gl ]; then echo "mesa-gl ${l}"; else echo "mesa-es ${l}"; fi ;;
			esac
		done < "${M}/link-${k}.txt"
	done >> "${li}.tmp"
	[ -f "${gallium}" ] || b_die "no libgallium in ${M}/link-gl.txt"
	{
		echo "gallium ${gallium}"
		echo "sdl ${PREFIX_PORT_INSTALL}/lib/libSDL2.a"
		cat "${li}.tmp"
		for t in "${WO}/lib/libwayland-client.a" "${WO}/lib/libwayland-egl.a" "${cur}" "${WX}/lib/libxkbcommon.a" \
				"${WO}/lib/libwlphx-compat.a" "${WO}/deps/libffi/lib/libffi.a" "${tail_mesa[@]}"; do
			echo "tail ${t}"
		done
		# libdrm-phoenix's --wrap=mmap (BO-token maps) and --wrap=ioctl (sync-file ioctls), the
		# compat layer's --wrap=close/write (epoll/eventfd emulation), pulled with -u
		for t in -Wl,--wrap=mmap -Wl,--wrap=ioctl -Wl,--wrap=close -Wl,--wrap=write -Wl,-u,__wrap_close -Wl,-u,__wrap_write; do
			echo "flag ${t}"
		done
	} > "${li}"
	rm -f "${li}.tmp"
	grep -q '^mesa-gl .*/libglapi_bridge\.a$' "${li}" || b_die "no libglapi_bridge.a in the desktop-GL list"
	grep -q '^mesa-es .*/libGLESv2\.a$' "${li}" || b_die "no libGLESv2.a in the GLES list"
	if grep -q '^mesa-gl .*/libGLESv2\.a$' "${li}"; then b_die "libGLESv2.a in the desktop-GL list"; fi
	echo "sdl2_kmsdrm: $(grep -c . "${li}") link items (${li})"

}

# USE rootfs: the desktop's game launcher and the games session into the image (games/: this
# port's own files, derived from the coordination repo's tools/gpu-lane/sdl2-wl/pi and
# conf/labwc-xfce-m8 with the image's program names).
_sdl2_kmsdrm_stage_session() {
	local r="${PREFIX_FS}/root" f
	for f in game-window.sh game-window-autostart.sh game-window-quit.sh; do
		b_install "${PREFIX_PORT}/games/${f}" /bin
	done
	mkdir -p "${r}/etc/xdg/labwc-xfce-games"
	for f in rc.xml menu.xml autostart environment; do
		install -m 644 "${PREFIX_PORT}/games/labwc-xfce-games/${f}" "${r}/etc/xdg/labwc-xfce-games/${f}"
	done
	# nothing may name a hand-staged program of the tools sessions
	if grep -nE '(foot|fuzzel|labwc|xfce-session|xfce-desktop)-2|-low\b|-wl2|-drm2|xfce-demo/bin/(thunar|xfce4-|xfdesktop)|simple-egl' \
			"${r}/etc/xdg/labwc-xfce-games"/* "${r}/bin/game-window.sh" "${r}/bin/game-window-autostart.sh" "${r}/bin/game-window-quit.sh"; then
		b_die "sdl2_kmsdrm: the games session names a program this image does not have (above)"
	fi
}
