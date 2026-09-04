#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="mc"
	version="4.8.31"
	desc="GNU Midnight Commander — console file manager (static, ncurses screen)"
	cpe23="cpe:2.3:a:midnight_commander:midnight_commander:${version}:*:*:*:*:*:*:*"

	source="https://ftp.midnight-commander.org"
	archive_filename="${name}-${version}.tar.xz"
	src_path="${name}-${version}/"

	size="2385632"
	sha256="24191cf8667675b8e31fc4a9d18a0a65bdc0598c2c5c4ea092494cd13ab4ab1a"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	conflicts=""
	# glib2 supplies libglib-2.0.a + libgmodule-2.0.a + libresolv.a (stub) and the
	# glib-2.0 headers (+ glibconfig.h under PREFIX_A/glib-2.0/include); it also
	# transitively pulls libiconv/libffi/zlib. ncurses supplies libncurses.a +
	# headers. libiconv is listed explicitly because mc's link closure needs
	# -liconv directly. All land in the shared PREFIX_H/PREFIX_A.
	depends="ncurses glib2 libiconv"

	supports="phoenix>=3.3"
}

# mc 4.8.31 is autotools. Phoenix gaps and how they are bridged:
#   --without-subshell        sidesteps the grantpt/ptsname pty dependency
#   --with-screen=ncurses     ncurses is ported; slang is not
#   NCURSES_WIDECHAR=0         our ncurses is NARROW; forces mc off the widec
#                              getcchar/setcchar path (only loss: dialog shadows)
#   (no mc-support stub any more)  Phoenix used to lack getmntent and
#                              nl_langinfo, so this port staged glibc-shaped
#                              headers into the SHARED PREFIX_H and linked a stub
#                              libmcsupport.a. libphoenix implements both now, so
#                              the stub is gone -- see p_prepare().
#   mc.cache                  cross AC_TRY_RUN answers (getmntent method, realpath,
#                              mktime) configure cannot probe when cross-compiling
#   fake-pkg-config.sh        answers configure's glib/gmodule pkg-config queries
#                              from the GLIB_CFLAGS/GLIB_LIBS env (no pkg-config DB)

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# mc 4.8.31 ships a config.sub/guess that predates the `phoenix` triplet. Copy a
	# phoenix-aware pair from an already-extracted dependency under port-sources
	# (ncurses/glib2 are deps, extracted first). Same idiom as the glib2/nano ports.
	local psrc donor
	psrc="$(dirname "${PREFIX_PORT_BUILD}")"
	donor="$(grep -l phoenix "${psrc}"/*/*/config.sub 2>/dev/null | grep -v '/mc-' | head -1)"
	if [ -n "${donor}" ]; then
		cp "${donor}" "${PREFIX_PORT_WORKDIR}/config.sub"
		[ -f "$(dirname "${donor}")/config.guess" ] && \
			cp "$(dirname "${donor}")/config.guess" "${PREFIX_PORT_WORKDIR}/config.guess"
	fi

	# The mc-support stub is retired: libphoenix implements the whole mntent family
	# (setmntent/getmntent/getmntent_r/addmntent/endmntent/hasmntopt) and
	# nl_langinfo, so mc's mountlist.c and strutil.c build against the real libc.
	#
	# Retiring it fixes two bugs beyond the obvious dead weight:
	#  - It staged its own <mntent.h> and <langinfo.h> into the SHARED PREFIX_H,
	#    i.e. one port mutated headers every other port sees. Since libphoenix's
	#    real <mntent.h> declares hasmntopt and the stub's did not, a consumer that
	#    picked up the stub copy got a configure/compile mismatch -- exactly the
	#    failure that broke tools/ports/build-mc.sh (coord 8d722cb3a).
	#  - mc now goes through real plumbing instead of a hardcoded answer: it
	#    setmntent()s MOUNTED ("/etc/mtab"). Be precise about the payoff -- Phoenix
	#    does not maintain /etc/mtab by default and libphoenix's setmntent is a
	#    plain fopen, so TODAY mc still lists no mounts. The difference is that it
	#    will list them the moment anything writes that file, whereas the stub
	#    could never report a mount at all.
	#
	# Note the deliberate behaviour change: the stub answered CODESET="UTF-8",
	# which sent mc down str_utf8_init(). libphoenix answers "ANSI_X3.4-1968"
	# on purpose (locale/langinfo.c) because its multibyte layer maps bytes 1:1
	# with no UTF-8 decoder -- so mc takes the 8-bit str_ascii_init() path, which
	# is the correct one for this libc and matches what ncurses does. Needs an
	# on-hardware look at box-drawing before the framework port replaces the
	# tools/ one.

	# Pre-seeded autoconf cache (configure loads it via --cache-file=mc.cache).
	cp "${PREFIX_PORT}/mc.cache" "${PREFIX_PORT_WORKDIR}/mc.cache"
}

p_build() {
	# glib headers: PREFIX_H/glib-2.0 + glibconfig.h under PREFIX_A/glib-2.0/include
	# (the glib2 port's install layout).
	local ginc="-I${PREFIX_H}/glib-2.0 -I${PREFIX_A}/glib-2.0/include"
	local gliblibs="-L${PREFIX_A} -lglib-2.0 -lgmodule-2.0 -lpthread -liconv -lm"

	# Force NCURSES_WIDECHAR=0 (narrow ncurses), force-include the shim (P_tmpdir),
	# and add the glib + ncurses include dirs. -static is load-bearing.
	local xcflags="${CFLAGS} -O2 -DNCURSES_WIDECHAR=0 ${ginc} -I${PREFIX_H} -I${PREFIX_H}/ncurses -include ${PREFIX_PORT}/mc-phoenix-shim.h"
	local xldflags="${LDFLAGS} -static -L${PREFIX_A}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			--datadir=/usr/share --sysconfdir=/etc \
			--cache-file=mc.cache \
			--with-screen=ncurses \
			--with-ncurses-includes="${PREFIX_H}" \
			--with-ncurses-libs="${PREFIX_A}" \
			--without-subshell --without-x --without-gpm --disable-nls \
			--disable-vfs-undelfs --disable-vfs-sftp --disable-vfs-ftp \
			--disable-doxygen-doc \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${xcflags}" CPPFLAGS="${CFLAGS} ${ginc}" \
			LDFLAGS="${xldflags}" \
			PKG_CONFIG="${PREFIX_PORT}/fake-pkg-config.sh" \
			GLIB_LIBDIR="${PREFIX_A}" \
			GLIB_CFLAGS="${ginc}" GLIB_LIBS="${gliblibs}" \
			GMODULE_CFLAGS="${ginc}" GMODULE_LIBS="${gliblibs}")
	fi

	make -C "${PREFIX_PORT_WORKDIR}"

	[ -x "${PREFIX_PORT_WORKDIR}/src/mc" ] || b_die "src/mc not produced"

	mkdir -p "${PREFIX_PROG_STRIPPED}"
	$STRIP -o "${PREFIX_PROG_STRIPPED}/mc" "${PREFIX_PORT_WORKDIR}/src/mc"
	b_install "${PREFIX_PROG_TO_INSTALL}/mc" /bin

	# Runtime share data: without the on-disk default skin mc falls back to a
	# monochrome built-in ("Unable to load 'default' skin"). Stage skins + editor
	# syntax to the compiled-in datadir (/usr/share/mc). Best-effort, non-fatal.
	if [ -n "${PREFIX_ROOTFS:-}" ]; then
		local share="${PREFIX_ROOTFS}/usr/share/mc"
		mkdir -p "${share}/skins" "${share}/syntax" 2>/dev/null || true
		cp -a "${PREFIX_PORT_WORKDIR}"/misc/skins/*.ini "${share}/skins/" 2>/dev/null || true
		cp -a "${PREFIX_PORT_WORKDIR}"/misc/syntax/*.syntax "${PREFIX_PORT_WORKDIR}"/misc/syntax/Syntax "${share}/syntax/" 2>/dev/null || true
		cp -a "${PREFIX_PORT_WORKDIR}"/misc/mc.ext.ini "${share}/" 2>/dev/null || true
	fi
}
