#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="enet"
	version="1.3.18"
	desc="ENet — thin, reliable UDP networking library (static)"
	cpe23="cpe:2.3:a:enet_project:enet:${version}:*:*:*:*:*:*:*"

	# Canonical upstream release tarball. The URL is plain http (enet.bespin.org
	# has no https), but the framework sha256-pins the archive below, so the
	# download is integrity-checked regardless of transport.
	source="http://enet.bespin.org/download/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="737164"
	sha256="2a8a0c5360d68bb4fcd11f2e4c47c69976e8d2c85b109dd7d60b1181a4f85d36"

	license="MIT"
	license_file="LICENSE"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

p_prepare() {
	# One portability patch: ENet's unix.c uses SOMAXCONN and MSG_TRUNC, which
	# Phoenix's lwip-backed socket headers don't define; the patch adds #ifndef
	# fallbacks. Everything else cross-compiles cleanly — ENet's "configure" is a
	# set of CheckFunctionExists / CheckTypeSize probes that run as link/compile
	# tests against the cross toolchain, so they detect libphoenix's actual socket
	# surface (poll / getaddrinfo / inet_pton / socklen_t / msghdr.msg_flags)
	# rather than the host's.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# CMake cross build, same shape as the libjpeg / sdl2 ports:
	# CMAKE_SYSTEM_NAME=Generic marks a cross build (no host detection). Folding
	# CFLAGS into LDFLAGS is LOAD-BEARING here: ENet's feature detection uses
	# check_function_exists(), which link-tests a tiny program. Without the
	# sysroot / -mcpu flags on the link step those probes fail to link and every
	# HAS_* silently defaults off, degrading the socket backend. With the fold
	# they resolve against libphoenix and the correct HAS_* are defined.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/build/Makefile" ]; then
		mkdir -p "${PREFIX_PORT_WORKDIR}/build"
		(cd "${PREFIX_PORT_WORKDIR}/build" && cmake \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
			-DCMAKE_INSTALL_INCLUDEDIR=include \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_SYSTEM_NAME=Generic \
			-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${CROSS}gcc" \
			-DCMAKE_C_FLAGS="${CFLAGS}" \
			.. && make)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)

	# ENet's CMakeLists hardcodes `ARCHIVE DESTINATION lib/static`, so `make
	# install` drops libenet.a under <prefix>/lib/static/ rather than the
	# framework's lib/ where consumers look for -lenet. Copy it up to PREFIX_A
	# and drop the stray subdir so PREFIX_BUILD stays clean. Headers already land
	# correctly (install DIRECTORY include/ -> include).
	mkdir -p "${PREFIX_A}"
	cp -a "${PREFIX_PORT_WORKDIR}/build/libenet.a" "${PREFIX_A}/"
	rm -rf "${PREFIX_A}/static"

	# ENet's CMake build installs no pkg-config file (upstream ships libenet.pc
	# only via its autotools path). Consumers that discover ENet with
	# `pkg_check_modules(ENET libenet>=...)` — SuperTuxKart's USE_SYSTEM_ENET
	# among them — need it, and the framework adds each dep's lib/pkgconfig to
	# PKG_CONFIG_PATH. Emit the same libenet.pc the autotools build would.
	mkdir -p "${PREFIX_A}/pkgconfig"
	cat >"${PREFIX_A}/pkgconfig/libenet.pc" <<EOF
prefix=${PREFIX_PORT_INSTALL}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libenet
Description: ENet reliable UDP networking library
Version: ${version}
Libs: -L\${libdir} -lenet
Cflags: -I\${includedir}
EOF
}
