#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="labwc_desktop"
	version="0.20.2"
	desc="labwc 0.20 Wayland compositor on wlroots 0.20 + foot 1.28, fuzzel 1.15, swaybg 1.2 -- the Wayland desktop"

	# Aggregate port: a lightweight Wayland desktop cross-built STATIC, as the coordination
	# repo's tools/gpu-lane/labwc-drm/build.sh builds it (M7 stage 1 and 1b):
	#
	#   pixman 0.46.4, libdisplay-info 0.2.0, libseat (seatd 0.9.1, noop backend, M6
	#   patches), libxml2 2.15.4, fribidi 1.0.16, pango 1.44.7 (over the ports cairo 1.16,
	#   harfbuzz and GLib 2.56), tllist 1.1.0, fcft 3.3.3,
	#   wlroots 0.20.2 (DRM + libinput + headless backends, GLES2 and pixman renderers,
	#   GBM allocator, libseat session), labwc 0.20.2 (no Xwayland, SVG, icons, NLS; the
	#   builtin keymap), foot 1.28.0, fuzzel 1.15.0 and swaybg 1.2.2.
	#
	# The programs are linked by hand (meson's own links would need Mesa's static
	# closure, which Mesa's .pc files do not describe).
	#
	# Graphics inputs (the graphics half of the new lane, other ports):
	#   mesa_drm[wayland]  ${PORT_DEP_mesa_drm}/wayland/prefix (EGL/GLES/gbm headers,
	#                      egl.pc) and ${PORT_DEP_mesa_drm}/wayland/link-gles.txt: the
	#                      archives a GLES program links, first line
	#                      "--whole-archive <libgallium-*.a>", libdrm.a / compat / libz.a
	#                      at the end (the tools build's mesa-drm --wayland egl-link.txt)
	#   libdrm_phoenix     ${PORT_DEP_libdrm_phoenix}/lib/libdrm.a + include/
	#   wayland_phoenix    libwayland 1.24.0, wayland-protocols 1.49 and libxkbcommon 1.13.2
	#                      (its libwayland/ and xkbcommon/ views: nothing else of its
	#                      prefix on a compile line), the baked US keymap (keymap-us.xkb),
	#                      and the M6 compat/shim sources and seatd patch set it recompiles
	#                      with this port's flags (share/wayland-phoenix/), the evdev codes
	#                      and libinput.h
	source="https://github.com/labwc/labwc/archive/refs/tags/"
	archive_filename=("labwc-${version}.tar.gz" "${version}.tar.gz")
	src_path="labwc-${version}/"

	size="553497"
	sha256="fae023b6fe022f7057556707a17cdb2d98e0138c5dffaedaa1dade975699f9e8"

	# labwc: GPL-2.0-only; wlroots, foot, fuzzel, swaybg, fcft, tllist, libwayland,
	# wayland-protocols, libxkbcommon, pixman, libdisplay-info, seatd, libxml2: MIT;
	# fribidi, pango: LGPL-2.1-or-later; files/ (compat, shims, labwc_builtin.c,
	# configuration, scripts): BSD-3-Clause; the generated wallpaper: CC0-1.0; the baked
	# keymap: xkeyboard-config data, MIT.
	license="GPL-2.0-only AND MIT AND LGPL-2.1-or-later AND BSD-3-Clause AND CC0-1.0"
	license_file="LICENSE"

	# A real conflict (and the private versioned-ports prefix): labwc links the ports
	# GLib 2.56 (through pango 1.44); gtk3_wayland carries GLib 2.88.
	conflicts="gtk3_wayland>=0.0"
	# zlib, libffi, expat/freetype/fontconfig/cairo (xorg_fonts), libpng16, harfbuzz,
	# libiconv, GLib/GObject 2.56 (glib2): the shared ports prefix, through private views.
	depends="wayland_phoenix libdrm_phoenix mesa_drm[wayland] xorg_fonts harfbuzz glib2 libpng libiconv libffi zlib"

	# rootfs: also copy the staging tree (stage/) into the image rootfs.
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   bin/          labwc, foot, fuzzel, swaybg: unstripped (addr2line) and -stripped
#   stage/ + stage.MANIFEST   the files for the target rootfs (new names only)
#   SHA256SUMS
# The libraries stay in the work tree (${PREFIX_PORT_BUILD}/out/prefix): nothing else
# links them.
#
# Host tools: wayland-scanner 1.24.0, meson >= 1.3, ninja, bison, glib-mkenums, python3,
# hwdata (pnp.ids).

# name|file|url|sha256 (the anchor, labwc, is fetched by the framework)
_labwcd_pkgs() {
	cat <<'EOF'
pixman|pixman-0.46.4.tar.xz|https://www.cairographics.org/releases/pixman-0.46.4.tar.xz|a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239
libdisplay-info|libdisplay-info-0.2.0.tar.xz|https://gitlab.freedesktop.org/emersion/libdisplay-info/-/releases/0.2.0/downloads/libdisplay-info-0.2.0.tar.xz|5a2f002a16f42dd3540c8846f80a90b8f4bdcd067a94b9d2087bc2feae974176
seatd|seatd-0.9.1.tar.gz|https://git.sr.ht/~kennylevinsen/seatd/archive/0.9.1.tar.gz|819979c922a0be258aed133d93920bce6a3d3565a60588d6d372ce9db2712cd3
libxml2|libxml2-2.15.4.tar.xz|https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz|98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
fribidi|fribidi-1.0.16.tar.xz|https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz|1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
pango|pango-1.44.7.tar.xz|https://download.gnome.org/sources/pango/1.44/pango-1.44.7.tar.xz|66a5b6cc13db73efed67b8e933584509f8ddb7b10a8a40c3850ca4a985ea1b1f
wlroots|wlroots-0.20.2.tar.gz|https://gitlab.freedesktop.org/-/project/12103/uploads/6a56af9eafb5240d2823772aeb1e2e7d/wlroots-0.20.2.tar.gz|80c567d2ed4efb2cfa6b077f22c7a710d9c78ba4e185a75e8694001114af0733
tllist|tllist-1.1.0.tar.gz|https://codeberg.org/dnkl/tllist/archive/1.1.0.tar.gz|0e7b7094a02550dd80b7243bcffc3671550b0f1d8ba625e4dff52517827d5d23
fcft|fcft-3.3.3.tar.gz|https://codeberg.org/dnkl/fcft/archive/3.3.3.tar.gz|b0c0f4a599f43723736c8565b8b84337c4195077f07f1bb8bb3252bb13a2306a
foot|foot-1.28.0.tar.gz|https://codeberg.org/dnkl/foot/archive/1.28.0.tar.gz|4296be402b5684d049534598e69db92b918f92beac9dab76b585207045f0b037
fuzzel|fuzzel-1.15.0.tar.gz|https://codeberg.org/dnkl/fuzzel/archive/1.15.0.tar.gz|95b6c022fc1f1c7ab586d47c1594417cc311bf41ea8f5f8b5641478da7b5cf3b
swaybg|swaybg-1.2.2.tar.gz|https://github.com/swaywm/swaybg/releases/download/v1.2.2/swaybg-1.2.2.tar.gz|a6652a0060a0bea3c3318d9d03b6dddac34f6aeca01b883eef9e58281f5202a1
EOF
}

# The M6 seatd patch set comes from wayland_phoenix, as the tools build takes it from
# weston-drm; every other patches/<name>/ is this port's.
_labwcd_patch_dir() {
	case "$1" in
		seatd) echo "${PORT_DEP_wayland_phoenix%/}/prefix/share/wayland-phoenix/patches/$1" ;;
		*) echo "${PREFIX_PORT}/patches/$1" ;;
	esac
}

# _labwcd_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_labwcd_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "labwc_desktop: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "labwc_desktop: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# _labwcd_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name>, as its own git
# repository with the patch set applied by `git apply`, one commit each (inside a
# directory of ANOTHER repository -- the buildroot is one -- git would apply relative to
# that repository's root and silently skip every path; weston-drm M6 §5.1). Re-extracted
# when the tarball or a patch changes.
_labwcd_extract() {
	local name="$1" from="$2" sum="$3" out="${PREFIX_PORT_BUILD}/out"
	local dir="${out}/src/${name}" stamp p pd
	pd="$(_labwcd_patch_dir "${name}")"
	stamp="$( { echo "${sum}"; cat "${pd}"/*.patch 2>/dev/null || true; } | sha256sum | cut -c1-16)"
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
	git -C "${dir}" add -A
	git -C "${dir}" -c user.name=build -c user.email=build@invalid commit -q -m "${name}"
	for p in "${pd}"/*.patch; do
		[ -e "${p}" ] || continue
		echo "labwc_desktop: apply ${name}/$(basename "${p}")"
		git -C "${dir}" apply --whitespace=nowarn "${p}"
		git -C "${dir}" add -A
		git -C "${dir}" -c user.name=build -c user.email=build@invalid commit -q -m "$(basename "${p}")"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built" "${out}/${name}.configured"   # a changed source rebuilds the package
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
	for t in meson ninja wayland-scanner bison glib-mkenums python3 git curl; do
		command -v "${t}" >/dev/null || b_die "labwc_desktop: host tool ${t} not found"
	done
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "1.24.0" ] ||
		b_die "labwc_desktop: host wayland-scanner is not 1.24.0"
	[ -f /usr/share/hwdata/pnp.ids ] || b_die "labwc_desktop: host hwdata (pnp.ids) missing"

	local n file url sum
	while IFS='|' read -r n file url sum; do
		_labwcd_extract "${n}" "$(_labwcd_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_labwcd_pkgs)
	_labwcd_extract labwc "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local B S TC WLR WLP WLV WXK
	B="${PREFIX_BUILD%/}"   # the shared ports prefix (the dependencies above)
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	WLR="$(b_dependency_dir wayland_phoenix)"
	WLR="${WLR%/}"
	WLP="${WLR}/prefix"
	WLV="${WLR}/libwayland"   # libwayland + wayland-protocols (+ wlphx-compat, not linked here)
	WXK="${WLR}/xkbcommon"    # libxkbcommon
	local WG="${WLP}/share/wayland-phoenix"   # the M6 glue sources (tools/gpu-lane/weston-drm)
	local F="${PREFIX_PORT}/files"
	local LCOMPAT_INC="${F}/compat/include"          # this port's gaps (shm_open, uchar.h...)
	local COMPAT_INC="${WG}/compat/include"          # M6: epoll/timerfd/signalfd/memfd...
	local MESA_COMPAT_INC="${WG}/mesa-compat/include"   # M3/M4 generic libphoenix gaps
	local SHIM_INC="${WG}/shims/include"
	local P="${out}/prefix" D="${out}/deps" LD_PREFIX="${out}/libdrm-prefix"
	local libdrm_src_prefix mesa_out
	libdrm_src_prefix="$(b_dependency_dir libdrm_phoenix)"
	libdrm_src_prefix="${libdrm_src_prefix%/}"
	mesa_out="$(b_dependency_dir mesa_drm)"
	mesa_out="${mesa_out%/}/wayland"
	local mesa_link="${mesa_out}/link-gles.txt"
	[ -f "${mesa_link}" ] || mesa_link="${mesa_out}/egl-link.txt"   # a tools mesa-drm --wayland out dir
	local jobs
	jobs="$(nproc)"
	local EVDEV_CODES_SHA=fc9c4946818cefcec359ad3a619d448c4cafffa4f8f9571894516fb980b26142
	local PHXCC PHXCXX

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" \
		"${B}/lib/libffi.a" "${B}/lib/libexpat.a" "${B}/lib/libz.a" "${B}/lib/libpng16.a" "${B}/lib/libfreetype.a" \
		"${B}/lib/libfontconfig.a" "${B}/lib/libharfbuzz.a" "${B}/lib/libcairo.a" "${B}/lib/libglib-2.0.a" \
		"${B}/lib/libgobject-2.0.a" "${B}/lib/libiconv.a" "${B}/lib/glib-2.0/include/glibconfig.h" \
		"${libdrm_src_prefix}/lib/libdrm.a" "${mesa_link}" "${mesa_out}/prefix/lib/pkgconfig/egl.pc" \
		"${WG}/compat/src/wlphx_epoll.c" "${WLP}/include/evdev/input-event-codes.h" "${WLP}/include/libinput.h" \
		"${WLV}/lib/libwayland-server.a" "${WLV}/lib/pkgconfig/wayland-server.pc" "${WLV}/share/pkgconfig/wayland-protocols.pc" \
		"${WXK}/lib/libxkbcommon.a" "${WXK}/lib/pkgconfig/xkbcommon.pc" "${WLR}/keymap-us.xkb"; do
		[ -e "${p}" ] || b_die "labwc_desktop: missing ${p}"
	done
	local libphx_syms
	libphx_syms="$("${TC}-nm" -g --defined-only "${S}/lib/libphoenix.a" 2>/dev/null || true)"
	_has_libc() { grep -qE " [TW] $1\$" <<<"${libphx_syms}"; }
	for p in memExport sys_fdpath; do
		_has_libc "${p}" || b_die "labwc_desktop: ${S}/lib/libphoenix.a has no ${p} (stale sysroot)"
	done

	mkdir -p "${P}/lib/pkgconfig" "${P}/include" "${D}" "${out}/bin" "${I}/bin"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")

	# gcc/g++ without -pthread (also inside @response files): Phoenix pthreads live in libphoenix
	local cc
	for cc in gcc g++; do
		cat >"${out}/bin/phx-${cc}" <<EOF
#!/bin/sh
# Generated by the phoenix-rtos-ports labwc_desktop recipe: ${TC}-${cc} without -pthread.
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

	# --- private views of the ports prefix ---
	# view <name> <version> <cflags-subdirs> <libs> <requires> <requires.private> <item>...
	#   item = inc:<path under $B/include> | lib:<path under $B/lib>   (files or directories)
	_view() {
		local name="$1" ver="$2" subs="$3" libs="$4" req="$5" reqp="$6" it cf="" s
		shift 6
		rm -rf "${D:?}/${name}"
		mkdir -p "${D}/${name}/include" "${D}/${name}/lib/pkgconfig"
		for it in "$@"; do
			case "${it}" in
				inc:*) mkdir -p "$(dirname "${D}/${name}/include/${it#inc:}")"
					cp -a "${B}/include/${it#inc:}" "${D}/${name}/include/${it#inc:}" ;;
				lib:*) mkdir -p "$(dirname "${D}/${name}/lib/${it#lib:}")"
					cp -a "${B}/lib/${it#lib:}" "${D}/${name}/lib/${it#lib:}" ;;
			esac
		done
		for s in ${subs}; do
			case "${s}" in
				.) cf="${cf} -I\${prefix}/include" ;;
				lib/*) cf="${cf} -I\${prefix}/${s}" ;;
				*) cf="${cf} -I\${prefix}/include/${s}" ;;
			esac
		done
		printf '%s\n' "prefix=${D}/${name}" "Name: ${name}" "Description: ${name} from the Phoenix ports prefix" \
			"Version: ${ver}" "Requires: ${req}" "Requires.private: ${reqp}" "Libs: -L\${prefix}/lib ${libs}" \
			"Cflags:${cf}" >"${D}/${name}/lib/pkgconfig/${name}.pc"
	}
	_pc_alias() {  # alias-name target-view version [extra variables...]
		local a="$1" t="$2" v="$3"
		shift 3
		{ printf '%s\n' "$@"; printf '%s\n' "Name: ${a}" "Description: alias of ${t}" "Version: ${v}" "Requires: ${t}" "Libs:" "Cflags:"; } \
			>"${D}/${t}/lib/pkgconfig/${a}.pc"
	}
	_pcver() { sed -n 's/^Version: *//p' "${B}/lib/pkgconfig/$1.pc"; }

	_write_cross() {
		local cross="${out}/phoenix-aarch64.cross" pkgc="${out}/pkg-config-phoenix" v libdir=""
		for v in "${D}"/*/lib/pkgconfig; do libdir="${libdir}:${v}"; done
		cat >"${pkgc}" <<EOF
#!/bin/sh
# pkg-config restricted to this build's prefix, the private ports views, wayland_phoenix's
# libwayland and xkbcommon views, the libdrm snapshot and the mesa_drm wayland prefix.
# (${P} first: its wlphx-compat.pc, the one wayland-server/-client.pc require, adds this
# port's lwphx-compat.)
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig${libdir}:${WLV}/lib/pkgconfig:${WLV}/share/pkgconfig:${WXK}/lib/pkgconfig:${LD_PREFIX}/lib/pkgconfig:${mesa_out}/prefix/lib/pkgconfig
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config --static "\$@"
EOF
		chmod +x "${pkgc}"
		# Include order: this port's compat, the M6 compat (epoll...), mesa-drm's generic gaps.
		local flags="'--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections', '-I${LCOMPAT_INC}', '-I${COMPAT_INC}', '-I${MESA_COMPAT_INC}'"
		# The compat archive on every link probe: configure checks (memfd_create, epoll,
		# shm_open...) see what the programs will have.
		local lflags="'--sysroot=${S}/', '-B${S}/lib/', '-L${B}/lib', '-Wl,-z,max-page-size=0x1000', '-Wl,-u,__wrap_close', '-Wl,-u,__wrap_write', '-Wl,-u,__wrap_read', '${P}/lib/liblwphx-compat.a', '${P}/lib/libwlphx-compat.a', '-Wl,--wrap=close', '-Wl,--wrap=write', '-Wl,--wrap=read'"
		cat >"${cross}" <<EOF
# Generated by the phoenix-rtos-ports labwc_desktop recipe (aarch64-phoenix, Pi 4).
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
	}

	_meson_pkg() {  # name builddir-name meson-args...
		local name="$1" bname="$2"
		shift 2
		local bd="${out}/${bname}"
		if [ -f "${out}/${name}.built" ]; then
			echo "labwc_desktop: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		# EXTRA_C_ARGS: appended to the cross file's c_args for this package only
		local cross="${out}/phoenix-aarch64.cross"
		if [ -n "${EXTRA_C_ARGS:-}" ]; then
			cross="${out}/${bname}.cross"
			sed "s|^c_args = \[\(.*\)\]|c_args = [\1, ${EXTRA_C_ARGS}]|" "${out}/phoenix-aarch64.cross" >"${cross}"
		fi
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${cross}" --prefix "${P}" \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false --wrap-mode=nodownload "$@" \
			>"${out}/${bname}-setup.log" 2>&1 || { tail -40 "${out}/${bname}-setup.log"; b_die "labwc_desktop: ${name}: meson setup failed"; }
		# MESON_TARGETS: build only these (packages whose tests cannot be switched off),
		# then install what was built
		# shellcheck disable=SC2086
		ninja -C "${bd}" -j"${jobs}" ${MESON_TARGETS:-} >"${out}/${bname}-ninja.log" 2>&1 || { grep -E -A5 'error|FAILED' "${out}/${bname}-ninja.log" | head -80; b_die "labwc_desktop: ${name}: build failed"; }
		meson install -C "${bd}" --no-rebuild >"${out}/${bname}-install.log" 2>&1 || { tail -20 "${out}/${bname}-install.log"; b_die "labwc_desktop: ${name}: install failed"; }
		echo "labwc_desktop: ${name}: built ($(grep -c 'warning:' "${out}/${bname}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	# The objects meson would link into an executable (its own link is not used).
	_ninja_objs() {  # builddir target
		# objects, then the program's own internal archives (implicit "| lib*.a" inputs)
		ninja -C "$1" -t query "$2" | awk '/input: c_LINKER|input: cpp_LINKER/ {on = 1; next} /^  [a-z]/ {on = 0}
			on && $1 ~ /\.o$/ {print $1} on && $1 == "|" && $2 !~ /^\// && $2 ~ /\.a$/ {print $2}'
	}

	# meson setup + ninja of one program's objects (programs are hand-linked below)
	_meson_objs() {  # name builddir-name program meson-args...
		local name="$1" bname="$2" targets="$3"
		shift 3
		local bd="${out}/${bname}"
		if [ ! -f "${out}/${name}.configured" ]; then
			rm -rf "${bd}"
			local cross="${out}/phoenix-aarch64.cross"
			if [ -n "${EXTRA_C_ARGS:-}" ]; then
				cross="${out}/${bname}.cross"
				sed "s|^c_args = \[\(.*\)\]|c_args = [\1, ${EXTRA_C_ARGS}]|" "${out}/phoenix-aarch64.cross" >"${cross}"
			fi
			meson setup "${bd}" "${out}/src/${name}" --cross-file "${cross}" --prefix /usr \
				--buildtype="${BUILDTYPE:-debugoptimized}" -Db_staticpic=false --wrap-mode=nodownload "$@" \
				>"${out}/${bname}-setup.log" 2>&1 || { tail -40 "${out}/${bname}-setup.log"; b_die "labwc_desktop: ${name}: meson setup failed"; }
			touch "${out}/${name}.configured"
		fi
		# only the program's objects: meson's own link would need Mesa's static closure
		local -a objs
		mapfile -t objs < <(_ninja_objs "${bd}" "${targets}")
		[ "${#objs[@]}" -gt 0 ] || b_die "labwc_desktop: no objects for ${targets} in ${bd}"
		ninja -C "${bd}" -j"${jobs}" "${objs[@]}" >"${out}/${bname}-ninja.log" 2>&1 ||
			{ grep -E -A6 'error|FAILED' "${out}/${bname}-ninja.log" | head -80; b_die "labwc_desktop: ${name}: build failed"; }
		echo "labwc_desktop: ${name}: objects built ($(grep -c 'warning:' "${out}/${bname}-ninja.log" || true) warning line(s))"
	}

	# --- dependency views (ports prefix) + libdrm-phoenix snapshot ---
	_view zlib "$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "${B}/include/zlib.h")" . -lz "" "" inc:zlib.h inc:zconf.h lib:libz.a
	_view libffi "$(_pcver libffi)" . -lffi "" "" inc:ffi.h inc:ffitarget.h lib:libffi.a
	_view expat "$(_pcver expat)" . -lexpat "" "" inc:expat.h inc:expat_config.h inc:expat_external.h lib:libexpat.a
	_view libpng16 "$(_pcver libpng16)" "libpng16" -lpng16 "" zlib inc:libpng16 lib:libpng16.a
	_pc_alias libpng libpng16 "$(_pcver libpng16)"
	_view freetype2 "$(_pcver freetype2)" freetype2 -lfreetype "" "" inc:freetype2 lib:libfreetype.a
	_view fontconfig "$(_pcver fontconfig)" . -lfontconfig freetype2 expat inc:fontconfig lib:libfontconfig.a
	# harfbuzz is C++: its static archive needs libstdc++ at every link
	_view harfbuzz "$(_pcver harfbuzz)" "harfbuzz ." "-lharfbuzz -lstdc++ -lm" "" freetype2 inc:harfbuzz lib:libharfbuzz.a
	_view libiconv 1.18 . -liconv "" "" inc:iconv.h lib:libiconv.a
	# GLib 2.56.4 (ports, LGPL): the tools pango's meson asks the .pc for are the host's
	_view glib-2.0 2.56.4 "glib-2.0 lib/glib-2.0/include" "-lglib-2.0 -lm" "" libiconv inc:glib-2.0 lib:libglib-2.0.a \
		lib:glib-2.0/include/glibconfig.h
	printf '%s\n' "glib_mkenums=/usr/bin/glib-mkenums" "glib_genmarshal=/usr/bin/glib-genmarshal" \
		"glib_compile_resources=/usr/bin/glib-compile-resources" | cat - "${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc" \
		>"${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc.t"
	mv "${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc.t" "${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc"
	mkdir -p "${D}/gobject-2.0/lib/pkgconfig"
	cp "${B}/lib/libgobject-2.0.a" "${D}/gobject-2.0/lib/"
	printf '%s\n' "prefix=${D}/gobject-2.0" "Name: gobject-2.0" "Description: GObject 2.56.4 from the ports prefix" \
		"Version: 2.56.4" "Requires: glib-2.0" "Requires.private: libffi" "Libs: -L\${prefix}/lib -lgobject-2.0" "Cflags:" \
		>"${D}/gobject-2.0/lib/pkgconfig/gobject-2.0.pc"
	# cairo 1.16 (ports): image + ft/fc + png surfaces; linked with THIS build's pixman 0.46
	_view cairo "$(_pcver cairo)" cairo -lcairo "" "pixman-1 fontconfig freetype2 libpng16 zlib" inc:cairo lib:libcairo.a
	_pc_alias cairo-ft cairo "$(_pcver cairo)"
	_pc_alias cairo-fc cairo "$(_pcver cairo)"
	_pc_alias cairo-png cairo "$(_pcver cairo)"

	# Stand-ins for the text stack (files/shims/): HarfBuzz's GLib script conversions (the
	# ports HarfBuzz has no hb-glib) and GLib 2.68's g_string_replace (labwc). Both go into
	# the private views, declared where the real ones would be.
	mkdir -p "${out}/shim-obj"
	local GFL=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${D}/glib-2.0/include/glib-2.0"
		-I"${D}/glib-2.0/lib/glib-2.0/include" -I"${D}/harfbuzz/include/harfbuzz" -I"${F}/shims/include")
	cp "${F}/shims/include/hb-glib.h" "${D}/harfbuzz/include/harfbuzz/"
	"${TC}-gcc" "${GFL[@]}" -c "${F}/shims/src/hb_glib_phoenix.c" -o "${out}/shim-obj/hb_glib_phoenix.o"
	"${TC}-gcc-ar" rcs "${D}/harfbuzz/lib/libhbglib-phoenix.a" "${out}/shim-obj/hb_glib_phoenix.o"
	sed -i 's|^Libs: \(.*\) -lharfbuzz |Libs: \1 -lhbglib-phoenix -lharfbuzz |' "${D}/harfbuzz/lib/pkgconfig/harfbuzz.pc"
	python3 - "${D}/glib-2.0/include/glib-2.0/glib/gstring.h" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
decl = ('/* labwc-drm: GLib 2.68 API, tools/gpu-lane/labwc-drm/shims/src/glib_compat.c */\n'
        'guint g_string_replace (GString *string, const gchar *find, const gchar *replace, guint limit);\n\n')
i = s.rindex('G_END_DECLS')
open(p, 'w').write(s[:i] + decl + s[i:])
PY
	"${TC}-gcc" "${GFL[@]}" -c "${F}/shims/src/glib_compat.c" -o "${out}/shim-obj/glib_compat.o"
	"${TC}-gcc-ar" rcs "${D}/glib-2.0/lib/liblwphx-glib.a" "${out}/shim-obj/glib_compat.o"
	sed -i 's|^Libs: \(.*\) -lglib-2.0 |Libs: \1 -llwphx-glib -lglib-2.0 |' "${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc"
	if ! grep -q lhbglib-phoenix "${D}/harfbuzz/lib/pkgconfig/harfbuzz.pc" ||
		! grep -q llwphx-glib "${D}/glib-2.0/lib/pkgconfig/glib-2.0.pc"; then
		b_die "labwc_desktop: text-stack shim .pc edit failed"
	fi

	rm -rf "${LD_PREFIX}"
	mkdir -p "${LD_PREFIX}/lib/pkgconfig"
	cp -a "${libdrm_src_prefix}/include" "${LD_PREFIX}/"
	cp -a "${libdrm_src_prefix}/lib/libdrm.a" "${LD_PREFIX}/lib/"
	cat >"${LD_PREFIX}/lib/pkgconfig/libdrm.pc" <<EOF
prefix=${LD_PREFIX}
includedir=\${prefix}/include
libdir=\${prefix}/lib

Name: libdrm
Description: libdrm-phoenix snapshot (upstream libdrm + Phoenix backend)
Version: 2.4.134
Libs: -L\${libdir} -ldrm -Wl,--wrap=mmap -Wl,--wrap=ioctl
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
	{ echo "source: ${libdrm_src_prefix}"; sha256sum "${LD_PREFIX}/lib/libdrm.a"; echo "mesa: ${mesa_out}"; } >"${I}/libdrm-snapshot.txt"

	# --- compat libraries (M6 wlphx + this port's lwphx) ---
	mkdir -p "${out}/compat-obj"
	local compat_defs=() f
	_has_libc msync || compat_defs+=(-DWLPHX_NEED_MSYNC)
	_has_libc pipe2 || compat_defs+=(-DWLPHX_NEED_PIPE2)
	local CFL=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${LCOMPAT_INC}" -I"${COMPAT_INC}" -I"${MESA_COMPAT_INC}")
	for f in wlphx_epoll wlphx_memfd wlphx_misc; do
		"${TC}-gcc" "${CFL[@]}" "${compat_defs[@]}" -c "${WG}/compat/src/${f}.c" -o "${out}/compat-obj/${f}.o"
	done
	rm -f "${P}/lib/libwlphx-compat.a" "${P}/lib/liblwphx-compat.a"
	"${TC}-gcc-ar" rcs "${P}/lib/libwlphx-compat.a" "${out}/compat-obj/"wlphx_*.o
	for f in lwphx_shm lwphx_pty lwphx_uchar lwphx_threads lwphx_locale lwphx_sem lwphx_wchar lwphx_epoll_pwait lwphx_misc lwphx_read; do
		"${TC}-gcc" "${CFL[@]}" -c "${F}/compat/src/${f}.c" -o "${out}/compat-obj/${f}.o"
	done
	"${TC}-gcc-ar" rcs "${P}/lib/liblwphx-compat.a" "${out}/compat-obj/"lwphx_*.o
	echo "labwc_desktop: compat stand-ins: ${compat_defs[*]:-none}"
	cat >"${P}/lib/pkgconfig/wlphx-compat.pc" <<EOF
prefix=${P}
Name: wlphx-compat
Description: weston-drm + labwc-drm libphoenix-gap shims (epoll, timerfd, signalfd, eventfd, memfd_create, shm_open...)
Version: 1.1
Libs: -Wl,-u,__wrap_close -Wl,-u,__wrap_write -Wl,-u,__wrap_read -L\${prefix}/lib -llwphx-compat -lwlphx-compat -Wl,--wrap=close -Wl,--wrap=write -Wl,--wrap=read
Cflags:
EOF
	_write_cross

	# --- libraries (libwayland, wayland-protocols, libxkbcommon: wayland_phoenix) ---
	# wlroots' pixman renderer needs >= 0.46
	_meson_pkg pixman pixman-build -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
		-Dopenmp=disabled -Dtimers=false
	_meson_pkg libdisplay-info display-info-build
	# libseat: noop backend only (M6 patches)
	_meson_pkg seatd seatd-build -Dlibseat-logind=disabled -Dlibseat-seatd=disabled -Dlibseat-builtin=disabled \
		-Dserver=disabled -Dexamples=disabled -Dman-pages=disabled -Dwerror=false

	# --- shims (M6: libudev, libinput-phoenix, libevdev, <linux/input.h>) ---
	mkdir -p "${P}/include/evdev" "${out}/shim-obj" "${P}/include/linux" "${P}/include/libevdev"
	local evc="${P}/include/evdev/input-event-codes.h"
	cp "${WLP}/include/evdev/input-event-codes.h" "${evc}"
	echo "${EVDEV_CODES_SHA}  ${evc}" | sha256sum -c --quiet - || b_die "labwc_desktop: input-event-codes.h sha256 mismatch"
	cp "${WLP}/include/libinput.h" "${P}/include/libinput.h"   # from the libinput 1.26.2 tarball (MIT)
	cp "${SHIM_INC}"/linux/*.h "${P}/include/linux/"
	# wlroots includes <linux/input-event-codes.h> directly
	printf '%s\n' "/* labwc-drm: <linux/input-event-codes.h> = FreeBSD's BSD-2 copy (see linux/input.h) */" \
		"#include <evdev/input-event-codes.h>" >"${P}/include/linux/input-event-codes.h"
	cp "${SHIM_INC}/libevdev/libevdev.h" "${P}/include/libevdev/"
	cp "${SHIM_INC}/libudev.h" "${P}/include/"
	local SFLAGS=(-O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${P}/include" -I"${LCOMPAT_INC}" -I"${COMPAT_INC}" -I"${MESA_COMPAT_INC}")
	"${TC}-gcc" "${SFLAGS[@]}" -c "${WG}/shims/src/udev_phoenix.c" -o "${out}/shim-obj/udev_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -c "${WG}/shims/src/libevdev_phoenix.c" -o "${out}/shim-obj/libevdev_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -c "${WG}/shims/src/libinput_phoenix.c" -o "${out}/shim-obj/libinput_phoenix.o"
	"${TC}-gcc" "${SFLAGS[@]}" -I"${WG}/phxhid" -c "${WG}/shims/src/libinput_phoenix_hid.c" -o "${out}/shim-obj/libinput_phoenix_hid.o"
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

	# --- the baked keymap (labwc patch 0002: used only when the rule names do not compile,
	# i.e. without the xkeyboard_config port's data): wayland_phoenix's keymap-us.xkb,
	# `xkbcli-compile-keymap --rules evdev --model pc105 --layout us` of libxkbcommon 1.13.2 ---
	local km="${WLR}/keymap-us.xkb"
	grep -q 'xkb_keymap' "${km}" || b_die "labwc_desktop: ${km} is not a keymap"
	python3 - "${km}" "${out}/labwc_keymap.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
with open(dst, 'w') as f:
    f.write('/* Generated by tools/gpu-lane/labwc-drm/build.sh from xkbcli-compile-keymap\n')
    f.write(' * --rules evdev --model pc105 --layout us (host xkeyboard-config). */\n')
    f.write('const char labwc_builtin_xkb_keymap[] =\n')
    for line in text.splitlines():
        f.write('\t"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n')
    f.write('\t;\n')
PY

	# --- wlroots 0.20 (drm + libinput + headless; gles2 + pixman; gbm; libseat) ---
	_meson_pkg wlroots wlroots-build -Dbackends=drm,libinput -Drenderers=gles2 -Dallocators=gbm \
		-Dsession=enabled -Dxwayland=disabled -Dexamples=false -Dlibliftoff=disabled \
		-Dcolor-management=disabled -Dxcb-errors=disabled -Dwerror=false
	grep -E 'drm-backend|libinput-backend|gles2-renderer|gbm-allocator|session|dmabuf_(linux|fallback)' \
		"${out}/wlroots-build-setup.log" | sed 's/^/labwc_desktop: wlroots: /' || true

	# --- text stack for labwc (libxml2, fribidi, pango over the ports cairo/harfbuzz/glib)
	# and for foot (tllist, fcft over the ports fontconfig/freetype/harfbuzz) ---
	# libxml2 with -fstack-protector-strong: libphoenix provides __stack_chk_guard and
	# __stack_chk_fail, and XML parsers are among the ports that get it first.
	EXTRA_C_ARGS="'-fstack-protector-strong'" _meson_pkg libxml2 libxml2-build -Dpython=disabled -Dzlib=disabled -Dicu=disabled -Diconv=disabled \
		-Dhttp=disabled -Dmodules=disabled -Dreadline=disabled -Dhistory=disabled -Ddocs=disabled \
		-Dsax1=enabled -Dcatalog=disabled -Ddebugging=disabled
	_meson_pkg fribidi fribidi-build -Ddocs=false -Dbin=false -Dtests=false
	_meson_pkg pango pango-build -Dintrospection=false -Dgtk_doc=false -Dinstall-tests=false
	_meson_pkg tllist tllist-build
	_meson_pkg fcft fcft-build -Ddocs=disabled -Dtest-text-shaping=false -Dexamples=false \
		-Dgrapheme-shaping=enabled -Drun-shaping=disabled -Dsvg-backend=none

	# --- labwc, foot, fuzzel, swaybg: meson builds the objects, the programs are
	# hand-linked below ---
	_meson_objs labwc labwc-build labwc -Dxwayland=disabled -Dsvg=disabled -Dicon=disabled -Dnls=disabled \
		-Dman-pages=disabled -Dlabnag=disabled -Dtest=disabled -Dsystemd-session=disabled -Dwerror=false
	# foot 1.28 (xterm-256color: ncurses' built-in fallback on Phoenix, no terminfo files).
	# foot's char32_t conversions are compat's UTF-8 ones: MB_CUR_MAX-sized buffers need 4.
	# libphoenix's wchar_t is 32 bits and holds code points (its C-locale mapping is byte =
	# U+0000..U+00FF), which is what __STDC_ISO_10646__ asserts (glibc's stdc-predef.h
	# defines it; foot refuses to build without it).
	# buildtype plain (+ -O2 -g): a "debug*" buildtype defines _DEBUG, which turns foot's
	# and fuzzel's UNITTEST blocks into constructors that run at every start (m7b: the XKB
	# errors of a Swedish-keymap test on the UART)
	BUILDTYPE=plain EXTRA_C_ARGS="'-O2', '-g', '-DLWPHX_UTF8_MB_CUR_MAX', '-D__STDC_ISO_10646__=201706L'" _meson_objs foot foot-build foot -Ddocs=disabled -Dthemes=false -Dime=true -Dgrapheme-clustering=disabled \
		-Dtests=false -Dterminfo=disabled -Ddefault-terminfo=xterm-256color -Dutmp-backend=none -Dwerror=false
	# fuzzel 1.15 (launcher: fcft + pixman, PNG icons, bundled nanosvg; no cairo)
	BUILDTYPE=plain EXTRA_C_ARGS="'-O2', '-g', '-DLWPHX_UTF8_MB_CUR_MAX', '-D__STDC_ISO_10646__=201706L'" _meson_objs fuzzel fuzzel-build fuzzel \
		-Denable-cairo=disabled -Dpng-backend=libpng -Dsvg-backend=nanosvg -Dwerror=false
	# swaybg 1.2 (wallpaper: cairo PNG loader, no gdk-pixbuf)
	_meson_objs swaybg swaybg-build swaybg -Dgdk-pixbuf=disabled -Dman-pages=disabled -Dwerror=false

	# --- link ---
	# Hand links (as weston-drm): Gallium whole-archive (the first line of the Mesa link
	# list), then one group of every other archive; --wrap=mmap/ioctl for libdrm-phoenix,
	# --wrap=close/write for the compat event loop descriptors.
	local OBJ="${out}/obj" gallium="" l
	mkdir -p "${OBJ}"
	local MESA_A=()
	while IFS= read -r l; do
		case "${l}" in
			"--whole-archive "*) gallium="${l#--whole-archive }" ;;
			*/libdrm.a) MESA_A+=("${LD_PREFIX}/lib/libdrm.a") ;;   # THIS build's snapshot
			*/libz.a) MESA_A+=("${D}/zlib/lib/libz.a") ;;
			"") ;;
			*) MESA_A+=("${l}") ;;
		esac
	done <"${mesa_link}"
	[ -f "${gallium}" ] || b_die "labwc_desktop: no gallium archive in ${mesa_link}"

	# shellcheck disable=SC2054 # the commas belong to the -Wl, options
	local LINK_BASE=("${TC}-g++" "${TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000
		-Wl,--wrap=close -Wl,--wrap=write -Wl,--wrap=read -Wl,-u,__wrap_close -Wl,-u,__wrap_write -Wl,-u,__wrap_read)
	# shellcheck disable=SC2054
	local LINK_DRM=("${LINK_BASE[@]}" -Wl,--wrap=mmap -Wl,--wrap=ioctl)
	local COMPAT_LIBS=("${P}/lib/liblwphx-compat.a" "${P}/lib/libwlphx-compat.a")
	local WLR_LIBS=("${P}/lib/libwlroots-0.20.a" "${WLV}/lib/libwayland-server.a" "${WLV}/lib/libwayland-client.a"
		"${WXK}/lib/libxkbcommon.a" "${P}/lib/libdisplay-info.a" "${P}/lib/libseat.a" "${P}/lib/libinput.a"
		"${P}/lib/libudev.a" "${P}/lib/libevdev.a" "${P}/lib/libpixman-1.a" "${D}/libffi/lib/libffi.a" "${COMPAT_LIBS[@]}")
	local BIN="${I}/bin"

	_link_prog() {  # base|drm output link-arguments...
		local kind="$1" o="$2"
		local -a L
		shift 2
		if [ "${kind}" = drm ]; then L=("${LINK_DRM[@]}"); else L=("${LINK_BASE[@]}"); fi
		# shellcheck disable=2086 # LINK_EXTRA: extra flags, one word each
		"${L[@]}" ${LINK_EXTRA:-} -Wl,-Map,"${out}/${o}.map" -o "${BIN}/${o}" "$@" >"${out}/${o}-link.log" 2>&1 ||
			{ grep -v 'warning: .* is not fully supported' "${out}/${o}-link.log" | head -60; b_die "labwc_desktop: ${o}: link failed"; }
		"${TC}-strip" -o "${BIN}/${o}-stripped" "${BIN}/${o}"
		echo "labwc_desktop: ${o}: $(stat -c %s "${BIN}/${o}") bytes, stripped $(stat -c %s "${BIN}/${o}-stripped")"
	}

	# The text stack (labwc: pango/cairo/libxml2/GLib; foot: fcft)
	local TEXT_LIBS=("${P}/lib/libpangocairo-1.0.a" "${P}/lib/libpangoft2-1.0.a" "${P}/lib/libpango-1.0.a"
		"${P}/lib/libfribidi.a" "${D}/cairo/lib/libcairo.a" "${D}/harfbuzz/lib/libhbglib-phoenix.a"
		"${D}/harfbuzz/lib/libharfbuzz.a" "${D}/fontconfig/lib/libfontconfig.a" "${D}/freetype2/lib/libfreetype.a"
		"${D}/expat/lib/libexpat.a" "${D}/libpng16/lib/libpng16.a" "${D}/zlib/lib/libz.a"
		"${D}/gobject-2.0/lib/libgobject-2.0.a" "${D}/glib-2.0/lib/liblwphx-glib.a" "${D}/glib-2.0/lib/libglib-2.0.a"
		"${D}/libiconv/lib/libiconv.a" "${P}/lib/libxml2.a")

	# labwc: the compositor, with the builtin keymap
	"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${out}" -c "${F}/src/labwc_builtin.c" -o "${OBJ}/labwc_builtin.o"
	local -a LABWC_OBJS FOOT_OBJS FUZZEL_OBJS SWAYBG_OBJS
	mapfile -t LABWC_OBJS < <(_ninja_objs "${out}/labwc-build" labwc)
	[ "${#LABWC_OBJS[@]}" -gt 50 ] || b_die "labwc_desktop: labwc objects not found"
	LABWC_OBJS=("${LABWC_OBJS[@]/#/${out}/labwc-build/}")
	# libm BEFORE libstdc++ (whose hypotf stub collides); g++ moves a plain -lm after it
	_link_prog drm labwc "${LABWC_OBJS[@]}" "${OBJ}/labwc_builtin.o" -Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${WLR_LIBS[@]}" "${TEXT_LIBS[@]}" "${MESA_A[@]}" -Wl,--end-group "${S}/lib/libm.a"

	# foot: the terminal (wl_shm client: no Mesa, no libdrm)
	mapfile -t FOOT_OBJS < <(_ninja_objs "${out}/foot-build" foot)
	[ "${#FOOT_OBJS[@]}" -gt 30 ] || b_die "labwc_desktop: foot objects not found"
	FOOT_OBJS=("${FOOT_OBJS[@]/#/${out}/foot-build/}")
	_link_prog base foot -Wl,--start-group "${FOOT_OBJS[@]}" "${P}/lib/libfcft.a" "${WLV}/lib/libwayland-client.a" "${WLV}/lib/libwayland-cursor.a" \
		"${WXK}/lib/libxkbcommon.a" "${P}/lib/libpixman-1.a" "${D}/harfbuzz/lib/libharfbuzz.a" "${D}/fontconfig/lib/libfontconfig.a" \
		"${D}/freetype2/lib/libfreetype.a" "${D}/expat/lib/libexpat.a" "${D}/libpng16/lib/libpng16.a" "${D}/zlib/lib/libz.a" \
		"${D}/libffi/lib/libffi.a" "${COMPAT_LIBS[@]}" -Wl,--end-group "${S}/lib/libm.a"

	# fuzzel: the launcher (wl_shm client, layer-shell)
	mapfile -t FUZZEL_OBJS < <(_ninja_objs "${out}/fuzzel-build" fuzzel)
	[ "${#FUZZEL_OBJS[@]}" -gt 10 ] || b_die "labwc_desktop: fuzzel objects not found"
	FUZZEL_OBJS=("${FUZZEL_OBJS[@]/#/${out}/fuzzel-build/}")
	_link_prog base fuzzel -Wl,--start-group "${FUZZEL_OBJS[@]}" "${P}/lib/libfcft.a" "${WLV}/lib/libwayland-client.a" \
		"${WLV}/lib/libwayland-cursor.a" "${WXK}/lib/libxkbcommon.a" "${P}/lib/libpixman-1.a" "${D}/harfbuzz/lib/libharfbuzz.a" \
		"${D}/fontconfig/lib/libfontconfig.a" "${D}/freetype2/lib/libfreetype.a" "${D}/expat/lib/libexpat.a" \
		"${D}/libpng16/lib/libpng16.a" "${D}/zlib/lib/libz.a" "${D}/libffi/lib/libffi.a" "${COMPAT_LIBS[@]}" \
		-Wl,--end-group "${S}/lib/libm.a"

	# swaybg: the wallpaper (cairo image surface from PNG, layer-shell background)
	mapfile -t SWAYBG_OBJS < <(_ninja_objs "${out}/swaybg-build" swaybg)
	[ "${#SWAYBG_OBJS[@]}" -gt 3 ] || b_die "labwc_desktop: swaybg objects not found"
	SWAYBG_OBJS=("${SWAYBG_OBJS[@]/#/${out}/swaybg-build/}")
	_link_prog base swaybg -Wl,--start-group "${SWAYBG_OBJS[@]}" "${WLV}/lib/libwayland-client.a" "${D}/cairo/lib/libcairo.a" \
		"${P}/lib/libpixman-1.a" "${D}/fontconfig/lib/libfontconfig.a" "${D}/freetype2/lib/libfreetype.a" \
		"${D}/expat/lib/libexpat.a" "${D}/libpng16/lib/libpng16.a" "${D}/zlib/lib/libz.a" "${D}/libffi/lib/libffi.a" \
		"${COMPAT_LIBS[@]}" -Wl,--end-group "${S}/lib/libm.a"

	# --- verification (the tools build's gate) ---
	local bad=0 o und n interp lw strs s bs
	for o in labwc foot fuzzel swaybg; do
		und="$("${TC}-nm" -u "${BIN}/${o}" || true)"
		n=$(grep -c . <<<"${und}" || true)
		interp=$("${TC}-readelf" -l "${BIN}/${o}" | grep -c INTERP || true)
		echo "labwc_desktop: ${o}: nm -u ${n}, PT_INTERP ${interp}; $("${TC}-size" "${BIN}/${o}" | awk 'NR==2 {printf "text %d data %d bss %d", $1, $2, $3}')"
		if [ "${n}" != 0 ] || [ "${interp}" != 0 ]; then head -10 <<<"${und}"; bad=1; fi
		lw="$(grep -v -E 'warning: .*(is not fully supported|dlopen|getpwnam|getpwuid|getgrnam|initgroups)' "${out}/${o}-link.log" 2>/dev/null | grep -c 'warning' || true)"
		echo "labwc_desktop:   link warnings beyond libphoenix attribute notes: ${lw}"
	done
	_check_syms() {  # binary symbols...
		local b="$1" s syms
		shift
		syms="$("${TC}-nm" "${BIN}/${b}")"
		for s in "$@"; do
			grep -qE " [TtDdRrBbWw] ${s}\$" <<<"${syms}" || { echo "labwc_desktop: ${b} symbol ${s}: NO"; bad=1; }
		done
	}
	_check_syms labwc wlr_drm_backend_create wlr_libinput_backend_create wlr_headless_backend_create wlr_gles2_renderer_create_with_drm_fd \
		wlr_pixman_renderer_create wlr_gbm_allocator_create wlr_drm_dumb_allocator_create wlr_session_create libseat_open_seat udev_enumerate_scan_devices \
		di_info_parse_edid labwc_builtin_xkb_keymap xkb_keymap_new_from_string pango_cairo_show_layout xmlReadMemory \
		g_string_replace hb_glib_script_to_script shm_open shm_unlink epoll_wait signalfd timerfd_settime eventfd \
		__wrap_mmap __wrap_ioctl __wrap_close drm_phoenix_ioctl kmsro_drm_screen_create gbmint_get_backend \
		wlr_layer_shell_v1_create wlr_foreign_toplevel_manager_v1_create wlr_xdg_output_manager_v1_create \
		wlr_xdg_activation_v1_create wlr_xdg_decoration_manager_v1_create
	_check_syms foot fcft_from_name wl_display_connect memfd_create epoll_wait epoll_pwait timerfd_settime posix_openpt \
		__wrap_read wlphx_timer_read \
		mbrtoc32 c32rtomb newlocale thrd_create sem_init __wrap_close
	strs="$(strings -a "${BIN}/labwc-stripped")"
	for s in 'using the builtin XKB keymap' 'libdrm-phoenix:' '/dev/dri/card0' 'EGL_KHR_platform_gbm' 'V3D 4.2' \
		'backend/drm' 'render/gles2' 'Unable to open' 'LIBINPUT-PHX' '/shm' 'rc.xml' 'menu.xml' 'autostart'; do
		n=$(grep -cF -- "${s}" <<<"${strs}" || true)
		[ "${n}" != 0 ] || { echo "labwc_desktop: labwc strings '${s}': 0"; bad=1; }
	done
	_check_syms fuzzel fcft_from_name2 wl_display_connect memfd_create epoll_wait timerfd_settime mbrtoc32 sem_init \
		zwlr_layer_shell_v1_interface png_read_info __wrap_close
	_check_syms swaybg wl_display_connect cairo_image_surface_create_from_png zwlr_layer_shell_v1_interface shm_open __wrap_close
	strs="$(strings -a "${BIN}/foot-stripped")"
	for s in 'xterm-256color' 'C.UTF-8' '/dev/ptmx' 'failed to create SHM backing memory file'; do
		n=$(grep -cF -- "${s}" <<<"${strs}" || true)
		[ "${n}" != 0 ] || { echo "labwc_desktop: foot strings '${s}': 0"; bad=1; }
	done
	for o in labwc-stripped foot-stripped fuzzel-stripped swaybg-stripped; do
		bs="$(strings -a "${BIN}/${o}")"
		for s in 'v3d-winsys:' phoenix_v3d_ioctl peek_next_scanout v3d-srv /dev/v3d-srv Xphoenix '[fbdev]' glamor_phoenix phxgl; do
			n=$(grep -cF -- "${s}" <<<"${bs}" || true)
			[ "${n}" = 0 ] || { echo "labwc_desktop: OLD-LANE string '${s}' in ${o}: ${n}"; bad=1; }
		done
	done
	(cd "${BIN}" && sha256sum labwc-stripped foot-stripped fuzzel-stripped swaybg-stripped) >"${I}/SHA256SUMS"
	cat "${I}/SHA256SUMS"
	[ "${bad}" = 0 ] || b_die "labwc_desktop: verification failed"

	# --- the staging tree: stage/ mirrors the target rootfs. labwc's own configuration
	# (/etc/xdg/labwc) is its XDG default; the XFCE session brings its own (xfce_wayland) ---
	local ST="${I}/stage"
	rm -rf "${ST}"
	for o in labwc foot fuzzel swaybg; do
		install -D -m 755 "${BIN}/${o}-stripped" "${ST}/bin/${o}"
	done
	for f in rc.xml menu.xml autostart environment; do
		install -D -m 644 "${F}/conf/${f}" "${ST}/etc/xdg/labwc/${f}"
	done
	install -D -m 644 "${F}/conf/foot/foot.ini" "${ST}/etc/xdg/foot/foot.ini"
	install -D -m 644 "${F}/conf/fuzzel/fuzzel.ini" "${ST}/etc/xdg/fuzzel/fuzzel.ini"
	# The same US keymap as XKB data: SDL's Wayland keyboard creates an xkb_context, which
	# fails ("Failed to create XKB context", the game exits) when no default include path
	# exists. The keymap itself comes from the compositor; this makes /usr/share/X11/xkb
	# real (keymap/ is a standard XKB component directory).
	install -D -m 644 "${km}" "${ST}/usr/share/X11/xkb/keymap/us.xkb"
	for f in foot mc bash; do
		install -D -m 644 "${F}/conf/applications/${f}.desktop" "${ST}/usr/share/applications/${f}.desktop"
	done
	# the wallpaper: generated (deterministic, stdlib only; CC0)
	mkdir -p "${ST}/usr/share/backgrounds/phoenix"
	python3 "${F}/conf/backgrounds/make-wallpaper.py" "${ST}/usr/share/backgrounds/phoenix/phoenix-gradient-1920x1080.png"
	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "labwc_desktop: staged $(wc -l <"${I}/stage.MANIFEST") file(s) into ${PREFIX_FS}/root"
	fi
}
