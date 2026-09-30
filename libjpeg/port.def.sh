#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libjpeg"
	version="3.0.4"
	desc="libjpeg-turbo — JPEG image codec with the classic libjpeg API (static)"
	cpe23="cpe:2.3:a:libjpeg-turbo:libjpeg-turbo:${version}:*:*:*:*:*:*:*"

	source="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${version}"
	archive_filename="libjpeg-turbo-${version}.tar.gz"
	src_path="libjpeg-turbo-${version}/"

	size="2400356"
	sha256="99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b"

	# libjpeg-turbo is tri-licensed: IJG (the classic libjpeg), a BSD-style
	# license (the SIMD extensions / TurboJPEG), and zlib (the associated tools).
	license="IJG AND BSD-3-Clause AND Zlib"
	license_file="LICENSE.md"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches: libjpeg-turbo cross-compiles cleanly. The tarball is already
	# extracted by the harness; the cmake configure lives in p_build (sdl2 pattern).
	:
}

p_build() {
	# CMake cross build, same shape as the sdl2 port: CMAKE_SYSTEM_NAME=Generic
	# marks a cross build (no host detection); fold CFLAGS into LDFLAGS so CMake's
	# compiler probes carry the sysroot / -mcpu flags on their link steps.
	#
	# Static libjpeg.a with the classic IJG API (jpeglib.h) — what WRaster / Dillo
	# / fltk / gdk-pixbuf / SuperTuxKart link against.
	#
	# WITH_SIMD=1: on aarch64 libjpeg-turbo 3.0 builds its Neon code from C
	# INTRINSICS (cmake reports "NEON_INTRINSICS = ON" with GCC >= 12), so there is
	# no assembler to wire for the cross toolchain, and init_simd() in
	# simd/arm/aarch64/jsimd.c turns Neon on unconditionally (Neon is mandatory in
	# ARMv8-A; the /proc/cpuinfo probe is Linux-only and not needed). The jsimd_*_neon
	# IDCT/colour-convert/upsample/Huffman kernels land in libjpeg.a. At run time
	# JSIMD_FORCENONE=1 in the environment falls back to the C paths (A/B switch).
	# WITH_TURBOJPEG stays off: it pulls extra libs/programs no consumer needs.
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
			-DENABLE_SHARED=0 \
			-DENABLE_STATIC=1 \
			-DWITH_SIMD=1 \
			-DWITH_TURBOJPEG=0 \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)

	# A configure that quietly fell back to the C paths would still build.
	"${CROSS}nm" "${PREFIX_PORT_INSTALL}/lib/libjpeg.a" | grep ' T jsimd_idct_islow_neon$' >/dev/null ||
		b_die "libjpeg: libjpeg.a has no Neon kernels (WITH_SIMD did not take effect)"
}
