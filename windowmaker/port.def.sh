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

	size="3397167"
	sha256="f22358ff60301670e1e2b502faad0f2da7ff8976632d538f95fe4638e9c6b714"

	license="GPL-2.0-or-later"
	license_file="COPYING"

	conflicts=""
	# Window Maker needs the FULL X11 client/toolkit lib stack (libXpm libXmu libXt
	# libXext libXrender libX11 libxcb ...) PLUS the antialiased-font stack (libXft
	# fontconfig expat freetype). Those are no longer separate ports: the toolkit
	# libs are bundled in xorg_libs (Layer 1) and the font stack in xorg_fonts
	# (Layer 2); both stage into $PREFIX_BUILD/{lib,include}. zlib comes in
	# transitively via xorg_fonts.
	depends="xorg_libs xorg_fonts"

	supports="phoenix>=3.3"
}

# Phoenix-RTOS source/libc adaptations:
#
#   libphoenix gap-fills carried in a tiny static lib (files/ftw-phoenix/,
#   compiled into libftw.a by p_build):
#     - nftw()/ftw()         : libphoenix has no <ftw.h> (WINGs/proplist.c)
#     - scandir()/alphasort(): absent from libphoenix <dirent.h> (util helpers)
#     - nice()               : no-op stub; no process-priority API (wmsetbg)
#   committed to libphoenix proper (so future builds need no gap-fill):
#     - _SC_LINE_MAX added to sysconf() (libphoenix commit) — WINGs/error.c
#   build-time defines against the current sysroot:
#     - -DWMAKER_SHELL="/bin/sh": retained compile-time hook for wmaker's shell.
#       No source patch is carried: WindowMaker's src/main.c hardcodes "/bin/sh"
#       which is exactly the Pi's shell path, so stock wmaker is correct as-is.
#       The define is harmless (unused unless a future #ifndef WMAKER_SHELL guard
#       is patched into main.c to relocate the shell) and kept for that hook.
#
# NOTE: the font/2D stack (freetype, fontconfig incl. its two Phoenix source
# fixes, libXft, expat, libpng, cairo) is NOT built here — it is the xorg_fonts
# aggregate port (a dependency). gperf (fontconfig codegen) is a host dep of
# xorg_fonts, not of this port.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# Framework-provided env: HOST is the autotools triplet (aarch64-phoenix);
	# TARGET (aarch64a72-generic-rpi4b) is NOT a valid config.sub machine. SYSROOT
	# is the project sysroot the layer ports also use. The X11 client/toolkit stack
	# (xorg_libs) and the font stack (xorg_fonts) both staged into
	# $PREFIX_BUILD/{lib,include}, so the combined dependency prefix IS $PREFIX_BUILD
	# (was /tmp/wmaker-deps under the old coordination-repo build).
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"
	: "${WMAKER_DEPS:=${PREFIX_BUILD%/}}"
	# Compile-time fallback shell; the NFS/SD root provides /bin/sh.
	local wmshell="${WMAKER_SHELL:-/bin/sh}"
	local TCGCC="${CROSS}gcc" TCAR="${CROSS}ar" TCRANLIB="${CROSS}ranlib"

	# libphoenix gap-fill lib: libphoenix ships no <ftw.h>/nftw() (WINGs/proplist.c),
	# no scandir()/alphasort() (util helpers) and no nice() (wmsetbg). The sources
	# live in files/ftw-phoenix/; build libftw.a and stage it + its headers into the
	# dependency prefix so the -include'd prototype header resolves and -lftw links.
	if [ ! -f "${WMAKER_DEPS}/lib/libftw.a" ]; then
		local ftwsrc="${PREFIX_PORT}/files/ftw-phoenix"
		"${TCGCC}" --sysroot="${SYSROOT}" -I"${ftwsrc}" -O2 -Wall \
			-c "${ftwsrc}/ftw.c" -o "${PREFIX_PORT_BUILD}/ftw.o" || b_die "windowmaker: libftw compile failed"
		"${TCAR}" rcs "${PREFIX_PORT_BUILD}/libftw.a" "${PREFIX_PORT_BUILD}/ftw.o"
		"${TCRANLIB}" "${PREFIX_PORT_BUILD}/libftw.a"
		mkdir -p "${WMAKER_DEPS}/lib" "${WMAKER_DEPS}/include"
		cp "${PREFIX_PORT_BUILD}/libftw.a" "${WMAKER_DEPS}/lib/"
		cp "${ftwsrc}/ftw.h" "${ftwsrc}/wmaker-phoenix-compat.h" "${WMAKER_DEPS}/include/"
		echo "windowmaker: libftw.a + ftw.h + wmaker-phoenix-compat.h staged into ${WMAKER_DEPS}"
	fi

	# -include wmaker-phoenix-compat.h: declares
	#   nice/scandir/alphasort (defined in libftw.a) at every call site.
	local gapdefs="-include wmaker-phoenix-compat.h"
	local pwddefs="-DMAXHOSTNAMELEN=256 -DO_NOFOLLOW=0 -DXOS_USE_MTSAFE_PWDAPI -D_POSIX_THREAD_SAFE_FUNCTIONS=200809L"
	# The \\\" quoting on -DWMAKER_SHELL survives the extra /bin/sh hop that
	# `make CFLAGS=...` performs before invoking gcc (a bare \" would be stripped,
	# leaving the '/' of /bin/sh to parse as division). Same fix as the xterm port.
	local cf="--sysroot=${SYSROOT} -std=gnu17 -I${WMAKER_DEPS}/include ${pwddefs} ${gapdefs} -DWMAKER_SHELL=\\\"${wmshell}\\\""
	local xclosure="-lXft -lfontconfig -lexpat -lfreetype -lXrender -lXpm -lXext -lXmu -lXt -lSM -lICE -lX11 -lxcb -lXau -lXdmcp -lpng16 -lz -lftw -lm"

	(cd "${PREFIX_PORT_WORKDIR}" && \
		PKG_CONFIG="pkg-config --static" \
		PKG_CONFIG_PATH="${WMAKER_DEPS}/lib/pkgconfig:${WMAKER_DEPS}/share/pkgconfig" \
		PKG_CONFIG_LIBDIR="${WMAKER_DEPS}/lib/pkgconfig:${WMAKER_DEPS}/share/pkgconfig" \
		./configure --host="${HOST}" --prefix="${PREFIX_PORT_INSTALL}" --datarootdir=/usr/share --sysconfdir=/etc \
			--disable-shared \
			--enable-png --disable-jpeg --disable-tiff --disable-gif --disable-webp \
			--disable-magick --disable-shm --disable-xinerama --disable-nls --disable-xlocale \
			--x-includes="${WMAKER_DEPS}/include" --x-libraries="${WMAKER_DEPS}/lib" \
			xorg_cv_malloc0_returns_null=no \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${cf}" \
			LDFLAGS="--sysroot=${SYSROOT} -static -L${WMAKER_DEPS}/lib -L${SYSROOT}/lib" \
			LIBS="${xclosure}") || b_die "windowmaker: configure failed"

	make -C "${PREFIX_PORT_WORKDIR}" CFLAGS="${cf}" || b_die "windowmaker: build failed"

	mkdir -p "${PREFIX_FS}/root/bin"
	cp -v "${PREFIX_PORT_WORKDIR}/src/wmaker" "${PREFIX_FS}/root/bin/wmaker"
	cp -v "${PREFIX_PORT_WORKDIR}/src/wmaker" "${PREFIX_PROG}"

	# Install + stage WindowMaker's RUNTIME DATA so the on-target wmaker finds its
	# compiled DATADIR (/usr/share/WindowMaker + /usr/share/WINGs) and SYSCONFDIR
	# (/etc/WindowMaker) — set via --prefix=/usr --sysconfdir=/etc above. Without
	# this the port shipped only the binary and wmaker fell back to bare compiled
	# defaults (no styles/pixmaps/icons, "could not load widget images" warnings).
	local stage="${PREFIX_PORT_BUILD}/install-stage"
	rm -rf "$stage"
	make -C "${PREFIX_PORT_WORKDIR}" install DESTDIR="$stage" >/dev/null 2>&1 || b_die "windowmaker: make install failed"
	mkdir -p "${PREFIX_FS}/root/usr/share" "${PREFIX_FS}/root/etc"
	cp -a "$stage/usr/share/WindowMaker" "${PREFIX_FS}/root/usr/share/"
	cp -a "$stage/usr/share/WINGs"       "${PREFIX_FS}/root/usr/share/"
	cp -a "$stage/etc/WindowMaker"       "${PREFIX_FS}/root/etc/"

	# Seed the per-user config that wmaker.inst would normally create.
	#
	# Window Maker refuses to come up without $HOME/GNUstep/Defaults: it warns
	# "could not find user GNUstep directory (/root/GNUstep/Defaults/WindowMaker)",
	# tries to run `wmaker.inst` to build it, gets "sh: wmaker.inst: not found"
	# (the installer script is not shipped), and then never draws a desktop -- the X
	# server takes over the screen and it stays black. Diagnosed on 2026-09-04 once
	# startx's own diagnostics were visible.
	#
	# wmaker.inst just copies the system defaults into the user's tree, so do that
	# here at build time instead of shipping a shell installer and running it on the
	# target. HOME is /root (startx sets it), hence /root/GNUstep.
	mkdir -p "${PREFIX_FS}/root/root/GNUstep/Defaults"
	for _wmdef in WindowMaker WMRootMenu WMState WMWindowAttributes; do
		[ -f "$stage/etc/WindowMaker/${_wmdef}" ] &&
			cp -a "$stage/etc/WindowMaker/${_wmdef}" \
				"${PREFIX_FS}/root/root/GNUstep/Defaults/${_wmdef}"
	done
	true
	echo "windowmaker: staged /usr/share/{WindowMaker,WINGs} + /etc/WindowMaker into the rootfs"
}
