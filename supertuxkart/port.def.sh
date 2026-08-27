#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="supertuxkart"
	version="1.4"
	desc="SuperTuxKart 1.4 — 3D kart racing game (stk-code; GLES2/SP renderer)"
	cpe23="cpe:2.3:a:supertuxkart:supertuxkart:${version}:*:*:*:*:*:*:*"

	# GitHub auto-generated source archive for tag 1.4. The remote file is served
	# as "1.4.tar.gz"; b_port_download's (filename, orig_filename) form saves it
	# under the descriptive local name below. This archive bundles stk-code's
	# in-tree deps (bullet, angelscript, mcpp, libsquish, mojoal, the Irrlicht
	# fork, graphics_engine, sheenbidi, tinygettext, shaderc + glslang/SPIRV) and
	# stk-code's own data/ (~46 MB, needed at configure for the CHECK_ASSETS /
	# data-folder tests). The ~1 GB art assets (stk-assets) are a separate RUNTIME
	# concern and deliberately NOT fetched here.
	source="https://github.com/supertuxkart/stk-code/archive/refs/tags"
	archive_filename=("stk-code-${version}.tar.gz" "${version}.tar.gz")
	src_path="stk-code-${version}/"

	size="32646035"
	sha256="40ff14ce0e1fde05fa9f427bfe1f75917a6f4efbf2c1a86421a7f794d05189b9"

	license="GPL-3.0-or-later"
	license_file="COPYING"

	conflicts=""
	# Every STK dependency that is NOT bundled in stk-code/lib is a framework
	# port; list them so port_manager builds/verifies them into the shared
	# install prefix first. freetype is provided by xorg_fonts. enet is NOT
	# listed: STK uses its bundled enet whenever USE_IPV6 is ON (the default), so
	# the ported enet is not consumed by this configuration.
	depends="sdl2 libjpeg libpng zlib xorg_fonts curl mbedtls sqlite3 libogg libvorbis libsamplerate harfbuzz"

	supports="phoenix>=3.3"
}

p_prepare() {
	# Three configure-portability patches (all Generic/cmake-4 fallout, none
	# touching runtime code):
	#  0001 FindFreetype.cmake — its non-Win/Apple/SunOS branch calls
	#       pkg_check_modules(freetype2), but under CMAKE_SYSTEM_NAME=Generic the
	#       UNIX-gated include(FindPkgConfig) never ran, so that command is
	#       undefined; route Generic through the existing manual-find branch.
	#  0002 CMakeLists.txt — STK forces policy CMP0043 to OLD, which host cmake
	#       4.x no longer supports (hard error); gate it on cmake < 4.0.
	#  0003 lib/shaderc/third_party/spirv-tools — its platform switch FATAL_ERRORs
	#       on unknown CMAKE_SYSTEM_NAME; add a Generic branch (treat as Linux),
	#       mirroring STK's own NintendoSwitch addition.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# ---------------------------------------------------------------------------
	# MILESTONE M2: cross-CONFIGURE only.
	#
	# This p_build runs cmake's configure+generate for aarch64-phoenix and stops.
	# It deliberately does NOT build or link the supertuxkart ELF — that is M3,
	# which still has open link-surface work (GLES entrypoints, libstdc++,
	# Irrlicht device/GL glue; see the port plan doc). A bounded smoke build of a
	# small socket-free bundled lib (mcpp) is attempted as a configure sanity
	# check and is non-fatal.
	# ---------------------------------------------------------------------------

	# All non-conflict framework ports install into ONE shared prefix, so every
	# PORT_DEP_* points at the same directory; use SDL2's as the anchor.
	local pfx="${PORT_DEP_sdl2}"
	local sysroot="${PREFIX_SYSROOT:-${pfx}/sysroot}"

	# Consumed by the committed toolchain file (aarch64-phoenix.cmake) for the
	# cross compilers, the flag surface and find-root confinement.
	export CROSS CFLAGS LDFLAGS
	export STK_PREFIX="${pfx}"
	export STK_SYSROOT="${sysroot}"

	# Fold CFLAGS into LDFLAGS so link-time configure probes (STK's
	# std::atomic<uint64_t> check, shaderc's compiler-flag checks) carry the
	# sysroot / -mcpu surface and link successfully.
	LDFLAGS="${CFLAGS} ${LDFLAGS}"

	local build="${PREFIX_PORT_WORKDIR}/build"
	if [ ! -f "${build}/CMakeCache.txt" ]; then
		mkdir -p "${build}"
		# NOTE on the Generic-vs-UNIX trap: CMAKE_SYSTEM_NAME=Generic (set by the
		# toolchain file) leaves CMake's UNIX var FALSE, so STK's UNIX-gated
		# defaults do NOT fire. We therefore pass the affected options explicitly:
		#   * -DUSE_GLES2=ON       (the arm/aarch64 auto-default is UNIX-gated)
		#   * bundled enet         (system-enet branch is UNIX-gated AND skipped
		#                           when USE_IPV6=ON anyway; USE_SYSTEM_ENET=OFF)
		# CMAKE_POLICY_VERSION_MINIMUM=3.5 is mandatory under host cmake 4.x
		# (STK's cmake_minimum_required(2.8.4) is otherwise rejected).
		(cd "${build}" && cmake \
			-DCMAKE_TOOLCHAIN_FILE="${PREFIX_PORT}/aarch64-phoenix.cmake" \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
			-DCMAKE_BUILD_TYPE=STKRelease \
			\
			-DUSE_GLES2=ON \
			-DUSE_MOJOAL=ON \
			-DUSE_WIIUSE=0 \
			-DUSE_SYSTEM_WIIUSE=0 \
			-DCHECK_ASSETS=OFF \
			-DBUILD_RECORDER=OFF \
			-DUSE_SYSTEM_ENET=OFF \
			-DUSE_SYSTEM_ANGELSCRIPT=OFF \
			-DUSE_SYSTEM_SQUISH=OFF \
			-DUSE_SQLITE3=ON \
			-DUSE_DNS_C=ON \
			-DUSE_CRYPTO_OPENSSL=OFF \
			\
			-DSDL2_LIBRARY="${pfx}/lib/libSDL2.a" \
			-DSDL2_INCLUDEDIR="${pfx}/include/SDL2" \
			-DJPEG_LIBRARY="${pfx}/lib/libjpeg.a" \
			-DJPEG_INCLUDE_DIR="${pfx}/include" \
			-DPNG_LIBRARY="${pfx}/lib/libpng.a" \
			-DPNG_PNG_INCLUDE_DIR="${pfx}/include" \
			-DZLIB_LIBRARY="${pfx}/lib/libz.a" \
			-DZLIB_INCLUDE_DIR="${pfx}/include" \
			-DFREETYPE_LIBRARY="${pfx}/lib/libfreetype.a" \
			-DFREETYPE_INCLUDE_DIRS="${pfx}/include/freetype2" \
			-DHARFBUZZ_LIBRARY="${pfx}/lib/libharfbuzz.a" \
			-DHARFBUZZ_INCLUDEDIR="${pfx}/include" \
			-DCURL_LIBRARY="${pfx}/lib/libcurl.a" \
			-DCURL_INCLUDE_DIR="${pfx}/include" \
			-DLIBSAMPLERATE_LIBRARY="${pfx}/lib/libsamplerate.a" \
			-DLIBSAMPLERATE_INCLUDEDIR="${pfx}/include" \
			-DSQLITE3_LIBRARY="${pfx}/lib/libsqlite3.a" \
			-DSQLITE3_INCLUDEDIR="${pfx}/include" \
			-DMBEDTLS_INCLUDE_DIRS="${pfx}/include" \
			-DMBEDCRYPTO_LIBRARY="${pfx}/lib/libmbedcrypto.a" \
			-DOGGVORBIS_OGG_INCLUDE_DIR="${pfx}/include" \
			-DOGGVORBIS_VORBIS_INCLUDE_DIR="${pfx}/include" \
			-DOGGVORBIS_OGG_LIBRARY="${pfx}/lib/libogg.a" \
			-DOGGVORBIS_VORBIS_LIBRARY="${pfx}/lib/libvorbis.a" \
			-DOGGVORBIS_VORBISFILE_LIBRARY="${pfx}/lib/libvorbisfile.a" \
			-DOGGVORBIS_VORBISENC_LIBRARY="${pfx}/lib/libvorbisenc.a" \
			-DPTHREAD_LIBRARY="${sysroot}/lib/libpthread.a" \
			"${PREFIX_PORT_WORKDIR}")
	fi

	echo ">> [supertuxkart] cmake configure complete (M2). Full build/link is M3."

	# Bounded, non-fatal smoke: a small socket-free bundled lib. Confirms the
	# generated build graph + cross toolchain actually compile a STK sub-target.
	(cd "${build}" && make mcpp) \
		&& echo ">> [supertuxkart] smoke build of bundled mcpp OK" \
		|| echo ">> [supertuxkart] mcpp smoke build skipped/failed (non-fatal at M2)"
}
