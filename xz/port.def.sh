#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xz"
	version="5.4.7"
	desc="XZ Utils LZMA compression — static liblzma"
	cpe23="cpe:2.3:a:tukaani:xz:${version}:*:*:*:*:*:*:*"

	source="https://github.com/tukaani-project/xz/releases/download/v${version}/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="2798247"
	sha256="8db6664c48ca07908b92baedcfe7f3ba23f49ef2476864518ab5db6723836e71"

	license="0BSD"
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
	# Static liblzma.a + <lzma.h> for aarch64-phoenix. Python's _lzma (and any xz
	# consumer) links -llzma. Library only: disable the xz/xzdec/lzmadec/lzmainfo
	# CLIs + nls + threads (Phoenix single-threaded lib use) + docs. Autoconf
	# cross-build, static-only per the framework (libiconv/oniguruma pattern).
	local xcflags="${CFLAGS} -I${PREFIX_H}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" \
			--prefix="${PREFIX_PORT_INSTALL}" --libdir="${PREFIX_A}" --includedir="${PREFIX_H}" \
			--enable-static --disable-shared \
			--disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links \
			--disable-scripts --disable-doc --disable-nls --disable-threads \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${xcflags}" CPPFLAGS="${xcflags}" LDFLAGS="${LDFLAGS} -L${PREFIX_A}")
	fi

	# Build + install only the library subtree (the top-level 'all' would also try
	# the disabled CLIs' plumbing).
	make -C "${PREFIX_PORT_WORKDIR}/src/liblzma"
	make -C "${PREFIX_PORT_WORKDIR}/src/liblzma" install
}
