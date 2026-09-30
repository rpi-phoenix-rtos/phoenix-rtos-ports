#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="wayland"
	version="1.24.0"
	desc="libwayland 1.24 + wayland-protocols 1.45 + the wlphx-compat libphoenix-gap library + shmsrv"

	source="https://gitlab.freedesktop.org/wayland/wayland/-/releases/${version}/downloads"
	archive_filename="wayland-${version}.tar.xz"
	src_path="wayland-${version}/"

	size="241764"
	sha256="82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536"

	# libwayland and wayland-protocols are MIT; wlphx-compat (glue/compat/) and shmsrv
	# (glue/shmsrv/) are our code, BSD-3-Clause.
	license="MIT AND BSD-3-Clause"
	license_file="COPYING"

	# NEW GPU LANE: private install prefix (see libdrm_phoenix).
	conflicts="wayland!=${version}"
	# libdrm_phoenix: only the build helpers (newlane.subr); libdrm itself is not linked here.
	depends="libdrm_phoenix libffi"

	# rootfs: also install /bin/shmsrv into the image (opt-in)
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the libwayland half of the coordination repo's tools/gpu-lane/weston-drm/build.sh
# (same tarballs + sha256, same patch, same meson options and compiler flags). What a consumer
# finds in ${PORT_DEP_wayland}:
#
#   lib/libwayland-{server,client,egl,cursor}.a, include/wayland-*.h, lib/pkgconfig/wayland-*.pc
#                          (wayland-server.pc / wayland-client.pc require wlphx-compat)
#   share/wayland-protocols/, share/pkgconfig/wayland-protocols.pc
#   lib/libwlphx-compat.a, lib/pkgconfig/wlphx-compat.pc   epoll/timerfd/signalfd/eventfd/ppoll,
#                          memfd_create over shmsrv, msync/pipe2 stand-ins; its Libs carry
#                          -Wl,--wrap=close -Wl,--wrap=write (the emulated event-loop descriptors)
#   compat/include/        the compat headers (epoll.h, timerfd.h, ... -- every consumer compiles
#                          with -I on it, followed by mesa-compat/include)
#   mesa-compat/include/   mesa_drm's generic libphoenix-gap headers (sys/file.h LOCK_*,
#                          static_assert, SCNxPTR, ...): libwayland is built before Mesa (Mesa's
#                          wayland platform needs it), so a copy of mesa_drm glue/compat/include
#   deps/libffi/           a private view of the ports prefix's libffi (header poisoning: the
#                          ports include/ also holds the old lane's GL/ and X11/ headers)
#   include/shm_proto.h, bin/shmsrv (+ prog/shmsrv unstripped)   the /shm server: memfd_create's
#                          backing (wl_shm pools, keymaps, xshmfence pages); a server, it moves to
#                          phoenix-rtos-devices with rpi4-kms / rpi4-v3d-async (MIGRATION.md 4.7)
#
# shmsrv lives here, not in a port of its own: a port needs an upstream archive and shmsrv has
# none; this is the lowest port that needs its wire header (glue/compat/src/wlphx_memfd.c).
#
# Host tools: meson, ninja and wayland-scanner, which must be exactly 1.24.0 (it generates
# the protocol code compiled into libwayland).

WAYLAND_PROTOCOLS_VERSION="1.45"
WAYLAND_PROTOCOLS_SHA256="4d2b2a9e3e099d017dc8107bf1c334d27bb87d9e4aff19a0c8d856d17cd41ef0"

p_prepare() {
	# 0001 os: Phoenix-RTOS peer credentials and MSG_CMSG_CLOEXEC
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" wayland

	local f="wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}.tar.xz"
	b_port_download "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/${WAYLAND_PROTOCOLS_VERSION}/downloads/" "${f}"
	echo "${WAYLAND_PROTOCOLS_SHA256}  ${PREFIX_PORT}/${f}" | sha256sum -c --quiet - || b_die "${f}: sha256 mismatch"
	if [ ! -d "${PREFIX_PORT_BUILD}/wayland-protocols" ]; then
		rm -rf "${PREFIX_PORT_BUILD}/wayland-protocols.tmp"
		mkdir -p "${PREFIX_PORT_BUILD}/wayland-protocols.tmp"
		tar -xf "${PREFIX_PORT}/${f}" -C "${PREFIX_PORT_BUILD}/wayland-protocols.tmp" --strip-components=1
		mv "${PREFIX_PORT_BUILD}/wayland-protocols.tmp" "${PREFIX_PORT_BUILD}/wayland-protocols"
	fi
}

# _wl_meson <cross file> <name> <source dir> <meson args...>   (tools: meson_pkg)
_wl_meson() {
	local cross="$1" name="$2" src="$3" bd="${PREFIX_PORT_BUILD}/$2-build"
	shift 3
	if [ ! -f "${bd}/build.ninja" ]; then
		rm -rf "${bd}"
		meson setup "${bd}" "${src}" --cross-file "${cross}" --prefix "${PREFIX_PORT_INSTALL}" \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false --wrap-mode=nodownload "$@"
	fi
	ninja -C "${bd}"
	ninja -C "${bd}" install
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local t
	for t in meson ninja wayland-scanner; do
		command -v "${t}" > /dev/null || b_die "host tool ${t} not found"
	done
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "${version}" ] || b_die "host wayland-scanner is not ${version}"
	for t in memExport sys_fdpath; do
		nl_has_libc "${t}" || b_die "${NL_SYSROOT}/lib/libphoenix.a has no ${t} (stale sysroot)"
	done

	local P="${PREFIX_PORT_INSTALL}" D="${PREFIX_PORT_INSTALL}/deps"
	local compat_inc="${PREFIX_PORT}/glue/compat/include" mesa_compat_inc="${PREFIX_PORT}/glue/mesa-compat/include"
	mkdir -p "${P}/lib/pkgconfig" "${P}/include" "${D}"

	# --- private view of the ports prefix's libffi (tools: dep_view) ---------------------------
	local ffi="${PORT_DEP_libffi}" ffiver
	ffiver="$(sed -n 's/^Version: //p' "${ffi}/lib/pkgconfig/libffi.pc")"
	rm -rf "${D}/libffi"
	mkdir -p "${D}/libffi/include" "${D}/libffi/lib/pkgconfig"
	cp "${ffi}/include/ffi.h" "${ffi}/include/ffitarget.h" "${D}/libffi/include/"
	cp "${ffi}/lib/libffi.a" "${D}/libffi/lib/"
	printf '%s\n' "prefix=${D}/libffi" "Name: libffi" "Description: libffi from the Phoenix ports prefix" \
		"Version: ${ffiver}" "Libs: -L\${prefix}/lib -lffi" "Cflags: -I\${prefix}/include" \
		> "${D}/libffi/lib/pkgconfig/libffi.pc"

	# --- wlphx-compat: epoll/timerfd/signalfd, memfd_create over shmsrv -------------------------
	local defs=() f o="${PREFIX_PORT_BUILD}/compat-obj" objs=()
	nl_has_libc msync || defs+=(-DWLPHX_NEED_MSYNC)
	nl_has_libc pipe2 || defs+=(-DWLPHX_NEED_PIPE2)
	rm -rf "${o}"
	mkdir -p "${o}"
	for f in wlphx_epoll wlphx_memfd wlphx_misc; do
		"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${compat_inc}" -I"${mesa_compat_inc}" \
			"${defs[@]}" -c "${PREFIX_PORT}/glue/compat/src/${f}.c" -o "${o}/${f}.o"
		objs+=("${o}/${f}.o")
	done
	echo "wayland: compat stand-ins: ${defs[*]:-none}"
	rm -f "${P}/lib/libwlphx-compat.a"
	# exactly the three objects: shmsrv.o (below, with its main()) must never land in the library
	"${NL_AR}" rcs "${P}/lib/libwlphx-compat.a" "${objs[@]}"
	cat > "${P}/lib/pkgconfig/wlphx-compat.pc" <<EOF
prefix=${P}
Name: wlphx-compat
Description: weston-drm libphoenix-gap shims (epoll, timerfd, signalfd, eventfd, ppoll, memfd_create, msync)
Version: 1.0
Libs: -Wl,-u,__wrap_close -Wl,-u,__wrap_write -L\${prefix}/lib -lwlphx-compat -Wl,--wrap=close -Wl,--wrap=write
Cflags:
EOF
	rm -rf "${P}/compat" "${P}/mesa-compat"
	mkdir -p "${P}/compat" "${P}/mesa-compat"
	cp -a "${compat_inc}" "${P}/compat/include"
	cp -a "${mesa_compat_inc}" "${P}/mesa-compat/include"

	# --- meson cross files + pkg-config ---------------------------------------------------------
	# compat/include first (epoll/timerfd/signalfd/memfd/sealing), then mesa-drm's generic
	# libphoenix-gap shims. The "weston" variant's link probes see the compat archive, so
	# libwayland-cursor's anonymous files (cursor theme pools) use memfd_create = shmsrv.
	local pkgc="${PREFIX_PORT_BUILD}/nl/pkg-config" cx lpre
	nl_pkgconfig "${pkgc}" "${P}/lib/pkgconfig:${P}/share/pkgconfig:${D}/libffi/lib/pkgconfig"
	cx="'-I${compat_inc}', '-I${mesa_compat_inc}'"
	lpre="'-L${PREFIX_BUILD%/}/lib'"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross.txt" "${pkgc}" "${cx}" "${lpre}"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross-weston.txt" "${pkgc}" "${cx}" "${lpre}" \
		"'-Wl,-u,__wrap_close', '-Wl,-u,__wrap_write', '${P}/lib/libwlphx-compat.a', '-Wl,--wrap=close', '-Wl,--wrap=write'"

	# --- libwayland (the scanner runs on the host) ----------------------------------------------
	_wl_meson "${PREFIX_PORT_BUILD}/nl/cross-weston.txt" wayland "${PREFIX_PORT_WORKDIR}" \
		-Dlibraries=true -Dscanner=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false
	local pc
	for pc in "${P}/lib/pkgconfig/wayland-server.pc" "${P}/lib/pkgconfig/wayland-client.pc"; do
		grep -q 'wlphx-compat' "${pc}" || printf 'Requires.private: wlphx-compat\n' >> "${pc}"
	done

	# --- wayland-protocols ----------------------------------------------------------------------
	_wl_meson "${PREFIX_PORT_BUILD}/nl/cross.txt" wayland-protocols "${PREFIX_PORT_BUILD}/wayland-protocols" -Dtests=false

	for t in libwayland-server libwayland-client libwayland-egl libwayland-cursor; do
		[ -f "${P}/lib/${t}.a" ] || b_die "${t}.a not installed"
	done
	nl_has_sym "${P}/lib/libwayland-server.a" wl_display_create || b_die "libwayland-server.a: no wl_display_create"
	! nl_has_sym "${P}/lib/libwlphx-compat.a" main || b_die "libwlphx-compat.a defines main()"
	for t in memfd_create epoll_wait timerfd_settime signalfd eventfd __wrap_close __wrap_write; do
		nl_has_sym "${P}/lib/libwlphx-compat.a" "${t}" || b_die "libwlphx-compat.a: no ${t}"
	done
	[ -f "${P}/share/pkgconfig/wayland-protocols.pc" ] || b_die "wayland-protocols.pc not installed"

	# --- shmsrv: the /shm server (memfd_create backing) -----------------------------------------
	local so="${PREFIX_PORT_BUILD}/shmsrv-obj"
	mkdir -p "${P}/bin" "${P}/prog" "${so}"
	cp "${PREFIX_PORT}/glue/shmsrv/shm_proto.h" "${P}/include/"
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -c "${PREFIX_PORT}/glue/shmsrv/shmsrv.c" \
		-o "${so}/shmsrv.o"
	"${NL_CC}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -o "${P}/prog/shmsrv" "${so}/shmsrv.o"
	"${NL_STRIP}" -o "${P}/bin/shmsrv" "${P}/prog/shmsrv"
	nl_no_undefined "${P}/prog/shmsrv"
	nl_forbid_old_lane "${P}/bin/shmsrv" /dev/v3d-srv Xphoenix '[fbdev]' glamor_phoenix phxgl

	if b_use rootfs; then
		b_install "${P}/bin/shmsrv" /bin
	fi
}
