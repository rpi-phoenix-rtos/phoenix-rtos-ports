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
	depends="libdrm_phoenix mesa_drm[opengl] zlib"

	# vulkan: ALSO build the SDL_VULKAN=ON variant (patches/vulkan/) into vulkan/ -- SDL's
	# stock KMSDRM Vulkan code (VK_KHR_display) for vkquake_drm. The default libSDL2.a stays
	# SDL_VULKAN=OFF, byte for byte what the GL games were measured with.
	iuse="vulkan"

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
#
# Installs: include/SDL2, lib/libSDL2.a (+ libSDL2main.a), vulkan/{include,lib} (USE vulkan),
# and share/gamedrm/ -- the game clones' shared hooks and checks (gamedrm_hooks.c,
# check-swap-order.sh, relink-sdl-gl-game.subr): installed here so that a change to them
# rebuilds the clones (they depend on this port).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	# whole new files (Phoenix HID input + audio driver), copied every prepare
	cp -a "${PREFIX_PORT}/overlay/." "${PREFIX_PORT_WORKDIR}/"
	if b_use vulkan; then
		local vs="${PREFIX_PORT_BUILD}/sdl-vk-src"
		[ -d "${vs}" ] || cp -a "${PREFIX_PORT_WORKDIR}" "${vs}"
		b_port_apply_patches "${vs}" vulkan
	fi
}

# _sdl2_kmsdrm_cmake <src> <build dir> <install prefix> <SDL_VULKAN ON|OFF>
_sdl2_kmsdrm_cmake() {
	local src="$1" bd="$2" ip="$3" vk="$4"
	if [ ! -f "${bd}/Makefile" ]; then
		mkdir -p "${bd}"
		(cd "${bd}" && PKG_CONFIG="${PREFIX_PORT_BUILD}/nl/pkg-config-sdl" cmake "${src}" \
			-DCMAKE_INSTALL_PREFIX="${ip}" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_SYSTEM_NAME=Generic \
			-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${NL_CC}" \
			-DCMAKE_CXX_COMPILER="${NL_CXX}" \
			-DCMAKE_AR="${NL_AR}" \
			-DCMAKE_RANLIB="${NL_RANLIB}" \
			-DCMAKE_C_FLAGS="${NL_TFLAGS[*]} -O2 -g -std=gnu17" \
			-DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" \
			-DCMAKE_EXE_LINKER_FLAGS="${NL_TFLAGS[*]} -Wl,-z,max-page-size=0x1000" \
			-DPKG_CONFIG_EXECUTABLE="${PREFIX_PORT_BUILD}/nl/pkg-config-sdl" \
			-DPHOENIX=ON \
			-DSDL_LIBC=ON \
			-DSDL_PTHREADS=ON \
			-DSDL_CLOCK_GETTIME=ON \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_TEST=OFF \
			-DSDL_X11=OFF \
			-DSDL_WAYLAND=OFF \
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

	local g="${PREFIX_PORT_INSTALL}/share/gamedrm"
	mkdir -p "${g}"
	install -m 644 "${PREFIX_PORT}/gamedrm/gamedrm_hooks.c" "${PREFIX_PORT}/gamedrm/relink-sdl-gl-game.subr" "${g}/"
	install -m 755 "${PREFIX_PORT}/gamedrm/check-swap-order.sh" "${g}/"
}
