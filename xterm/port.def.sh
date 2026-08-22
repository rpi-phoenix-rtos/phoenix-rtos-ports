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

	# size/sha256 of the github tag archive (xterm-snapshots mirror).
	size="1567721"
	sha256="f440dea104c81c6888aca8cc0855bb07070353bd2776d7f1d518c8d59a244874"

	license="MIT"
	license_file="COPYING"

	conflicts=""
	# xterm needs the full X11 client/toolkit lib stack (libXaw libXmu libXt libSM
	# libICE libXext libXpm libXrender libX11 libxcb libXau libXdmcp). Those are no
	# longer separate ports: they are all bundled in the aggregate xorg_libs
	# (Layer 1), which stages them into $PREFIX_BUILD/{lib,include}.
	depends="xorg_libs"

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

	# Framework-provided env: HOST is the autotools triplet (aarch64-phoenix);
	# TARGET (aarch64a72-generic-rpi4b) is NOT a valid config.sub machine. SYSROOT
	# is the project sysroot the layer ports also use. The X11 client/toolkit stack
	# was staged by xorg_libs into $PREFIX_BUILD/{lib,include}, so point the X lib
	# prefix there (was /tmp/x11-phoenix under the old coordination-repo build).
	local SYSROOT="${PREFIX_BUILD%/}/sysroot"

	# CORE X bitmap fonts only (no Xft/fontconfig); curses/termcap features off
	# (covered by the stub).
	: "${XLIB_PREFIX:=${PREFIX_BUILD%/}}"

	# PIN xterm's termcap/terminfo link-test OFF (cf_cv_lib_tgetent / _part_tgetent):
	# xtermcap.c is redirected to the no-curses phoenix_termcap stub (plain-tgetent
	# #else path). If configure is left to auto-detect tgetent it defines USE_TERMINFO
	# (finding a tgetent-providing lib in the sysroot) -> xtermcap.c then #includes
	# curses/term terminfo headers we don't ship -> "setupterm/tigetstr/cur_term
	# undeclared" build failure. This bit us in a CLEAN --with-showcase --with-ports
	# build (2026-08-22): the earlier build-verify ran against a dirty sysroot where
	# the auto-detect happened to fail. Forcing both cache vars to "no" makes configure
	# define NEITHER USE_TERMCAP nor USE_TERMINFO -> the plain-tgetent path the stub
	# satisfies. (Mirrors tools/x11-port/build-xterm.sh, which documented this.)
	(cd "${PREFIX_PORT_WORKDIR}" && \
		PKG_CONFIG="pkg-config --static" \
		PKG_CONFIG_PATH="${XLIB_PREFIX}/lib/pkgconfig:${XLIB_PREFIX}/share/pkgconfig" \
		PKG_CONFIG_LIBDIR="${XLIB_PREFIX}/lib/pkgconfig:${XLIB_PREFIX}/share/pkgconfig" \
		cf_cv_lib_tgetent=no cf_cv_lib_part_tgetent=no \
		./configure --host="${HOST}" --prefix="${PREFIX_PORT_INSTALL}" \
			--x-includes="${XLIB_PREFIX}/include" --x-libraries="${XLIB_PREFIX}/lib" \
			--disable-freetype --disable-luit --disable-imake --without-utempter \
			--disable-toolbar --disable-double-buffer --disable-session-mgt \
			--without-xpm --disable-tcap-fkeys --disable-tcap-query \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="--sysroot=${SYSROOT} -std=gnu17 -I${XLIB_PREFIX}/include" \
			LDFLAGS="--sysroot=${SYSROOT} -static -L${XLIB_PREFIX}/lib -L${SYSROOT}/lib")

	# The no-curses termcap stub object, appended to the X link closure.
	"${CROSS}gcc" --sysroot="${SYSROOT}" -I"${XLIB_PREFIX}/include" \
		-c "${PREFIX_PORT_WORKDIR}/phoenix_termcap.c" \
		-o "${PREFIX_PORT_WORKDIR}/phoenix_termcap.o"

	local xclosure="${PREFIX_PORT_WORKDIR}/phoenix_termcap.o -lXaw7 -lXmu -lXt -lSM -lICE -lXpm -lXrender -lXext -lX11 -lxcb -lXau -lXdmcp -lphoenix -lc"

	# NOTE the triple-backslash quoting on the string-valued -D macros: this CFLAGS
	# value is handed to `make CFLAGS=...`, which re-expands it in the recipe's
	# /bin/sh before invoking gcc. A single \" would be stripped by that shell, so
	# gcc would see -DDEFSHELL_NAME=/bin/sh (the '/' then parses as division:
	# "expected expression before '/' token"). \\\" survives one extra shell hop
	# so gcc receives -DDEFSHELL_NAME="/bin/sh" (a real string literal).
	make -C "${PREFIX_PORT_WORKDIR}" xterm \
		CFLAGS="--sysroot=${SYSROOT} -std=gnu17 -include ${fdset_shim} -I${wctype_inc} -I${XLIB_PREFIX}/include -DDEFSHELL_NAME=\\\"${defshell}\\\" -DP_tmpdir=\\\"/tmp\\\"" \
		LDFLAGS="--sysroot=${SYSROOT} -static -L${XLIB_PREFIX}/lib -L${SYSROOT}/lib" \
		EXTRA_LOADFLAGS="${xclosure}"

	mkdir -p "${PREFIX_FS}/root/bin"
	cp -v "${PREFIX_PORT_WORKDIR}/xterm" "${PREFIX_FS}/root/bin/xterm"
	cp -v "${PREFIX_PORT_WORKDIR}/xterm" "${PREFIX_PROG}"
}
