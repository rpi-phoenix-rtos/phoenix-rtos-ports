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

# GNU coreutils via its own autoconf + gnulib. Two Phoenix patches (patches/):
#   0001 renames gnulib gettime/settime (they clash with libphoenix symbols)
#   0002 teaches gnulib's stdio internals (freadahead/freading/freadptr/fpending/
#        fseterr/freadseek) about Phoenix's FILE struct
# config.site supplies the cross ac_cv_* answers: configure otherwise mis-guesses
# many present Phoenix libc functions as "missing/broken" when cross-compiling and
# pulls in gnulib replacements that then fail to build (the load-bearing one is
# ac_cv_func_chown_works=yes, which stops gnulib compiling rpl_chown).
#
# 99 of 104 tools build + link cleanly against libphoenix (zero missing symbols).
# 5 are skipped pending header/macro work or an external lib, so `make -k` builds
# the rest and we install whatever built:
#   stat  - no <sys/vfs.h>/struct statfs in Phoenix
#   sort  - RLIMIT_* macros absent from <sys/resource.h>
#   stty  - missing termios flag macros
#   factor, expr - need GMP (external library, not ported)

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/Makefile" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && CONFIG_SITE="${PREFIX_PORT}/config.site" ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu --prefix="${PREFIX_PORT_INSTALL}" \
			CC="${HOST}-gcc" AR="${HOST}-ar" RANLIB="${HOST}-ranlib" \
			CFLAGS="${CFLAGS}" LDFLAGS="${CFLAGS} ${LDFLAGS} -static" \
			--disable-nls --disable-acl --disable-xattr --without-selinux --disable-libcap)
	fi
}

p_build() {
	local n=0 f name

	# -k: 5 tools (see header) fail to compile on header/macro gaps or need GMP;
	# keep going and build+install the other 99.
	make -k -C "${PREFIX_PORT_WORKDIR}" || true
	make -k -C "${PREFIX_PORT_WORKDIR}" install || true

	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	for f in "${PREFIX_PORT_INSTALL}/bin/"*; do
		[ -f "${f}" ] || continue
		name="$(basename "${f}")"
		cp -a "${f}" "${PREFIX_PROG}/${name}"
		${STRIP} -o "${PREFIX_PROG_STRIPPED}/${name}" "${PREFIX_PROG}/${name}"
		b_install "${PREFIX_PROG_TO_INSTALL}/${name}" /usr/bin
		n=$((n + 1))
	done

	echo "coreutils: installed ${n} tools (stat/sort/stty/factor/expr skipped - see port.def.sh)"
	[ "${n}" -ge 80 ] || b_die "coreutils: only ${n} tools built (expected ~99) - build broke"
}
