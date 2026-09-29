#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="sdl2_kmsdrm"
	version="2.30.12"
	desc="SDL 2.30.12 with its stock KMSDRM video driver on Mesa GBM/EGL (new GPU lane) + Phoenix HID input and audio"

	# the same release tarball as the sdl2 port (the old lane's /dev/fb0 SDL)
	source="https://github.com/libsdl-org/SDL/releases/download/release-${version}"
	archive_filename="SDL2-${version}.tar.gz"
	src_path="SDL2-${version}/"

	size="7588596"
	sha256="ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea"

	license="Zlib"
	license_file="LICENSE.txt"

	# NEW GPU LANE: private install prefix -- the sdl2 port's libSDL2.a/headers in the shared
	# prefix are the old lane's and stay exactly as they are.
	conflicts="sdl2_kmsdrm!=${version}"
	# SDL is configured against the desktop-GL Mesa build (SDL_OPENGL + SDL_OPENGLES).
	# wayland: the desktop-GL Mesa with the EGL wayland platform, libwayland + wlphx-compat
	# (the `wayland` port, the one that Mesa's wayland platform is built on) and libxkbcommon +
	# <linux/input.h> (wayland_phoenix).
	depends="libdrm_phoenix mesa_drm[opengl] zlib wayland? ( mesa_drm[waylandgl] wayland wayland_phoenix )"

	# vulkan: ALSO build the SDL_VULKAN=ON variant (patches/vulkan/) into vulkan/ -- SDL's
	# stock KMSDRM Vulkan code (VK_KHR_display) for vkquake_drm. The default libSDL2.a stays
	# SDL_VULKAN=OFF, byte for byte what the GL games were measured with.
	#
	# wayland: ALSO build the SDL_WAYLAND=ON variant (patches/wayland/) into wayland/ -- SDL's
	# stock Wayland video driver (xdg-shell windows, server-side decorations, wl_seat input)
	# next to KMSDRM, on Mesa's EGL wayland platform with desktop GL + GLES: the WINDOWED games
	# on the desktop (M8: quakespasm_drm, yquake2_drm, quake3_drm, supertuxkart_drm with USE
	# wayland) and ffplay-wl. Installs share/gamewl/ (the -wl clones' hooks + relink body).
	#
	# rootfs (with wayland): install the windowed-game session helpers into the image:
	# /bin/game-window.sh (one game in a window of the running desktop), game-window-autostart.sh,
	# game-window-quit.sh and the labwc configuration of the games session
	# /etc/xdg/labwc-xfce-m8/ (`export CONF_DIR=/etc/xdg/labwc-xfce-m8` before /bin/xfce-session).
	iuse="vulkan wayland rootfs"
	required_use="rootfs? ( wayland )"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build.sh (steps 2-3: the SDL
# build; its step 4, quakespasm-drm, is the quakespasm_drm port) and build-vkquake-drm.sh
# step 1 (the Vulkan variant). Same patches, overlay, cmake options and flags.
#
#   patches/0001-0004   = ports/sdl2 0001-0004 (Phoenix cmake branch, dynapi off, thread prio)
#   patches/0005        cmake: the Phoenix audio driver (overlay/src/audio/phoenix)
#   patches/0006        KMSDRM: bind GBM + EGL statically (no dlopen)
#   patches/0007        KMSDRM: Phoenix HID input (overlay/src/core/phoenix: /dev/kbd0, /dev/mouse0)
#   patches/0008        KMSDRM: XRGB8888 scan-out
#   patches/0009        KMSDRM: submit the frame before waiting for the previous flip
#                       (frame pacing: quake2-drm 30.00 -> 60.00 fps)
#   patches/0010        KMSDRM: release the locked GBM buffers before destroying the EGL surface
#                       (upstream 9cc2f248f5; exit use-after-free in Mesa's release_buffer)
#   patches/vulkan/0001 (USE vulkan) PHOENIX in SDL_VULKAN's condition + SDL_VIDEO_VULKAN
#   patches/wayland/0101-0103 (USE wayland; = tools/gpu-lane/sdl2-wl/patches) the Phoenix
#                       Wayland video driver in cmake, a clipboard pipe without sigtimedwait,
#                       fractional-scale uint32_t
#
# Installs: include/SDL2, lib/libSDL2.a (+ libSDL2main.a), vulkan/{include,lib} (USE vulkan),
# and share/gamedrm/ -- the game clones' shared hooks and checks (gamedrm_hooks.c,
# check-swap-order.sh, relink-sdl-gl-game.subr): installed here so that a change to them
# rebuilds the clones (they depend on this port).
# USE wayland (tools/gpu-lane/sdl2-wl/build.sh steps 1-3): wayland/{include,lib} and
#   wayland/link-inputs.txt -- the link group of a Wayland SDL GL program, one item per line in
#   link order: "gallium <libgallium>" (whole-archive), "sdl <libSDL2.a>", "mesa-gl <a>" (the
#   desktop-GL archives, libglapi_bridge.a first), "mesa-es <a>" (the GLES archives, with
#   libGLESv2.a), "tail <a>" (libwayland-client/-egl/-cursor, libxkbcommon, wlphx-compat,
#   libffi, libdrm, the Mesa compat shim, zlib) and "flag <ld flag>" (--wrap=mmap/ioctl for
#   libdrm-phoenix, --wrap=close/write + -u for the compat layer's emulated descriptors);
#   share/gamewl/ (gamewl_hooks.c, relink-sdl-gl-game-wl.subr, the session helpers).
#   libwayland-cursor's os_create_anonymous_file() clashes with Mesa's (util/anon_file.c,
#   another signature; hidden from each other as shared libraries): the link uses a private
#   copy of libwayland-cursor.a with its copy renamed (the tools build renamed Mesa's instead).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	# whole new files (Phoenix HID input + audio driver), copied every prepare
	cp -a "${PREFIX_PORT}/overlay/." "${PREFIX_PORT_WORKDIR}/"
	if b_use vulkan; then
		local vs="${PREFIX_PORT_BUILD}/sdl-vk-src"
		[ -d "${vs}" ] || cp -a "${PREFIX_PORT_WORKDIR}" "${vs}"
		b_port_apply_patches "${vs}" vulkan
	fi
	if b_use wayland; then
		local ws="${PREFIX_PORT_BUILD}/sdl-wl-src"
		[ -d "${ws}" ] || cp -a "${PREFIX_PORT_WORKDIR}" "${ws}"
		b_port_apply_patches "${ws}" wayland
	fi
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

	# pkg-config sees ONLY Mesa's desktop-GL prefix (egl, gbm), libdrm-phoenix and Mesa's
	# private zlib view (egl/gbm list zlib in Requires.private); -pthread removed (Mesa's .pc)
	local M="${PORT_DEP_mesa_drm}/gl"
	grep -qx 'opengl=true' "${M}/opengl.txt" 2>/dev/null || b_die "${M} is not a desktop-GL Mesa build"
	nl_pkgconfig "${PREFIX_PORT_BUILD}/nl/pkg-config-sdl" \
		"${M}/prefix/lib/pkgconfig:${PORT_DEP_libdrm_phoenix}/lib/pkgconfig:${PORT_DEP_mesa_drm}/zlib-prefix/lib/pkgconfig" \
		strip-pthread

	_sdl2_kmsdrm_cmake "${PREFIX_PORT_WORKDIR}" "${PREFIX_PORT_BUILD}/sdl-build" "${PREFIX_PORT_INSTALL}" OFF

	# The configuration must be what this port is about -- fail loudly otherwise.
	local cfg="${PREFIX_PORT_INSTALL}/include/SDL2/SDL_config.h" d
	for d in SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_OPENGL_EGL SDL_VIDEO_OPENGL SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX \
			SDL_THREAD_PTHREAD SDL_TIMER_UNIX; do
		grep -qE "^#define ${d} +1" "${cfg}" || b_die "SDL_config.h lacks ${d} 1"
	done
	for d in SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC SDL_VIDEO_DRIVER_PHOENIX SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_WAYLAND \
			SDL_INPUT_LINUXEV SDL_LOADSO_DLOPEN SDL_VIDEO_VULKAN; do
		if grep -qE "^#define ${d}( |$)" "${cfg}"; then b_die "SDL_config.h defines ${d}"; fi
	done

	if b_use vulkan; then
		local vp="${PREFIX_PORT_INSTALL}/vulkan"
		_sdl2_kmsdrm_cmake "${PREFIX_PORT_BUILD}/sdl-vk-src" "${PREFIX_PORT_BUILD}/sdl-vk-build" "${vp}" ON
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

	if b_use wayland; then _sdl2_kmsdrm_wayland; fi

	local g="${PREFIX_PORT_INSTALL}/share/gamedrm"
	mkdir -p "${g}"
	install -m 644 "${PREFIX_PORT}/gamedrm/gamedrm_hooks.c" "${PREFIX_PORT}/gamedrm/relink-sdl-gl-game.subr" "${g}/"
	install -m 755 "${PREFIX_PORT}/gamedrm/check-swap-order.sh" "${g}/"

	if b_use rootfs; then _sdl2_kmsdrm_stage_session; fi
}

# USE wayland: SDL with its Wayland video driver (+ KMSDRM) into wayland/, the link group of a
# Wayland SDL GL program (wayland/link-inputs.txt) and share/gamewl/. The tools build is
# tools/gpu-lane/sdl2-wl/build.sh steps 1-3 (its step 4, quakespasm-wl, is quakespasm_drm's
# USE wayland); the same patches (sdl2_kmsdrm's + patches/wayland), cmake options and flags.
_sdl2_kmsdrm_wayland() {
	local wp="${PREFIX_PORT_INSTALL}/wayland" nl="${PREFIX_PORT_BUILD}/nl"
	local M="${PORT_DEP_mesa_drm}/waylandgl" WO="${PORT_DEP_wayland:?}" WX="${PORT_DEP_wayland_phoenix:?}/prefix"
	local MB="${M}/mesa-build" LDP="${PORT_DEP_libdrm_phoenix}"
	local p
	command -v wayland-scanner > /dev/null || b_die "wayland: host wayland-scanner not found"
	grep -qx 'opengl=true' "${M}/opengl.txt" 2>/dev/null || b_die "${M} is not a desktop-GL Mesa build (mesa_drm USE waylandgl)"
	for p in "${M}/link-gl.txt" "${M}/link-gles.txt" "${WO}/lib/libwayland-client.a" "${WO}/lib/libwayland-egl.a" \
			"${WO}/lib/libwayland-cursor.a" "${WO}/lib/libwlphx-compat.a" "${WO}/deps/libffi/lib/libffi.a" \
			"${WO}/compat/include/sys/mman.h" "${WX}/lib/libxkbcommon.a" "${WX}/lib/pkgconfig/xkbcommon.pc" \
			"${WX}/include/xkbcommon/xkbcommon.h" "${WX}/include/linux/input.h" "${WX}/include/evdev/input-event-codes.h"; do
		[ -e "${p}" ] || b_die "wayland: missing ${p}"
	done

	# <linux/input.h> (BTN_* / KEY_* for SDL_waylandevents.c): wayland_phoenix's shim over the
	# evdev codes, alone -- the rest of its linux/ shims (types.h, ioctl.h...) stays off SDL's path
	local wi="${PREFIX_PORT_BUILD}/wl-include"
	rm -rf "${wi}"
	mkdir -p "${wi}/linux" "${wi}/evdev"
	cp "${WX}/include/linux/input.h" "${wi}/linux/"
	cp "${WX}/include/evdev/input-event-codes.h" "${wi}/evdev/"
	# libxkbcommon through a private view (only its own headers; wayland_phoenix's include/
	# also holds a second libwayland and the libinput/libudev shims)
	local xv="${PREFIX_PORT_BUILD}/xkbcommon-view" xver
	xver="$(sed -n 's/^Version: //p' "${WX}/lib/pkgconfig/xkbcommon.pc")"
	rm -rf "${xv}"
	mkdir -p "${xv}/include" "${xv}/lib/pkgconfig"
	cp -a "${WX}/include/xkbcommon" "${xv}/include/"
	printf '%s\n' "prefix=${xv}" "Name: xkbcommon" "Description: libxkbcommon from wayland_phoenix (private view)" \
		"Version: ${xver}" "Libs: ${WX}/lib/libxkbcommon.a" "Cflags: -I\${prefix}/include" > "${xv}/lib/pkgconfig/xkbcommon.pc"

	# pkg-config sees ONLY: Mesa's waylandgl prefix (egl, gbm), libdrm-phoenix, Mesa's private
	# zlib, the Wayland client stack + its libffi view, and the xkbcommon view
	nl_pkgconfig "${nl}/pkg-config-sdl-wl" \
		"${M}/prefix/lib/pkgconfig:${LDP}/lib/pkgconfig:${PORT_DEP_mesa_drm}/zlib-prefix/lib/pkgconfig:${WO}/lib/pkgconfig:${WO}/share/pkgconfig:${WO}/deps/libffi/lib/pkgconfig:${xv}/lib/pkgconfig" \
		strip-pthread
	_sdl2_kmsdrm_cmake "${PREFIX_PORT_BUILD}/sdl-wl-src" "${PREFIX_PORT_BUILD}/sdl-wl-build" "${wp}" OFF \
		"${nl}/pkg-config-sdl-wl" "-I${WO}/compat/include -I${wi}" ON

	local cfg="${wp}/include/SDL2/SDL_config.h" d
	for d in SDL_VIDEO_DRIVER_WAYLAND SDL_VIDEO_DRIVER_KMSDRM SDL_VIDEO_OPENGL_EGL SDL_VIDEO_OPENGL SDL_VIDEO_OPENGL_ES2 \
			SDL_INPUT_PHOENIX SDL_AUDIO_DRIVER_PHOENIX SDL_THREAD_PTHREAD SDL_TIMER_UNIX HAVE_MEMFD_CREATE; do
		grep -qE "^#define ${d} +1" "${cfg}" || b_die "wayland: SDL_config.h lacks ${d} 1"
	done
	for d in SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_EGL SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_CURSOR \
			SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_XKBCOMMON SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR HAVE_LIBDECOR_H \
			SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC SDL_VIDEO_DRIVER_PHOENIX SDL_VIDEO_DRIVER_X11 SDL_INPUT_LINUXEV \
			SDL_LOADSO_DLOPEN SDL_VIDEO_VULKAN; do
		if grep -qE "^#define ${d}( |$)" "${cfg}"; then b_die "wayland: SDL_config.h defines ${d}"; fi
	done
	nl_has_sym "${wp}/lib/libSDL2.a" Wayland_CreateDevice || b_die "wayland: libSDL2.a has no Wayland_CreateDevice"

	# libwayland-cursor with its os_create_anonymous_file() renamed (see the header)
	local cur="${wp}/lib/libwayland-cursor-phx.a"
	nl_has_sym "${WO}/lib/libwayland-cursor.a" os_create_anonymous_file \
		|| b_die "wayland: ${WO}/lib/libwayland-cursor.a defines no os_create_anonymous_file (the rename is stale)"
	"${NL_OBJCOPY}" --redefine-sym os_create_anonymous_file=wlcursor_os_create_anonymous_file \
		"${WO}/lib/libwayland-cursor.a" "${cur}"
	nl_has_sym "${cur}" wlcursor_os_create_anonymous_file || b_die "wayland: the libwayland-cursor rename failed"
	if nl_has_sym "${cur}" os_create_anonymous_file; then b_die "wayland: ${cur} still defines os_create_anonymous_file"; fi

	# --- the link group (tools: build-out/link-inputs.txt) ---------------------------------------
	# Mesa's two lists: "--whole-archive <libgallium>", its archives, then libdrm.a / the compat
	# shim / libz.a (Mesa's tail_libs), which go to the end of the group here.
	local tail_mesa=("${LDP}/lib/libdrm.a" "${PORT_DEP_mesa_drm}/compat/libmesadrm-compat.a" "${PORT_DEP_zlib}/lib/libz.a")
	local li="${wp}/link-inputs.txt" l k gallium="" t
	: > "${li}.tmp"
	for k in gl gles; do
		while IFS= read -r l; do
			case "${l}" in
				"--whole-archive "*) gallium="${l#--whole-archive }" ;;
				*) for t in "${tail_mesa[@]}"; do [ "${l}" = "${t}" ] && continue 2; done
					[ -f "${l}" ] || b_die "wayland: ${M}/link-${k}.txt names a missing ${l}"
					if [ "${k}" = gl ]; then echo "mesa-gl ${l}"; else echo "mesa-es ${l}"; fi ;;
			esac
		done < "${M}/link-${k}.txt"
	done >> "${li}.tmp"
	[ -f "${gallium}" ] || b_die "wayland: no libgallium in ${M}/link-gl.txt"
	{
		echo "gallium ${gallium}"
		echo "sdl ${wp}/lib/libSDL2.a"
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
	grep -q '^mesa-gl .*/libglapi_bridge\.a$' "${li}" || b_die "wayland: no libglapi_bridge.a in the desktop-GL list"
	grep -q '^mesa-es .*/libGLESv2\.a$' "${li}" || b_die "wayland: no libGLESv2.a in the GLES list"
	if grep -q '^mesa-gl .*/libGLESv2\.a$' "${li}"; then b_die "wayland: libGLESv2.a in the desktop-GL list"; fi
	echo "sdl2_kmsdrm: wayland: $(grep -c . "${li}") link items (${li})"

	# --- share/gamewl: the -wl clones' hooks + relink body, the session helpers ------------------
	local g="${PREFIX_PORT_INSTALL}/share/gamewl"
	rm -rf "${g}"
	mkdir -p "${g}"
	install -m 644 "${PREFIX_PORT}/gamewl/gamewl_hooks.c" "${PREFIX_PORT}/gamewl/relink-sdl-gl-game-wl.subr" "${g}/"
}

# USE rootfs (with wayland): the windowed-game session helpers into the image. The labwc
# configuration is the tools' labwc-xfce-m8/ with the hand-staged program names of the
# tools session rewritten to the image's, as xfce_wayland stages labwc-xfce-demo/.
# TODO(TD-26): the M8 session's own names (labwc-xfce-m8, the "M8 " log prefix) go when P4
# names the session's files for the image.
_sdl2_kmsdrm_stage_session() {
	local r="${PREFIX_FS}/root" f
	local demo_sed=(-e 's|/usr/lib/xfce-demo/bin/thunar|/bin/thunar-wl|g'
		-e 's|/usr/lib/xfce-demo/bin/xfce4-|/bin/xfce4-|g'
		-e 's|/usr/lib/xfce-demo/bin/xfdesktop|/bin/xfdesktop|g'
		-e 's|/bin/foot-2|/bin/foot|g' -e 's|/bin/fuzzel-2|/bin/fuzzel|g')
	for f in game-window.sh game-window-autostart.sh game-window-quit.sh; do
		b_install "${PREFIX_PORT}/gamewl/pi/${f}" /bin
	done
	mkdir -p "${r}/etc/xdg/labwc-xfce-m8"
	for f in rc.xml menu.xml autostart environment; do
		sed "${demo_sed[@]}" "${PREFIX_PORT}/gamewl/labwc-xfce-m8/${f}" > "${r}/etc/xdg/labwc-xfce-m8/${f}"
		chmod 644 "${r}/etc/xdg/labwc-xfce-m8/${f}"
	done
	if grep -nE '/bin/(foot|fuzzel|labwc)-2|xfce-demo/bin/(thunar|xfce4-|xfdesktop)' "${r}/etc/xdg/labwc-xfce-m8"/*; then
		b_die "sdl2_kmsdrm: the games session config still names a program this image does not have (above)"
	fi
}
