#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="bash"
	version="5.2.21"
	desc="GNU Bourne-Again SHell"
	cpe23="cpe:2.3:a:gnu:bash:${version}:*:*:*:*:*:*:*"

	source="https://ftp.gnu.org/gnu/bash/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="10952391"
	sha256="c8e31bdc59b69aaffc5b36509905ba3e5cbb12747091d27b4b977f078560d5b8"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	iuse=""
	depends=""
	conflicts=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"

	if [ ! -f "$PREFIX_PORT_WORKDIR/config.h" ]; then
		# Cross config.cache carries the run-test answers autoconf cannot compute
		# for the target, plus the HAVE_* forces that make bash use libphoenix's
		# getenv/setenv/putenv/unsetenv, strtoimax and wcswidth instead of its own
		# lib/sh fallbacks. Without these the static link hits multiple-definition:
		#   - bash_cv_getenv_redef=no  -> CAN_REDEFINE_GETENV undefined (drops lib/sh/getenv.c)
		#   - ac_cv_func_strtoimax=yes -> HAVE_STRTOIMAX (drops lib/sh/strtoimax.o body)
		#   - ac_cv_func_wcswidth=yes  -> HAVE_WCSWIDTH (drops lib/sh/wcswidth.c)
		cp -a "$PREFIX_PORT/config.cache" "$PREFIX_PORT_WORKDIR/config.cache"

		# -fcommon: termcap's PC/UP/BC/ospeed are tentative-definition globals
		# shared across translation units; gcc>=10 defaults to -fno-common, which
		# would turn them into multiple-definition link errors.
		# shellcheck disable=2153 # CFLAGS, LDFLAGS are externally provided
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --prefix="${PREFIX_PORT_INSTALL}" \
			--cache-file=config.cache CC="${HOST}-gcc" \
			CFLAGS="${CFLAGS} -fcommon -Wno-error=implicit-function-declaration -Wno-error=implicit-int" LDFLAGS="${CFLAGS} ${LDFLAGS} -Wl,--allow-multiple-definition" \
			--without-bash-malloc --enable-static-link --disable-nls)
	fi
}

p_build() {
	# bash compiles host build-tools (mkbuiltins) with the build compiler
	# (modern gcc, strict). Those tools are pre-ANSI K&R, so relax the build-tool
	# flags to gnu89 and demote implicit-declaration/int to warnings.
	make -C "${PREFIX_PORT_WORKDIR}" \
		CFLAGS_FOR_BUILD="-g -DCROSS_COMPILING -std=gnu89 -Wno-error=implicit-function-declaration -Wno-error=implicit-int"

	$STRIP -o "$PREFIX_PROG_STRIPPED/bash" "$PREFIX_PORT_WORKDIR/bash"
	cp -a "$PREFIX_PORT_WORKDIR/bash" "$PREFIX_PROG/bash"

	b_install "$PREFIX_PROG_TO_INSTALL/bash" /bin
}
