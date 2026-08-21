#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libffi"
	version="3.4.6"
	desc="Foreign Function Interface library (static) — required by glib2/gobject"
	cpe23="cpe:2.3:a:libffi_project:libffi:${version}:*:*:*:*:*:*:*"

	source="https://github.com/libffi/libffi/releases/download/v${version}"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1391684"
	sha256="b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e"

	license="MIT"
	license_file="LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# configure + build live in p_build (CROSS available there, sdl2 pattern).
	:
}

p_build() {
	# Static libffi.a + ffi.h for aarch64-phoenix. glib-2.56's configure
	# HARD-requires libffi even for a core build (gobject's gclosure marshalling).
	# --disable-multi-os-directory keeps the lib out of a $libdir/<triplet>/ subdir.
	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu \
			--prefix="${PREFIX_PORT_INSTALL}" --libdir="${PREFIX_A}" --includedir="${PREFIX_H}" \
			--enable-static --disable-shared --disable-docs --disable-multi-os-directory \
			CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}")
	fi

	make -C "${PREFIX_PORT_WORKDIR}"
	make -C "${PREFIX_PORT_WORKDIR}" install

	# libffi historically drops ffi.h/ffitarget.h under $libdir/libffi-<ver>/include
	# rather than $includedir; make sure downstream ports find them in PREFIX_H.
	if [ ! -f "${PREFIX_H}/ffi.h" ] && [ -d "${PREFIX_A}/libffi-${version}/include" ]; then
		cp -a "${PREFIX_A}/libffi-${version}/include/"*.h "${PREFIX_H}/"
	fi
}
