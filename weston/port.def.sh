#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="weston"
	version="14.0.2"
	desc="Weston 14 (DRM backend, GL renderer, kiosk shell; static) + libxkbcommon, libdisplay-info, libseat, the libinput/libudev/libevdev shims (new GPU lane)"

	# Aggregate port (as xorg_fonts): the anchor is Weston; libxkbcommon 1.7.0,
	# libdisplay-info 0.2.0, seatd 0.9.1 and libinput 1.26.2 (its libinput.h only) are fetched
	# and sha256-checked in p_prepare.
	source="https://gitlab.freedesktop.org/wayland/weston/-/releases/${version}/downloads"
	archive_filename="weston-${version}.tar.xz"
	src_path="weston-${version}/"

	size="2043392"
	sha256="b47216b3530da76d02a3a1acbf1846a9cd41d24caa86448f9c46f78f20b6e0ac"

	# Weston, libxkbcommon, libdisplay-info, seatd, libinput.h: MIT. The shims, weston_builtin.c
	# and the Pi launcher are our code (BSD-3-Clause); input-event-codes.h (FreeBSD): BSD-2-Clause.
	license="MIT AND BSD-3-Clause AND BSD-2-Clause"
	license_file="COPYING"

	# NEW GPU LANE: private install prefix (see libdrm_phoenix).
	conflicts="weston!=${version}"
	# pixman comes from xorg_libs, expat from xorg_fonts (both in the shared ports prefix, used
	# through private views); libffi through the wayland port's view.
	depends="wayland mesa_drm[wayland] libdrm_phoenix xorg_libs xorg_fonts zlib"

	# rootfs: also install weston, weston-simple-shm, weston-simple-egl, /bin/weston-m6a.sh and
	# /etc/xdg/weston/weston-drm.ini into the image (opt-in; shmsrv comes from the wayland port)
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/weston-drm/build.sh (the Weston half; the
# libwayland half, the wlphx-compat library and shmsrv are the `wayland` port). Same tarballs +
# sha256, same patches (patches/{weston,seatd}), same meson options, compiler flags, link lines
# and checks. What this port installs in ${PREFIX_PORT_INSTALL}:
#
#   lib/{libxkbcommon,libdisplay-info,libseat,libudev,libinput,libevdev}.a + headers + .pc
#                        (libudev/libinput/libevdev are the shims: a fixed device table,
#                        libinput-phoenix over usbkbd/usbmouse, libevdev_event_code_from_name;
#                        include/linux/input.h is built over FreeBSD's input-event-codes.h)
#   share/keymap-us.xkb  the baked default keymap (evdev/pc105/us, compiled on the HOST:
#                        Phoenix has no xkeyboard-config)
#   bin/{weston,weston-simple-shm,weston-simple-egl} (stripped), prog/ (unstripped + .map)
#   share/weston/{weston-drm.ini,weston-m6a.sh}
#
# Host tools: meson, ninja, bison, wayland-scanner 1.24.0, python3, and xkbcli or
# xkeyboard-config (/usr/share/X11/xkb; a host libxkbcommon is built for xkbcli when missing).
#
# Links are by hand (as Xorg-drm / kmscube): meson's own executables would need Mesa's full
# static closure, which Mesa's .pc files do not describe. Every program is linked
# -Wl,--wrap=close -Wl,--wrap=write (the compat event loop descriptors), the DRM ones also
# -Wl,--wrap=mmap -Wl,--wrap=ioctl (libdrm-phoenix: BO-token maps, emulated sync files).

WESTON_SUBPKGS=(
	"libxkbcommon|libxkbcommon-1.7.0.tar.xz|https://xkbcommon.org/download/|libxkbcommon-1.7.0.tar.xz|65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247"
	"libdisplay-info|libdisplay-info-0.2.0.tar.xz|https://gitlab.freedesktop.org/emersion/libdisplay-info/-/releases/0.2.0/downloads/|libdisplay-info-0.2.0.tar.xz|5a2f002a16f42dd3540c8846f80a90b8f4bdcd067a94b9d2087bc2feae974176"
	"seatd|seatd-0.9.1.tar.gz|https://git.sr.ht/~kennylevinsen/seatd/archive/|0.9.1.tar.gz|819979c922a0be258aed133d93920bce6a3d3565a60588d6d372ce9db2712cd3"
	"libinput|libinput-1.26.2.tar.gz|https://gitlab.freedesktop.org/libinput/libinput/-/archive/1.26.2/|libinput-1.26.2.tar.gz|5c1c4150f217fea1db2d1fd88e2607b2f1928cfde65c34da65a9f24dcfd69464"
)
# FreeBSD's BSD-2 copy of the evdev event codes (<linux/input.h> shim, glue/shims/include/linux/input.h)
WESTON_EVDEV_CODES_COMMIT="f492ef8318f580081047da41905c3b339e924387"
WESTON_EVDEV_CODES_SHA="fc9c4946818cefcec359ad3a619d448c4cafffa4f8f9571894516fb980b26142"
WESTON_WAYLAND_VERSION="1.24.0"

p_prepare() {
	# Weston 0001-0008 (builtin module table, static meson build, builtin XKB keymap, ...)
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" weston

	local rec name file base orig sum dir
	for rec in "${WESTON_SUBPKGS[@]}"; do
		IFS='|' read -r name file base orig sum <<< "${rec}"
		b_port_download "${base}" "${file}" "${orig}"
		echo "${sum}  ${PREFIX_PORT}/${file}" | sha256sum -c --quiet - || b_die "${file}: sha256 mismatch"
		dir="${PREFIX_PORT_BUILD}/src/${name}"
		# a patched tree whose patch markers are gone (the framework resets them when it
		# re-extracts the anchor) is re-extracted too, so the patches apply to pristine source
		if [ -d "${PREFIX_PORT}/patches/${name}" ] && [ ! -d "${PREFIX_BUILD_MARKERS}/${name}" ]; then
			rm -rf "${dir}"
		fi
		if [ ! -d "${dir}" ]; then
			rm -rf "${dir}.tmp"
			mkdir -p "${dir}.tmp"
			tar -xf "${PREFIX_PORT}/${file}" -C "${dir}.tmp" --strip-components=1
			mv "${dir}.tmp" "${dir}"
		fi
		# only seatd carries patches (0001-0004: librt optional, no MSG_CMSG_CLOEXEC,
		# noop-backend-only libseat, no unit tests in cross builds)
		if [ -d "${PREFIX_PORT}/patches/${name}" ]; then
			b_port_apply_patches "${dir}" "${name}"
		fi
	done
}

# _weston_meson [--cross <file>] <name> <meson args...>   (tools: meson_pkg)
_weston_meson() {
	local cross="${PREFIX_PORT_BUILD}/nl/cross.txt"
	if [ "$1" = --cross ]; then cross="$2"; shift 2; fi
	local name="$1" bd="${PREFIX_PORT_BUILD}/$1-build"
	shift
	if [ ! -f "${bd}/build.ninja" ]; then
		rm -rf "${bd}"
		meson setup "${bd}" "${PREFIX_PORT_BUILD}/src/${name}" --cross-file "${cross}" --prefix "${PREFIX_PORT_INSTALL}" \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false --wrap-mode=nodownload "$@"
	fi
	ninja -C "${bd}"
	ninja -C "${bd}" install
}

# _weston_view <name> <lib basename> <version> <include subdir> <headers...>   (tools: dep_view)
#   a private view of one library of the shared ports prefix (${NL_VIEW_SRC}): exactly its
#   headers -- the ports include/ also holds the old lane's GL/ and X11/ headers
_weston_view() {
	local name="$1" lib="$2" ver="$3" sub="$4" h d="${PREFIX_PORT_INSTALL}/deps/$1"
	shift 4
	rm -rf "${d}"
	mkdir -p "${d}/include/${sub}" "${d}/lib/pkgconfig"
	for h in "$@"; do cp "${NL_VIEW_SRC}/include/${h}" "${d}/include/${sub}/"; done
	cp "${NL_VIEW_SRC}/lib/lib${lib}.a" "${d}/lib/"
	printf '%s\n' "prefix=${d}" "Name: ${name}" "Description: ${name} from the Phoenix ports prefix" \
		"Version: ${ver}" "Libs: -L\${prefix}/lib -l${lib}" "Cflags: -I\${prefix}/include${sub:+/${sub}}" \
		> "${d}/lib/pkgconfig/${name}.pc"
}

# _weston_mesa_private <archive> -> the path to link
#   Mesa's util/anon_file.c and Weston's shared/os-compatibility.c both export
#   os_create_anonymous_file() -- with different signatures (hidden from each other in shared
#   builds). Private copies of the Mesa archives that define or call it get Mesa's renamed;
#   everything else is linked in place.
_weston_mesa_private() {
	local a="$1" c ml="${PREFIX_PORT_BUILD}/mesa-link" mv="${PORT_DEP_mesa_drm}/wayland"
	# (no `nm | grep -q` under pipefail: grep's early exit SIGPIPEs nm and fails the test)
	if ! grep -q ' os_create_anonymous_file$' <<< "$("${NL_NM}" "${a}" 2>/dev/null)"; then
		echo "${a}"
		return
	fi
	mkdir -p "${ml}"
	c="${ml}/$(echo "${a#"${mv}"/}" | tr '/' '_')"
	if [ ! -f "${c}" ] || [ "${a}" -nt "${c}" ]; then
		if [ "$(head -c 8 "${a}")" = '!<thin>' ]; then
			# meson's internal libraries are thin archives (objcopy cannot copy them):
			# rebuild a regular archive from the renamed members
			local t="${c}.d" i=0 m
			rm -rf "${t}" "${c}"
			mkdir -p "${t}"
			while IFS= read -r m; do
				case "${m}" in /*) ;; *) m="$(dirname "${a}")/${m}" ;; esac
				i=$((i + 1))
				"${NL_OBJCOPY}" --redefine-sym os_create_anonymous_file=mesa_os_create_anonymous_file "${m}" \
					"${t}/$(printf '%04d' "${i}")-$(basename "${m}")"
			done < <("${NL_AR}" t "${a}")
			"${NL_AR}" rcs "${c}" "${t}"/*.o
			rm -rf "${t}"
		else
			"${NL_OBJCOPY}" --redefine-sym os_create_anonymous_file=mesa_os_create_anonymous_file "${a}" "${c}"
		fi
	fi
	echo "${c}"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"
	nl_host_python "${PREFIX_PORT_BUILD}/nl"

	local t
	for t in meson ninja wayland-scanner bison; do
		command -v "${t}" > /dev/null || b_die "host tool ${t} not found"
	done
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "${WESTON_WAYLAND_VERSION}" ] \
		|| b_die "host wayland-scanner is not ${WESTON_WAYLAND_VERSION}"
	for t in memExport sys_fdpath; do
		nl_has_libc "${t}" || b_die "${NL_SYSROOT}/lib/libphoenix.a has no ${t} (stale sysroot)"
	done

	local P="${PREFIX_PORT_INSTALL}" D="${PREFIX_PORT_INSTALL}/deps" WL="${PORT_DEP_wayland}"
	local MV="${PORT_DEP_mesa_drm}/wayland"
	local COMPAT_INC="${WL}/compat/include" MESA_COMPAT_INC="${PORT_DEP_mesa_drm}/compat/include"
	local LD_PREFIX="${D}/libdrm" SHIM_INC="${PREFIX_PORT}/glue/shims/include"
	local compat_a="${WL}/lib/libwlphx-compat.a"
	mkdir -p "${P}/lib/pkgconfig" "${P}/include" "${P}/share" "${D}"
	[ -f "${MV}/link-gles.txt" ] || b_die "${MV}/link-gles.txt missing (mesa_drm built without USE wayland?)"

	# --- private dependency views + libdrm-phoenix view -----------------------------------------
	NL_VIEW_SRC="${PORT_DEP_xorg_fonts}"
	_weston_view expat expat "$(sed -n 's/^Version: //p' "${NL_VIEW_SRC}/lib/pkgconfig/expat.pc")" "" \
		expat.h expat_config.h expat_external.h
	NL_VIEW_SRC="${PORT_DEP_xorg_libs}"
	_weston_view pixman-1 pixman-1 "$(sed -n 's/^Version: //p' "${NL_VIEW_SRC}/lib/pkgconfig/pixman-1.pc")" pixman-1 \
		pixman-1/pixman.h pixman-1/pixman-version.h
	NL_VIEW_SRC="${PORT_DEP_zlib}"
	_weston_view zlib z "$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "${NL_VIEW_SRC}/include/zlib.h")" "" zlib.h zconf.h
	rm -rf "${LD_PREFIX}"
	mkdir -p "${LD_PREFIX}/lib/pkgconfig"
	cp -a "${PORT_DEP_libdrm_phoenix}/include" "${LD_PREFIX}/"
	cp -a "${PORT_DEP_libdrm_phoenix}/lib/libdrm.a" "${LD_PREFIX}/lib/"
	# The wraps every program that maps DRM buffers or uses emulated sync files needs (M3 2.6, M5 9.3).
	cat > "${LD_PREFIX}/lib/pkgconfig/libdrm.pc" <<EOF
prefix=${LD_PREFIX}
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: libdrm
Description: libdrm-phoenix (upstream libdrm + Phoenix backend)
Version: 2.4.134
Libs: -L\${libdir} -ldrm -Wl,--wrap=mmap -Wl,--wrap=ioctl
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF

	# --- meson cross files + pkg-config ---------------------------------------------------------
	# compat/include first (epoll/timerfd/signalfd/memfd/sealing), then mesa-drm's generic
	# libphoenix-gap shims. Weston's own configure probes (memfd_create, posix_fallocate...) and
	# the links meson does itself must see the compat archive: HAVE_MEMFD_CREATE routes
	# weston's anonymous files (wl_shm pools of its clients, keymaps) to shmsrv.
	local pkgc="${PREFIX_PORT_BUILD}/nl/pkg-config" cx lpre
	nl_pkgconfig "${pkgc}" "${P}/lib/pkgconfig:${P}/share/pkgconfig:${WL}/lib/pkgconfig:${WL}/share/pkgconfig:${WL}/deps/libffi/lib/pkgconfig:${D}/expat/lib/pkgconfig:${D}/pixman-1/lib/pkgconfig:${D}/zlib/lib/pkgconfig:${LD_PREFIX}/lib/pkgconfig:${MV}/prefix/lib/pkgconfig"
	cx="'-I${COMPAT_INC}', '-I${MESA_COMPAT_INC}'"
	lpre="'-L${PREFIX_BUILD%/}/lib'"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross.txt" "${pkgc}" "${cx}" "${lpre}"
	nl_meson_cross "${PREFIX_PORT_BUILD}/nl/cross-weston.txt" "${pkgc}" "${cx}" "${lpre}" \
		"'-Wl,-u,__wrap_close', '-Wl,-u,__wrap_write', '${compat_a}', '-Wl,--wrap=close', '-Wl,--wrap=write'"

	# --- libraries ------------------------------------------------------------------------------
	# libxkbcommon: no X11/wayland tools, no registry
	_weston_meson libxkbcommon -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
		-Denable-tools=false -Denable-xkbregistry=false -Denable-bash-completion=false \
		-Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale
	_weston_meson libdisplay-info
	# libseat: noop backend only (opens devices directly)
	_weston_meson seatd -Dlibseat-logind=disabled -Dlibseat-seatd=disabled -Dlibseat-builtin=disabled \
		-Dserver=disabled -Dexamples=disabled -Dman-pages=disabled -Dwerror=false

	# --- shims: libudev (fixed device table), libinput-phoenix (usbkbd/usbmouse), libevdev -----
	# Only libinput.h is used from the libinput tarball (MIT); the implementation is ours.
	local so="${PREFIX_PORT_BUILD}/shim-obj" evc="${P}/include/evdev/input-event-codes.h" repo_root
	mkdir -p "${P}/include/evdev" "${so}"
	if [ ! -f "${evc}" ] || ! echo "${WESTON_EVDEV_CODES_SHA}  ${evc}" | sha256sum -c --quiet - > /dev/null 2>&1; then
		repo_root="$(cd "${PREFIX_PORT}/../../.." && pwd)"
		if [ -d "${repo_root}/external/freebsd-src/.git" ] && \
				git -C "${repo_root}/external/freebsd-src" show "${WESTON_EVDEV_CODES_COMMIT}:sys/dev/evdev/input-event-codes.h" > "${evc}" 2> /dev/null; then
			:
		else
			curl -sSfL -o "${evc}" "https://raw.githubusercontent.com/freebsd/freebsd-src/${WESTON_EVDEV_CODES_COMMIT}/sys/dev/evdev/input-event-codes.h" \
				|| b_die "input-event-codes.h: download failed"
		fi
		echo "${WESTON_EVDEV_CODES_SHA}  ${evc}" | sha256sum -c --quiet - || b_die "input-event-codes.h sha256 mismatch"
	fi
	cp "${PREFIX_PORT_BUILD}/src/libinput/src/libinput.h" "${P}/include/libinput.h"
	mkdir -p "${P}/include/linux" "${P}/include/libevdev"
	cp "${SHIM_INC}"/linux/*.h "${P}/include/linux/"
	cp "${SHIM_INC}/libevdev/libevdev.h" "${P}/include/libevdev/"
	cp "${SHIM_INC}/libudev.h" "${P}/include/"
	local SFLAGS=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${P}/include" -I"${COMPAT_INC}" -I"${MESA_COMPAT_INC}")
	"${NL_CC}" "${SFLAGS[@]}" -c "${PREFIX_PORT}/glue/shims/src/udev_phoenix.c" -o "${so}/udev_phoenix.o"
	"${NL_CC}" "${SFLAGS[@]}" -c "${PREFIX_PORT}/glue/shims/src/libevdev_phoenix.c" -o "${so}/libevdev_phoenix.o"
	"${NL_CC}" "${SFLAGS[@]}" -c "${PREFIX_PORT}/glue/shims/src/libinput_phoenix.c" -o "${so}/libinput_phoenix.o"
	# phxhid_evdev_map.h: a copy of the coordination repo's tools/gpu-lane/xorg-drm/src one
	# (Xorg-drm's phxhid driver shares the HID -> evdev table)
	"${NL_CC}" "${SFLAGS[@]}" -I"${PREFIX_PORT}/glue/phxhid" \
		-c "${PREFIX_PORT}/glue/shims/src/libinput_phoenix_hid.c" -o "${so}/libinput_phoenix_hid.o"
	rm -f "${P}/lib/libudev.a" "${P}/lib/libinput.a" "${P}/lib/libevdev.a"
	"${NL_AR}" rcs "${P}/lib/libudev.a" "${so}/udev_phoenix.o"
	"${NL_AR}" rcs "${P}/lib/libinput.a" "${so}/libinput_phoenix.o" "${so}/libinput_phoenix_hid.o"
	"${NL_AR}" rcs "${P}/lib/libevdev.a" "${so}/libevdev_phoenix.o"
	_weston_shim_pc() {  # name version libs description
		printf '%s\n' "prefix=${P}" "Name: $1" "Description: $4" "Version: $2" "Libs: -L\${prefix}/lib $3" \
			"Cflags: -I\${prefix}/include" > "${P}/lib/pkgconfig/$1.pc"
	}
	_weston_shim_pc libudev 256 -ludev "weston-drm shim: libudev over a fixed device table"
	_weston_shim_pc libinput 1.26.2 "-linput -ludev" "weston-drm shim: libinput-phoenix (usbkbd, usbmouse)"
	_weston_shim_pc libevdev 1.13.0 -levdev "weston-drm shim: libevdev_event_code_from_name"

	# --- baked keymap: evdev/pc105/us compiled on the HOST (Phoenix has no xkeyboard-config) ------
	# A native libxkbcommon (same source) provides xkbcli-compile-keymap unless the host has xkbcli.
	local km="${P}/share/keymap-us.xkb" gen="${PREFIX_PORT_BUILD}/gen"
	mkdir -p "${gen}"
	if [ ! -s "${km}" ]; then
		if command -v xkbcli > /dev/null; then
			xkbcli compile-keymap --rules evdev --model pc105 --layout us > "${km}"
		else
			local hb="${PREFIX_PORT_BUILD}/host-xkbcommon-build"
			if [ ! -x "${hb}/xkbcli-compile-keymap" ]; then
				rm -rf "${hb}"
				meson setup "${hb}" "${PREFIX_PORT_BUILD}/src/libxkbcommon" --buildtype=release -Denable-x11=false \
					-Denable-wayland=false -Denable-docs=false -Denable-tools=true -Denable-xkbregistry=false \
					-Denable-bash-completion=false -Dxkb-config-root=/usr/share/X11/xkb
				ninja -C "${hb}" xkbcli-compile-keymap
			fi
			[ -d /usr/share/X11/xkb/rules ] || b_die "host has no xkeyboard-config (/usr/share/X11/xkb)"
			"${hb}/xkbcli-compile-keymap" --rules evdev --model pc105 --layout us > "${km}"
		fi
	fi
	grep -q 'xkb_keymap' "${km}" || b_die "${km} is not a keymap"
	python3 - "${km}" "${gen}/weston_keymap.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
with open(dst, 'w') as f:
    f.write('/* Generated by tools/gpu-lane/weston-drm/build.sh from xkbcli compile-keymap\n')
    f.write(' * --rules evdev --model pc105 --layout us (host xkeyboard-config). */\n')
    f.write('const char weston_builtin_xkb_keymap[] =\n')
    for line in text.splitlines():
        f.write('\t"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n')
    f.write('\t;\n')
PY

	# --- Weston: meson builds the archives and objects; the programs are linked below ----------
	local WB="${PREFIX_PORT_BUILD}/weston-build"
	local WESTON_TARGETS=(libweston/libweston-14.a frontend/libexec_weston.a libweston/backend-drm/drm-backend.a
		libweston/renderer-gl/gl-renderer.a kiosk-shell/kiosk-shell.a)
	if [ ! -f "${WB}/build.ninja" ]; then
		rm -rf "${WB}"
		meson setup "${WB}" "${PREFIX_PORT_WORKDIR}" --cross-file "${PREFIX_PORT_BUILD}/nl/cross-weston.txt" --prefix /usr \
			--buildtype=debugoptimized -Db_staticpic=false --wrap-mode=nodownload \
			-Dbackend-drm=true -Dbackend-drm-screencast-vaapi=false -Dbackend-headless=false \
			-Dbackend-pipewire=false -Dbackend-rdp=false -Dscreenshare=false -Dbackend-vnc=false \
			-Dbackend-wayland=false -Dbackend-x11=false -Dbackend-default=drm -Drenderer-gl=true \
			-Dxwayland=false -Dsystemd=false -Dremoting=false -Dpipewire=false \
			-Dshell-desktop=false -Dshell-ivi=false -Dshell-kiosk=true -Dshell-fullscreen=false \
			-Dcolor-management-lcms=false -Dimage-jpeg=false -Dimage-webp=false -Dtools=[] \
			-Ddemo-clients=false -Dsimple-clients=shm,egl -Dresize-pool=false -Dwcap-decode=false \
			-Dtests=false -Ddoc=false
	fi
	ninja -C "${WB}" "${WESTON_TARGETS[@]}"

	# --- link -------------------------------------------------------------------------------------
	#   gallium whole-archive (the DRI frontend + drivers), then one group of every other
	#   archive; -Wl,--wrap=mmap/ioctl for libdrm-phoenix, --wrap=close/write for the compat
	#   event loop descriptors (-u pulls them first).
	local WL_OBJ="${PREFIX_PORT_BUILD}/weston-obj" OFLAGS=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}")
	mkdir -p "${WL_OBJ}" "${P}/bin" "${P}/prog"
	"${NL_CC}" "${OFLAGS[@]}" -I"${gen}" -c "${PREFIX_PORT}/glue/src/weston_builtin.c" -o "${WL_OBJ}/weston_builtin.o"
	# The objects meson would link into an executable (its own link is not used).
	_weston_ninja_objs() {
		ninja -C "${WB}" -t query "$1" | awk '/input: c_LINKER/ {on = 1; next} /^  [a-z]/ {on = 0} on && $1 ~ /\.o$/ {print $1}'
	}
	local SHM_OBJS EGL_OBJS
	mapfile -t SHM_OBJS < <(_weston_ninja_objs clients/weston-simple-shm)
	mapfile -t EGL_OBJS < <(_weston_ninja_objs clients/weston-simple-egl)
	[ "${#SHM_OBJS[@]}" -gt 0 ] && [ "${#EGL_OBJS[@]}" -gt 0 ] || b_die "no objects for the simple clients (ninja -t query)"
	ninja -C "${WB}" frontend/weston.p/executable.c.o shared/libshared.a "${SHM_OBJS[@]}" "${EGL_OBJS[@]}"
	SHM_OBJS=("${SHM_OBJS[@]/#/${WB}/}")
	EGL_OBJS=("${EGL_OBJS[@]/#/${WB}/}")

	# Mesa's wayland build: its link list (mesa_drm wayland/link-gles.txt = the tools'
	# egl-link.txt), with the os_create_anonymous_file rename where needed
	local gallium="" l i MESA_A=()
	while IFS= read -r l; do
		case "${l}" in
			"--whole-archive "*) gallium="${l#--whole-archive }" ;;
			*) MESA_A+=("${l}") ;;
		esac
	done < "${MV}/link-gles.txt"
	[ -f "${gallium}" ] || b_die "no gallium archive in ${MV}/link-gles.txt"
	gallium="$(_weston_mesa_private "${gallium}")"
	for i in "${!MESA_A[@]}"; do
		MESA_A[i]="$(_weston_mesa_private "${MESA_A[i]}")"
	done
	local LINK_BASE=("${NL_CXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000
		-Wl,--wrap=close -Wl,--wrap=write -Wl,-u,__wrap_close -Wl,-u,__wrap_write)
	local LINK_DRM=("${LINK_BASE[@]}" -Wl,--wrap=mmap -Wl,--wrap=ioctl)
	local WL_LIBS=("${WL}/lib/libwayland-server.a" "${WL}/lib/libwayland-client.a" "${WL}/lib/libwayland-egl.a"
		"${P}/lib/libxkbcommon.a" "${P}/lib/libdisplay-info.a" "${P}/lib/libseat.a" "${P}/lib/libinput.a"
		"${P}/lib/libudev.a" "${P}/lib/libevdev.a" "${D}/pixman-1/lib/libpixman-1.a" "${WL}/deps/libffi/lib/libffi.a"
		"${compat_a}")
	_weston_link() {  # base|drm output link-arguments...
		local kind="$1" o="$2"
		local -a L
		shift 2
		if [ "${kind}" = drm ]; then L=("${LINK_DRM[@]}"); else L=("${LINK_BASE[@]}"); fi
		"${L[@]}" -Wl,-Map,"${P}/prog/${o}.map" -o "${P}/prog/${o}" "$@" > "${PREFIX_PORT_BUILD}/${o}-link.log" 2>&1 \
			|| { grep -v 'warning: .* is not fully supported' "${PREFIX_PORT_BUILD}/${o}-link.log" | head -60; b_die "${o}: link failed"; }
		"${NL_STRIP}" -o "${P}/bin/${o}" "${P}/prog/${o}"
		echo "weston: ${o}: $(stat -c %s "${P}/prog/${o}") bytes, stripped $(stat -c %s "${P}/bin/${o}")"
	}
	_weston_link drm weston "${WB}/frontend/weston.p/executable.c.o" "${WL_OBJ}/weston_builtin.o" \
		-Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${WB}/frontend/libexec_weston.a" "${WB}/kiosk-shell/kiosk-shell.a" \
		"${WB}/libweston/backend-drm/drm-backend.a" "${WB}/libweston/renderer-gl/gl-renderer.a" \
		"${WB}/libweston/libweston-14.a" "${MESA_A[@]}" "${WL_LIBS[@]}" -Wl,--end-group -lm
	# weston-simple-shm: wl_shm + xdg-shell only (pixels by the CPU into a memfd pool = shmsrv)
	_weston_link base weston-simple-shm "${SHM_OBJS[@]}" \
		-Wl,--start-group "${WB}/shared/libshared.a" "${WL_LIBS[@]}" -Wl,--end-group -lm
	# weston-simple-egl: GLES on the wayland-egl platform (Mesa wayland)
	_weston_link drm weston-simple-egl "${EGL_OBJS[@]}" \
		-Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${WB}/shared/libshared.a" "${MESA_A[@]}" "${WL}/lib/libwayland-cursor.a" "${WL_LIBS[@]}" \
		-Wl,--end-group -lm

	# --- verification -----------------------------------------------------------------------------
	local o s n bs
	for o in weston weston-simple-shm weston-simple-egl; do
		nl_no_undefined "${P}/prog/${o}"
	done
	_weston_check_syms() {  # binary symbols...
		local b="$1" s
		shift
		for s in "$@"; do
			nl_has_sym "${P}/prog/${b}" "${s}" || b_die "${b}: symbol ${s} missing"
		done
	}
	_weston_check_syms weston weston_builtin_modules weston_builtin_xkb_keymap weston_backend_init gl_renderer_interface \
		wet_shell_init __wrap_mmap __wrap_ioctl __wrap_close __wrap_write drm_phoenix_ioctl drmPhoenixMmap \
		epoll_wait timerfd_settime signalfd eventfd memfd_create libseat_open_seat libinput_udev_assign_seat \
		udev_enumerate_scan_devices di_info_parse_edid xkb_keymap_new_from_string kmsro_drm_screen_create \
		v3d_drm_screen_create_renderonly gbmint_get_backend mesa_os_create_anonymous_file os_create_anonymous_file
	_weston_check_syms weston-simple-shm memfd_create os_create_anonymous_file wl_display_connect __wrap_close
	_weston_check_syms weston-simple-egl dri2_initialize_wayland wl_egl_window_create __wrap_mmap drm_phoenix_ioctl
	[ "$(nl_count_strings "${P}/bin/weston-simple-egl" V3D_PHOENIX_SHARED_SCANOUT)" != 0 ] \
		|| b_die "weston-simple-egl: no 'V3D_PHOENIX_SHARED_SCANOUT' (mesa_drm patch 0012)"
	for s in 'drm-backend.so' 'gl-renderer.so' 'kiosk-shell.so' 'linked into the program' 'using the builtin XKB keymap' \
			'DRM backend' 'libdrm-phoenix:' 'DRMPHX_TRACE' '/dev/dri/card0' '/dev/dri/renderD128' '/kmsbuf' 'LIBINPUT-PHX' \
			'noop' 'EGL_KHR_platform_gbm' 'V3D 4.2' 'xkb_keymap' '/shm'; do
		n="$(nl_count_strings "${P}/bin/weston" "${s}")"
		[ "${n}" != 0 ] || b_die "weston: string '${s}' missing"
	done
	for bs in weston weston-simple-shm weston-simple-egl; do
		nl_forbid_old_lane "${P}/bin/${bs}" /dev/v3d-srv Xphoenix '[fbdev]' glamor_phoenix phxgl
	done
	sha256sum "${P}"/bin/weston "${P}"/bin/weston-simple-shm "${P}"/bin/weston-simple-egl

	mkdir -p "${P}/share/weston"
	install -m 644 "${PREFIX_PORT}/glue/conf/weston-drm.ini" "${P}/share/weston/weston-drm.ini"
	install -m 755 "${PREFIX_PORT}/glue/pi/weston-m6a.sh" "${P}/share/weston/weston-m6a.sh"

	if b_use rootfs; then
		b_install "${P}/bin/weston" "${P}/bin/weston-simple-shm" "${P}/bin/weston-simple-egl" \
			"${P}/share/weston/weston-m6a.sh" /bin
		mkdir -p "${PREFIX_FS}/root/etc/xdg/weston"
		install -m 644 "${P}/share/weston/weston-drm.ini" "${PREFIX_FS}/root/etc/xdg/weston/weston-drm.ini"
	fi
}
