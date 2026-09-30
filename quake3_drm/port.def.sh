#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="quake3_drm"
	version="1.32"
	desc="quake3e-drm + the quake3 launcher: the quake3 port's engine linked on the GPU stack (SDL KMSDRM + Mesa GBM/EGL desktop GL)"

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

	# Private prefix. `quake3` (the engine port: compiles the engine, installs nothing) is a
	# dependency so that its objects and port-sources/<it>/engine-link.sh exist first.
	conflicts="quake3_drm!=${version}"
	depends="quake3 sdl2_kmsdrm mesa_drm[opengl] libdrm_phoenix zlib"

	# rootfs: install /usr/bin/quake3e-drm and its launcher /usr/bin/quake3 into the image
	# (the XFCE menu entry "Quake III Arena" = /bin/game-window.sh quake3: the same program in a
	# window of the desktop)
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build-quake3-drm.sh. The quake3
# port's engine objects (its port-sources/quake3-1.32/engine-link.sh) linked on the GPU stack
# in the quakespasm-drm shape: quake3e's opengl1 renderer is a desktop (compatibility) GL
# program, so it links Mesa's libglapi_bridge.a (the static desktop gl* entry points) instead
# of libGLESv2.a; quake3e resolves its qgl* pointers through SDL_GL_GetProcAddress, the bridge
# only matters for gl* referenced at link time. The body and its proofs: sdl2_kmsdrm's
# gamedrm/relink-sdl-gl-game.subr. The launcher is the shipped one (glue/quake3-launcher.c, a
# copy of the coordination repo's tools/quake3-port/quake3-launcher.c) with only its exec
# target rewritten (/usr/bin/quake3e -> /usr/bin/quake3e-drm).
# ONE binary: sdl2_kmsdrm's libSDL2.a has the KMSDRM AND the Wayland video drivers, Mesa's GL
# build EGL on GBM and on Wayland, so the program runs full screen on KMS from psh and in a
# window of the desktop (/bin/game-window.sh: SDL_VIDEODRIVER=wayland + the windowed
# arguments, which the launcher forwards; = the tools' build-quake3-wl.sh build of the same objects).

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
	g_main

	game_desktop_entry quake3 "Quake III Arena" "Quake III Arena (quake3e) in a window on the desktop"
}
