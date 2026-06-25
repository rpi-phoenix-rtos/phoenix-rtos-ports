#!/usr/bin/env bash
:
# DRAFT — see README.md. This recipe cannot yet be driven end-to-end by the
# ports build because Window Maker's dependencies (the X11 client/toolkit lib
# stack AND the antialiased-font stack expat/fontconfig/libXft) are not yet
# phoenix-rtos ports. The canonical, working build lives in the coordination
# repo at tools/x11-port/build-wmaker.sh. This recipe mirrors that build's
# flags so it is ready to wire up once those deps land as ports (feeds task #12).
#shellcheck disable=2034
{
	ports_api=1

	name="windowmaker"
	version="0.95.9"
	desc="Window Maker — NeXTSTEP-style X11 window manager"

	# Release tarball ships a generated ./configure. Top dir is WindowMaker-<ver>.
	source="https://github.com/window-maker/wmaker/releases/download/wmaker-${version}/"
	archive_filename="WindowMaker-${version}.tar.gz"
	src_path="WindowMaker-${version}/"

	# fill in via b_port for reproducibility
	size=""
	sha256=""

	license="GPL-2.0-or-later"
	license_file="COPYING"

	conflicts=""
	# NOTE: depends on the FULL X11 client/toolkit lib stack PLUS the
	# antialiased-font stack (expat, fontconfig, libXft) — none of which are
	# phoenix-rtos ports yet. On the RPi4 port they are cross-built into
	# /tmp/x11-phoenix (build-x11-phoenix.sh) and /tmp/wmaker-deps
	# (build-wmaker.sh) in the coordination repo. See README.md (feeds task #12).
	depends="libXft fontconfig expat libXpm libXmu libXt libXext libXrender libX11 libxcb"

	supports="phoenix>=3.3"
}

# Phoenix-RTOS source/libc adaptations (the canonical copies are in the
# coordination repo):
#
#   libphoenix gap-fills carried in a tiny static lib (tools/x11-port/ftw-phoenix/):
#     - nftw()/ftw()         : libphoenix has no <ftw.h> (WINGs/proplist.c)
#     - scandir()/alphasort(): absent from libphoenix <dirent.h> (util helpers)
#     - nice()               : no-op stub; no process-priority API (wmsetbg)
#   committed to libphoenix proper (so future builds need no gap-fill):
#     - _SC_LINE_MAX added to sysconf() (libphoenix commit) — WINGs/error.c
#   build-time defines against the current sysroot:
#     - -Drint=round         : libphoenix libm has no rint(); round() suffices
#       for wmaker's UI coordinate/colour rounding (wcolorpanel.c, wbrowser.c)
#   source patch:
#     - src/main.c ExecuteShellCommand() hardcodes shell="/bin/sh"; guard with
#       #ifndef WMAKER_SHELL and override via -DWMAKER_SHELL (the netboot Pi's
#       shell is /nfstest/bin/sh, not /bin/sh) — same trap as the xterm/JWM ports.
#
# fontconfig (a wmaker build dependency) also needed two Phoenix source fixes:
#   - fccache.c: libphoenix <sys/time.h> ships a non-standard value-based
#     timercmp(); fontconfig passes struct-timeval pointers — redefine it with
#     the standard pointer-based form after the include.
#   - fccompat.c: FcRandom()'s rand_r() path had a non-constant static
#     initializer; seed lazily. Forced onto rand_r() (libphoenix has no
#     random()/initstate()/setstate()) via configure cache vars.
# HOST build dep: gperf (fontconfig codegen).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# Where the X11 lib stack + font stack were installed. On the RPi4 port this
	# is the coordination repo's combined dependency prefix (see README).
	: "${WMAKER_DEPS:=/tmp/wmaker-deps}"
	# Compile-time fallback shell; on a netboot RAM root /bin/sh does not exist.
	local wmshell="${WMAKER_SHELL:-/nfstest/bin/sh}"

	# libphoenix gap-fill lib (nftw/scandir/nice) + its -include'd prototypes.
	local gapdefs="-D_SC_LINE_MAX=5 -Drint=round -include wmaker-phoenix-compat.h"
	local pwddefs="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L"
	local cf="--sysroot=${SYSROOT} -I${WMAKER_DEPS}/include ${pwddefs} ${gapdefs} -DWMAKER_SHELL=\"${wmshell}\""
	local xclosure="-lXft -lfontconfig -lexpat -lfreetype -lXrender -lXpm -lXext -lXmu -lXt -lSM -lICE -lX11 -lxcb -lXau -lXdmcp -lz -lftw -lm"

	(cd "${PREFIX_PORT_WORKDIR}" && \
		PKG_CONFIG="pkg-config --static" \
		PKG_CONFIG_PATH="${WMAKER_DEPS}/lib/pkgconfig:${WMAKER_DEPS}/share/pkgconfig" \
		PKG_CONFIG_LIBDIR="${WMAKER_DEPS}/lib/pkgconfig:${WMAKER_DEPS}/share/pkgconfig" \
		./configure --host="${TARGET}" --prefix="${PREFIX}" --sysconfdir="${PREFIX}/etc" \
			--disable-shared \
			--disable-png --disable-jpeg --disable-tiff --disable-gif --disable-webp \
			--disable-magick --disable-shm --disable-xinerama --disable-nls --disable-xlocale \
			--x-includes="${WMAKER_DEPS}/include" --x-libraries="${WMAKER_DEPS}/lib" \
			xorg_cv_malloc0_returns_null=no \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${cf}" \
			LDFLAGS="--sysroot=${SYSROOT} -static -L${WMAKER_DEPS}/lib -L${SYSROOT}/lib" \
			LIBS="${xclosure}")

	make -C "${PREFIX_PORT_WORKDIR}" CFLAGS="${cf}"

	mkdir -p "${PREFIX_FS}/root/bin"
	cp -v "${PREFIX_PORT_WORKDIR}/src/wmaker" "${PREFIX_FS}/root/bin/wmaker"
	cp -v "${PREFIX_PORT_WORKDIR}/src/wmaker" "${PREFIX_PROG}"
}
