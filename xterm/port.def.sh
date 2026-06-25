#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xterm"
	version="396"
	desc="The standard X Window System terminal emulator"

	# The github snapshots mirror ships a generated ./configure (the
	# invisible-island.net release uses the same sources). The tarball's top
	# directory is xterm-snapshots-xterm-<ver>, so src_path differs from name.
	source="https://github.com/ThomasDickey/xterm-snapshots/archive/refs/tags/"
	archive_filename="xterm-${version}.tar.gz"
	src_path="xterm-snapshots-xterm-${version}/"

	# size/sha256 of the github archive (fill in via b_port for reproducibility).
	size=""
	sha256=""

	license="MIT"
	license_file="COPYING"

	conflicts=""
	# NOTE: xterm depends on the full X11 client/toolkit lib stack
	# (libXaw libXmu libXt libSM libICE libXext libXpm libXrender libX11
	# libxcb libXau libXdmcp). On the RPi4 port these are NOT yet phoenix-rtos
	# ports — they are cross-built into /tmp/x11-phoenix by
	# tools/x11-port/build-x11-phoenix.sh in the coordination repo. Until that
	# stack lands as ports, this recipe CANNOT be driven end-to-end by the ports
	# build. See README.md in this directory (feeds task #12).
	depends="libXaw libXmu libXt libXext libXpm libXrender libX11 libxcb"

	supports="phoenix>=3.3"
}

# Phoenix-RTOS source adaptations (see the coordination repo's
# tools/x11-port/patches/xterm-396-phoenix.patch — the canonical copy). The
# patch adds, all keyed on __phoenix__:
#   - get_pty(): open the SVR4 /dev/ptmx multiplexor (posixsrv) + unlockpt +
#     ptsname, instead of the BSD /dev/ptyXX search or openpty().
#   - USE_POSIX_TERMIOS: so <termios.h> (and struct winsize) are included.
#   - USE_SYSV_PGRP: POSIX setsid()/setpgrp(void), not the 2-arg BSD setpgrp.
#   - resetShell(): DEFSHELL_NAME fallback (compile-time default shell path).
#   - xtermcap.h: include the no-curses termcap stub instead of <curses.h>.
# Plus three drop-in source files (no curses/wctype on Phoenix):
#   - phoenix_termcap.[ch]: tgetent()/tgetstr() "no termcap database" stubs.
#   - a local <wctype.h> shim (isw*/tow* over the narrow ctype for ASCII).
#   - a force-included fd_set shim (fd_mask + __fds_bits) for Xlib's Xpoll.h.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# Drop in the Phoenix-specific source files shipped with this recipe.
	cp -v "${PREFIX_PORT}/files/phoenix_termcap.c" "${PREFIX_PORT_WORKDIR}/"
	cp -v "${PREFIX_PORT}/files/phoenix_termcap.h" "${PREFIX_PORT_WORKDIR}/"
}

p_build() {
	# The compile-time fallback shell. On a netboot RAM root /bin/sh does not
	# exist (the rootfs is mounted elsewhere); override accordingly per variant.
	local defshell="${XTERM_DEFSHELL:-/bin/sh}"
	local fdset_shim="${PREFIX_PORT}/files/xterm-phoenix-fdset-shim.h"
	local wctype_inc="${PREFIX_PORT}/files/include"

	# CORE X bitmap fonts only (no Xft/fontconfig); curses/termcap features off
	# (covered by the stub). --x-includes/--x-libraries must point at wherever
	# the X11 lib stack was installed (see README — currently /tmp/x11-phoenix).
	: "${XLIB_PREFIX:=/tmp/x11-phoenix}"

	(cd "${PREFIX_PORT_WORKDIR}" && \
		PKG_CONFIG="pkg-config --static" \
		PKG_CONFIG_PATH="${XLIB_PREFIX}/lib/pkgconfig:${XLIB_PREFIX}/share/pkgconfig" \
		PKG_CONFIG_LIBDIR="${XLIB_PREFIX}/lib/pkgconfig:${XLIB_PREFIX}/share/pkgconfig" \
		./configure --host="${TARGET}" --prefix="${PREFIX}" \
			--x-includes="${XLIB_PREFIX}/include" --x-libraries="${XLIB_PREFIX}/lib" \
			--disable-freetype --disable-luit --disable-imake --without-utempter \
			--disable-toolbar --disable-double-buffer --disable-session-mgt \
			--without-xpm --disable-tcap-fkeys --disable-tcap-query \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="--sysroot=${SYSROOT} -I${XLIB_PREFIX}/include" \
			LDFLAGS="--sysroot=${SYSROOT} -static -L${XLIB_PREFIX}/lib -L${SYSROOT}/lib")

	# The no-curses termcap stub object, appended to the X link closure.
	"${CROSS}gcc" --sysroot="${SYSROOT}" -I"${XLIB_PREFIX}/include" \
		-c "${PREFIX_PORT_WORKDIR}/phoenix_termcap.c" \
		-o "${PREFIX_PORT_WORKDIR}/phoenix_termcap.o"

	local xclosure="${PREFIX_PORT_WORKDIR}/phoenix_termcap.o -lXaw7 -lXmu -lXt -lSM -lICE -lXpm -lXrender -lXext -lX11 -lxcb -lXau -lXdmcp -lphoenix -lc"

	make -C "${PREFIX_PORT_WORKDIR}" xterm \
		CFLAGS="--sysroot=${SYSROOT} -include ${fdset_shim} -I${wctype_inc} -I${XLIB_PREFIX}/include -DDEFSHELL_NAME=\"${defshell}\" -DP_tmpdir=\"/tmp\"" \
		LDFLAGS="--sysroot=${SYSROOT} -static -L${XLIB_PREFIX}/lib -L${SYSROOT}/lib" \
		EXTRA_LOADFLAGS="${xclosure}"

	mkdir -p "${PREFIX_FS}/root/bin"
	cp -v "${PREFIX_PORT_WORKDIR}/xterm" "${PREFIX_FS}/root/bin/xterm"
	cp -v "${PREFIX_PORT_WORKDIR}/xterm" "${PREFIX_PROG}"
}
