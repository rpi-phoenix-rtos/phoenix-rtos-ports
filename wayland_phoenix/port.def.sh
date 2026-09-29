#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="wayland_phoenix"
	version="1.24.0"
	desc="libwayland 1.24 + wayland-protocols 1.45 + libxkbcommon 1.7 + the Phoenix Wayland compat/shims (new GPU lane base)"

	# The Wayland base every new-lane Wayland program builds on, as the M6 Weston
	# build (tools/gpu-lane/weston-drm/build.sh in the coordination repo) builds it:
	#
	#   libwayland 1.24.0     server, client, cursor, egl (+ patches/wayland: peer
	#                         credentials and MSG_CMSG_CLOEXEC on Phoenix)
	#   wayland-protocols 1.45
	#   libxkbcommon 1.7.0    no X11/wayland tools, no registry
	#   libwlphx-compat.a     epoll/timerfd/signalfd/eventfd over poll, memfd_create
	#                         over shmsrv (/shm; files/shmsrv/shm_proto.h is its wire
	#                         protocol), msync/pipe2 stand-ins if libphoenix lacks
	#                         them (files/compat, BSD-3-Clause)
	#   libudev.a libinput.a libevdev.a
	#                         the shims: libudev over a fixed device table,
	#                         libinput-phoenix (usbkbd, usbmouse, phxhid), libevdev
	#                         (files/shims, BSD-3-Clause); <linux/input.h> over
	#                         FreeBSD's BSD-2 input-event-codes.h (files/, pinned)
	#   keymap-us.xkb         evdev/pc105/us, compiled once on a build host
	#                         (files/keymap-us.xkb; see below)
	#
	# It also installs those glue SOURCES and the M6 patch sets (wayland, seatd)
	# under share/wayland-phoenix/, for ports that must recompile them with their
	# own flags (labwc_desktop; Weston).
	source="https://gitlab.freedesktop.org/wayland/wayland/-/releases/${version}/downloads/"
	archive_filename="wayland-${version}.tar.xz"
	src_path="wayland-${version}/"

	size="241764"
	sha256="82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536"

	# libwayland, wayland-protocols, libxkbcommon: MIT/X11-style (the anchor's
	# COPYING); files/: BSD-3-Clause (ours) and BSD-2-Clause (FreeBSD's evdev codes,
	# the phxhid usage map).
	license="MIT AND BSD-3-Clause AND BSD-2-Clause"
	license_file="COPYING"

	# NEW GPU LANE: private install prefix (see dbus/port.def.sh).
	conflicts="wayland_phoenix!=${version}"
	# libffi (libwayland), expat (xorg_fonts), pixman 0.42 (xorg_libs), zlib
	depends="libffi xorg_fonts xorg_libs zlib"

	supports="phoenix>=3.3"
}

# name|file|url|sha256 -- the anchor (wayland) is fetched by the framework
_wlphx_pkgs() {
	cat <<'EOF'
wayland-protocols|wayland-protocols-1.45.tar.xz|https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.45/downloads/wayland-protocols-1.45.tar.xz|4d2b2a9e3e099d017dc8107bf1c334d27bb87d9e4aff19a0c8d856d17cd41ef0
libxkbcommon|libxkbcommon-1.7.0.tar.xz|https://xkbcommon.org/download/libxkbcommon-1.7.0.tar.xz|65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247
libinput|libinput-1.26.2.tar.gz|https://gitlab.freedesktop.org/libinput/libinput/-/archive/1.26.2/libinput-1.26.2.tar.gz|5c1c4150f217fea1db2d1fd88e2607b2f1928cfde65c34da65a9f24dcfd69464
EOF
}

# _wlphx_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name>, as its own git
# repository with patches/<name>/*.patch applied by `git apply` (inside a directory of
# ANOTHER repository -- the buildroot is one -- git would apply relative to that
# repository's root and silently skip every path). Re-extracted when the tarball or a
# patch changes.
_wlphx_extract() {
	local name="$1" from="$2" sum="$3" out="${PREFIX_PORT_BUILD}/out"
	local dir="${out}/src/${name}" stamp p
	stamp="$( { echo "${sum}"; cat "${PREFIX_PORT}/patches/${name}"/*.patch 2>/dev/null || true; } | sha256sum | cut -c1-16)"
	[ "$(cat "${dir}.stamp" 2>/dev/null || true)" != "${stamp}" ] || return 0
	rm -rf "${dir}" "${dir}.tmp"
	mkdir -p "${dir}.tmp"
	if [ -d "${from}" ]; then
		cp -a "${from}/." "${dir}.tmp/"
	else
		tar -xf "${from}" -C "${dir}.tmp" --strip-components=1
	fi
	mv "${dir}.tmp" "${dir}"
	git -C "${dir}" init -q
	git -C "${dir}" add -A -f
	git -C "${dir}" -c user.name=build -c user.email=build@invalid commit -q -m "${name}"
	for p in "${PREFIX_PORT}/patches/${name}"/*.patch; do
		[ -e "${p}" ] || continue
		echo "wayland_phoenix: apply ${name}/$(basename "${p}")"
		git -C "${dir}" apply --whitespace=nowarn "${p}"
		git -C "${dir}" add -A -f
		git -C "${dir}" -c user.name=build -c user.email=build@invalid commit -q -m "$(basename "${p}")"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built"
}

# _wlphx_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_wlphx_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "wayland_phoenix: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "wayland_phoenix: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# The link inputs every port links implicitly (libphoenix.a, libgcc.a, libstdc++.a) changed.
# Each package here builds under an out/<name>.built stamp, so the framework's default --
# delete the linked programs and let the build relink them -- would leave them deleted: the
# stamp says "up to date" and nothing relinks (gtk3-demo, P1 build 2026-09-29). Drop the
# stamps instead: every package is set up and built again from its unpacked sources.
p_relink() {
	rm -f "${PREFIX_PORT_BUILD}/out"/*.built "${PREFIX_PORT_BUILD}/out"/*.configured
}

p_prepare() {
	local t
	for t in meson ninja wayland-scanner bison git curl python3; do
		command -v "${t}" >/dev/null || b_die "wayland_phoenix: host tool ${t} not found"
	done
	# The scanner generates the protocol code: it must be the libwayland release built here.
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "${version}" ] ||
		b_die "wayland_phoenix: host wayland-scanner is not ${version}"

	_wlphx_extract wayland "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
	local n file url sum
	while IFS='|' read -r n file url sum; do
		_wlphx_extract "${n}" "$(_wlphx_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_wlphx_pkgs)
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local B S TC PHXCC PHXCXX
	B="${PREFIX_BUILD%/}"   # libffi, expat, pixman, zlib: the shared ports prefix
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	local F="${PREFIX_PORT}/files"
	local COMPAT_INC="${F}/compat/include"
	local MESA_COMPAT_INC="${F}/mesa-compat/include"   # generic libphoenix-gap headers (mesa-drm's compat/include)
	local SHIM_INC="${F}/shims/include"
	local P="${I}/prefix" D="${out}/deps"
	local jobs
	jobs="$(nproc)"
	local EVDEV_CODES_SHA=fc9c4946818cefcec359ad3a619d448c4cafffa4f8f9571894516fb980b26142

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" \
		"${B}/lib/libffi.a" "${B}/lib/libexpat.a" "${B}/lib/libpixman-1.a" "${B}/lib/libz.a"; do
		[ -e "${p}" ] || b_die "wayland_phoenix: missing ${p}"
	done
	local libphx_syms
	libphx_syms="$("${TC}-nm" -g --defined-only "${S}/lib/libphoenix.a" 2>/dev/null || true)"
	_has_libc() { grep -qE " [TW] $1\$" <<<"${libphx_syms}"; }
	for p in memExport sys_fdpath; do
		_has_libc "${p}" || b_die "wayland_phoenix: ${S}/lib/libphoenix.a has no ${p} (stale sysroot)"
	done

	# A fresh work tree (no .built stamps) starts from an empty prefix: nothing from an
	# earlier build of a different recipe may linger in the installed files.
	if [ ! -f "${out}/prefix.fresh" ]; then
		rm -rf "${P}"
		touch "${out}/prefix.fresh"
	fi
	mkdir -p "${P}/lib/pkgconfig" "${P}/include" "${D}" "${out}/bin"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")

	# gcc without -pthread (also inside @response files): Phoenix pthreads live in libphoenix
	local cc
	for cc in gcc g++; do
		cat >"${out}/bin/phx-${cc}" <<EOF
#!/bin/sh
# Generated by the phoenix-rtos-ports wayland_phoenix recipe: ${TC}-${cc} without -pthread.
for a; do
	shift
	case "\$a" in
		-pthread) ;;
		@*) f="\${a#@}"
			if [ -f "\$f" ] && grep -q -- '-pthread' "\$f"; then
				sed 's/\\(^\\| \\)-pthread\\( \\|\$\\)/\\1\\2/g' "\$f" > "\$f.nopthread"
				set -- "\$@" "@\$f.nopthread"
			else
				set -- "\$@" "\$a"
			fi ;;
		*) set -- "\$@" "\$a" ;;
	esac
done
exec "${TC}-${cc}" "\$@"
EOF
		chmod +x "${out}/bin/phx-${cc}"
	done
	PHXCC="${out}/bin/phx-gcc"
	PHXCXX="${out}/bin/phx-g++"

	# --- private dependency views (exactly one library's headers each: the shared
	# include dir also holds GL/ and X11/ headers and is never on a search path) ---
	_dep_view() {  # name lib-basename version cflags-subdir headers...
		local name="$1" lib="$2" ver="$3" sub="$4" h
		shift 4
		rm -rf "${D:?}/${name}"
		mkdir -p "${D}/${name}/include/${sub}" "${D}/${name}/lib/pkgconfig"
		for h in "$@"; do cp "${B}/include/${h}" "${D}/${name}/include/${sub}/"; done
		cp "${B}/lib/lib${lib}.a" "${D}/${name}/lib/"
		printf '%s\n' "prefix=${D}/${name}" "Name: ${name}" "Description: ${name} from the Phoenix ports prefix" \
			"Version: ${ver}" "Libs: -L\${prefix}/lib -l${lib}" "Cflags: -I\${prefix}/include${sub:+/${sub}}" \
			>"${D}/${name}/lib/pkgconfig/${name}.pc"
	}
	_dep_view libffi ffi "$(sed -n 's/^Version: //p' "${B}/lib/pkgconfig/libffi.pc")" "" ffi.h ffitarget.h
	_dep_view expat expat "$(sed -n 's/^Version: //p' "${B}/lib/pkgconfig/expat.pc")" "" expat.h expat_config.h expat_external.h
	_dep_view pixman-1 pixman-1 "$(sed -n 's/^Version: //p' "${B}/lib/pkgconfig/pixman-1.pc")" pixman-1 pixman-1/pixman.h pixman-1/pixman-version.h
	_dep_view zlib z "$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "${B}/include/zlib.h")" "" zlib.h zconf.h

	# --- compat library ---
	rm -rf "${out}/compat-obj"   # never archive a stale object (an old shmsrv.o carried a main())
	mkdir -p "${out}/compat-obj"
	local compat_defs=() f compat_objs=()
	_has_libc msync || compat_defs+=(-DWLPHX_NEED_MSYNC)
	_has_libc pipe2 || compat_defs+=(-DWLPHX_NEED_PIPE2)
	for f in wlphx_epoll wlphx_memfd wlphx_misc; do
		"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${COMPAT_INC}" -I"${MESA_COMPAT_INC}" \
			"${compat_defs[@]}" -c "${F}/compat/src/${f}.c" -o "${out}/compat-obj/${f}.o"
		compat_objs+=("${out}/compat-obj/${f}.o")
	done
	echo "wayland_phoenix: compat stand-ins: ${compat_defs[*]:-none}"
	rm -f "${P}/lib/libwlphx-compat.a"
	"${TC}-gcc-ar" rcs "${P}/lib/libwlphx-compat.a" "${compat_objs[@]}"
	cat >"${P}/lib/pkgconfig/wlphx-compat.pc" <<EOF
prefix=${P}
Name: wlphx-compat
Description: weston-drm libphoenix-gap shims (epoll, timerfd, signalfd, eventfd, ppoll, memfd_create, msync)
Version: 1.0
Libs: -Wl,-u,__wrap_close -Wl,-u,__wrap_write -L\${prefix}/lib -lwlphx-compat -Wl,--wrap=close -Wl,--wrap=write
Cflags:
EOF

	# --- meson cross files + pkg-config ---
	local cross="${out}/phoenix-aarch64.cross" pkgc="${out}/pkg-config-phoenix"
	cat >"${pkgc}" <<EOF
#!/bin/sh
# pkg-config restricted to this build's prefix and the private ports views.
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig:${D}/libffi/lib/pkgconfig:${D}/expat/lib/pkgconfig:${D}/pixman-1/lib/pkgconfig:${D}/zlib/lib/pkgconfig
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config --static "\$@"
EOF
	chmod +x "${pkgc}"
	local flags="'--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections', '-I${COMPAT_INC}', '-I${MESA_COMPAT_INC}'"
	local lflags="'--sysroot=${S}/', '-B${S}/lib/', '-L${B}/lib', '-Wl,-z,max-page-size=0x1000'"
	# libwayland's configure probes and links see the compat archive: libwayland-cursor's
	# anonymous files (cursor theme pools) then use memfd_create = shmsrv, as Weston's do.
	local lflags_compat="${lflags}, '-Wl,-u,__wrap_close', '-Wl,-u,__wrap_write', '${P}/lib/libwlphx-compat.a', '-Wl,--wrap=close', '-Wl,--wrap=write'"
	cat >"${cross}" <<EOF
# Generated by the phoenix-rtos-ports wayland_phoenix recipe (aarch64-phoenix, Pi 4).
[binaries]
c = '${PHXCC}'
cpp = '${PHXCXX}'
ar = '${TC}-gcc-ar'
nm = '${TC}-nm'
strip = '${TC}-strip'
objcopy = '${TC}-objcopy'
pkg-config = '${pkgc}'

[host_machine]
system = 'phoenix'
cpu_family = 'aarch64'
cpu = 'cortex-a72'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = [${flags}]
cpp_args = [${flags}]
c_link_args = [${lflags}]
cpp_link_args = [${lflags}]
default_library = 'static'
EOF
	sed -e "s|^c_link_args = .*|c_link_args = [${lflags_compat}]|" -e "s|^cpp_link_args = .*|cpp_link_args = [${lflags_compat}]|" \
		"${cross}" >"${out}/phoenix-aarch64-compat.cross"

	_meson_pkg() {  # [--cross <file>] name builddir-name meson-args...
		local cf="${cross}"
		if [ "$1" = --cross ]; then cf="$2"; shift 2; fi
		local name="$1" bname="$2"
		shift 2
		local bd="${out}/${bname}"
		if [ -f "${out}/${name}.built" ]; then
			echo "wayland_phoenix: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${cf}" --prefix "${P}" \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false --wrap-mode=nodownload "$@" \
			>"${out}/${bname}-setup.log" 2>&1 || { tail -40 "${out}/${bname}-setup.log"; b_die "wayland_phoenix: ${name}: meson setup failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${bname}-ninja.log" 2>&1 || { grep -E -A5 'error|FAILED' "${out}/${bname}-ninja.log" | head -60; b_die "wayland_phoenix: ${name}: build failed"; }
		ninja -C "${bd}" install >"${out}/${bname}-install.log" 2>&1 || { tail -20 "${out}/${bname}-install.log"; b_die "wayland_phoenix: ${name}: install failed"; }
		echo "wayland_phoenix: ${name}: built ($(grep -c 'warning:' "${out}/${bname}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}
	# Every installed wayland-{server,client}.pc pulls the compat archive (epoll & co. for
	# the event loop, --wrap=close) into whatever links it.
	_pc_require_compat() {
		local pc
		for pc in "$@"; do
			grep -q 'wlphx-compat' "${pc}" || printf 'Requires.private: wlphx-compat\n' >>"${pc}"
		done
	}

	# --- libraries ---
	_meson_pkg --cross "${out}/phoenix-aarch64-compat.cross" wayland wayland-build -Dlibraries=true -Dscanner=false \
		-Dtests=false -Ddocumentation=false -Ddtd_validation=false
	_pc_require_compat "${P}/lib/pkgconfig/wayland-server.pc" "${P}/lib/pkgconfig/wayland-client.pc"
	_meson_pkg wayland-protocols wayland-protocols-build -Dtests=false
	_meson_pkg libxkbcommon xkbcommon-build -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
		-Denable-tools=false -Denable-xkbregistry=false -Denable-bash-completion=false \
		-Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale

	# --- shims: libudev (fixed device table), libinput-phoenix (usbkbd/usbmouse/phxhid),
	# libevdev. Only libinput.h is used from the libinput tarball (MIT); the
	# implementation is ours. ---
	mkdir -p "${P}/include/evdev" "${out}/shim-obj" "${P}/include/linux" "${P}/include/libevdev"
	local evc="${P}/include/evdev/input-event-codes.h"
	cp "${F}/input-event-codes.h" "${evc}"
	echo "${EVDEV_CODES_SHA}  ${evc}" | sha256sum -c --quiet - || b_die "wayland_phoenix: input-event-codes.h sha256 mismatch"
	cp "${out}/src/libinput/src/libinput.h" "${P}/include/libinput.h"
	cp "${SHIM_INC}"/linux/*.h "${P}/include/linux/"
	cp "${SHIM_INC}/libevdev/libevdev.h" "${P}/include/libevdev/"
	cp "${SHIM_INC}/libudev.h" "${P}/include/"
	local SFLAGS=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${P}/include" -I"${COMPAT_INC}" -I"${MESA_COMPAT_INC}")
	"${TC}-gcc" "${SFLAGS[@]}" -c "${F}/shims/src/udev_phoenix.c" -o "${out}/shim-obj/udev_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -c "${F}/shims/src/libevdev_phoenix.c" -o "${out}/shim-obj/libevdev_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -c "${F}/shims/src/libinput_phoenix.c" -o "${out}/shim-obj/libinput_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -I"${F}/phxhid" -c "${F}/shims/src/libinput_phoenix_hid.c" -o "${out}/shim-obj/libinput_phoenix_hid.o"
	rm -f "${P}/lib/libudev.a" "${P}/lib/libinput.a" "${P}/lib/libevdev.a"
	"${TC}-gcc-ar" rcs "${P}/lib/libudev.a" "${out}/shim-obj/udev_phoenix.o"
	"${TC}-gcc-ar" rcs "${P}/lib/libinput.a" "${out}/shim-obj/libinput_phoenix.o" "${out}/shim-obj/libinput_phoenix_hid.o"
	"${TC}-gcc-ar" rcs "${P}/lib/libevdev.a" "${out}/shim-obj/libevdev_phoenix.o"
	_shim_pc() {  # name version libs description
		printf '%s\n' "prefix=${P}" "Name: $1" "Description: $4" "Version: $2" "Libs: -L\${prefix}/lib $3" \
			"Cflags: -I\${prefix}/include" >"${P}/lib/pkgconfig/$1.pc"
	}
	_shim_pc libudev 256 -ludev "weston-drm shim: libudev over a fixed device table"
	_shim_pc libinput 1.26.2 "-linput -ludev" "weston-drm shim: libinput-phoenix (usbkbd, usbmouse)"
	_shim_pc libevdev 1.13.0 -levdev "weston-drm shim: libevdev_event_code_from_name"

	# --- the baked default keymap (Phoenix has no xkeyboard-config) ---
	# files/keymap-us.xkb is the output of `xkbcli compile-keymap --rules evdev --model
	# pc105 --layout us` (libxkbcommon 1.7.0 over the build host's xkeyboard-config, MIT)
	# that the M6/M7 builds baked in -- committed so this build no longer depends on the
	# host's xkeyboard-config. Regenerate with that command if the layout must change.
	grep -q 'xkb_keymap' "${F}/keymap-us.xkb" || b_die "wayland_phoenix: files/keymap-us.xkb is not a keymap"
	cp "${F}/keymap-us.xkb" "${I}/keymap-us.xkb"

	# --- the glue sources + M6 patch sets, for consumers that recompile them ---
	local G="${P}/share/wayland-phoenix"
	rm -rf "${G}"
	mkdir -p "${G}/patches"
	cp -a "${F}/compat" "${F}/shims" "${F}/shmsrv" "${F}/mesa-compat" "${F}/phxhid" "${G}/"
	cp -a "${PREFIX_PORT}/patches/wayland" "${PREFIX_PORT}/patches/seatd" "${G}/patches/"

	# --- verification ---
	local l
	for l in libwayland-server.a libwayland-client.a libwayland-cursor.a libwayland-egl.a libxkbcommon.a \
		libwlphx-compat.a libudev.a libinput.a libevdev.a; do
		[ -f "${P}/lib/${l}" ] || b_die "wayland_phoenix: ${l} not installed"
	done
	[ -f "${P}/share/pkgconfig/wayland-protocols.pc" ] || b_die "wayland_phoenix: wayland-protocols.pc not installed"
	"${TC}-nm" "${P}/lib/libwlphx-compat.a" | grep -qE ' T (epoll_wait|memfd_create)$' ||
		b_die "wayland_phoenix: libwlphx-compat.a lacks epoll_wait/memfd_create"
	(cd "${P}/lib" && sha256sum ./*.a) >"${I}/SHA256SUMS"
	sed 's/^/wayland_phoenix: /' "${I}/SHA256SUMS"
}
