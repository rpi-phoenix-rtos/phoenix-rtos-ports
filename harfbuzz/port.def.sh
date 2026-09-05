#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="harfbuzz"
	version="14.4.0"
	desc="HarfBuzz text-shaping engine (static, freetype backend) — SuperTuxKart"
	cpe23="cpe:2.3:a:harfbuzz:harfbuzz:${version}:*:*:*:*:*:*:*"

	source="https://github.com/harfbuzz/harfbuzz/releases/download/${version}/"
	archive_filename="${name}-${version}.tar.xz"
	src_path="${name}-${version}/"

	size="20107692"
	sha256="2357ed966c6ced7bfa720b0640c0231065af01158fbea215093ffa15aed44371"

	license="MIT"
	license_file="COPYING"

	conflicts=""
	# freetype is provided by the xorg_fonts port (which builds freetype
	# harfbuzz-less specifically to break the freetype<->harfbuzz cycle), so
	# depending on it here is safe and non-circular. It is the only port that
	# installs libfreetype.a + include/freetype2 into the shared build prefix.
	depends="xorg_fonts"

	supports="phoenix>=3.3"
}

p_prepare() {
	# No patches. Upstream dropped autotools at 3.0, so CMake -- which this port
	# already drove -- is now the only build system, and the 2.6.7 pin (kept only
	# for parity with the old tools/x11-port autotools script) had no reason left.
	# harfbuzz sets its own -fno-rtti -fno-exceptions -fno-threadsafe-statics for
	# GNU and ships an amalgam C++ source, so no config header generation is
	# needed; the check_funcs/check_cxx probes are cross-safe compile tests.
	:
}

p_build() {
	# CMake C++ cross build. reset_env only exports CFLAGS/LDFLAGS (not CXXFLAGS),
	# so drive the C++ toolchain explicitly and reuse CFLAGS for the C++ flags
	# (the sysroot/-mcpu surface is identical) — the proven fltk C++ pattern.
	# Fold the flags into LDFLAGS so harfbuzz's link probes carry sysroot/-mcpu.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	# Minimal shaping build for STK: freetype backend ON (STK's font manager is
	# expected to use the hb-ft glyph bridge, hb_ft_font_create*), glib/icu/cairo
	# OFF (harfbuzz's own unicode funcs suffice; STK uses harfbuzz standalone),
	# subset lib OFF (STK does not need it), and the raster/vector/GPU libraries
	# 14.x adds -- all default ON, none of them wanted for shaping alone. find_package(Freetype) is pinned via
	# its FindFreetype cache vars (FREETYPE_LIBRARY + the two include-dir vars)
	# straight at the xorg_fonts-provided freetype in ${PORT_DEP_xorg_fonts};
	# pinning them kills the Generic-mode fallthrough to a host /usr freetype.
	local ftroot="${PORT_DEP_xorg_fonts}"

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
			-DCMAKE_CXX_COMPILER="${CROSS}g++" \
			-DCMAKE_C_FLAGS="${CFLAGS}" \
			-DCMAKE_CXX_FLAGS="${CFLAGS}" \
			-DBUILD_SHARED_LIBS=OFF \
			-DHB_HAVE_FREETYPE=ON \
			-DHB_HAVE_GLIB=OFF \
			-DHB_HAVE_ICU=OFF \
			-DHB_HAVE_GRAPHITE2=OFF \
			-DHB_BUILD_SUBSET=OFF \
			-DHB_BUILD_UTILS=OFF \
			-DHB_BUILD_RASTER=OFF \
			-DHB_BUILD_VECTOR=OFF \
			-DHB_BUILD_GPU=OFF \
			-DFREETYPE_LIBRARY="${ftroot}/lib/libfreetype.a" \
			-DFREETYPE_INCLUDE_DIR_ft2build="${ftroot}/include/freetype2" \
			-DFREETYPE_INCLUDE_DIR_freetype2="${ftroot}/include/freetype2" \
			.. && make install)
	fi

	(cd "${PREFIX_PORT_WORKDIR}/build" && make install)

	# harfbuzz's CMake build installs no pkg-config file. Consumers that discover
	# it with pkg_check_modules (pango, fontconfig-adjacent stacks) want one;
	# emit the same harfbuzz.pc upstream's autotools build would. SuperTuxKart
	# itself uses find_library(HARFBUZZ) + <hb.h> and does not need it.
	mkdir -p "${PREFIX_A}/pkgconfig"
	cat >"${PREFIX_A}/pkgconfig/harfbuzz.pc" <<EOF
prefix=${PREFIX_PORT_INSTALL}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: harfbuzz
Description: HarfBuzz text shaping library
Version: ${version}
Requires.private: freetype2
Libs: -L\${libdir} -lharfbuzz
Cflags: -I\${includedir}/harfbuzz
EOF
}
