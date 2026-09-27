#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libepoxy"
	version="1.5.10"
	desc="libepoxy (GL/EGL dispatch) built static with a static-EGL dispatch patch, against mesa_drm (new GPU lane)"

	# GitHub tag archive, served as "1.5.10.tar.gz", saved under the descriptive name
	source="https://github.com/anholt/libepoxy/archive/refs/tags"
	archive_filename=("libepoxy-${version}.tar.gz" "${version}.tar.gz")
	src_path="libepoxy-${version}/"

	size="332078"
	sha256="a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15"

	license="MIT"
	license_file="COPYING"

	# NEW GPU LANE: private install prefix (see libdrm_phoenix)
	conflicts="libepoxy!=${version}"
	# mesa_drm: EGL/GLES headers + egl.pc of the default GLES build (glamor's Mesa);
	# xorg_libs: the ports' .pc search path the tools build had (nothing X11 is configured)
	depends="libdrm_phoenix mesa_drm zlib xorg_libs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/xorg-drm/build.sh (its libepoxy step):
# the same patch, meson options, cross file (compat include = xorg-drm/compat/include, here
# glue/compat-include: an identical copy of xorg_server_drm/glue/compat/include) and
# pkg-config search path. A port of its own because Xorg-drm (xorg_server_drm) and the
# Wayland-desktop ports (GTK 3) both link it.
#
#   ${PREFIX_PORT_INSTALL}/lib/libepoxy.a, include/epoxy/, lib/pkgconfig/epoxy.pc

p_prepare() {
	# 0001: resolve GL/EGL entry points through the statically linked Mesa (no dlopen)
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local B="${PORT_DEP_xorg_libs%/}" M="${PORT_DEP_mesa_drm}" dp="${PREFIX_PORT_INSTALL}"
	local ldpc="${PORT_DEP_libdrm_phoenix}/lib/pkgconfig"
	[ -f "${M}/gles/prefix/lib/pkgconfig/egl.pc" ] || b_die "no ${M}/gles/prefix/lib/pkgconfig/egl.pc (mesa_drm)"
	# the tools build's search path, in its order: deps prefix, libdrm, Mesa (GLES build),
	# Mesa's zlib view, the ports prefix (X11 libraries only; its include/ has no GL/EGL/GLES/
	# gbm/drm headers -- xorg_server_drm checks that)
	nl_pkgconfig "${PREFIX_PORT_BUILD}/nl/pkg-config" \
		"${dp}/lib/pkgconfig:${dp}/share/pkgconfig:${ldpc}:${M}/gles/prefix/lib/pkgconfig:${M}/zlib-prefix/lib/pkgconfig:${B}/lib/pkgconfig:${B}/share/pkgconfig"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross.txt" "${PREFIX_PORT_BUILD}/nl/pkg-config" \
		"'-I${PREFIX_PORT}/glue/compat-include'" "'-L${B}/lib'"

	local bd="${PREFIX_PORT_BUILD}/epoxy-build"
	if [ ! -f "${bd}/build.ninja" ]; then
		meson setup "${bd}" "${PREFIX_PORT_WORKDIR}" --cross-file "${PREFIX_PORT_BUILD}/nl/cross.txt" \
			--prefix "${dp}" --libdir lib --buildtype=debugoptimized -Db_ndebug=true -Db_staticpic=false \
			-Dglx=no -Degl=yes -Dx11=false -Dtests=false -Ddocs=false
	fi
	ninja -C "${bd}"
	ninja -C "${bd}" install
	[ -f "${dp}/lib/libepoxy.a" ] || b_die "libepoxy.a not installed"
	nl_has_sym "${dp}/lib/libepoxy.a" epoxy_static_proc_address || b_die "libepoxy.a: no epoxy_static_proc_address (patch 0001)"
}
