#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="gzip"
	version="1.15"
	desc="GNU gzip (DEFLATE compressor: gzip/gunzip/zcat)"
	cpe23="cpe:2.3:a:gnu:gzip:${version}:*:*:*:*:*:*:*"

	source="https://ftp.gnu.org/gnu/gzip/"
	archive_filename="${name}-${version}.tar.xz"
	src_path="${name}-${version}/"

	size="808900"
	sha256="9aa0cc780dec156b8282844833b342ab7cb08c25d2cd9a1869cdd0df31deff48"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

# GNU gzip via its own autoconf + gnulib. 1.15 is the newest stable (it was
# published on 2026-09-20, the day this port was written, and fixes a wrong-file
# removal race, a use of uninitialized memory and two .lzh decoder overflows).
# Should its freshness ever be the suspect, 1.13 (2023-08-19) is a drop-in
# fallback for this recipe -- same gnulib surface, both patches apply with the
# same hunks:
#
#     version="1.13"  size="838248"
#     sha256="7454eb6935db17c6655576c2e1b0fabefd38b4d0936e0f87f48cd062ce91a057"
#
# The port replaces busybox's 2017 gzip/gunzip/zcat in the base system, and GNU
# tar shells out to the `gzip` binary installed here (/usr/bin, same directory
# as the coreutils tools).
#
# Phoenix specifics:
#
#   patches/0001 renames gnulib's gettime()/settime() -- libphoenix declares
#                unrelated functions of those names, with different signatures,
#                in <sys/time.h> (same fix as the coreutils port's patch 0001).
#
#   config.site  supplies the cross ac_cv_*/gl_cv_* answers. Inherited from the
#                coreutils recipe, where every entry was validated against this
#                same libphoenix: without them configure mis-guesses present
#                Phoenix libc functions as missing or broken (an AC_RUN_IFELSE
#                cannot run when cross-compiling, so it takes its failure
#                branch) and pulls in gnulib replacements that then fail to
#                build or silently shadow a working libc.
#
#   -O2          load-bearing: at -O0 GCC defines __NO_INLINE__, gnulib's
#                gl_cv_c_inline_effective test fails, HAVE_INLINE stays
#                undefined and gnulib's extern-inline helpers are neither
#                inlined at the call sites nor emitted out of line -> undefined
#                references at link.
#
#   -DGNU_STANDARD=0
#                turns gzip's argv[0] dispatch back on (gzip.c disables it by
#                default), so a copy of the binary named `gunzip` decompresses
#                and one named `zcat` decompresses to stdout. Upstream ships
#                gunzip/zcat as /bin/sh wrappers around gzip; those are useless
#                here -- busybox's sh is going away, and the `.in` -> script
#                rule bakes the BUILD host's $(SHELL) path into the shebang.
#                So we install three copies of the one binary instead, and no
#                shell scripts at all (zgrep/zdiff/zless/gzexe/znew/zforce/
#                zcmp/zmore are deliberately not shipped).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	# Guarded on Makefile (not config.status) so that
	# b_port_invalidate_stale_configure -- which removes Makefile whenever the
	# libphoenix API fingerprint changes -- actually forces a re-configure.
	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && CONFIG_SITE="${PREFIX_PORT}/config.site" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			CC="${HOST}-gcc" AR="${HOST}-ar" RANLIB="${HOST}-ranlib" \
			CFLAGS="${CFLAGS} -O2 -DGNU_STANDARD=0" LDFLAGS="${CFLAGS} ${LDFLAGS} -static" \
			--disable-nls)
	fi
}

p_build() {
	local bin="${PREFIX_PORT_WORKDIR}/gzip" name

	# A binary OLDER THAN THE LIBC IT LINKS is stale even when make has nothing
	# to do (gzip's Makefile does not know about libphoenix.a, so a core rebuild
	# leaves it untouched and older). Same test as
	# scripts/check-no-stale-binaries.sh; deleting it makes make relink.
	local libc_ref="${PREFIX_SYSROOT}/lib/libphoenix.a"
	if [ -f "${libc_ref}" ] && [ -f "${bin}" ] && [ "${libc_ref}" -nt "${bin}" ]; then
		echo "gzip: binary older than libphoenix.a; relinking"
		rm -f "${bin}"
	fi

	# Only the library and the program: the top-level `all` also recurses into
	# doc/ and tests/, neither of which produces anything we install. lib must
	# come first -- automake lists lib/libgzip.a as a prerequisite of gzip but
	# generates no rule to build it from the top Makefile. version.c/version.h
	# are BUILT_SOURCES, which a targeted `make gzip` does not trigger.
	make -C "${PREFIX_PORT_WORKDIR}/lib"
	make -C "${PREFIX_PORT_WORKDIR}" version.c version.h gzip

	"${CROSS}readelf" -h "${bin}" | grep -q 'AArch64' ||
		b_die "gzip: built binary is not an AArch64 executable"

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	cp -a "${bin}" "${PREFIX_PROG}/gzip"
	${STRIP} -o "${PREFIX_PROG_STRIPPED}/gzip" "${PREFIX_PROG}/gzip"

	# gunzip/zcat are the same binary under another name -- see the header note
	# on -DGNU_STANDARD=0. b_install keeps the basename, so the copies have to
	# carry the final names already.
	for name in gunzip zcat; do
		cp -a "${PREFIX_PROG}/gzip" "${PREFIX_PROG}/${name}"
		cp -a "${PREFIX_PROG_STRIPPED}/gzip" "${PREFIX_PROG_STRIPPED}/${name}"
	done

	for name in gzip gunzip zcat; do
		b_install "${PREFIX_PROG_TO_INSTALL}/${name}" /usr/bin
	done
}
