: # SPDX-License-Identifier: BSD-3-Clause
{
	ports_api=1
	name="tar"
	version="1.35"
	desc="GNU tar archiving utility"
	cpe23="cpe:2.3:a:gnu:tar:${version}:*:*:*:*:*:*:*"
	source="https://ftp.gnu.org/gnu/tar/"
	archive_filename="tar-${version}.tar.xz"
	src_path="tar-${version}/"
	size="2317208"
	sha256="4d62ff37342ec7aed748535323930c7cf94acf71c3591882b26a7ea50f3edc16"
	license="GPL-3.0-or-later"
	license_file="COPYING"
	conflicts=""
	depends="libiconv"
	supports="phoenix>=3.3"
}

# GNU tar via its own autoconf + gnulib (the same gnulib coreutils uses, so the
# Phoenix traps are the same ones). Patches (patches/):
#   0001 renames gnulib gettime/settime to gl_gettime/gl_settime -- libphoenix's
#        <sys/time.h> declares an incompatible gettime()/settime() pair, so a TU
#        including both headers fails to compile (and would clash at link)
#   0002 teaches gnulib's __fpending() replacement about Phoenix's FILE struct;
#        libphoenix has no __fpending, and gnulib's #if ladder over the known libc
#        layouts ends in "#error Please port gnulib fpending.c to your platform"
#   0003 appends $(LIBICONV) to tar's LDADD -- src/utf8.c calls iconv()/iconv_open()
#        but upstream never links the library (on glibc iconv is in libc); Phoenix
#        gets it from the libiconv port, hence depends="libiconv"
#   0004 rejects --checkpoint-action=wait at parse time: libphoenix DECLARES
#        sigwait() in <signal.h> but does not define it, so linking fails. Drop
#        this patch once libphoenix implements sigwait().
#
# config.site supplies the cross ac_cv_* answers (shared with the coreutils port):
# when cross-compiling, configure mis-guesses many present Phoenix libc functions
# as missing/broken and pulls in gnulib replacements that then fail to build.
#
# Compression is NOT linked in: GNU tar shells out to external programs for
# -z/-j/-J/--lzma/... The program names are compile-time constants (gzip, bzip2,
# xz, lzma, lzip, lzop, zstd, compress), each overridable with --with-<name>=PROG.
# There are two different exec paths, and they do not agree:
#   * creating  (sys_child_open_for_compress) -> execv("/bin/sh", {"-c", prog})
#     i.e. it needs a HARDCODED /bin/sh that can run a one-word command line
#   * extracting (run_decompress_program)     -> execvp(prog, {prog, "-d"})
#     i.e. a bare name resolved through PATH, no shell involved
# Uncompressed archives (plain tar) need neither.
#
# --with-rmt takes an absolute path, not "no": `--without-rmt` is an explicit
# configure error ("Invalid argument to --with-rmt"). Pointing it at a path that
# does not exist on the image keeps the remote-tape code inert and stops tar's
# own rmt(8) from being built.

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
			--disable-nls --disable-acl --without-posix-acls --without-xattrs \
			--without-selinux --with-rmt=/usr/sbin/rmt)
	fi
}

p_build() {
	local f name n=0

	# STALENESS, by the ABI contract: a binary older than the libphoenix it links
	# calls renumbered syscalls silently. tar's Makefile does not know about
	# libphoenix.a, so a core rebuild leaves src/tar untouched and older; drop it
	# and let make relink (objects stay current, so the relink is cheap).
	local libc_ref="${PREFIX_SYSROOT}/lib/libphoenix.a"
	if [ -f "${libc_ref}" ] && [ -f "${PREFIX_PORT_WORKDIR}/src/tar" ] &&
		[ "${libc_ref}" -nt "${PREFIX_PORT_WORKDIR}/src/tar" ]; then
		echo "tar: src/tar is older than libphoenix.a; relinking"
		rm -f "${PREFIX_PORT_WORKDIR}/src/tar"
	fi

	make -C "${PREFIX_PORT_WORKDIR}"

	# Install from src/ rather than `make install` (whose install-am wants docs,
	# info pages and the po/ tree). Filter to aarch64 ELF executables, as coreutils
	# does, so host build helpers and scripts are skipped.
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

	[ -x "${PREFIX_PROG}/tar" ] || b_die "tar: src/tar was not built"
	echo "tar: installed ${n} binary/binaries"
}
