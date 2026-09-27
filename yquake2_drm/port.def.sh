#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="yquake2_drm"
	version="8.71"
	desc="yquake2-drm + quake2-drm launcher: the yquake2 port's engine relinked on the new GPU lane (SDL KMSDRM + Mesa GBM/EGL/GLES)"

	# The yquake2 port's archive. This clone RELINKS the yquake2 port's objects (it compiles
	# nothing of the engine); the archive is extracted only because every framework port has
	# one -- it is the source of what the clone ships, and it carries the licence file.
	commit="a9e88f6c555b017812ae348572ce7a6bf5677479"
	source="https://github.com/yquake2/yquake2/archive"
	archive_filename="${commit}.tar.gz"
	src_path="yquake2-${commit}/"

	size="2735399"
	sha256="0cfc23f1127698a2a498bd480b381703eac6664be47e98a525357bd9a6c1e5fd"

	license="GPL-2.0-or-later"
	license_file="LICENSE"

	# NEW GPU LANE: private prefix. `yquake2` (and through it `sdl2`) is a dependency so that
	# port_manager builds the engine objects and the build.log holding its final link first.
	conflicts="yquake2_drm!=${version}"
	depends="yquake2 sdl2 sdl2_kmsdrm mesa_drm[opengl] libdrm_phoenix zlib"

	# rootfs: install /usr/bin/yquake2-drm and its launcher /usr/bin/quake2-drm into the image
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/sdl2-drm/build-quake2-drm.sh. The yquake2
# port's own final link (from its build.log) re-run with the old SDL-GL glue objects and the
# old group (ports libSDL2.a, libGL-phoenix.a, libv3d-phoenix.a) swapped for the new stack in
# the stk-drm shape: yQuake2's ref_gl3 is a GLES3 renderer that loads every gl* through glad +
# SDL_GL_GetProcAddress (= eglGetProcAddress), so it links libGLESv2 + the shared glapi, not
# the desktop-GL bridge. The body and its proofs: sdl2_kmsdrm's gamedrm/relink-sdl-gl-game.subr.
# The launcher is the shipped one (glue/quake2-launcher.c, a copy of the coordination repo's
# tools/yquake2-port/quake2-launcher.c) with only its exec target rewritten
# (/usr/bin/yquake2 -> /usr/bin/yquake2-drm): the same ram-stage-play of /usr/share/quake2 to
# /tmp/quake2 and the same video/demo arguments.

p_prepare() {
	:
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"
	# shellcheck disable=SC1091
	. "${PORT_DEP_sdl2_kmsdrm}/share/gamedrm/relink-sdl-gl-game.subr"

	G_APP=quake2-drm
	G_ENGINE=yquake2
	G_PORTDIR=yquake2-8.71
	G_GL=gles
	G_API_TEXT=GLES
	G_LAUNCHER_SRC="${PREFIX_PORT}/glue/quake2-launcher.c"
	G_LAUNCHER=quake2
	G_ENGINE_SYMS="GL3_Init GL3_EndFrame gladLoadGLES2Loader GetRefAPI Qcommon_Init"
	G_DO_CONTROL=1
	g_main
}
