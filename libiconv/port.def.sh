#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libiconv"
	version="1.18"
	desc="GNU libiconv — character-set conversion library (static)"
	cpe23="cpe:2.3:a:gnu:libiconv:${version}:*:*:*:*:*:*:*"

	source="https://ftp.gnu.org/gnu/libiconv"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="5822590"
	sha256="3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8"

	license="LGPL-2.1-or-later"
	license_file="COPYING.LIB"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# configure + build both live in p_build (CROSS available there, sdl2 pattern).
	:
}

p_build() {
	# Static libiconv.a + <iconv.h> for aarch64-phoenix. Phoenix libc has no iconv;
	# glib2 / Midnight Commander / Dillo all need the iconv ABI for charset
	# conversion. --disable-nls avoids the gettext/gnulib NLS machinery (Phoenix
	# has no message catalogs); --enable-static/--disable-shared per the framework.
	local icflags="${CFLAGS} -I${PREFIX_H}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" \
			--prefix="${PREFIX_PORT_INSTALL}" --libdir="${PREFIX_A}" --includedir="${PREFIX_H}" \
			--enable-static --disable-shared --disable-nls --disable-rpath \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${icflags}" CPPFLAGS="${icflags}" LDFLAGS="${LDFLAGS} -L${PREFIX_A}")
	fi

	make -C "${PREFIX_PORT_WORKDIR}"
	make -C "${PREFIX_PORT_WORKDIR}" install
}
