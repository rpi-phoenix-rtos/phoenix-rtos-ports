#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="zlib"
	version="1.3.1"
	desc="General purpose compression library"
	cpe23="cpe:2.3:a:zlib:zlib:${version}:*:*:*:*:*:*:*"

	source="https://zlib.net/fossils/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1512791"
	sha256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

	license="Zlib"
	license_file="zlib.h"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# changing LDFLAGS from "ld" params format to "gcc" params - prefixing with -Wl, and changing spaces to colons
	LDFLAGS="${CFLAGS} $LDFLAGS"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/build/Makefile" ]; then
		mkdir -p "${PREFIX_PORT_WORKDIR}/build"
		(cd "${PREFIX_PORT_WORKDIR}/build" && cmake -DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" -DCMAKE_BUILD_TYPE=Release -DZLIB_BUILD_EXAMPLES=OFF -DSKIP_INSTALL_MAN=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)
}
