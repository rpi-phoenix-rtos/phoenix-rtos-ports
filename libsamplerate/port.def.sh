#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libsamplerate"
	version="0.2.2"
	desc="Secret Rabbit Code — sample rate conversion (static); MojoAL pitch dep"
	cpe23="cpe:2.3:a:libsndfile:libsamplerate:${version}:*:*:*:*:*:*:*"

	source="https://github.com/libsndfile/libsamplerate/releases/download/${version}/"
	archive_filename="${name}-${version}.tar.xz"
	src_path="${name}-${version}/"

	size="3319468"
	sha256="3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893"

	license="BSD-2-Clause"
	license_file="COPYING"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches. The only optional deps (libsndfile via find_package(SndFile),
	# FFTW) are pulled solely for the examples/tests; both are turned off in
	# p_build so no external lib is required. config.h is generated from
	# config.h.cmake by check_include/check_symbol compile probes, cross-safe
	# under CMAKE_SYSTEM_NAME=Generic.
	:
}

p_build() {
	# CMake cross build, same shape as the libogg port. Fold CFLAGS into LDFLAGS
	# so CMake's compiler/link probes carry sysroot/-mcpu.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	# LIBSAMPLERATE_EXAMPLES / BUILD_TESTING OFF => no SndFile / FFTW / ALSA
	# lookups; LIBSAMPLERATE_INSTALL ON so the archive + header + .pc install.
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
			-DLIBSAMPLERATE_EXAMPLES=OFF \
			-DBUILD_TESTING=OFF \
			-DLIBSAMPLERATE_INSTALL=ON \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)
}
