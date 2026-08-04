#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="sdl2"
	version="2.30.12"
	desc="Simple DirectMedia Layer 2 (cross-platform multimedia library)"

	source="https://github.com/libsdl-org/SDL/releases/download/release-${version}"
	archive_filename="SDL2-${version}.tar.gz"
	src_path="SDL2-${version}/"

	size="7588596"
	sha256="ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea"

	license="Zlib"
	license_file="LICENSE.txt"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# Phase 1 ("build plumbing"): produce a static libSDL2.a for aarch64-phoenix
	# with the portable SDL core + the pthread thread backend (over libphoenix's
	# libc-integrated pthreads) + the unix CLOCK_MONOTONIC timer. Video, audio,
	# input, loadso etc. use SDL's dummy drivers for now; the phoenix video/
	# input/audio drivers are added in later phases (see the SDL2 port plan).
	#
	# CMAKE_SYSTEM_NAME=Generic marks this a cross build (no host detection);
	# -DPHOENIX=ON selects the Phoenix branch added by patches/0002 which skips
	# the Linux host probes that would otherwise leak -I/usr/include into every
	# try_compile. See patches/*.patch for the per-change rationale.

	# Match the zlib port: fold CFLAGS into LDFLAGS so the gcc-driver link steps
	# in CMake's compiler probes carry the sysroot/-mcpu flags too.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/build/Makefile" ]; then
		mkdir -p "${PREFIX_PORT_WORKDIR}/build"
		(cd "${PREFIX_PORT_WORKDIR}/build" && cmake \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_SYSTEM_NAME=Generic \
			-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${CROSS}gcc" \
			-DCMAKE_CXX_COMPILER="${CROSS}g++" \
			-DPHOENIX=ON \
			-DSDL_LIBC=ON \
			-DSDL_PTHREADS=ON \
			-DSDL_CLOCK_GETTIME=ON \
			-DSDL_SHARED=OFF \
			-DSDL_STATIC=ON \
			-DSDL_X11=OFF \
			-DSDL_WAYLAND=OFF \
			-DSDL_KMSDRM=OFF \
			-DSDL_PULSEAUDIO=OFF \
			-DSDL_ALSA=OFF \
			-DSDL_PIPEWIRE=OFF \
			-DSDL_JACK=OFF \
			-DSDL_OPENGLES=OFF \
			-DSDL_OSS=OFF \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)
}
