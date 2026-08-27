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
	# Portability patches. 0001-0003 are M2 configure fallout (Generic/cmake-4);
	# 0004-0010 are M3 build fallout (libc/libstdc++ gaps on the compile surface).
	# None change renderer behaviour.
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
	#  0004 simde-common.h — Phoenix <fenv.h> is a poison-pill stub (#errors on
	#       include); skip both fenv-detection blocks so simde uses its non-fenv
	#       rounding fallback (SIMDE_HAVE_FENV_H left undefined).
	#  0005 vk_mem_alloc.h — Phoenix libc has no aligned_alloc/posix_memalign; add
	#       a __phoenix__ vma_aligned_alloc/free using a base-stashing malloc.
	#  0006 irrlicht/irrTypes.h — libphoenix ships no wide-char printf; add a
	#       self-contained swprintf() shim (numeric + wide-%s) for Irrlicht/STK.
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
	# ---------------------------------------------------------------------------
	# MILESTONE M3: cross-CONFIGURE + BUILD + LINK the supertuxkart ELF.
	#
	# Stages: (1) cmake configure/generate for aarch64-phoenix; (2) `make` the
	# full game (all STK src + bundled Irrlicht/GE/bullet/angelscript/mojoal/
	# shaderc on the GLES2/SP path); (3) compile the shared SDL2 phoenix GL glue;
	# (4) final group-link against our in-process Mesa/V3D GL stack — CMake's own
	# link step cannot resolve the GLES entrypoints (they live in libGL-phoenix.a,
	# not in the static STK libs), so we relink from CMake's computed object/lib
	# list (link.txt) with the yQuake2 gl3 --start-group recipe.
	# ---------------------------------------------------------------------------

	# All non-conflict framework ports install into ONE shared prefix, so every
	# PORT_DEP_* points at the same directory; use SDL2's as the anchor.
	local pfx="${PORT_DEP_sdl2}"
	local sysroot="${PREFIX_SYSROOT:-${pfx}/sysroot}"

	# This port reaches build artifacts outside the ports tree (the not-yet-
	# portified Mesa/V3D GL stack): repo_root/external/mesa (GLES2/GLES3 headers +
	# glue includes) and repo_root/tools/.gpu-libs (libGL/libv3d). Same anchor
	# pattern as the yquake2 port.
	local repo_root; repo_root="$(cd "${PREFIX_PORT}/../../.." && pwd)"
	local mesa="${repo_root}/external/mesa"
	local gpu_libs="${repo_root}/tools/.gpu-libs"
	local mcompat="${repo_root}/tools/v3d-driver-port/phoenix_mesa_compat.h"
	local sdl2_glue; sdl2_glue="$(cd "${PREFIX_PORT}/../sdl2/glue" && pwd)"

	# Consumed by the committed toolchain file (aarch64-phoenix.cmake) for the
	# cross compilers, the flag surface and find-root confinement.
	export CROSS CFLAGS LDFLAGS
	export STK_PREFIX="${pfx}"
	export STK_SYSROOT="${sysroot}"

	# Two compile-surface additions folded into the flags every sub-project sees:
	#   * -I${mesa}/include so Irrlicht's <GLES2/gl2.h> / <GLES3/gl3.h> resolve
	#     (STK's Irrlicht/GE select GLES purely by preprocessor define and never
	#     find_package a GL lib, so no headers are on the include path otherwise).
	#   * a force-included compat header supplying a few BSD socket constants that
	#     libphoenix omits but bundled enet/dnsc reference (macro-only, C+C++ safe).
	CFLAGS="${CFLAGS} -I${mesa}/include -include ${PREFIX_PORT}/stk_phoenix_compat.h"

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

	echo ">> [supertuxkart] cmake configure complete. Building game objects + libs."

	# ----- Stage 2: compile all STK src + bundled libs (GLES2/SP path) ----------
	# CMake's own link of the supertuxkart target CANNOT succeed here: the GLES
	# entrypoints referenced by Irrlicht/GE are unresolved in the static libs
	# (resolved only against libGL-phoenix.a at the executable link, stage 4). So
	# we let `make` compile everything and reach — and fail — its link step, then
	# relink ourselves. `make -k` keeps going so every real compile error surfaces
	# in one pass; a genuine compile failure is caught by the object check below.
	(cd "${build}" && make -k -j"$(nproc)" supertuxkart) || true

	# ----- Prerequisite check: GL stack artifacts (fail loud) -------------------
	local sdllib="${pfx}/lib/libSDL2.a"
	local gllib="${gpu_libs}/libGL-phoenix.a"
	local v3dlib="${gpu_libs}/libv3d-phoenix.a"
	local p missing=0
	for p in "${sdllib}" "${gllib}" "${v3dlib}" "${mcompat}" \
		"${sdl2_glue}/sdl_phoenix_glctx.c" "${sdl2_glue}/sdl_phoenix_glstubs.c"; do
		[ -f "${p}" ] || { echo "supertuxkart: MISSING prerequisite: ${p}" >&2; missing=1; }
	done
	for p in "${mesa}/src" "${mesa}/include" "/tmp/mesa-v3d-build/src"; do
		[ -d "${p}" ] || { echo "supertuxkart: MISSING prerequisite dir: ${p}" >&2; missing=1; }
	done
	[ "${missing}" = 0 ] || b_die "GL/V3D stack not present. Build it first: tools/v3d-driver-port/build-gl-phoenix.py (+ build-v3d-phoenix.py) → tools/.gpu-libs + /tmp/mesa-v3d-build."

	local linktxt="${build}/CMakeFiles/supertuxkart.dir/link.txt"
	[ -f "${linktxt}" ] || b_die "supertuxkart: CMake link.txt missing — the make stage did not reach linking (a real compile error). See the build log."

	# ----- Stage 3: compile the SDL2 phoenix GL-context glue --------------------
	# Identical seam to the yquake2 gl3 port: the SDL2 static lib is built with
	# undefined PHOENIX_GL_* / phxgl_* that this glue provides, bridging the ES
	# 3.0 context request (SDL_GL_CONTEXT_PROFILE_ES) to the in-process Mesa/V3D
	# winsys and blitting the default FBO to /dev/fb0.
	local gluedir="${build}/_phoenix_glue"
	mkdir -p "${gluedir}"
	local base_cc="${CFLAGS}"
	local mesa_cc="${CFLAGS} -O2 -ffreestanding -fno-strict-aliasing -Wno-error -Wno-undef \
		-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DHAVE_STRUCT_TIMESPEC \
		-include ${mcompat} -I${mesa}/src -I${mesa}/include -I${mesa}/src/mesa \
		-I${mesa}/src/mapi -I${mesa}/src/compiler -I${mesa}/src/gallium/include \
		-I${mesa}/src/gallium/auxiliary -I${mesa}/src/util -I/tmp/mesa-v3d-build/src \
		-I${pfx}/include -I${pfx}/include/SDL2"
	# shellcheck disable=2086
	"${CROSS}gcc" ${mesa_cc} -c -o "${gluedir}/sdl_phoenix_glctx.o" "${sdl2_glue}/sdl_phoenix_glctx.c"
	# shellcheck disable=2086
	"${CROSS}gcc" ${base_cc} -O2 -ffreestanding -fno-strict-aliasing -Wno-error \
		-c -o "${gluedir}/sdl_phoenix_glstubs.o" "${sdl2_glue}/sdl_phoenix_glstubs.c"

	# ----- Stage 4: final group-link --------------------------------------------
	# Re-run CMake's computed link line (objects + STK/bundled static libs) with
	# the glue objects and a trailing --start-group appended. The group resolves,
	# in one extra pass, two classes the single-pass CMake order leaves undefined:
	#   * the GLES entrypoints (glClear, glDrawArrays, ...) referenced by the STK
	#     libs but provided only by libGL-phoenix.a / libv3d-phoenix.a;
	#   * ogg/vorbis/zlib — CMake lists libogg/libvorbis/libvorbisfile as raw file
	#     paths in producer-first order (no CMake-target dependency info), so
	#     vorbisfile->vorbis->ogg and zlib back-references cannot resolve L-to-R.
	# The group also covers the libGL<->libv3d<->SDL<->glue cycle. Driver is g++
	# (from link.txt) so libstdc++/libm come in automatically; the 8 MB committed
	# stack matches the heavier C++ (yQuake2 used 4 MB).
	echo ">> [supertuxkart] final group-link against libGL/libv3d + SDL2 glue"
	local linkcmd; linkcmd="$(cat "${linktxt}")"
	eval "${linkcmd} '${gluedir}/sdl_phoenix_glctx.o' '${gluedir}/sdl_phoenix_glstubs.o' \
		-Wl,--start-group '${sdllib}' '${gllib}' '${v3dlib}' \
		'${pfx}/lib/libz.a' '${pfx}/lib/libogg.a' '${pfx}/lib/libvorbis.a' \
		'${pfx}/lib/libvorbisfile.a' '${pfx}/lib/libvorbisenc.a' \
		-Wl,--end-group -Wl,-z,stack-size=8388608"

	local elf="${build}/bin/supertuxkart"
	[ -f "${elf}" ] || b_die "supertuxkart: final link produced no ELF (see link errors above)."

	# Install the engine binary. The ~1 GB art assets (stk-assets) are a separate
	# RUNTIME concern staged outside the port (see the port plan §6).
	mkdir -p "${PREFIX_PROG}" "${PREFIX_PROG_STRIPPED}"
	cp "${elf}" "${PREFIX_PROG}/supertuxkart"
	"${STRIP}" -o "${PREFIX_PROG_STRIPPED}/supertuxkart" "${elf}"
	b_install "${PREFIX_PROG_TO_INSTALL}/supertuxkart" /usr/bin
	echo ">> [supertuxkart] M3 complete: /usr/bin/supertuxkart linked + installed."
}
