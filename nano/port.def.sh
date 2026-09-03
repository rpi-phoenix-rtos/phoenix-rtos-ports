#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="nano"
	version="2.2.6"
	desc="GNU nano — small console text editor (static, links the ncurses port)"
	cpe23="cpe:2.3:a:gnu:nano:${version}:*:*:*:*:*:*:*"

	source="https://www.nano-editor.org/dist/v2.2"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1572388"
	sha256="be68e133b5e81df41873d32c517b3e5950770c00fc5f4dd23810cd635abce67a"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	conflicts=""
	depends="ncurses"

	supports="phoenix>=3.3"
}

# patches/0001-nano-bool-init-not-null.patch fixes `bool edit_refresh_needed =
# NULL;` (global.c). nano 2.2.6 predates C99 <stdbool.h> being the norm, and its
# `bool` here resolves to ncurses' NCURSES_BOOL (an int), so a NULL initializer
# is an int-from-pointer conversion. That was a warning for a decade; GCC 14+
# makes -Wint-conversion an error, so the build stops on it. Upstream nano
# corrected this in later releases.
#
# nano 2.2.x is used deliberately: it bundles NO gnulib, so it sidesteps the
# gnulib-vs-Phoenix namespace collisions (gettime/getprogname/...) that block the
# modern (6.x) nano. The only Phoenix gaps are P_tmpdir + the passwd-enumeration
# API — the former is supplied by the force-included nano-phoenix-shim.h, the
# latter by libphoenix's own getpwent/setpwent/endpwent.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# nano 2.2.6 (2010) ships a config.sub/guess that predates the `phoenix`
	# triplet, so ./configure would reject `aarch64-phoenix`. Copy a phoenix-aware
	# pair from an already-extracted dependency under port-sources (ncurses is a
	# dep, built + extracted first). Same donor-copy idiom as the glib2 port.
	local psrc donor
	psrc="$(dirname "${PREFIX_PORT_BUILD}")"
	donor="$(grep -l phoenix "${psrc}"/*/*/config.sub 2>/dev/null | grep -v nano | head -1)"
	if [ -n "${donor}" ]; then
		cp "${donor}" "${PREFIX_PORT_WORKDIR}/config.sub"
		[ -f "$(dirname "${donor}")/config.guess" ] && \
			cp "$(dirname "${donor}")/config.guess" "${PREFIX_PORT_WORKDIR}/config.guess"
	fi
}

p_build() {
	# ncurses (headers + libncurses.a) live in the shared PREFIX_H/PREFIX_A. Force
	# -include the shim (P_tmpdir). ac_cv_lib_* answers steer nano's configure at
	# the NARROW ncurses (no ncursesw). -static is load-bearing: the deliverable is
	# a static aarch64-phoenix ELF with zero undefined symbols.
	local xcflags="${CFLAGS} -O2 -I${PREFIX_H} -I${PREFIX_H}/ncurses -include ${PREFIX_PORT}/nano-phoenix-shim.h"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --disable-nls --disable-utf8 \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CPPFLAGS="${CFLAGS} -I${PREFIX_H} -I${PREFIX_H}/ncurses" \
			CFLAGS="${xcflags}" \
			LDFLAGS="${LDFLAGS} -static -L${PREFIX_A}" LIBS="-lncurses" \
			ac_cv_lib_ncursesw_initscr=no ac_cv_lib_ncurses_initscr=yes)
	fi

	make -C "${PREFIX_PORT_WORKDIR}" CFLAGS="${xcflags}"

	[ -x "${PREFIX_PORT_WORKDIR}/src/nano" ] || b_die "src/nano not produced"

	mkdir -p "${PREFIX_PROG_STRIPPED}"
	$STRIP -o "${PREFIX_PROG_STRIPPED}/nano" "${PREFIX_PORT_WORKDIR}/src/nano"
	b_install "${PREFIX_PROG_TO_INSTALL}/nano" /bin
}
