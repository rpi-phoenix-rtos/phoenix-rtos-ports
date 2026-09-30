#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="libxshmfence_phoenix"
	version="1.3.2"
	desc="libxshmfence with the Phoenix-RTOS backend (DRI3 fences as polled words in shmsrv memory)"

	source="https://www.x.org/releases/individual/lib"
	archive_filename="libxshmfence-${version}.tar.xz"
	src_path="libxshmfence-${version}/"

	size="259024"
	sha256="870df257bc40b126d91b5a8f1da6ca8a524555268c50b59c0acd1a27f361606f"

	# upstream MIT; the Phoenix-RTOS backend (patches/0001, xshmfence_phoenix.c) MIT as well
	license="MIT"
	license_file="COPYING"

	# NEW GPU LANE: private install prefix (see libdrm_phoenix)
	conflicts="libxshmfence_phoenix!=${version}"
	# wayland: shmsrv's wire header shm_proto.h (the fence pages are allocated from /shm;
	# the wayland port builds and installs shmsrv); xorg_libs: X11/Xfuncproto.h (xorgproto);
	# libdrm_phoenix: the new-lane build helpers
	depends="libdrm_phoenix wayland xorg_libs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/x11-drm/build.sh (its --xshmfence-only
# part): the same patch, the same two-source compile without configure, the same flags.
# Xorg-drm (xorg_server_drm) and every DRI3 client (eglx11-demo, Mesa's x11 platform) must
# link THIS archive: the struct xshmfence layout is shared across processes.
#
#   ${PREFIX_PORT_INSTALL}/lib/libxshmfence.a, include/X11/xshmfence.h, lib/pkgconfig/xshmfence.pc

p_prepare() {
	# 0001: the Phoenix-RTOS backend (xshmfence_phoenix.c): a polled word in shmsrv memory
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local shm_inc="${PORT_DEP_wayland}/include" x11_inc="${PORT_DEP_xorg_libs}/include"
	local sp="${PREFIX_PORT_INSTALL}" o="${PREFIX_PORT_BUILD}/xshmfence-obj" f s
	[ -f "${shm_inc}/shm_proto.h" ] || b_die "no ${shm_inc}/shm_proto.h (wayland port: shmsrv)"
	[ -f "${x11_inc}/X11/Xfuncproto.h" ] || b_die "no ${x11_inc}/X11/Xfuncproto.h (xorg_libs port)"

	rm -rf "${o}" "${sp}/lib/libxshmfence.a"
	mkdir -p "${o}" "${sp}/lib/pkgconfig" "${sp}/include/X11"
	# No configure: two sources, the backend chosen by HAVE_PHOENIX_FENCE (mkostemp/SHMDIR only
	# for the warned /tmp fallback). -idirafter the ports include/ is safe here: libc +
	# X11/Xfuncproto.h only.
	for f in xshmfence_alloc xshmfence_phoenix; do
		"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -DHAVE_PHOENIX_FENCE=1 -DHAVE_MKOSTEMP=1 \
			-DSHMDIR='"/tmp"' -I"${PREFIX_PORT_WORKDIR}/src" -I"${shm_inc}" -idirafter "${x11_inc}" \
			-c "${PREFIX_PORT_WORKDIR}/src/${f}.c" -o "${o}/${f}.o"
	done
	# D = deterministic: the same bytes every build (Xorg-drm and the clients record its sha256).
	"${NL_AR}" rcsD "${sp}/lib/libxshmfence.a" "${o}"/*.o
	cp "${PREFIX_PORT_WORKDIR}/src/xshmfence.h" "${sp}/include/X11/"
	# shellcheck disable=SC2016 # ${prefix}-style variables are pkg-config's
	printf '%s\n' "prefix=${sp}" 'libdir=${prefix}/lib' 'includedir=${prefix}/include' '' \
		'Name: xshmfence' 'Description: X shared memory fences (Phoenix-RTOS backend: polled word in shmsrv memory)' \
		"Version: ${version}" 'Libs: -L${libdir} -lxshmfence' 'Cflags: -I${includedir}' > "${sp}/lib/pkgconfig/xshmfence.pc"

	local syms
	syms="$("${NL_NM}" -g --defined-only "${sp}/lib/libxshmfence.a" 2>/dev/null || true)"
	for s in xshmfence_alloc_shm xshmfence_map_shm xshmfence_await xshmfence_trigger xshmfence_phoenix_alloc_shm; do
		grep -qE " T ${s}\$" <<< "${syms}" || b_die "${s} missing from libxshmfence.a"
	done
	if grep -qE ' U pthread_' <<< "$("${NL_NM}" "${sp}/lib/libxshmfence.a" 2>/dev/null)"; then
		b_die "libxshmfence.a still references pthread (wrong backend)"
	fi
	echo "libxshmfence_phoenix: $(stat -c %s "${sp}/lib/libxshmfence.a") bytes, backend=phoenix (no pthread refs)"
}
