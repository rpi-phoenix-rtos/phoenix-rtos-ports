: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="grep"
	version="3.11"
	desc="GNU grep (print lines matching a pattern)"
	cpe23="cpe:2.3:a:gnu:grep:${version}:*:*:*:*:*:*:*"
	source="https://ftp.gnu.org/gnu/grep/"
	archive_filename="grep-${version}.tar.xz"
	src_path="grep-${version}/"
	size="1703776"
	sha256="1db2aedde89d0dea42b16d9528f894c8d15dae4e190b59aecc78f5a951276eab"
	license="GPL-3.0-or-later"
	license_file="COPYING"
	conflicts=""
	depends=""
	supports="phoenix>=3.3"
}

# GNU grep via its own autoconf + gnulib -- the same machinery as the coreutils
# port, so the same Phoenix traps apply. Only one of coreutils' four patches has
# any counterpart here, because grep's gnulib slice is much smaller:
#
#   patches/0001 teaches gnulib's __fpending() replacement about Phoenix's FILE
#                struct (coreutils' 0002 does the same, plus freadahead/
#                freading/freadptr/fseterr/freadseek, which grep does not pull
#                in). libphoenix has no __fpending(), so lib/fpending.c is
#                compiled and its per-libc ladder otherwise ends in
#                "#error Please port gnulib fpending.c to your platform!".
#                Implementing __fpending() in libphoenix would retire it.
#
# coreutils' 0001 (gettime/settime rename), 0003 (mini-gmp) and 0004 (stty) have
# no counterpart: grep's gnulib ships no gettime.c/settime.c, no mini-gmp and no
# stty.
#
# config.site is coreutils' cross ac_cv_* answer set (every entry is a true
# statement about libphoenix; configure ignores the keys it never asks about)
# plus one grep-specific line: am_cv_func_iconv=no. AM_ICONV is a real link
# test, and the shared sysroot may or may not already hold the libiconv port's
# libiconv.a depending on which ports were built first -- so letting it be found
# would make grep's link line depend on build order. grep only uses iconv in
# gnulib's propername.c (transliterating translated author names), which
# --disable-nls makes moot.
#
# Notes for whoever touches this next:
#   * grep needs the INCLUDED gnulib regex, not libphoenix's. dfasearch.c calls
#     the GNU API (re_set_syntax/re_compile_pattern/re_search); libphoenix ships
#     a FreeBSD-derived POSIX-only regex, so configure correctly falls back to
#     gnulib's and renames the entry points to rpl_*. Do not "fix" that with a
#     gl_cv_func_re_*_working=yes -- it would link against an API that is not
#     there.
#   * -P / --perl-regexp is OFF (--disable-perl-regexp). grep >= 3.8 requires
#     PCRE2 (src/pcresearch.c includes <pcre2.h>); the in-tree `pcre` port is
#     8.42, i.e. PCRE1, so it cannot satisfy it. Enabling -P means porting PCRE2
#     first; there is nothing to retry with the current ports tree.
#   * egrep/fgrep are NOT binaries. Since grep 3.8 they are generated /bin/sh
#     wrapper scripts (src/egrep.sh -> `exec grep -E "$@"` plus an obsolescence
#     warning on stderr), so they need a working /bin/sh in the image -- today
#     busybox's ash, and bash-as-sh once busybox goes.

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		# -O2 is load-bearing: at -O0, GCC defines __NO_INLINE__, so gnulib's
		# gl_cv_c_inline_effective test fails, HAVE_INLINE stays undefined, and the
		# gnulib extern-inline helpers are neither inlined at call sites nor emitted
		# out-of-line -> undefined references at link.
		(cd "${PREFIX_PORT_WORKDIR}" && CONFIG_SITE="${PREFIX_PORT}/config.site" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			CC="${HOST}-gcc" AR="${HOST}-ar" RANLIB="${HOST}-ranlib" \
			CFLAGS="${CFLAGS} -O2" LDFLAGS="${CFLAGS} ${LDFLAGS} -static" \
			--disable-nls --disable-perl-regexp)
	fi
}

p_build() {
	# A one-binary port fails loudly: no `make -k`, no "at least N tools" check.
	# Top-level make (grep is a recursive automake project: src/ links
	# ../lib/libgreputils.a, which only the top level knows how to build), but
	# NOT `make install` -- install-am installs into PREFIX_PORT_INSTALL, while
	# b_install below is what actually populates the image rootfs.
	make -C "${PREFIX_PORT_WORKDIR}"

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"

	# The binary. Check it really is an aarch64 ELF executable the same way
	# coreutils does, so a host-arch build helper or a leftover script can never
	# be installed as if it were the target binary.
	local f name
	f="${PREFIX_PORT_WORKDIR}/src/grep"
	"${CROSS}readelf" -h "${f}" 2>/dev/null | grep -q 'AArch64' ||
		b_die "grep: src/grep is missing or not an aarch64 ELF executable - build broke"
	cp -a "${f}" "${PREFIX_PROG}/grep"
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/grep" "${PREFIX_PROG}/grep"
	b_install "${PREFIX_PROG_TO_INSTALL}/grep" /usr/bin

	# egrep/fgrep: generated shell wrappers, not binaries (see header). Configure
	# bakes the BUILD host's $SHELL into the shebang, which is meaningless on the
	# target, so rewrite it to /bin/sh.
	for name in egrep fgrep; do
		f="${PREFIX_PORT_WORKDIR}/src/${name}"
		[ -f "${f}" ] || b_die "grep: src/${name} was not generated - build broke"
		sed -e '1s|^#!.*$|#!/bin/sh|' "${f}" >"${PREFIX_PROG}/${name}"
		chmod +x "${PREFIX_PROG}/${name}"
		b_install "${PREFIX_PROG}/${name}" /usr/bin
	done

	echo "grep: installed grep (ELF) + egrep/fgrep (/bin/sh wrappers)"
}
