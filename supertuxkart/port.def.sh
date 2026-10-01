#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="supertuxkart"
	version="1.4"
	desc="SuperTuxKart 1.4 (stk-code; GLES2/SP renderer): the CMake build for the SDL2 KMSDRM + Mesa GPU stack, linked by supertuxkart_drm"
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
	depends="sdl2_kmsdrm mesa_drm[opengl] libjpeg libpng zlib xorg_fonts curl mbedtls sqlite3 libogg libvorbis libsamplerate harfbuzz"

	supports="phoenix>=3.3"
}

p_prepare() {
	# Portability patches. 0001-0003 are M2 configure fallout (Generic/cmake-4);
	# 0004-0010 are M3 build fallout (libc/libstdc++ gaps on the compile surface).
	# None change renderer behaviour. 0011 is a real bug fix: a 44 KiB PCM buffer
	# on a 4 KiB thread stack (see the patch header for the byte-exact proof).
	#  0001 FindFreetype.cmake — its non-Win/Apple/SunOS branch calls
	#       pkg_check_modules(freetype2), but under CMAKE_SYSTEM_NAME=Generic the
	#       UNIX-gated include(FindPkgConfig) never ran, so that command is
	#       undefined; route Generic through the existing manual-find branch.
	#  0002 CMakeLists.txt — STK forces policy CMP0043 to OLD, which host cmake
	#       4.x no longer supports (hard error); gate it on cmake < 4.0.
	#  0003 lib/shaderc/third_party/spirv-tools — its platform switch FATAL_ERRORs
	#       on unknown CMAKE_SYSTEM_NAME; add a Generic branch (treat as Linux but
	#       with timers OFF: Phoenix rusage lacks ru_maxrss/minflt/majflt and has
	#       no CLOCK_PROCESS_CPUTIME_ID, so util/timer.* would not compile).
	#  0004 simde-common.h — skip both fenv-detection blocks so simde uses its
	#       non-fenv rounding fallback (SIMDE_HAVE_FENV_H left undefined). Still
	#       needed although libphoenix now implements <fenv.h> (it used to be
	#       libmcs's #error stub): the toolchain's libstdc++ was configured without
	#       _GLIBCXX_HAVE_FENV_H, so in C++ its <fenv.h>/<cfenv> wrappers include
	#       nothing, and simde would see the header yet no FE_* or fe*() at all.
	#  0005 vk_mem_alloc.h — Phoenix libc has no aligned_alloc/posix_memalign; add
	#       a __phoenix__ vma_aligned_alloc/free using a base-stashing malloc.
	#  0006 irrlicht/irrTypes.h — Irrlicht passes wchar_t* to swprintf's %s, which
	#       the standard (and libphoenix's swprintf) reads as a multibyte string;
	#       redirect swprintf by macro to a shim (numeric + wide-%s) for Irrlicht/STK.
	#  0007 glslang glslang/CMakeLists.txt — add Generic to the OSDependent/Unix
	#       gate, else libOSDependent.a is never built and the link degrades to a
	#       bare, unprovided -lOSDependent.
	#  0008 glslang OSDependent/Unix/ossource.cpp — drop the unused <semaphore.h>
	#       include (Phoenix has none) and route thread cleanup through the
	#       Android/Fuchsia path (no pthread_setcanceltype / PTHREAD_CANCEL_*).
	#  0009 src/guiengine/widgets/spinner_widget.cpp — Phoenix libstdc++ has no
	#       wide iostreams; format via a narrow stream widened through stringw.
	#  0010 src/utils/translation.cpp — Phoenix locale.h lacks LC_MESSAGES; route
	#       it through the existing Windows LC_CTYPE branch.
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# cmake-configure + compile the whole game (all STK src + bundled Irrlicht/GE/
	# bullet/angelscript/mojoal/shaderc on the GLES2/SP path) against the KMSDRM SDL
	# (sdl2_kmsdrm) and the Mesa GLES headers. This port links NOTHING and installs
	# nothing: CMake's own link of bin/supertuxkart cannot succeed (the gl* entry points
	# Irrlicht/GE reference and SDL's EGL/GBM calls are resolved only by the Mesa
	# archives), so `make -k` compiles everything, fails that one link, and the linking
	# port (supertuxkart_drm) relinks from the build tree.
	#
	# THE INTERFACE: ${PREFIX_PORT_WORKDIR}/build (objects + static libs) and CMake's
	# computed link line build/CMakeFiles/supertuxkart.dir/link.txt (paths relative to
	# build/, `-o bin/supertuxkart`, naming sdl2_kmsdrm's libSDL2.a once). A consumer runs
	# it from build/ with -o redirected and appends its library group: the GPU stack plus
	# zlib, ogg/vorbis/vorbisfile/vorbisenc and mbedtls/mbedx509/mbedcrypto, which CMake
	# lists out of order or not at all. (Up to GPU migration P3 this port linked
	# /usr/bin/supertuxkart itself, on the /dev/fb0 SDL and the in-process GL winsys.)

	# All non-conflict framework ports install into ONE shared prefix, so every
	# PORT_DEP_* of those points at the same directory; use zlib's as the anchor.
	local pfx="${PORT_DEP_zlib%/}"
	local sysroot="${PREFIX_SYSROOT:-${pfx}/sysroot}"
	local SP="${PORT_DEP_sdl2_kmsdrm:?}" GLINC="${PORT_DEP_mesa_drm:?}/src-include" p
	for p in "${SP}/lib/libSDL2.a" "${SP}/include/SDL2/SDL.h" "${GLINC}/GLES2/gl2.h" "${GLINC}/KHR/khrplatform.h"; do
		[ -e "${p}" ] || b_die "supertuxkart: missing ${p}"
	done

	# Consumed by the committed toolchain file (aarch64-phoenix.cmake) for the
	# cross compilers, the flag surface and find-root confinement.
	export CROSS CFLAGS LDFLAGS
	export STK_PREFIX="${pfx}"
	export STK_SYSROOT="${sysroot}"

	# Three compile-surface additions folded into the flags every sub-project sees:
	#   * -I<sdl2_kmsdrm>/include/SDL2 FIRST: ahead of the shared ports prefix that CFLAGS
	#     names, which may still hold the deleted sdl2 port's headers from an earlier build;
	#   * -I<mesa_drm>/src-include so Irrlicht's <GLES2/gl2.h> / <GLES3/gl3.h> resolve
	#     (STK's Irrlicht/GE select GLES purely by preprocessor define and never
	#     find_package a GL lib, so no headers are on the include path otherwise);
	#   * a force-included compat header supplying a few BSD socket constants that
	#     libphoenix omits but bundled enet/dnsc reference (macro-only, C+C++ safe);
	#   * -fmacro-prefix-map of the source tree, AFTER the framework's own map (the last
	#     matching map wins): __FILE__ in asserts and log calls becomes src/..., lib/...
	#     instead of a path left relative to the framework's top dir (.buildroot/_build/...).
	CFLAGS="-I${SP}/include/SDL2 ${CFLAGS} -I${GLINC} -include ${PREFIX_PORT}/stk_phoenix_compat.h -fmacro-prefix-map=${PREFIX_PORT_WORKDIR%/}/="

	# Fold CFLAGS into LDFLAGS so link-time configure probes (STK's
	# std::atomic<uint64_t> check, shaderc's compiler-flag checks) carry the
	# sysroot / -mcpu surface and link successfully.
	LDFLAGS="${CFLAGS} ${LDFLAGS}"

	local build="${PREFIX_PORT_WORKDIR}/build"
	# The configure is skipped when a CMakeCache.txt exists, and the cache holds the flags and
	# the SDL paths of the configure that made it: reconfigure whenever they change (as they
	# did when the /dev/fb0 SDL of the shared prefix gave way to sdl2_kmsdrm).
	local stamp="${PREFIX_PORT_BUILD}/configure-inputs.sha" want
	want="$(printf '%s\n' "${CFLAGS}" "${LDFLAGS}" "${SP}" "${GLINC}" | sha256sum | cut -d' ' -f1)"
	if [ -f "${build}/CMakeCache.txt" ] && [ "$(cat "${stamp}" 2>/dev/null)" != "${want}" ]; then
		echo ">> [supertuxkart] configure inputs changed: reconfiguring from scratch"
		rm -rf "${build}"
	fi
	if [ ! -f "${build}/CMakeCache.txt" ]; then
		mkdir -p "${build}"
		# NOTE on the Generic-vs-UNIX trap: CMAKE_SYSTEM_NAME=Generic (set by the
		# toolchain file) leaves CMake's UNIX var FALSE, so STK's UNIX-gated
		# defaults do NOT fire. We therefore pass the affected options explicitly:
		#   * -DUSE_GLES2=ON       (the arm/aarch64 auto-default is UNIX-gated)
		#   * bundled enet         (system-enet branch is UNIX-gated AND skipped
		#                           when USE_IPV6=ON anyway; USE_SYSTEM_ENET=OFF)
		# STK_INSTALL_DATA_DIR, absolute, is the SUPERTUXKART_DATADIR compiled in (the
		# data dir used when $SUPERTUXKART_DATADIR is unset): the image's, not
		# CMAKE_INSTALL_PREFIX's. Nothing here runs `make install`.
		# CMAKE_POLICY_VERSION_MINIMUM=3.5 is mandatory under host cmake 4.x
		# (STK's cmake_minimum_required(2.8.4) is otherwise rejected).
		(cd "${build}" && cmake \
			-DCMAKE_TOOLCHAIN_FILE="${PREFIX_PORT}/aarch64-phoenix.cmake" \
			-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
			-DSTK_INSTALL_DATA_DIR=/usr/share/supertuxkart \
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
			-DSDL2_LIBRARY="${SP}/lib/libSDL2.a" \
			-DSDL2_INCLUDEDIR="${SP}/include/SDL2" \
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
			"${PREFIX_PORT_WORKDIR}") || b_die "supertuxkart: cmake configure failed"
		echo "${want}" > "${stamp}"
	fi

	echo ">> [supertuxkart] cmake configure complete. Building game objects + libs."

	# Compile all STK src + bundled libs (GLES2/SP path); CMake's own link of the
	# supertuxkart target is EXPECTED to fail (see the top of p_build). `make -k` keeps going
	# so every real compile error surfaces in one pass. CMake writes link.txt at generate
	# time, so a compile failure shows as objects named by link.txt that do not exist
	# (checked below).
	echo ">> [supertuxkart] NOTE: CMake's own link of bin/supertuxkart is EXPECTED"
	echo ">> [supertuxkart]   to fail below with undefined GL/EGL/zlib/mbedtls symbols;"
	echo ">> [supertuxkart]   supertuxkart_drm links the program from this build tree."
	(cd "${build}" && make -k -j"$(nproc)" supertuxkart) || true


	local linktxt="${build}/CMakeFiles/supertuxkart.dir/link.txt" f n=0
	[ -f "${linktxt}" ] || b_die "supertuxkart: CMake link.txt missing — the configure did not generate the supertuxkart target. See the build log."
	[ "$(grep -oF " ${SP}/lib/libSDL2.a " "${linktxt}" | wc -l)" = 1 ] \
		|| b_die "supertuxkart: link.txt does not name ${SP}/lib/libSDL2.a exactly once"
	for f in $(tr ' ' '\n' < "${linktxt}" | grep -E '\.(o|obj)$'); do
		[ -f "${build}/${f}" ] || [ -f "${f}" ] || b_die "supertuxkart: object named by link.txt missing: ${f} (a compile error)"
		n=$((n + 1))
	done
	echo ">> [supertuxkart] build tree ready for supertuxkart_drm: ${n} objects + the static libs in ${linktxt}"
}
