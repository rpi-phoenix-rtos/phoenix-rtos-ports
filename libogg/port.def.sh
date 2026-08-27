#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libogg"
	version="1.3.5"
	desc="Ogg bitstream container library (static) — base for libvorbis"
	cpe23="cpe:2.3:a:xiph:libogg:${version}:*:*:*:*:*:*:*"

	source="https://downloads.xiph.org/releases/ogg/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="593071"
	sha256="0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664"

	license="BSD-3-Clause"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches: libogg is two source files (framing.c, bitwise.c) plus a
	# generated config_types.h. Its "configure" is compile-only check_type_size /
	# check_include_files probes, which are cross-safe under CMAKE_SYSTEM_NAME=
	# Generic (no run step), so they size the target's int types correctly.
	:
}

p_build() {
	# CMake cross build, same shape as the libjpeg port. libogg defaults to a
	# static lib (BUILD_SHARED_LIBS=OFF) and installs libogg.a to lib/ + headers
	# to include/ogg/ (it honours CMAKE_INSTALL_LIBDIR, no lib/static quirk).
	# Fold CFLAGS into LDFLAGS so CMake's compiler probes carry sysroot/-mcpu.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/build/Makefile" ]; then
		mkdir -p "${PREFIX_PORT_WORKDIR}/build"
		(cd "${PREFIX_PORT_WORKDIR}/build" && cmake \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
			-DCMAKE_INSTALL_LIBDIR=lib \
			-DCMAKE_INSTALL_INCLUDEDIR=include \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_SYSTEM_NAME=Generic \
			-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${CROSS}gcc" \
			-DCMAKE_C_FLAGS="${CFLAGS}" \
			-DBUILD_SHARED_LIBS=OFF \
			-DINSTALL_DOCS=OFF \
			-DINSTALL_PKG_CONFIG_MODULE=ON \
			-DINSTALL_CMAKE_PACKAGE_MODULE=OFF \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)
}
