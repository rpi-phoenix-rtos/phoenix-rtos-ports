#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="xfce_wayland"
	version="4.20"
	desc="XFCE 4.20 on Wayland (libxfce4util, xfconf, libxfce4ui, garcon, exo, libxfce4windowing, Thunar, panel, xfdesktop, settings, appfinder) -- new GPU lane"

	# Aggregate port: XFCE 4.20 for labwc, cross-built STATIC on the GTK 3 Wayland-only
	# stack of gtk3_wayland, as the coordination repo's tools/gpu-lane/xfce-wayland/
	# build.sh builds it (M7 stage 4):
	#
	#   libxfce4util 4.20.1       (meson)     base library: paths, kiosk, i18n helpers
	#   xfconf 4.20.0             (autotools) libxfconf + xfconfd (GDBus; per-channel XML)
	#   libxfce4ui 4.20.2         (autotools) GTK 3 widgets + libxfce4kbd-private; no X11/SM/
	#                                         startup-notification/libgtop/gudev/glade
	#   garcon 4.20.0             (autotools) freedesktop menus (+ garcon-gtk3)
	#   exo 4.20.0                (autotools) GTK 3 extensions
	#   libxfce4windowing 4.20.7  (meson)     Wayland backend only (wlr-foreign-toplevel)
	#   Thunar 4.20.10            (autotools) + thunarx; no gudev/libnotify/exif/plugins
	#   xfce4-panel 4.20.8        (meson)     Wayland + gtk-layer-shell; internal plugins linked in
	#   xfdesktop 4.20.2          (meson)     Wayland: background through gtk-layer-shell
	#   xfce4-settings 4.20.5     (autotools) settings manager + appearance; no X11
	#   xfce4-appfinder 4.20.0    (autotools)
	#   adwaita-icon-theme 3.38.0             the last release with PNG full-colour icons (no
	#                                         librsvg on Phoenix); symbolic icons encoded to
	#                                         .symbolic.png on the build host
	#   shared-mime-info 2.4                  GIO's content types (mime.cache, host-built)
	#
	# Everything is configured --prefix /usr --sysconfdir /etc and installed with DESTDIR
	# (the compiled-in /etc/xdg/xfce4, /usr/share/xfce4, /usr/lib/xfce4/... are the
	# target's). Runtime pairing, not a build dependency: xfconfd is started by D-Bus
	# activation, i.e. the session needs the dbus port's dbus-daemon.
	source="https://archive.xfce.org/src/xfce/libxfce4util/4.20/"
	archive_filename="libxfce4util-4.20.1.tar.bz2"
	src_path="libxfce4util-4.20.1/"

	size="636675"
	sha256="84bfc4daab9e466193540c3665eee42b2cf4d24e3f38fc3e8d1e0a2bebe3b8f1"

	# Thunar, xfconf, xfce4-panel, xfdesktop, xfce4-settings, xfce4-appfinder, exo's tools:
	# GPL-2.0-or-later; libxfce4util, libxfce4ui, garcon, exo, libxfce4panel:
	# LGPL-2.0-or-later; libxfce4windowing: LGPL-2.1-or-later; adwaita-icon-theme:
	# LGPL-3.0-only OR CC-BY-SA-3.0; shared-mime-info (data): GPL-2.0-or-later;
	# files/ (compat, msgfmt stand-in, pngify-icon-theme.py, configuration): BSD-3-Clause.
	license="GPL-2.0-or-later AND LGPL-2.0-or-later AND LGPL-2.1-or-later AND (LGPL-3.0-only OR CC-BY-SA-3.0) AND BSD-3-Clause"
	license_file="COPYING"

	# A real conflict (and the private versioned-ports prefix): XFCE links gtk3_wayland's
	# GLib 2.88, never the ports glib2 2.56.
	conflicts="glib2>=0.0"
	# gtk3_wayland: the whole GTK stack + its private ports views; wayland_phoenix: the M6
	# compat headers (epoll & co.) the Wayland clients compile with.
	depends="gtk3_wayland wayland_phoenix"

	# rootfs: also copy the staging tree (stage/) into the image rootfs.
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   destdir/usr, destdir/etc     everything built here (DESTDIR install)
#   bin/                         the programs, unstripped (addr2line) and -stripped
#   data/icons/{Adwaita,hicolor} the PNG-only icon themes; data/mime/mime.cache
#   stage/ + stage.MANIFEST      the files for the target rootfs (new names only)
#
# Host tools: meson, ninja, python3 (+ gi with GdkPixbuf for icon rendering),
# wayland-scanner 1.24.0, glib-compile-resources, gdbus-codegen, glib-mkenums,
# glib-genmarshal, gtk-encode-symbolic-svg, gtk-update-icon-cache, update-mime-database.
# No gettext: files/bin/msgfmt stands in (no translations are shipped).

# name|file|url|sha256 (the anchor, libxfce4util, is fetched by the framework)
_xfcewl_pkgs() {
	cat <<'EOF'
xfconf|xfconf-4.20.0.tar.bz2|https://archive.xfce.org/src/xfce/xfconf/4.20/xfconf-4.20.0.tar.bz2|8bc43c60f1716b13cf35fc899e2a36ea9c6cdc3478a8f051220eef0f53567efd
libxfce4ui|libxfce4ui-4.20.2.tar.bz2|https://archive.xfce.org/src/xfce/libxfce4ui/4.20/libxfce4ui-4.20.2.tar.bz2|5d3d67b1244a10cee0e89b045766c05fe1035f7938f0410ac6a3d8222b5df907
garcon|garcon-4.20.0.tar.bz2|https://archive.xfce.org/src/xfce/garcon/4.20/garcon-4.20.0.tar.bz2|7fb8517c12309ca4ddf8b42c34bc0c315e38ea077b5442bfcc4509415feada8f
exo|exo-4.20.0.tar.bz2|https://archive.xfce.org/src/xfce/exo/4.20/exo-4.20.0.tar.bz2|4277f799245f1efde01cd917fd538ba6b12cf91c9f8a73fe2035fd5456ec078d
libxfce4windowing|libxfce4windowing-4.20.7.tar.bz2|https://archive.xfce.org/src/xfce/libxfce4windowing/4.20/libxfce4windowing-4.20.7.tar.bz2|01320b279648ab5b13263f8d260bc5958be599e1eaac77c4503598dfcf96c8bb
thunar|thunar-4.20.10.tar.bz2|https://archive.xfce.org/src/xfce/thunar/4.20/thunar-4.20.10.tar.bz2|a5a32b51028dc821155e44cdec025fe70398bae193c619ae6ba8b1babf6f49f1
xfce4-panel|xfce4-panel-4.20.8.tar.bz2|https://archive.xfce.org/src/xfce/xfce4-panel/4.20/xfce4-panel-4.20.8.tar.bz2|d69cb1f377953aeb1fb9bdbcef12c246bea66586e3f2868f3b758e0e8ce3d3fe
xfdesktop|xfdesktop-4.20.2.tar.bz2|https://archive.xfce.org/src/xfce/xfdesktop/4.20/xfdesktop-4.20.2.tar.bz2|1d9bd76015fb6e9aca05e73cd998c7c66ed4fc8c10b626e08fc2eb7c39df3f7b
xfce4-settings|xfce4-settings-4.20.5.tar.bz2|https://archive.xfce.org/src/xfce/xfce4-settings/4.20/xfce4-settings-4.20.5.tar.bz2|a5fbe0e511cce29d603320ade575ad4001bd570e60f37760233237ba478affe8
xfce4-appfinder|xfce4-appfinder-4.20.0.tar.bz2|https://archive.xfce.org/src/xfce/xfce4-appfinder/4.20/xfce4-appfinder-4.20.0.tar.bz2|82ca82f77dc83e285db45438c2fe31df445148aa986ffebf2faabee4af9e7304
adwaita-icon-theme|adwaita-icon-theme-3.38.0.tar.xz|https://download.gnome.org/sources/adwaita-icon-theme/3.38/adwaita-icon-theme-3.38.0.tar.xz|6683a1aaf2430ccd9ea638dd4bfe1002bc92b412050c3dba20e480f979faaf97
shared-mime-info|shared-mime-info-2.4.tar.gz|https://gitlab.freedesktop.org/xdg/shared-mime-info/-/archive/2.4/shared-mime-info-2.4.tar.gz|531291d0387eb94e16e775d7e73788d06d2b2fdd8cd2ac6b6b15287593b6a2de
EOF
}

# _xfcewl_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_xfcewl_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "xfce_wayland: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "xfce_wayland: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# _xfcewl_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name> as its own git
# repository (inside ANOTHER repository -- the buildroot is one -- `git am` would skip
# paths), patches/<name>/*.patch applied by `git am`; re-extracted when the tarball or a
# patch changes.
_xfcewl_extract() {
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
		echo "xfce_wayland: apply ${name}/$(basename "${p}")"
		git -C "${dir}" -c user.name=build -c user.email=build@invalid am -q --whitespace=nowarn "${p}"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built"
}

p_prepare() {
	local t
	for t in meson ninja wayland-scanner python3 git curl glib-compile-resources glib-mkenums glib-genmarshal \
		gdbus-codegen gtk-encode-symbolic-svg gtk-update-icon-cache update-mime-database; do
		command -v "${t}" >/dev/null || b_die "xfce_wayland: host tool ${t} not found"
	done
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "1.24.0" ] ||
		b_die "xfce_wayland: host wayland-scanner is not 1.24.0"

	_xfcewl_extract libxfce4util "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
	local n file url sum
	while IFS='|' read -r n file url sum; do
		_xfcewl_extract "${n}" "$(_xfcewl_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_xfcewl_pkgs)
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local S TC gtk_out WLP
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	gtk_out="$(b_dependency_dir gtk3_wayland)"
	gtk_out="${gtk_out%/}"
	WLP="$(b_dependency_dir wayland_phoenix)"
	local F="${PREFIX_PORT}/files"
	local PHXCC="${gtk_out}/host-bin/phx-gcc"   # drops -pthread, also inside @response files
	local COMPAT_INC="${WLP%/}/prefix/share/wayland-phoenix/compat/include"
	local GS="${out}/gtk"                  # the GTK snapshot (a symlink tree, see below)
	local X="${I}/destdir"                 # DESTDIR of this build
	local P="${X}/usr"
	local SYSD="${GS}/deps/sys"            # libintl stub, libiconv, resolv (from the GTK build)
	local XCOMPAT_INC="${F}/compat/include"   # libphoenix gaps (files/compat/src): first on the include path
	local XCOMPAT_A="${out}/compat/libxfphx-compat.a"
	local WAYLAND_VERSION=1.24.0
	local jobs
	jobs="$(nproc)"

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" "${PHXCC}" \
		"${gtk_out}/destdir/usr/lib/libgtk-3.a" "${gtk_out}/destdir/usr/lib/libgtk-layer-shell.a" \
		"${gtk_out}/deps/wayland/lib/libwlphx-compat.a" "${gtk_out}/phoenix-aarch64-gtk.cross"; do
		[ -e "${p}" ] || b_die "xfce_wayland: missing ${p}"
	done
	# the GTK prefix must be a /usr one: its pkg-config files say prefix=/usr
	grep -q '^prefix=/usr$' "${gtk_out}/destdir/usr/lib/pkgconfig/gtk+-3.0.pc" ||
		b_die "xfce_wayland: ${gtk_out} is not a /usr-configured GTK build"

	# A fresh work tree (no .built stamps) starts from an empty install.
	if [ ! -f "${out}/install.fresh" ]; then
		rm -rf "${X:?}" "${I:?}/bin" "${I:?}/data" "${I:?}/stage"
		touch "${out}/install.fresh"
	fi
	mkdir -p "${out}/hostbin" "${I}/bin" "${out}/obj" "${P}/lib/pkgconfig"
	# msgfmt stand-in + xdt-gen-visibility (from the libxfce4util tarball, GPL: not committed)
	export PATH="${F}/bin:${out}/hostbin:${PATH}"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")
	install -m 755 "${out}/src/libxfce4util/xdt-gen-visibility" "${out}/hostbin/xdt-gen-visibility"

	# --- the GTK snapshot, cross files, pkg-config ---
	# A symlink tree of gtk3_wayland's destdir/ and deps/ (the tools build copies them:
	# 1.6 GB): the .pc files resolve their prefix from where they lie (--define-prefix), so
	# the tree must have its own directories, but the files may be links. GLib's .pc tool
	# variables are rewritten by --define-prefix into this tree (a variable starting with
	# the old prefix /usr moves with it): the tools there must be the HOST's, so those
	# links are pointed at the host's instead (gtk3_wayland's install is not touched).
	local stamp
	stamp="$(sha256sum "${gtk_out}/SHA256SUMS" "${gtk_out}/destdir/usr/lib/libgtk-3.a" \
		"${gtk_out}/destdir/usr/lib/libglib-2.0.a" "${gtk_out}/deps/wayland/lib/libwlphx-compat.a" | sha256sum | cut -c1-16)"
	if [ "$(cat "${GS}.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		rm -rf "${GS}"
		mkdir -p "${GS}"
		cp -rs "${gtk_out}/destdir" "${gtk_out}/deps" "${GS}/"
		cp -a "${gtk_out}/SHA256SUMS" "${GS}/SHA256SUMS.gtk3_wayland"
		echo "${stamp}" >"${GS}.stamp"
		rm -f "${out}"/*.built   # everything is rebuilt on a new GTK stack
		echo "xfce_wayland: snapshot of ${gtk_out} (stamp ${stamp})"
	fi
	for t in glib-compile-resources glib-compile-schemas glib-mkenums glib-genmarshal gdbus-codegen gobject-query; do
		[ -e "${GS}/destdir/usr/bin/${t}" ] || [ -L "${GS}/destdir/usr/bin/${t}" ] || continue
		command -v "${t}" >/dev/null || continue
		ln -sfn "$(command -v "${t}")" "${GS}/destdir/usr/bin/${t}"
	done
	{ echo "gtk3_wayland: ${gtk_out} (stamp ${stamp})"
	  sha256sum "${GS}"/destdir/usr/lib/lib{gtk-3,gdk-3,glib-2.0,gio-2.0,gtk-layer-shell}.a "${GS}/deps/wayland/lib/libwlphx-compat.a" |
		sed "s|${GS}/||"; } >"${I}/snapshots.txt"

	local pkgc="${out}/pkg-config-phoenix" v libdirs="" c
	for v in zlib libffi expat pixman-1 libpng16 libjpeg freetype2 fontconfig wayland epoxy; do
		libdirs="${libdirs}:${GS}/deps/${v}/lib/pkgconfig"
	done
	# the HOST's wayland-scanner (xfce4-settings' configure asks pkg-config for it)
	mkdir -p "${out}/hostpc"
	printf '%s\n' "wayland_scanner=$(command -v wayland-scanner)" "Name: Wayland Scanner" \
		"Description: the build host's wayland-scanner" "Version: ${WAYLAND_VERSION}" >"${out}/hostpc/wayland-scanner.pc"
	libdirs="${libdirs}:${out}/hostpc"
	cat >"${pkgc}" <<EOF
#!/bin/sh
# pkg-config over this build's DESTDIR, the GTK snapshot and its private ports views. Every
# installed .pc says prefix=/usr; --define-prefix takes each file's prefix from where it lies.
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig:${GS}/destdir/usr/lib/pkgconfig:${GS}/destdir/usr/share/pkgconfig${libdirs}:${GS}/deps/wayland/share/pkgconfig
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
exec /usr/bin/pkg-config --static --define-prefix "\$@"
EOF
	chmod +x "${pkgc}"
	# the GTK build's cross files, pointed at the snapshot and this pkg-config
	for c in "" -wl -gtk; do
		# + the compat headers first and the compat archive whole (meson puts c_link_args
		# before the objects; --gc-sections drops what a program does not use)
		sed -e "s|${gtk_out}/deps|${GS}/deps|g" -e "s|${gtk_out}/destdir|${GS}/destdir|g" \
			-e "s|^pkg-config = .*|pkg-config = '${pkgc}'|" \
			-e "s#^\(c\|cpp\)_args = \[#\1_args = ['-I${XCOMPAT_INC}', '-fmacro-prefix-map=../src/=', '-fmacro-prefix-map=${GS}/destdir/usr/include/=', #" \
			-e "s#^\(c\|cpp\)_link_args = \[#\1_link_args = ['-Wl,--whole-archive,${XCOMPAT_A},--no-whole-archive', '-Wl,--gc-sections', #" \
			-e "s|^# Generated by .*|# Generated by the phoenix-rtos-ports xfce_wayland recipe from gtk3_wayland's phoenix-aarch64${c}.cross|" \
			"${gtk_out}/phoenix-aarch64${c}.cross" >"${out}/phoenix-aarch64${c}.cross"
	done

	# XFCE C code: gnu11 (GCC 16 defaults to C23, where `()` means `(void)` and bool is a
	# keyword). -fmacro-prefix-map: __FILE__ in g_return_if_fail() messages without
	# build-host paths.
	local CF_BASE="-O2 -g -std=gnu11 ${TFLAGS[*]} -fmacro-prefix-map=${out}/src/= -fmacro-prefix-map=${GS}/destdir/usr/include/= -I${XCOMPAT_INC} -I${SYSD}/include"
	local CF_GTK="${CF_BASE} -I${COMPAT_INC} -I${GS}/deps/wayland/include"
	local LD_BASE="--sysroot=${S}/ -B${S}/lib/ -L${SYSD}/lib -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--whole-archive,${XCOMPAT_A},--no-whole-archive"  # one token: libtool reorders a bare .a

	_meson_pkg() {  # [--cross <file>] name meson-args...
		local cross="${out}/phoenix-aarch64.cross"
		if [ "$1" = --cross ]; then cross="$2"; shift 2; fi
		local name="$1"
		shift
		local bd="${out}/${name}-build"
		if [ -f "${out}/${name}.built" ]; then
			echo "xfce_wayland: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${cross}" --prefix /usr --sysconfdir /etc \
			--localstatedir /var --libdir lib --buildtype=debugoptimized -Db_staticpic=false -Db_ndebug=if-release \
			--wrap-mode=nodownload "$@" >"${out}/${name}-setup.log" 2>&1 || { tail -40 "${out}/${name}-setup.log"; b_die "xfce_wayland: ${name}: meson setup failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${name}-ninja.log" 2>&1 || { grep -E -A3 'error:|undefined reference|FAILED' "${out}/${name}-ninja.log" | cut -c1-400 | head -60; b_die "xfce_wayland: ${name}: build failed"; }
		DESTDIR="${X}" ninja -C "${bd}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "xfce_wayland: ${name}: install failed"; }
		echo "xfce_wayland: ${name}: built ($(grep -c 'warning:' "${out}/${name}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	_ac_pkg() {  # [--gtk] name configure-args...
		local cf="${CF_BASE}"
		if [ "$1" = --gtk ]; then cf="${CF_GTK}"; shift; fi
		local name="$1"
		shift
		local bd="${out}/${name}-build" src="${out}/src/${name}"
		if [ -f "${out}/${name}.built" ]; then
			echo "xfce_wayland: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		mkdir -p "${bd}"
		(cd "${bd}" && "${src}/configure" --host=aarch64-phoenix --build="$("${src}/config.guess")" \
			--prefix=/usr --sysconfdir=/etc --localstatedir=/var --libdir=/usr/lib --disable-shared --enable-static \
			--disable-nls --disable-silent-rules --disable-maintainer-mode \
			CC="${PHXCC}" CFLAGS="${cf}" LDFLAGS="${LD_BASE}" PKG_CONFIG="${pkgc}" \
			AR="${TC}-gcc-ar" RANLIB="${TC}-gcc-ranlib" NM="${TC}-nm" STRIP="${TC}-strip" "$@") \
			>"${out}/${name}-configure.log" 2>&1 || { tail -40 "${out}/${name}-configure.log"; b_die "xfce_wayland: ${name}: configure failed"; }
		make -C "${bd}" -j"${jobs}" >"${out}/${name}-make.log" 2>&1 || { grep -E -A3 'error:|undefined reference|\*\*\*' "${out}/${name}-make.log" | cut -c1-400 | head -60; b_die "xfce_wayland: ${name}: build failed"; }
		make -C "${bd}" DESTDIR="${X}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "xfce_wayland: ${name}: install failed"; }
		# static archives only: libtool .la files would point later links at /usr/lib (the host's)
		find "${X}" -name '*.la' -delete
		echo "xfce_wayland: ${name}: built ($(grep -c 'warning:' "${out}/${name}-make.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	# --- compat (files/compat/src: libphoenix gaps) ---
	mkdir -p "${out}/compat"
	local cobjs=() o
	for c in "${F}"/compat/src/*.c; do
		o="${out}/compat/$(basename "${c%.c}").o"
		"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${XCOMPAT_INC}" -c "${c}" -o "${o}"
		cobjs+=("${o}")
	done
	rm -f "${XCOMPAT_A}"
	"${TC}-gcc-ar" rcs "${XCOMPAT_A}" "${cobjs[@]}"

	# --- stage 1: the XFCE libraries ---
	_meson_pkg libxfce4util -Dintrospection=false -Dvala=disabled -Dgtk-doc=false
	_ac_pkg xfconf --disable-gsettings-backend --disable-introspection --disable-vala --disable-checks \
		--disable-profiling --with-helper-path-prefix=/usr/lib --with-bash-completion-dir=no
	_ac_pkg --gtk libxfce4ui --disable-x11 --enable-wayland --disable-libsm --disable-startup-notification \
		--disable-glibtop --disable-epoxy --disable-gudev --disable-introspection --disable-vala --disable-gladeui2 \
		--disable-tests --with-vendor-info=Phoenix-RTOS
	_ac_pkg --gtk garcon --disable-introspection
	_ac_pkg --gtk exo --enable-gio-unix
	_meson_pkg --cross "${out}/phoenix-aarch64-wl.cross" libxfce4windowing -Dx11=disabled -Dwayland=enabled \
		-Dintrospection=false -Dvala=disabled -Dgtk-doc=false -Dtests=false

	# --- stage 2: Thunar ---
	_ac_pkg --gtk thunar --without-x --disable-gudev --disable-notifications --disable-exif --enable-pcre2 \
		--disable-apr-plugin --disable-sbr-plugin --disable-tpa-plugin --disable-uca-plugin \
		--disable-wallpaper-plugin --disable-introspection --with-helper-path-prefix=/usr/lib

	# --- stage 3: xfce4-panel (Wayland + gtk-layer-shell; the internal plugins linked in: patch 0001) ---
	_meson_pkg --cross "${out}/phoenix-aarch64-wl.cross" xfce4-panel -Dx11=disabled -Dwayland=enabled \
		-Dgtk-layer-shell=enabled -Ddbusmenu=disabled -Dintrospection=false -Dvala=disabled -Dgtk-doc=false \
		-Dbuiltin-plugins=true -Dhelper-path-prefix=/usr/lib

	# --- stage 4: xfdesktop (the backdrop on gtk-layer-shell; window icons, no file icons) ---
	_meson_pkg --cross "${out}/phoenix-aarch64-wl.cross" xfdesktop -Dx11=disabled -Dwayland=enabled \
		-Ddesktop-menu=enabled -Ddesktop-icons=true -Dfile-icons=false -Dthunarx=disabled -Dnotifications=disabled \
		-Dtests=false -Dfile-manager-fallback=/bin/thunar-wl \
		-Ddefault-backdrop-filename=backgrounds/phoenix/phoenix-gradient-1920x1080.png

	# --- stage 5: xfce4-settings, xfce4-appfinder ---
	_ac_pkg --gtk xfce4-settings --disable-x11 --enable-wayland --disable-xrandr --disable-xcursor \
		--disable-xorg-libinput --disable-libxklavier --disable-libnotify --enable-gtk-layer-shell \
		--disable-upower-glib --disable-colord --disable-sound-settings --with-helper-path-prefix=/usr/lib
	_ac_pkg --gtk xfce4-appfinder

	# --- data: PNG icon themes, MIME database (data/ mirrors the target) ---
	local D="${I}/data"
	stamp="$( { cat "${F}/tools/pngify-icon-theme.py"; echo adwaita-icon-theme-3.38.0 6683a1aaf2430ccd9ea638dd4bfe1002bc92b412050c3dba20e480f979faaf97;
		find "${P}/share/icons/hicolor" -type f -printf '%P %s\n' | sort; } | sha256sum | cut -c1-16)"
	if [ "$(cat "${D}/icons.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		rm -rf "${D}/icons"
		mkdir -p "${D}/icons"
		# Adwaita 3.38: full-colour PNGs as shipped (menu/toolbar/dialog/panel sizes), the
		# symbolic SVGs encoded at 16 and 24 px
		python3 "${F}/tools/pngify-icon-theme.py" --name Adwaita --inherits hicolor --sizes 16,24 \
			--include-sizes 16x16,22x22,24x24,32x32,48x48 --jobs "${jobs}" \
			"${D}/icons/Adwaita" "${out}/src/adwaita-icon-theme/Adwaita"
		# hicolor: the XFCE programs' own icons (org.xfce.*); scalable ones rendered to PNG
		python3 "${F}/tools/pngify-icon-theme.py" --name hicolor --inherits "" --sizes 16,24,32,48 \
			--comment "Fallback icon theme (XFCE application icons, PNG only)" --jobs "${jobs}" \
			"${D}/icons/hicolor" "${P}/share/icons/hicolor"
		for t in Adwaita hicolor; do
			gtk-update-icon-cache -f -q -t "${D}/icons/${t}"
		done
		echo "${stamp}" >"${D}/icons.stamp"
	fi
	# shared-mime-info 2.4: GIO's content types (xdgmime reads mime.cache alone when valid)
	rm -rf "${D}/mime"
	mkdir -p "${D}/mime/packages"
	cp "${out}/src/shared-mime-info/data/freedesktop.org.xml.in" "${D}/mime/packages/freedesktop.org.xml"
	update-mime-database -n "${D}/mime"
	find "${D}/mime" -mindepth 1 -maxdepth 1 ! -name mime.cache -exec rm -rf {} +

	# --- programs: collect, strip, verify ---
	# name|installed path (under destdir/usr)|symbols that must be linked in
	local PROGS=(
		"xfconfd|lib/xfce4/xfconf/xfconfd|g_bus_own_name xfconf_backend_factory_get_backend g_dbus_connection_register_object"
		"xfconf-query|bin/xfconf-query|xfconf_channel_get_property xfconf_init"
		"gdbus|bin/gdbus|g_dbus_connection_new_for_address_sync _g_dbus_auth_mechanism_anon_get_type"
		"xfce4-panel|bin/xfce4-panel|panel_builtin_plugins xfce_panel_builtin_applicationsmenu_init xfce_panel_builtin_clock_init xfce_panel_builtin_tasklist_init xfce_panel_builtin_windowmenu_init xfce_panel_builtin_launcher_init xfce_panel_builtin_separator_init xfce_panel_builtin_actions_init gtk_layer_init_for_window xfw_screen_get_default garcon_menu_new_for_path"
		"xfdesktop|bin/xfdesktop|xfce_desktop_new gtk_layer_init_for_window xfw_screen_get_default gdk_wayland_display_get_type"
		"xfce4-settings-manager|bin/xfce4-settings-manager|garcon_menu_new_for_path xfconf_channel_get gdk_wayland_display_get_type"
		"xfce4-appearance-settings|bin/xfce4-appearance-settings|xfconf_channel_get gtk_icon_theme_get_default gdk_wayland_display_get_type"
		"xfce4-appfinder|bin/xfce4-appfinder|garcon_menu_new_applications xfconf_channel_get gdk_wayland_display_get_type"
		"thunar|bin/thunar|thunar_application_get gdk_wayland_display_get_type xfconf_channel_get exo_icon_view_new xfce_dialog_show_error thunarx_provider_factory_get_default g_file_monitor_directory"
	)
	local bad=0 rec name path syms f und n interp allsyms x11 s
	local BIN="${I}/bin"
	: >"${BIN}/SHA256SUMS.tmp"
	for rec in "${PROGS[@]}"; do
		IFS='|' read -r name path syms <<<"${rec}"
		f="${P}/${path}"
		[ "${name}" = gdbus ] && f="${GS}/destdir/usr/${path}"
		[ -f "${f}" ] || { echo "xfce_wayland: ${name}: MISSING (${f})"; bad=1; continue; }
		cp -aL "${f}" "${BIN}/${name}"
		"${TC}-strip" -o "${BIN}/${name}-stripped" "${BIN}/${name}"
		und="$("${TC}-nm" -u "${BIN}/${name}" || true)"
		n=$(grep -c . <<<"${und}" || true)
		interp="$("${TC}-readelf" -l "${BIN}/${name}" | grep -c INTERP || true)"
		allsyms="$("${TC}-nm" "${BIN}/${name}")"
		x11="$(grep -cE ' (XOpenDisplay|XInternAtom|xcb_connect|gdk_x11_display_get_type|SmcOpenConnection|wnck_screen_get_default)$' <<<"${allsyms}" || true)"
		echo "xfce_wayland: ${name}: nm -u ${n}, PT_INTERP ${interp}, X11 symbols ${x11}; $("${TC}-size" "${BIN}/${name}" | awk 'NR==2 {printf "text %d data %d bss %d", $1, $2, $3}'); stripped $(stat -c %s "${BIN}/${name}-stripped")"
		if [ "${n}" != 0 ] || [ "${interp}" != 0 ] || [ "${x11}" != 0 ]; then head -10 <<<"${und}"; bad=1; fi
		for s in ${syms}; do
			grep -qE " [TtWwDdBbRr] ${s}\$" <<<"${allsyms}" || { echo "xfce_wayland: ${name}: symbol ${s} missing"; bad=1; }
		done
		o="$(strings -a "${BIN}/${name}-stripped" | grep -cE 'port-sources|versioned-ports|/home/' || true)"
		echo "xfce_wayland:   build-host path strings: ${o}"
		(cd "${BIN}" && sha256sum "${name}-stripped") >>"${BIN}/SHA256SUMS.tmp"
	done
	{ echo "# xfce_wayland port build"; cat "${BIN}/SHA256SUMS.tmp"; } >"${I}/SHA256SUMS"
	rm -f "${BIN}/SHA256SUMS.tmp"
	cat "${I}/SHA256SUMS"
	[ "${bad}" = 0 ] || b_die "xfce_wayland: verification failed"

	# --- the staging tree: stage/ mirrors the target rootfs (new names only; the m7f-thunar
	# and m7h-xfce staging, docs/gpu-new-lane/M7-wayland-desktop.md in the coordination repo)
	local ST="${I}/stage"
	rm -rf "${ST}"
	_st() {  # mode source target-path
		install -D -m "$1" "$2" "${ST}/$3"
	}
	_st 755 "${BIN}/xfconfd-stripped" usr/lib/xfce4/xfconf/xfconfd
	_st 755 "${BIN}/xfconf-query-stripped" bin/xfconf-query
	_st 644 "${P}/share/dbus-1/services/org.xfce.Xfconf.service" usr/share/dbus-1/services/org.xfce.Xfconf.service
	_st 755 "${F}/pi/xfce-desktop.sh" bin/xfce-desktop.sh
	for f in rc.xml menu.xml autostart environment; do
		_st 644 "${F}/conf/labwc-xfce/${f}" "etc/xdg/labwc-xfce/${f}"
	done
	for f in "${F}"/conf/xfconf/*.xml "${X}"/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/*.xml; do
		_st 644 "${f}" "etc/xdg/xfce4/xfconf/xfce-perchannel-xml/$(basename "${f}")"
	done
	_st 755 "${BIN}/thunar-stripped" bin/thunar-wl
	_st 644 "${F}/conf/applications/thunar.desktop" usr/share/applications/thunar.desktop
	_st 644 "${D}/mime/mime.cache" usr/share/mime/mime.cache
	mkdir -p "${ST}/usr/share/icons"
	cp -a "${D}/icons/Adwaita" "${D}/icons/hicolor" "${ST}/usr/share/icons/"
	_st 755 "${BIN}/xfce4-panel-stripped" bin/xfce4-panel
	for f in "${P}"/share/xfce4/panel/plugins/*.desktop; do
		_st 644 "${f}" "usr/share/xfce4/panel/plugins/$(basename "${f}")"
	done
	_st 644 "${X}/etc/xdg/xfce4/panel/default.xml" etc/xdg/xfce4/panel/default.xml
	# garcon's menu (the applications menu plugin, xfce4-appfinder)
	_st 644 "${X}/etc/xdg/menus/xfce-applications.menu" etc/xdg/menus/xfce-applications.menu
	for f in "${P}"/share/desktop-directories/*.directory; do
		_st 644 "${f}" "usr/share/desktop-directories/$(basename "${f}")"
	done
	_st 755 "${BIN}/xfdesktop-stripped" bin/xfdesktop
	for p in xfce4-settings-manager xfce4-appearance-settings xfce4-appfinder; do
		_st 755 "${BIN}/${p}-stripped" "bin/${p}"
	done
	for f in xfce-settings-manager xfce-ui-settings xfce4-appfinder xfce4-run; do
		_st 644 "${P}/share/applications/${f}.desktop" "usr/share/applications/${f}.desktop"
	done
	_st 644 "${X}/etc/xdg/menus/xfce-settings-manager.menu" etc/xdg/menus/xfce-settings-manager.menu
	_st 644 "${X}/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml" etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml
	# GIO's own gdbus (from the GTK stack) under a new name
	_st 755 "${BIN}/gdbus-stripped" bin/gdbus-wl

	# --- the XFCE session in one command: /bin/xfce-session (the tools' stage-demo tree,
	# m7i-xfce-demo / m7l-session2), on the programs staged above rather than copies of its
	# own. files/pi/xfce-session is the tools script verbatim; /bin/xfce-session is a small
	# generated wrapper that points its knobs at this image: the programs above, labwc and
	# foot from labwc_desktop, the servers started at boot (/sbin, /bin/shmsrv) and labwc's
	# GLES2 renderer (the desktop composited on the V3D, as xfce-session-2). The demo's
	# labwc/panel/fuzzel configs and .desktop files are staged with the same program names.
	# TODO(TD-26): the demo's own path names (/usr/lib/xfce-demo, /etc/xdg/*-demo) and the
	# rewrite below go when P4 names the session's files for the image.
	_st 755 "${F}/pi/xfce-session" usr/lib/xfce-demo/xfce-session
	_st 755 "${F}/pi/xfce-demo-loginctl" usr/lib/xfce-demo/bin/loginctl
	local demo_sed=(-e 's|/usr/lib/xfce-demo/bin/thunar|/bin/thunar-wl|g'
		-e 's|/usr/lib/xfce-demo/bin/xfce4-|/bin/xfce4-|g'
		-e 's|/usr/lib/xfce-demo/bin/xfdesktop|/bin/xfdesktop|g'
		-e 's|/bin/foot-2|/bin/foot|g' -e 's|/bin/fuzzel-2|/bin/fuzzel|g')
	_st_demo() {  # source target-path: a config file, program paths rewritten for the image
		mkdir -p "$(dirname "${ST}/$2")"
		sed "${demo_sed[@]}" "$1" >"${ST}/$2"
		chmod 644 "${ST}/$2"
	}
	for f in rc.xml menu.xml autostart environment; do
		_st_demo "${F}/conf/labwc-xfce-demo/${f}" "etc/xdg/labwc-xfce-demo/${f}"
	done
	_st_demo "${F}/conf/xfce-demo/xfce4-panel.xml" etc/xdg/xfce-demo/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml
	_st_demo "${F}/conf/xfce-demo/fuzzel.ini" etc/xdg/xfce-demo/fuzzel/fuzzel.ini
	for f in "${F}"/conf/xfce-demo/applications/*.desktop; do
		_st_demo "${f}" "usr/share/xfce-demo/applications/$(basename "${f}")"
	done
	if grep -rnE '/bin/(foot|fuzzel|labwc)-2|xfce-demo/bin/(thunar|xfce4-|xfdesktop)' \
			"${ST}/etc/xdg/labwc-xfce-demo" "${ST}/etc/xdg/xfce-demo" "${ST}/usr/share/xfce-demo"; then
		b_die "xfce_wayland: a demo config still names a program this image does not have (above)"
	fi
	printf '%s\n' '#!/bin/bash' \
		'# xfce-session: the XFCE 4.20 desktop on labwc, composited on the V3D, until Log Out.' \
		'# Knobs (psh `export NAME=value` first): /usr/lib/xfce-demo/xfce-session.' \
		'# Generated by the xfce_wayland port.' \
		'export LABWC=${LABWC:-/bin/labwc}' \
		'export SESSION_SCRIPT=${SESSION_SCRIPT:-/bin/xfce-desktop.sh}' \
		'export THUNAR=${THUNAR:-/bin/thunar-wl}' \
		'export PANEL=${PANEL:-/bin/xfce4-panel}' \
		'export XFDESKTOP=${XFDESKTOP:-/bin/xfdesktop}' \
		'export RENDERER=${RENDERER:-gles2}' \
		'export V3DA_CMD=${V3DA_CMD:-/sbin/rpi4-v3d-async -r 1 -m serial -i}' \
		'export KMS_CMD=${KMS_CMD:-/sbin/rpi4-kms -G -p 96 -C}' \
		'export SHMSRV_CMD=${SHMSRV_CMD:-/bin/shmsrv}' \
		'exec /bin/bash /usr/lib/xfce-demo/xfce-session "$@"' >"${ST}/bin/xfce-session"
	chmod 755 "${ST}/bin/xfce-session"

	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"
	echo "xfce_wayland: stage: $(wc -l <"${I}/stage.MANIFEST") files"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "xfce_wayland: staged into ${PREFIX_FS}/root"
	fi
}
