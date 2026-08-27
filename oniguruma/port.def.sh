#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="oniguruma"
	version="6.9.9"
	desc="Oniguruma regular-expression library (static)"
	cpe23="cpe:2.3:a:oniguruma_project:oniguruma:${version}:*:*:*:*:*:*:*"

	source="https://github.com/kkos/oniguruma/releases/download/v${version}/"
	archive_filename="onig-${version}.tar.gz"
	src_path="onig-${version}/"

	size="957444"
	sha256="60162bd3b9fc6f4886d4c7a07925ffd374167732f55dce8c491bfd9cd818a6cf"

	license="BSD-2-Clause"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# configure + build both live in p_build (CROSS available there, libiconv pattern).
	:
}

p_build() {
	# Static libonig.a + <oniguruma.h> for aarch64-phoenix. jq's regex builtins
	# (test/match/sub/gsub/splits/scan) need this; core jq is unaffected without it.
	# Autoconf cross-build, static-only per the framework (no shared loader on the
	# netboot/SD image path). --disable-shared keeps just the .a; the POSIX regex
	# wrapper (regcomp/regexec) is left enabled (harmless, jq uses the native API).
	local ocflags="${CFLAGS} -I${PREFIX_H}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" \
			--prefix="${PREFIX_PORT_INSTALL}" --libdir="${PREFIX_A}" --includedir="${PREFIX_H}" \
			--enable-static --disable-shared \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${ocflags}" CPPFLAGS="${ocflags}" LDFLAGS="${LDFLAGS} -L${PREFIX_A}")
	fi

	make -C "${PREFIX_PORT_WORKDIR}"
	make -C "${PREFIX_PORT_WORKDIR}" install
}
