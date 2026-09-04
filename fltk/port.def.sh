#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="fltk"
	version="1.3.10"
	desc="Fast Light Toolkit — small static C++ GUI toolkit (X11 client, prereq for Dillo)"
	cpe23="cpe:2.3:a:fltk:fltk:${version}:*:*:*:*:*:*:*"

	source="https://www.fltk.org/pub/fltk/${version}"
	archive_filename="fltk-${version}-source.tar.gz"
	src_path="fltk-${version}/"

	size="5573019"
	sha256="c1c96d4f2ca7844f4b7945b4670aff2846f150cd5f3e23e3e4c70a61807108c7"

	# FLTK is LGPL-2.0 with a static-linking exception (see COPYING). Ports/recipes
	# may carry (L)GPL; only the Phoenix core repos must stay GPL-free.
	license="LGPL-2.0-only"
	license_file="COPYING"

	conflicts=""
	# X client stack + image codecs, all now framework ports (xorg_libs provides
	# libX11/libxcb/libXau/libXdmcp/libXext; zlib is pulled transitively by libpng).
	depends="xorg_libs libpng libjpeg"

	supports="phoenix>=3.3"
}

p_prepare() {
	# No source patches: FLTK 1.3.10 ships a phoenix-aware config.sub and
	# cross-compiles cleanly. configure lives in p_build (CROSS available there,
	# matching the sdl2 port). 
	:
}

p_build() {
	# FLTK is a C++ X11 client toolkit. Link against the framework X client stack
	# + image libs installed in the target prefix (PREFIX_H/PREFIX_A). The shim
	#
	# DISABLED: gl (no GLX on the fbdev X stack yet), xft/xinerama/xcursor/xfixes/
	# xdbe (those X extension libs are not in the stack; FLTK falls back to core
	# XLFD fonts + core cursors), and the bundled local png/jpeg/zlib (use the
	# framework ones for a consistent image stack). The ac_cv_lib_png_* cache vars
	# defeat FLTK's png probe, which links `-lpng` without `-lz` and thus gets a
	# static-link false negative against the prefix libpng16.
	local xcflags="${CFLAGS} -I${PREFIX_H}"
	# Fold CFLAGS into LDFLAGS so configure's cross link probes carry sysroot/-mcpu.
	local xldflags="${CFLAGS} ${LDFLAGS} -L${PREFIX_A}"

	if [ ! -f "${PREFIX_PORT_WORKDIR}/config.status" ]; then
		(cd "${PREFIX_PORT_WORKDIR}" && ./configure \
			--host="${HOST}" --build=x86_64-pc-linux-gnu \
			--prefix="${PREFIX_PORT_INSTALL}" \
			--enable-static --disable-shared \
			--disable-gl --disable-xft --disable-xinerama --disable-xcursor \
			--disable-xfixes --disable-xdbe \
			--disable-localpng --disable-localjpeg --disable-localzlib \
			--x-includes="${PREFIX_H}" --x-libraries="${PREFIX_A}" \
			CC="${CROSS}gcc" CXX="${CROSS}g++" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
			CFLAGS="${xcflags}" CXXFLAGS="${xcflags}" CPPFLAGS="${xcflags}" LDFLAGS="${xldflags}" \
			ac_cv_lib_png_png_read_info=yes \
			ac_cv_lib_png_png_get_valid=yes \
			ac_cv_lib_png_png_set_tRNS_to_alpha=yes)
	fi

	# Build only the libraries (src/). The top-level "all" also builds fluid and
	# the test/ programs, some of which try to RUN the freshly cross-built fluid
	# on the host and break the build. We ship the .a files + headers only.
	make -C "${PREFIX_PORT_WORKDIR}/src"

	mkdir -p "${PREFIX_A}" "${PREFIX_H}/FL"
	cp -a "${PREFIX_PORT_WORKDIR}"/lib/libfltk*.a "${PREFIX_A}/"
	cp -a "${PREFIX_PORT_WORKDIR}/FL/." "${PREFIX_H}/FL/"
	[ -f "${PREFIX_PORT_WORKDIR}/fltk-config" ] && \
		install -m 755 "${PREFIX_PORT_WORKDIR}/fltk-config" "${PREFIX_PORT_INSTALL}/bin/fltk-config" 2>/dev/null || true
}
