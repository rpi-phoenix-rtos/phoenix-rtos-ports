: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="coreutils"
	version="9.5"
	desc="GNU core utilities (basic file, shell and text manipulation)"
	cpe23="cpe:2.3:a:gnu:coreutils:${version}:*:*:*:*:*:*:*"
	source="https://ftp.gnu.org/gnu/coreutils/"
	archive_filename="coreutils-${version}.tar.xz"
	src_path="coreutils-${version}/"
	size="6007136"
	sha256="cd328edeac92f6a665de9f323c93b712af1858bc2e0d88f3f7100469470a1b8a"
	license="GPL-3.0-or-later"
	license_file="COPYING"
	conflicts=""
	depends=""
	supports="phoenix>=3.3"
}

# GNU coreutils via its own autoconf + gnulib. Phoenix patches (patches/):
#   0001 renames gnulib gettime/settime (they clash with libphoenix symbols)
#   0002 teaches gnulib's stdio internals (freadahead/freading/freadptr/fpending/
#        fseterr/freadseek) about Phoenix's FILE struct
#   0003 teaches the bundled mini-gmp.h that Phoenix's <stdio.h> provides FILE
#   0004 gives stty's raw/-raw help printf an empty-string base arg so it does not
#        collapse to a trailing comma when IUCLC/IXANY/IMAXBEL/XCASE are all absent
# config.site supplies the cross ac_cv_* answers: configure otherwise mis-guesses
# many present Phoenix libc functions as "missing/broken" when cross-compiling and
# pulls in gnulib replacements that then fail to build (the load-bearing one is
# ac_cv_func_chown_works=yes, which stops gnulib compiling rpl_chown).
#
# All 104 tools build + link cleanly against libphoenix (zero missing symbols).
# (sort + stat build: libphoenix gained the RLIMIT_* ids sort keys on and a
#  statfs()/<sys/statfs.h> implementation stat needs. factor + expr build via the
#  0003 mini-gmp FILE fix — no external GMP needed. stty builds via 0004, which
#  fixes a coreutils upstream trailing-comma when all four optional termios input
#  flags are absent — Phoenix's termios omits IUCLC/IXANY/IMAXBEL/XCASE.)

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		# -O2 is load-bearing: at -O0, GCC defines __NO_INLINE__, so gnulib's
		# gl_cv_c_inline_effective test fails, HAVE_INLINE stays undefined, and the
		# gnulib extern-inline helpers (mbszero &c.) are neither inlined at call
		# sites nor emitted out-of-line -> undefined references at link.
		(cd "${PREFIX_PORT_WORKDIR}" && CONFIG_SITE="${PREFIX_PORT}/config.site" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			CC="${HOST}-gcc" AR="${HOST}-ar" RANLIB="${HOST}-ranlib" \
			CFLAGS="${CFLAGS} -O2" LDFLAGS="${CFLAGS} ${LDFLAGS} -static" \
			--disable-nls --disable-acl --disable-xattr --without-selinux --disable-libcap)
	fi
}

p_build() {
	local n=0 f name

	# All 104 tools build (stty included — see header: patch 0004 + libphoenix now
	# defines the four termios input flags IUCLC/IXANY/IMAXBEL/XCASE). -k is kept
	# defensively so a future single-tool break still ships the rest; install is
	# from src/ below (not `make install`, whose install-am wants the full `all`).
	#
	# STALENESS, done by the ABI contract rather than by "was it relinked just now".
	# An earlier version of this recipe touched a marker before make and installed
	# only tools newer than it. That conflates two different things, and on
	# 2026-09-04 it broke a build: an incremental pass where make had nothing to do
	# relinked nothing, so ZERO tools were newer than the marker and the count check
	# below killed the build with "only 0 tools built - build broke" although every
	# tool was present and correct.
	#
	# A tool is stale iff it is OLDER THAN THE LIBC IT LINKS -- the same test
	# scripts/check-no-stale-binaries.sh uses, and the one that matters (a binary
	# linked against a previous libphoenix calls renumbered syscalls silently).
	# coreutils' Makefile does not know about libphoenix.a, so a core rebuild leaves
	# the tools untouched and older; delete those and let make relink them. Their
	# objects are still current, so the relink is cheap, and afterwards everything in
	# src/ is installable.
	local libc_ref="${PREFIX_SYSROOT}/lib/libphoenix.a" dropped=0
	if [ -f "${libc_ref}" ]; then
		for f in "${PREFIX_PORT_WORKDIR}/src/"*; do
			[ -f "${f}" ] || continue
			case "${f}" in *.o | *.a | *.so | *.c | *.h | *.py | *.sh | *.x) continue ;; esac
			"${CROSS}readelf" -h "${f}" 2>/dev/null | grep -q 'AArch64' || continue
			if [ "${libc_ref}" -nt "${f}" ]; then
				rm -f "${f}"
				dropped=$((dropped + 1))
			fi
		done
		[ "${dropped}" -eq 0 ] ||
			echo "coreutils: dropped ${dropped} tool(s) older than libphoenix.a; relinking"
	fi
	rm -f "${PREFIX_PORT_WORKDIR}/.build-start"

	make -k -C "${PREFIX_PORT_WORKDIR}" || true

	# Install the built tools straight from src/. Filter to aarch64 ELF executables:
	# that skips the .o/.a/scripts and the host-arch build helpers (make-prime-list).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	for f in "${PREFIX_PORT_WORKDIR}/src/"*; do
		[ -f "${f}" ] || continue
		case "${f}" in *.o | *.a | *.so | *.c | *.h | *.py | *.sh | *.x) continue ;; esac
		"${CROSS}readelf" -h "${f}" 2>/dev/null | grep -q 'AArch64' || continue
		name="$(basename "${f}")"
		cp -a "${f}" "${PREFIX_PROG}/${name}"
		${STRIP} -o "${PREFIX_PROG_STRIPPED}/${name}" "${PREFIX_PROG}/${name}"
		b_install "${PREFIX_PROG_TO_INSTALL}/${name}" /usr/bin
		n=$((n + 1))
	done

	echo "coreutils: installed ${n} tools (full set, stty included)"
	[ "${n}" -ge 100 ] || b_die "coreutils: only ${n} tools built (expected ~105) - build broke"
}
