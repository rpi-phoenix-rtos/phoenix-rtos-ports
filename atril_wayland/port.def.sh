#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="atril_wayland"
	version="1.28.7"
	desc="Atril 1.28 PDF viewer (Poppler 26.09 backend built in) on the GTK 3 Wayland stack -- new GPU lane desktop"

	# Aggregate port: Atril, MATE's GTK 3 document viewer, with the Poppler PDF backend,
	# cross-built STATIC on the GTK 3 Wayland-only stack of gtk3_wayland, as the
	# coordination repo's tools/gpu-lane/atril-wayland/build.sh builds it (M7 stage 6):
	#
	#   libxml2 2.15.4      (meson)  Atril: PDF XMP metadata, the toolbar editor
	#   lcms2 2.19.1        (meson)  Poppler: ICC colour management
	#   openjpeg 2.5.4      (cmake)  Poppler: JPEG 2000 (JPX) images
	#   Poppler 26.09.0     (cmake)  PDF rendering: core + poppler-glib (cairo); no Qt/cpp/
	#                                utils/NSS/GPGME/curl/tiff/boost/harfbuzz (files/poppler-options.sh)
	#   Atril 1.28.7        (meson)  the PDF backend LINKED IN (patch 0004), no X11/SM, no
	#                                mate-desktop, no D-Bus daemon, no keyring, no caja
	#                                extension/thumbnailer/previewer/introspection
	#
	# Everything is configured --prefix /usr --sysconfdir /etc and installed with DESTDIR,
	# on gtk3_wayland's /usr-configured install (a symlink snapshot, as xfce_wayland reads it).
	source="https://github.com/mate-desktop/atril/releases/download/v${version}"
	archive_filename="atril-${version}.tar.xz"
	src_path="atril-${version}/"

	size="2215752"
	sha256="91942545e858cb0036b52e2f5d4372cdea813d9fde9702daefdf84854e6bb667"

	# Atril: GPL-2.0-or-later (libdocument/libview: LGPL-2.0-or-later); Poppler:
	# GPL-2.0-only OR GPL-3.0-only; libxml2, lcms2: MIT; openjpeg: BSD-2-Clause; files/
	# (poppler-options.sh, the msgfmt stand-in, make-sample-pdf.py, the session script,
	# atril.desktop) and the generated sample PDF: BSD-3-Clause.
	license="GPL-2.0-or-later AND LGPL-2.0-or-later AND (GPL-2.0-only OR GPL-3.0-only) AND MIT AND BSD-2-Clause AND BSD-3-Clause"
	license_file="COPYING"

	# A real conflict (and the private versioned-ports prefix): Atril links gtk3_wayland's
	# GLib 2.88, never the ports glib2 2.56.
	conflicts="glib2>=0.0"
	# gtk3_wayland: the whole GTK stack + its private ports views (zlib, libpng16, libjpeg,
	# freetype2, fontconfig, ...); wayland_phoenix: the M6 compat headers its -wl cross file
	# names.
	depends="gtk3_wayland wayland_phoenix"

	# rootfs: also copy the staging tree (stage/) into the image rootfs.
	iuse="rootfs"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   destdir/usr                  everything built here (static .a, headers, .pc, atril)
#   bin/                         atril, unstripped (addr2line) and -stripped
#   data/                        Atril's compiled GSettings schema, the sample PDF
#   stage/ + stage.MANIFEST      the files for the target rootfs
#   SHA256SUMS
#
# On the target: /usr/bin/atril, its schema in /usr/share/atril/schemas (patch 0005: Atril
# appends that directory to GSETTINGS_SCHEMA_DIR itself, so gtk3_wayland's shared
# /usr/share/glib-2.0/schemas/gschemas.compiled is never replaced and no wrapper is needed),
# its icons under /usr/share/atril/icons, /usr/share/applications/atril.desktop (the XFCE
# applications menu: Office) and /usr/share/doc/phoenix/sample.pdf.
#
# Host tools: meson, ninja, cmake >= 3.28, python3 with pycairo + the DejaVu fonts (the
# sample PDF), glib-compile-resources, glib-compile-schemas, glib-mkenums, glib-genmarshal,
# gdbus-codegen. No gettext: files/bin/msgfmt stands in (no translations are shipped).

# name|file|url|sha256 (the anchor, atril, is fetched by the framework)
_atrilwl_pkgs() {
	cat <<'EOF'
libxml2|libxml2-2.15.4.tar.xz|https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz|98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
lcms2|lcms2-2.19.1.tar.gz|https://github.com/mm2/Little-CMS/releases/download/lcms2.19.1/lcms2-2.19.1.tar.gz|bfc54f7bab59fbc921012014a8032e4cba4abd46db47d46b76416a8c0b2815c8
openjpeg|openjpeg-2.5.4.tar.gz|https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.4.tar.gz|a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a
poppler|poppler-26.09.0.tar.xz|https://poppler.freedesktop.org/poppler-26.09.0.tar.xz|8059eadb6805340768f138c465b57f8164c92b4a0773c37ef031ea6c0d987b2e
EOF
}

# _atrilwl_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_atrilwl_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "atril_wayland: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "atril_wayland: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# _atrilwl_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name> as its own git
# repository (inside ANOTHER repository -- the buildroot is one -- `git am` would skip
# paths), patches/<name>/*.patch applied by `git am`; re-extracted when the tarball or a
# patch changes.
_atrilwl_extract() {
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
		echo "atril_wayland: apply ${name}/$(basename "${p}")"
		git -C "${dir}" -c user.name=build -c user.email=build@invalid am -q --whitespace=nowarn "${p}"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built"
}

# The link inputs every port links implicitly (libphoenix.a, libgcc.a, libstdc++.a) changed.
# Each package here builds under an out/<name>.built stamp, so the framework's default --
# delete the linked programs and let the build relink them -- would leave atril deleted: the
# stamp says "up to date" and nothing relinks. Drop the stamps instead: every package is set
# up and built again from its unpacked sources.
p_relink() {
	rm -f "${PREFIX_PORT_BUILD}/out"/*.built
}

p_prepare() {
	local t
	for t in meson ninja cmake python3 git curl glib-compile-resources glib-compile-schemas glib-mkenums \
		glib-genmarshal gdbus-codegen; do
		command -v "${t}" >/dev/null || b_die "atril_wayland: host tool ${t} not found"
	done

	_atrilwl_extract atril "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
	local n file url sum
	while IFS='|' read -r n file url sum; do
		_atrilwl_extract "${n}" "$(_atrilwl_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_atrilwl_pkgs)
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local S TC gtk_out
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	gtk_out="$(b_dependency_dir gtk3_wayland)"
	gtk_out="${gtk_out%/}"
	local F="${PREFIX_PORT}/files"
	local PHXCC="${gtk_out}/host-bin/phx-gcc"    # drop -pthread, also inside @response files
	local PHXCXX="${gtk_out}/host-bin/phx-g++"
	local GS="${out}/gtk"                  # the GTK snapshot (a symlink tree, see below)
	local X="${I}/destdir"                 # DESTDIR of this build
	local P="${X}/usr"
	local SYSD="${GS}/deps/sys"            # libintl stub, libiconv, resolv (from the GTK build)
	local SCHEMAS_DIR=/usr/share/atril/schemas   # Atril's compiled schema on the target (patch 0005)
	local jobs
	jobs="$(nproc)"

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-g++" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" "${TC}-readelf" \
		"${PHXCC}" "${PHXCXX}" "${gtk_out}/destdir/usr/lib/libgtk-3.a" "${gtk_out}/deps/wayland/lib/libwlphx-compat.a" \
		"${gtk_out}/phoenix-aarch64.cross" "${gtk_out}/phoenix-aarch64-wl.cross" "${F}/bin/msgfmt"; do
		[ -e "${p}" ] || b_die "atril_wayland: missing ${p}"
	done
	# the GTK prefix must be a /usr one: its pkg-config files say prefix=/usr
	grep -q '^prefix=/usr$' "${gtk_out}/destdir/usr/lib/pkgconfig/gtk+-3.0.pc" ||
		b_die "atril_wayland: ${gtk_out} is not a /usr-configured GTK build"
	# the sample PDF is drawn with pycairo on the build host. The ports framework runs with a
	# Python venv first on PATH that may lack it: take the first interpreter that has it.
	local pycairo=""
	for pycairo in python3 /usr/bin/python3; do
		"${pycairo}" -c 'import cairo' 2>/dev/null && break
		pycairo=""
	done
	[ -n "${pycairo}" ] || b_die "atril_wayland: no host python3 with pycairo for make-sample-pdf.py (install python3-cairo)"

	# the Poppler configuration: the SAME list as the tools build and its host test
	# shellcheck source=files/poppler-options.sh
	. "${F}/poppler-options.sh"

	# A fresh work tree (no .built stamps) starts from an empty install.
	if [ ! -f "${out}/install.fresh" ]; then
		rm -rf "${X:?}" "${I:?}/bin" "${I:?}/data" "${I:?}/stage"
		touch "${out}/install.fresh"
	fi
	mkdir -p "${I}/bin" "${I}/data" "${P}/lib/pkgconfig"
	# msgfmt stand-in (Atril's meson merges its .desktop/metainfo through it)
	export PATH="${F}/bin:${PATH}"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")

	# --- the GTK snapshot, cross files, pkg-config, CMake toolchain ---
	# A symlink tree of gtk3_wayland's destdir/ and deps/ (as xfce_wayland): the .pc files
	# resolve their prefix from where they lie (--define-prefix), so the tree needs its own
	# directories, but the files may be links. GLib's .pc tool variables move into this tree
	# with --define-prefix: those links point at the HOST's tools instead.
	local stamp t
	stamp="$(sha256sum "${gtk_out}/SHA256SUMS" "${gtk_out}/destdir/usr/lib/libgtk-3.a" \
		"${gtk_out}/destdir/usr/lib/libglib-2.0.a" "${gtk_out}/deps/wayland/lib/libwlphx-compat.a" | sha256sum | cut -c1-16)"
	if [ "$(cat "${GS}.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		rm -rf "${GS}"
		mkdir -p "${GS}"
		cp -rs "${gtk_out}/destdir" "${gtk_out}/deps" "${GS}/"
		cp -a "${gtk_out}/SHA256SUMS" "${GS}/SHA256SUMS.gtk3_wayland"
		echo "${stamp}" >"${GS}.stamp"
		rm -f "${out}"/*.built   # everything is rebuilt on a new GTK stack
		echo "atril_wayland: snapshot of ${gtk_out} (stamp ${stamp})"
	fi
	for t in glib-compile-resources glib-compile-schemas glib-mkenums glib-genmarshal gdbus-codegen gobject-query; do
		[ -e "${GS}/destdir/usr/bin/${t}" ] || [ -L "${GS}/destdir/usr/bin/${t}" ] || continue
		command -v "${t}" >/dev/null || continue
		ln -sfn "$(command -v "${t}")" "${GS}/destdir/usr/bin/${t}"
	done
	{ echo "gtk3_wayland: ${gtk_out} (stamp ${stamp})"
	  sha256sum "${GS}"/destdir/usr/lib/lib{gtk-3,gdk-3,glib-2.0,gio-2.0,cairo,pango-1.0}.a "${GS}/deps/wayland/lib/libwlphx-compat.a" |
		sed "s|${GS}/||"; } >"${I}/snapshots.txt"

	local PKGC="${out}/pkg-config-phoenix" v libdirs="" c
	for v in zlib libffi expat pixman-1 libpng16 libjpeg freetype2 fontconfig wayland epoxy; do
		libdirs="${libdirs}:${GS}/deps/${v}/lib/pkgconfig"
	done
	cat >"${PKGC}" <<EOF
#!/bin/sh
# pkg-config over this build's DESTDIR, the GTK snapshot and its private ports views. Every
# installed .pc says prefix=/usr; --define-prefix takes each file's prefix from where it lies.
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig:${GS}/destdir/usr/lib/pkgconfig:${GS}/destdir/usr/share/pkgconfig${libdirs}:${GS}/deps/wayland/share/pkgconfig
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
exec /usr/bin/pkg-config --static --define-prefix "\$@"
EOF
	chmod +x "${PKGC}"
	# the GTK build's cross files, pointed at the snapshot and this pkg-config; plus
	# -fmacro-prefix-map (no build-host paths in g_return_if_fail() messages) and --gc-sections
	for c in "" -wl; do
		sed -e "s|${gtk_out}/deps|${GS}/deps|g" -e "s|${gtk_out}/destdir|${GS}/destdir|g" \
			-e "s|^pkg-config = .*|pkg-config = '${PKGC}'|" \
			-e "s#^\(c\|cpp\)_args = \[#\1_args = ['-fmacro-prefix-map=../src/=', '-fmacro-prefix-map=${out}/src/=', '-fmacro-prefix-map=${GS}/destdir/usr/include/=', '-fmacro-prefix-map=${P}/include/=', #" \
			-e "s#^\(c\|cpp\)_link_args = \[#\1_link_args = ['-Wl,--gc-sections', #" \
			-e "s|^# Generated by .*|# Generated by the phoenix-rtos-ports atril_wayland recipe from gtk3_wayland's phoenix-aarch64${c}.cross|" \
			"${gtk_out}/phoenix-aarch64${c}.cross" >"${out}/phoenix-aarch64${c}.cross"
	done

	# CMake (openjpeg, Poppler): one root that looks like a /usr tree -- this build's DESTDIR,
	# the GTK snapshot and the ports views, as symlinks -- so CMake's own Find modules
	# (Freetype, Fontconfig, JPEG, PNG, ZLIB) search only target files (FIND_ROOT_PATH ONLY);
	# programs (glib-mkenums) come from the host.
	rm -rf "${out}/cmake-root"
	mkdir -p "${out}/cmake-root/usr"
	cat >"${out}/phoenix-aarch64.cmake" <<EOF
# Generated by the phoenix-rtos-ports atril_wayland recipe (aarch64-phoenix, Pi 4).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "${PHXCC}")
set(CMAKE_CXX_COMPILER "${PHXCXX}")
set(CMAKE_AR "${TC}-gcc-ar")
set(CMAKE_RANLIB "${TC}-gcc-ranlib")
set(CMAKE_NM "${TC}-nm")
set(CMAKE_STRIP "${TC}-strip")
set(CMAKE_C_FLAGS_INIT "${TFLAGS[*]} -I${SYSD}/include -fmacro-prefix-map=${out}/src/=")
set(CMAKE_CXX_FLAGS_INIT "${TFLAGS[*]} -I${SYSD}/include -fmacro-prefix-map=${out}/src/=")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${S}/ -B${S}/lib/ -L${SYSD}/lib -Wl,-z,max-page-size=0x1000")
set(CMAKE_FIND_ROOT_PATH "${out}/cmake-root")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(PKG_CONFIG_EXECUTABLE "${PKGC}")
EOF

	_refresh_cmake_root() {  # the symlink /usr view, after each install into DESTDIR
		local r="${out}/cmake-root/usr" v
		rm -rf "${r}"
		mkdir -p "${r}/include" "${r}/lib"
		for v in zlib libffi expat pixman-1 libpng16 libjpeg freetype2 fontconfig; do
			cp -asf "${GS}/deps/${v}/include/." "${r}/include/"
			cp -asf "${GS}/deps/${v}/lib/." "${r}/lib/"
		done
		cp -asf "${SYSD}/include/." "${r}/include/"
		cp -asf "${SYSD}/lib/." "${r}/lib/"
		cp -asf "${GS}/destdir/usr/include/." "${r}/include/"
		cp -asf "${GS}/destdir/usr/lib/." "${r}/lib/"
		if [ -d "${P}/include" ]; then cp -asf "${P}/include/." "${r}/include/"; fi
		cp -asf "${P}/lib/." "${r}/lib/"
		rm -rf "${r}/lib/pkgconfig"   # pkg-config is PKG_CONFIG_EXECUTABLE (the views' .pc)
	}

	_meson_pkg() {  # [--cross <file>] name meson-args...
		local cross="${out}/phoenix-aarch64.cross"
		if [ "$1" = --cross ]; then cross="$2"; shift 2; fi
		local name="$1"
		shift
		local bd="${out}/${name}-build"
		if [ -f "${out}/${name}.built" ]; then
			echo "atril_wayland: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${cross}" --prefix /usr --sysconfdir /etc \
			--localstatedir /var --libdir lib --buildtype=debugoptimized -Db_staticpic=false -Db_ndebug=if-release \
			--wrap-mode=nodownload "$@" >"${out}/${name}-setup.log" 2>&1 || { tail -40 "${out}/${name}-setup.log"; b_die "atril_wayland: ${name}: meson setup failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${name}-ninja.log" 2>&1 || { grep -E -A3 'error:|undefined reference|multiple definition|FAILED' "${out}/${name}-ninja.log" | cut -c1-400 | head -60; b_die "atril_wayland: ${name}: build failed"; }
		DESTDIR="${X}" ninja -C "${bd}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "atril_wayland: ${name}: install failed"; }
		echo "atril_wayland: ${name}: built ($(grep -c 'warning:' "${out}/${name}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	_cmake_pkg() {  # name cmake-args...
		local name="$1"
		shift
		local bd="${out}/${name}-build"
		if [ -f "${out}/${name}.built" ]; then
			echo "atril_wayland: ${name}: up to date"
			return 0
		fi
		_refresh_cmake_root
		rm -rf "${bd}"
		cmake -S "${out}/src/${name}" -B "${bd}" -G Ninja -DCMAKE_TOOLCHAIN_FILE="${out}/phoenix-aarch64.cmake" \
			-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" \
			>"${out}/${name}-setup.log" 2>&1 || { grep -v '^--' "${out}/${name}-setup.log" | tail -40; b_die "atril_wayland: ${name}: cmake failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${name}-ninja.log" 2>&1 || { grep -E -A3 'error:|undefined reference|FAILED' "${out}/${name}-ninja.log" | cut -c1-400 | head -60; b_die "atril_wayland: ${name}: build failed"; }
		DESTDIR="${X}" ninja -C "${bd}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "atril_wayland: ${name}: install failed"; }
		echo "atril_wayland: ${name}: built ($(grep -c 'warning:' "${out}/${name}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	# --- libraries ---
	# libxml2: tree + XPath; no zlib, iconv, ICU, HTTP, modules, python
	_meson_pkg libxml2 -Dpython=disabled -Dzlib=disabled -Dicu=disabled -Diconv=disabled \
		-Dhttp=disabled -Dmodules=disabled -Dreadline=disabled -Dhistory=disabled -Ddocs=disabled \
		-Dsax1=enabled -Dcatalog=disabled -Ddebugging=disabled
	# lcms2: no tiff/jpeg utilities, no GPL plugins
	_meson_pkg lcms2 -Dtests=disabled -Djpeg=disabled -Dtiff=disabled -Dutils=false -Dfastfloat=false -Dthreaded=false
	# openjpeg: libopenjp2 only
	_cmake_pkg openjpeg -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF \
		-DBUILD_DOC=OFF -DBUILD_JPIP=OFF -DBUILD_THIRDPARTY=OFF -DBUILD_PKGCONFIG_FILES=ON
	# Poppler: core + glib/cairo (patch 0001: fontconfig 2.14). FindPNG looks for <png.h>
	# directly under include/: the ports view has it in include/libpng16/.
	_cmake_pkg poppler "${POPPLER_OPTS[@]}" -DENABLE_UTILS=OFF -DTESTDATADIR="${out}/src/poppler/test" \
		-DPNG_PNG_INCLUDE_DIR="${out}/cmake-root/usr/include/libpng16"
	# Atril: the PDF backend built in (patch 0004); no X11/mate-desktop (0001-0003); its own
	# schema directory (0005)
	_meson_pkg --cross "${out}/phoenix-aarch64-wl.cross" atril -Dc_std=gnu11 \
		-Dpdf=enabled -Dps=disabled -Ddvi=disabled -Dt1lib=disabled -Ddjvu=disabled -Dtiff=disabled -Dpixbuf=disabled \
		-Dcomics=disabled -Dxps=disabled -Depub=disabled -Dcaja=disabled -Dx11=disabled -Dmate_desktop=disabled \
		-Dbuiltin_backends=true -Dschemas_dir="${SCHEMAS_DIR}" -Dgtk_unix_print=false -Dkeyring=false \
		-Dpreviewer=false -Dthumbnailer=false -Ddocs=false -Dhelp_files=false -Dintrospection=false -Denable_dbus=false

	# --- data: the compiled schema, the sample PDF ---
	local D="${I}/data"
	rm -rf "${D}/schemas"
	mkdir -p "${D}/schemas"
	cp "${X}${SCHEMAS_DIR}/org.mate.Atril.gschema.xml" "${D}/schemas/"
	glib-compile-schemas --strict "${D}/schemas"
	echo "atril_wayland: ${SCHEMAS_DIR}/gschemas.compiled: $(stat -c %s "${D}/schemas/gschemas.compiled") bytes"
	"${pycairo}" "${F}/tools/make-sample-pdf.py" "${D}/sample.pdf"
	echo "atril_wayland: sample.pdf: $(stat -c %s "${D}/sample.pdf") bytes"

	# --- program: collect, strip, verify ---
	local BIN="${I}/bin" bad=0 f und n interp allsyms x11 s strs
	f="${P}/bin/atril"
	[ -f "${f}" ] || b_die "atril_wayland: ${f} missing"
	cp -a "${f}" "${BIN}/atril"
	"${TC}-strip" -o "${BIN}/atril-stripped" "${BIN}/atril"
	und="$("${TC}-nm" -u "${BIN}/atril" || true)"
	n=$(grep -c . <<<"${und}" || true)
	interp="$("${TC}-readelf" -l "${BIN}/atril" | grep -c INTERP || true)"
	allsyms="$("${TC}-nm" "${BIN}/atril")"
	x11="$(grep -cE ' (XOpenDisplay|XInternAtom|xcb_connect|gdk_x11_display_get_type|gdk_x11_window_set_user_time|SmcOpenConnection|IceOpenConnection|mate_image_menu_item_new)$' <<<"${allsyms}" || true)"
	echo "atril_wayland: atril: nm -u ${n}, PT_INTERP ${interp}, X11/SM/mate-desktop symbols ${x11}; $("${TC}-size" "${BIN}/atril" | awk 'NR==2 {printf "text %d data %d bss %d", $1, $2, $3}'); stripped $(stat -c %s "${BIN}/atril-stripped")"
	if [ "${n}" != 0 ] || [ "${interp}" != 0 ] || [ "${x11}" != 0 ]; then head -10 <<<"${und}"; bad=1; fi
	# the PDF backend (built in), Poppler (glib + core + cairo output + JPX + CMS), GTK on Wayland
	for s in ev_builtin_backends ev_builtin_pdfdocument_register ev_module_new_builtin \
		poppler_document_new_from_file poppler_page_render _ZN14CairoOutputDev9startPageEiP8GfxStateP4XRef \
		opj_decode cmsCreateTransform xmlXPathNewContext gdk_wayland_display_get_type gtk_image_menu_item_new_with_label \
		ev_view_presentation_new egg_sm_client_get ev_resource_data; do
		grep -qE " [TtWwVvDdBbRr] ${s}\$" <<<"${allsyms}" || { echo "atril_wayland: atril: symbol ${s} missing"; bad=1; }
	done
	strs="$(strings -a "${BIN}/atril-stripped")"
	for s in 'pdfdocument' 'application/pdf' 'PDF Documents' "${SCHEMAS_DIR}" 'org.mate.Atril' 'wayland-0'; do
		n=$(grep -cF -- "${s}" <<<"${strs}" || true)
		echo "atril_wayland: atril strings '${s}': ${n}"
		[ "${n}" != 0 ] || bad=1
	done
	n="$(grep -cE 'port-sources|versioned-ports|/home/' <<<"${strs}" || true)"
	echo "atril_wayland:   build-host path strings: ${n}"
	{ echo "# atril_wayland port build"
	  sha256sum "${BIN}/atril-stripped" "${D}/schemas/gschemas.compiled" "${D}/sample.pdf" | sed "s|${I}/||"; } >"${I}/SHA256SUMS"
	cat "${I}/SHA256SUMS"
	[ "${bad}" = 0 ] || b_die "atril_wayland: verification failed"

	# --- the staging tree: stage/ mirrors the target rootfs (the m7j-atril staging,
	# docs/gpu-new-lane/M7-wayland-desktop.md "Stage 6" in the coordination repo), with the
	# program under its own name /usr/bin/atril (the tools staged /bin/atril-wl)
	local ST="${I}/stage"
	rm -rf "${ST}"
	_st() {  # mode source target-path
		install -D -m "$1" "$2" "${ST}/$3"
	}
	# a verbatim tools file with the program names rewritten for the image
	local name_sed=(-e 's|/bin/atril-wl|/usr/bin/atril|g')
	_st_img() {  # mode source target-path
		mkdir -p "$(dirname "${ST}/$3")"
		sed "${name_sed[@]}" "$2" >"${ST}/$3"
		chmod "$1" "${ST}/$3"
	}
	_st 755 "${BIN}/atril-stripped" usr/bin/atril
	_st_img 644 "${F}/conf/atril.desktop" usr/share/applications/atril.desktop
	_st 644 "${D}/schemas/gschemas.compiled" "${SCHEMAS_DIR#/}/gschemas.compiled"
	_st 644 "${D}/sample.pdf" usr/share/doc/phoenix/sample.pdf
	_st 644 "${P}/share/atril/hand-open.png" usr/share/atril/hand-open.png
	# Atril's own action icons (its private hicolor search path: ATRILDATADIR/icons) + the
	# application icon, which the .desktop entry names by absolute path (the staged hicolor
	# theme and its cache are xfce_wayland's)
	while IFS= read -r f; do
		_st 644 "${P}/share/atril/icons/${f}" "usr/share/atril/icons/${f}"
	done < <(cd "${P}/share/atril/icons" && find . -name '*.png' -printf '%P\n' | sort)
	local sz
	for sz in 16x16 22x22 24x24 48x48; do
		_st 644 "${P}/share/icons/hicolor/${sz}/apps/atril.png" "usr/share/atril/icons/hicolor/${sz}/apps/atril.png"
	done
	if grep -nE '/bin/atril-wl' "${ST}/usr/share/applications/atril.desktop"; then
		b_die "atril_wayland: a staged file still names a program this image does not have (above)"
	fi
	grep -q '^Exec=/usr/bin/atril %U$' "${ST}/usr/share/applications/atril.desktop" ||
		b_die "atril_wayland: atril.desktop: Exec is not /usr/bin/atril"
	f="$(sed -n 's/^Icon=//p' "${ST}/usr/share/applications/atril.desktop")"
	[ -f "${ST}${f}" ] || b_die "atril_wayland: atril.desktop: Icon ${f} is not staged"

	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"
	echo "atril_wayland: stage: $(wc -l <"${I}/stage.MANIFEST") files"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "atril_wayland: staged into ${PREFIX_FS}/root"
	fi
}
