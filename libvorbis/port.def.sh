#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libvorbis"
	version="1.3.7"
	desc="Ogg Vorbis audio codec (static) — libvorbis + vorbisfile + vorbisenc"
	cpe23="cpe:2.3:a:xiph:libvorbis:${version}:*:*:*:*:*:*:*"

	source="https://downloads.xiph.org/releases/vorbis/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="1658963"
	sha256="0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab"

	license="BSD-3-Clause"
	license_file="COPYING"

	conflicts=""
	depends="libogg"

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches. Vorbis's CMake build never defines HAVE_CONFIG_H, so lib/os.h
	# does not pull the autotools config.h; and its alloca() uses (mdct.c,
	# mapping0.c, res0.c) resolve against gcc's __builtin_alloca (alloca.h also
	# exists in the phoenix sysroot). All configure work is CheckIncludeFiles /
	# CheckLibraryExists compile probes, cross-safe under CMAKE_SYSTEM_NAME=Generic.
	:
}

p_build() {
	# CMake cross build, same shape as the libogg port. Fold CFLAGS into LDFLAGS
	# so CMake's compiler/link probes carry sysroot/-mcpu.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	# THE libogg dependency wiring. libvorbis does find_package(Ogg REQUIRED) via
	# its bundled cmake/FindOgg.cmake, which (a) honours pre-seeded cache vars
	# OGG_INCLUDE_DIR / OGG_LIBRARY, short-circuiting pkg-config, and (b) requests
	# no version so the empty OGG_VERSION_STRING cannot fail the check. Our libogg
	# port installed libogg.a + ogg/ogg.h into PORT_DEP_libogg (the shared build
	# prefix); pin both cache vars at it explicitly. This is deterministic and
	# needs no PKG_CONFIG_PATH / CMAKE_PREFIX_PATH gymnastics (port_manager also
	# exports PKG_CONFIG_PATH pointing at ogg.pc as a belt-and-suspenders fallback).
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
			-DINSTALL_CMAKE_PACKAGE_MODULE=OFF \
			-DOGG_INCLUDE_DIR="${PORT_DEP_libogg}/include" \
			-DOGG_LIBRARY="${PORT_DEP_libogg}/lib/libogg.a" \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)
}
