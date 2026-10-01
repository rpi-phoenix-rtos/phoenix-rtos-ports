#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="harfbuzz_icu"
	version="14.4.0"
	desc="HarfBuzz's ICU bridge (libharfbuzz-icu: hb_icu_get_unicode_funcs, hb_icu_script_*) for the harfbuzz port"
	cpe23="cpe:2.3:a:harfbuzz:harfbuzz:${version}:*:*:*:*:*:*:*"

	# The same release as the harfbuzz port, which this port must always match.
	source="https://github.com/harfbuzz/harfbuzz/releases/download/${version}/"
	archive_filename="harfbuzz-${version}.tar.xz"
	src_path="harfbuzz-${version}/"

	size="20107692"
	sha256="2357ed966c6ced7bfa720b0640c0231065af01158fbea215093ffa15aed44371"

	license="MIT"
	license_file="COPYING"

	conflicts=""
	# xorg_fonts: the freetype the harfbuzz port is configured against (see p_build).
	depends="harfbuzz==${version} icu xorg_fonts"

	supports="phoenix>=3.3"
}

# Why a port of its own, and not HB_HAVE_ICU=ON in the harfbuzz port:
#
# * With HarfBuzz's CMake build, ICU support is ADDITIVE: it adds one library,
#   harfbuzz-icu (src/hb-icu.cc), and leaves libharfbuzz.a as it is (HAVE_ICU
#   only matters to hb-unicode.cc together with HAVE_ICU_BUILTIN, which the CMake
#   build never sets, so libharfbuzz keeps its own UCD functions as the default).
#   WebKit wants exactly that extra library (its FindHarfBuzz.cmake: COMPONENTS
#   ICU = hb-icu.h + libharfbuzz-icu, ComplexTextControllerHarfBuzz calls
#   hb_icu_get_unicode_funcs()).
# * Any edit to the harfbuzz recipe changes its digest, and port_manager then
#   rebuilds harfbuzz and everything that depends on it (labwc_desktop and
#   supertuxkart, and their dependents) from scratch, and would make ICU a build
#   dependency of every desktop image -- for no change in what those programs
#   link.
#
# So this port builds the same HarfBuzz release with the harfbuzz port's CMake
# options plus HB_HAVE_ICU=ON, builds ONLY the harfbuzz-icu target, and installs
# only what that adds: lib/libharfbuzz-icu.a, include/harfbuzz/hb-icu.h and
# lib/pkgconfig/harfbuzz-icu.pc. Never `make install` here: it would rewrite the
# harfbuzz port's libharfbuzz.a and headers in the shared prefix behind its back.
#
# A consumer links -lharfbuzz-icu -lharfbuzz -lfreetype -licuuc -licudata -lm
# -lstdc++ (libm before libstdc++, see p_build), or `pkg-config --static
# harfbuzz-icu`. GTK's private meson HarfBuzz
# (gtk3_wayland) is the same 14.4.0 source, so this library is link-compatible
# with it too.

p_prepare() {
	# No patches.
	:
}

p_build() {
	local ftroot icuroot b="${PREFIX_PORT_WORKDIR}/build"
	ftroot="$(realpath -m "$(b_dependency_dir xorg_fonts)")"
	icuroot="$(realpath -m "$(b_dependency_dir icu)")"

	# As the harfbuzz port: CFLAGS for C++ too, folded into LDFLAGS for the probes.
	LDFLAGS="${CFLAGS} $LDFLAGS"

	# Freetype is located the way the harfbuzz port does it, so the configuration
	# (and thus every compile definition hb-icu.cc sees) is the same; it does not
	# matter to hb-icu.cc itself.

	# ICU is pinned through FindICU's cache variables, straight at the icu port:
	# with CMAKE_SYSTEM_NAME=Generic, FindICU would otherwise be free to pick up
	# the build host's /usr/include/unicode.
	if [ ! -f "${b}/Makefile" ]; then
		mkdir -p "${b}"
		(cd "${b}" && cmake \
			-DCMAKE_INSTALL_PREFIX="${PREFIX_PORT_INSTALL}" \
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
			-DHB_HAVE_ICU=ON \
			-DHB_HAVE_GRAPHITE2=OFF \
			-DHB_BUILD_SUBSET=OFF \
			-DHB_BUILD_UTILS=OFF \
			-DHB_BUILD_RASTER=OFF \
			-DHB_BUILD_VECTOR=OFF \
			-DHB_BUILD_GPU=OFF \
			-DFREETYPE_LIBRARY="${ftroot}/lib/libfreetype.a" \
			-DFREETYPE_INCLUDE_DIR_ft2build="${ftroot}/include/freetype2" \
			-DFREETYPE_INCLUDE_DIR_freetype2="${ftroot}/include/freetype2" \
			-DICU_ROOT="${icuroot}" \
			-DICU_INCLUDE_DIR="${icuroot}/include" \
			-DICU_UC_LIBRARY_RELEASE="${icuroot}/lib/libicuuc.a" \
			..)
	fi

	grep -q "^ICU_UC_LIBRARY_RELEASE:FILEPATH=${icuroot}/lib/libicuuc.a$" "${b}/CMakeCache.txt" ||
		b_die "harfbuzz_icu: CMake did not take the icu port's libicuuc.a"
	grep -q "^ICU_INCLUDE_DIR:PATH=${icuroot}/include$" "${b}/CMakeCache.txt" ||
		b_die "harfbuzz_icu: CMake did not take the icu port's headers"

	make -C "${b}" harfbuzz-icu

	[ -f "${b}/libharfbuzz-icu.a" ] || b_die "harfbuzz_icu: libharfbuzz-icu.a was not built"
	"${CROSS}nm" "${b}/libharfbuzz-icu.a" | grep -q " T hb_icu_get_unicode_funcs$" ||
		b_die "harfbuzz_icu: hb_icu_get_unicode_funcs is not in libharfbuzz-icu.a"

	# The static link line a consumer needs resolves on Phoenix (built, not
	# installed: ICU's data makes every such program ~14 MB). hb_icu's script and
	# composition callbacks are exercised, so the ICU objects are really pulled.
	# -lm BEFORE -lstdc++: the toolchain's libstdc++.a carries a hypotf stub
	# (math_stubs_float.o) that collides with libphoenix libm's once HarfBuzz pulls
	# hypotf in (labwc_desktop links libm early for the same reason).
	cat >"${b}/hb-icu-link.c" <<'EOF'
#include <hb.h>
#include <hb-icu.h>
int main(void)
{
	hb_unicode_funcs_t *u = hb_icu_get_unicode_funcs();
	hb_codepoint_t ab;
	return !(hb_unicode_script(u, 0x0644) == HB_SCRIPT_ARABIC &&
		hb_unicode_compose(u, 'e', 0x0301, &ab) && ab == 0x00e9);
}
EOF
	# shellcheck disable=2086 # CFLAGS/LDFLAGS must word-split
	"${CROSS}gcc" ${CFLAGS} -I"${PREFIX_PORT_WORKDIR}/src" -I"${icuroot}/include" "${b}/hb-icu-link.c" \
		-o "${b}/hb-icu-link" ${LDFLAGS} -L"${b}" -L"${PREFIX_A}" -L"${icuroot}/lib" -L"${ftroot}/lib" \
		-lharfbuzz-icu -lharfbuzz -lfreetype -licuuc -licudata -lm -lstdc++ -lpthread ||
		b_die "harfbuzz_icu: a program using hb-icu does not link"

	mkdir -p "${PREFIX_A}/pkgconfig" "${PREFIX_H}/harfbuzz"
	cp -a "${b}/libharfbuzz-icu.a" "${PREFIX_A}/"
	cp -a "${PREFIX_PORT_WORKDIR}/src/hb-icu.h" "${PREFIX_H}/harfbuzz/"

	# As the harfbuzz port writes harfbuzz.pc: the CMake build installs no .pc.
	cat >"${PREFIX_A}/pkgconfig/harfbuzz-icu.pc" <<EOF
prefix=${PREFIX_PORT_INSTALL}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: harfbuzz-icu
Description: HarfBuzz text shaping library ICU integration
Version: ${version}
Requires: harfbuzz
Requires.private: icu-uc
Libs: -L\${libdir} -lharfbuzz-icu
Cflags: -I\${includedir}/harfbuzz
EOF
}
