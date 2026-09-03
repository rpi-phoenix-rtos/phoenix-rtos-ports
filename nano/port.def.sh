#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="nano"
	version="9.2"
	desc="GNU nano — small console text editor (static, links the ncurses port)"
	cpe23="cpe:2.3:a:gnu:nano:${version}:*:*:*:*:*:*:*"

	source="https://www.nano-editor.org/dist/v9"
	archive_filename="${name}-${version}.tar.xz"
	src_path="${name}-${version}/"

	size="1760684"
	sha256="05ecb99247b782e8a5b3a25ed4101dd034b0236902f7449bc9795b717642f7e9"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	conflicts=""
	depends="ncurses"

	supports="phoenix>=3.3"
}

# Was pinned to 2.2.6 (2010) on the grounds that 2.2.x bundles NO gnulib and so
# avoids the gnulib-vs-libphoenix namespace collisions. That rationale is
# OBSOLETE: the coreutils port later shipped a fully gnulib-based program on
# Phoenix by renaming the colliding functions inside gnulib's own sources, and
# the same technique carries nano. Measured collision surface in 9.2 is exactly
# TWO symbols, both from the `gettime` module:
#
#   libphoenix  int  gettime(time_t *raw, time_t *offs)     (sys/time.h:34)
#   gnulib      void gettime(struct timespec *)             (lib/timespec.h:93)
#   libphoenix  int  settime(time_t offs)                   (sys/time.h:37)
#   gnulib      int  settime(struct timespec const *)        (lib/timespec.h:94)
#
# Same names, different types, both visible => conflicting declarations. Note a
# -D rename CANNOT fix this: -Dgettime=gl_gettime rewrites libphoenix's
# declaration too and reproduces the clash under the new name. So patch 0001
# renames the five gnulib sites (timespec.h decls, gettime.c definition + its
# use in current_timespec, two utimens.c calls), exactly as coreutils does.
#
# `getprogname` is NOT a collision any more: libphoenix declares it with
# gnulib's own signature (stdlib.h:126), so that module self-disables.
#
# Patch 0002 is unrelated to collisions and is required regardless: gnulib's
# lib/fseterr.c is a hard `#error` for unknown platforms, and it is compiled
# whenever ac_cv_func___fseterr=no — i.e. on Phoenix. It adds a Phoenix branch
# writing libphoenix's FILE error bit directly (F_ERROR == 1<<3), which is what
# every other platform branch in that file does.
#
# Dropped along with the version bump:
#   - the `bool edit_refresh_needed = NULL` patch: that variable no longer
#     exists in 9.2.
#   - the config.sub/config.guess donor-copy: 9.2's own config.sub knows
#     `phoenix*`, so the aarch64-phoenix triplet is accepted as shipped.
#   - nano-phoenix-shim.h: it existed only for P_tmpdir, which libphoenix now
#     defines (stdio.h:50). Keeping the force-include would be actively harmful
#     — it pulls <pwd.h> ahead of config.h, and gnulib headers #error on that.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# ncurses (headers + libncurses.a) live in the shared PREFIX_H/PREFIX_A. The
	# ac_cv_lib_* answers steer nano at the NARROW ncurses (no ncursesw).
	# -static is load-bearing: the deliverable is a static aarch64-phoenix ELF
	# with zero undefined symbols.
	local xcflags="${CFLAGS} -O2 -I${PREFIX_H} -I${PREFIX_H}/ncurses"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		# NCURSES_CFLAGS/NCURSES_LIBS are preset deliberately. nano 9.x tries
		# PKG_CHECK_MODULES([NCURSES],[ncurses]) FIRST, and on a dev host that
		# query succeeds against the HOST's ncurses and silently injects host
		# include/lib paths into a cross build. Presetting both makes the macro
		# skip the pkg-config query entirely.
		#
		# --disable-libmagic: the libmagic block is on-unless-disabled and its
		# AC_CHECK_LIB(z, inflate) would pull the sysroot zlib in as an
		# undeclared dependency. --disable-maintainer-mode: 9.2 ships
		# AM_MAINTAINER_MODE([enable]), and we must never re-run autotools here.
		(cd "${PREFIX_PORT_WORKDIR}" && CONFIG_SITE="${PREFIX_PORT}/config.site" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu \
			--disable-nls --disable-utf8 --disable-libmagic --disable-maintainer-mode \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CPPFLAGS="${CFLAGS} -I${PREFIX_H} -I${PREFIX_H}/ncurses" \
			CFLAGS="${xcflags}" \
			LDFLAGS="${LDFLAGS} -static -L${PREFIX_A}" LIBS="-lncurses" \
			NCURSES_CFLAGS="-I${PREFIX_H}/ncurses" NCURSES_LIBS="-L${PREFIX_A} -lncurses" \
			ac_cv_lib_ncursesw_initscr=no ac_cv_lib_ncurses_initscr=yes)
	fi

	make -C "${PREFIX_PORT_WORKDIR}" CFLAGS="${xcflags}"

	[ -x "${PREFIX_PORT_WORKDIR}/src/nano" ] || b_die "src/nano not produced"

	mkdir -p "${PREFIX_PROG_STRIPPED}"
	$STRIP -o "${PREFIX_PROG_STRIPPED}/nano" "${PREFIX_PORT_WORKDIR}/src/nano"
	b_install "${PREFIX_PROG_TO_INSTALL}/nano" /bin
}
