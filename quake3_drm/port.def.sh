#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="quake3_drm"
	version="1.32"
	desc="quake3e-drm + quake3-drm launcher: the quake3 port's engine relinked on the new GPU lane (SDL KMSDRM + Mesa GBM/EGL desktop GL)"

	# The quake3 port's archive (quake3e at its pinned commit). This clone RELINKS the quake3
	# port's objects; the archive is extracted only because every framework port has one.
	commit="f694bbbca42140332688a0f3a1c2c1a9e3497004"
	source="https://github.com/ec-/quake3e/archive"
	archive_filename="${commit}.tar.gz"
	src_path="Quake3e-${commit}/"

	size="18307818"
	sha256="3141036d6888d12f4eb02038bcb7ce0020067a9aad6c97d90f6105fc644ba952"

	license="GPL-2.0-or-later"
	license_file="COPYING.txt"

	# NEW GPU LANE: private prefix. `quake3` (and `sdl2`) are dependencies so that the engine
	# objects and the build.log holding the port's final link exist first.
	conflicts="quake3_drm!=${version}"
	depends="quake3 sdl2 sdl2_kmsdrm mesa_drm[opengl] libdrm_phoenix zlib wayland? ( sdl2_kmsdrm[wayland] )"

	# rootfs: install /usr/bin/quake3e-drm and its launcher /usr/bin/quake3-drm into the image,
	# the launcher also as /usr/bin/quake3
	# wayland: ALSO relink the WINDOWED clone for the Wayland desktop (M8): /usr/bin/quake3e-wl
	# + its launcher /usr/bin/quake3-wl, and (with rootfs) the XFCE menu entry "Quake III
	# (window)" = /bin/game-window.sh quake3 (sdl2_kmsdrm USE rootfs)
	iuse="rootfs wayland"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build-quake3-drm.sh. The quake3
# port's own final link re-run with the old SDL-GL glue and group swapped for the new stack in
# the quakespasm-drm shape: quake3e's opengl1 renderer is a desktop (compatibility) GL program,
# so it links Mesa's libglapi_bridge.a (the static desktop gl* entry points) instead of
# libGLESv2.a; quake3e resolves its qgl* pointers through SDL_GL_GetProcAddress, the bridge
# only matters for gl* referenced at link time. The body and its proofs: sdl2_kmsdrm's
# gamedrm/relink-sdl-gl-game.subr. The launcher is the shipped one (glue/quake3-launcher.c, a
# copy of the coordination repo's tools/quake3-port/quake3-launcher.c) with only its exec
# target rewritten (/usr/bin/quake3e -> /usr/bin/quake3e-drm).
# USE wayland: tools/gpu-lane/sdl2-wl/build-quake3-wl.sh -- the same relink on sdl2_kmsdrm's
# Wayland link group, desktop GL half (share/gamewl/relink-sdl-gl-game-wl.subr).

p_prepare() {
	:
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"
	# shellcheck disable=SC1091
	. "${PORT_DEP_sdl2_kmsdrm}/share/gamedrm/relink-sdl-gl-game.subr"

	G_APP=quake3-drm
	G_ENGINE=quake3e
	G_PORTDIR=quake3-1.32
	G_GL=gl
	G_API_TEXT="desktop GL"
	G_LAUNCHER_SRC="${PREFIX_PORT}/glue/quake3-launcher.c"
	G_LAUNCHER=quake3
	G_ENGINE_SYMS="GLimp_Init GLimp_EndFrame GetRefAPI Com_Init VM_Compile"
	G_DO_CONTROL=1
	g_main

	# TODO(TD-26): the plain command name runs this program (GPU migration P1: the default
	# image). P4 gives the programs the plain names themselves.
	if b_use rootfs; then
		install -m 755 "${PREFIX_PORT_INSTALL}/bin/quake3-drm" "${PREFIX_FS}/root/usr/bin/quake3"
	fi

	if b_use wayland; then
		# shellcheck disable=SC1091
		. "${PORT_DEP_sdl2_kmsdrm}/share/gamewl/relink-sdl-gl-game-wl.subr"
		G_APP=quake3-wl
		# the -drm clone above ran the control relink on the same engine objects
		G_DO_CONTROL=0
		gwl_main
		gamewl_desktop_entry quake3 "Quake III Arena (window)" "Quake III Arena (quake3e) in a window on the desktop"
	fi
}
