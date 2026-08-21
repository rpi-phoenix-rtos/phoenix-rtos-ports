#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libpng"
	version="1.6.40"
	desc="Official PNG reference library (static)"
	cpe23="cpe:2.3:a:libpng:libpng:${version}:*:*:*:*:*:*:*"

	source="https://download.sourceforge.net/libpng/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1507931"
	sha256="8f720b363aa08683c9bf2a563236f45313af2c55d542b5481ae17dd8d183bb42"

	license="libpng-2.0"
	license_file="LICENSE"

	conflicts=""
	depends="zlib"

	supports="phoenix>=3.3"
}

p_prepare() {
	if [ ! -f "$PREFIX_PORT_WORKDIR/Makefile" ]; then
		# libpng finds zlib via the framework install prefix (PREFIX_H/PREFIX_A):
		# CPPFLAGS carries -I because libpng's pnglibconf preprocessing rules use
		# $(CPPFLAGS), not $(CFLAGS), for the zlib.h include.
		(cd "$PREFIX_PORT_WORKDIR" && "./configure" --host="${HOST}" \
			--prefix="$PREFIX_PORT_INSTALL" --libdir="$PREFIX_A" --includedir="$PREFIX_H" \
			--disable-shared --enable-static \
			CPPFLAGS="${CFLAGS} -I${PREFIX_H}" CFLAGS="${CFLAGS} -I${PREFIX_H}" \
			LDFLAGS="${LDFLAGS} -L${PREFIX_A}")
	fi
}

p_build() {
	make -C "$PREFIX_PORT_WORKDIR" install
}
