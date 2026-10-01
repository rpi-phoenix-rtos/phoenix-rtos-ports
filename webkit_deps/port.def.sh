#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="webkit_deps"
	version="2.54.0"
	desc="WebKit 2.54 network + format libraries: libsoup 3.6, glib-networking (OpenSSL), libpsl, nghttp2, brotli, woff2, libwebp, libxml2, libxslt"

	# Aggregate port: the libraries WPE/WebKitGTK 2.54 needs from outside WebKit for
	# networking and web formats (docs/browser/PLAN.md, milestone B0, track A2, in the
	# coordination repo), cross-built STATIC on gtk3_wayland's GLib 2.88:
	#
	#   brotli 1.2.0          (cmake)  Content-Encoding: br (libsoup), WOFF2 (woff2)
	#   woff2 1.0.2           (cmake)  WOFF2 web fonts (C++; patch: <cstdint> in output.h)
	#   libwebp 1.6.0         (cmake)  WebP images: libwebp, libwebpdemux, libwebpmux, libsharpyuv
	#   nghttp2 1.70.0        (cmake)  HTTP/2 framing for libsoup (library only)
	#   libpsl 0.23.3         (meson)  public-suffix list, compiled in (no IDNA runtime library)
	#   libxml2 2.15.4        (meson)  WebCore's XML parser (+ libxslt)
	#   libxslt 1.1.45        (cmake)  XSLT (WebKit ENABLE_XSLT); libexslt
	#   glib-networking 2.90.0 (meson) GIO's TLS backend over OpenSSL 3.5 (the ports openssl;
	#                                  patch: a static-only build links no shared module)
	#   libsoup 3.6.6         (meson)  HTTP/1.1 + HTTP/2 client: cookies (sqlite3), HSTS, brotli
	#   soup-smoke            (USE smoke) the B0 Pi check (files/src/soup-smoke.c)
	#
	# Everything is configured --prefix /usr and installed with DESTDIR into destdir/usr
	# (static .a, headers, .pc, CMake package files), so a WebKit build gets ONE /usr-shaped
	# tree to find_package() in. All of it stays in this port's private prefix: the libraries
	# never reach the shared ports prefix, whose include directory is on every port's
	# compile line (a curl, python or glib2 configure would otherwise find brotli/nghttp2/
	# libxml2 headers there and change what it builds).
	#
	# The TLS backend is a GIO module, normally dlopen()ed from GIO_MODULE_DIR. Statically,
	# a program registers it itself, once, before it creates any TLS connection (a
	# SoupSession, a GSocketClient with TLS):
	#
	#     extern void g_io_openssl_load (GIOModule *module);
	#     g_io_openssl_load (NULL);
	#
	# With module == NULL glib-networking registers the "gio-tls-backend" extension point
	# and its types statically (g_type_module_register_type() falls back to
	# g_type_register_static_simple()), and GIO then picks "openssl" as the default
	# GTlsBackend. Its default GTlsDatabase is OpenSSL's default paths: OPENSSLDIR
	# /etc/ssl (openssl/port.def.sh) -> /etc/ssl/cert.pem, which the ca_certificates port
	# installs (SSL_CERT_FILE overrides it). Link with
	# `pkg-config --static --libs gioopenssl libsoup-3.0` (this port's pkg-config-phoenix).
	source="https://download.gnome.org/sources/libsoup/3.6/"
	archive_filename="libsoup-3.6.6.tar.xz"
	src_path="libsoup-3.6.6/"

	size="1572004"
	sha256="51ed0ae06f9d5a40f401ff459e2e5f652f9a510b7730e1359ee66d14d4872740"

	# libsoup: LGPL-2.0-or-later; glib-networking: LGPL-2.1-or-later WITH its OpenSSL
	# linking exception (LICENSE_EXCEPTION); brotli, woff2, nghttp2, libpsl, libxml2,
	# libxslt/libexslt: MIT; libwebp: BSD-3-Clause; the public suffix list compiled into
	# libpsl: MPL-2.0; files/src: BSD-3-Clause (ours).
	license="LGPL-2.0-or-later AND LGPL-2.1-or-later AND MIT AND BSD-3-Clause AND MPL-2.0"
	license_file="COPYING"

	# A real conflict (and the private versioned-ports prefix): libsoup and glib-networking
	# link gtk3_wayland's GLib 2.88 (GIO), never the ports glib2 2.56.
	conflicts="glib2>=0.0"
	# gtk3_wayland: GLib/GIO + its private ports views (zlib, libffi, libintl/libiconv/
	# resolv stubs) and its compiler wrappers; openssl: glib-networking's TLS; sqlite3:
	# libsoup's cookie jar and HSTS database.
	depends="gtk3_wayland openssl sqlite3"

	# rootfs: also copy the staging tree (stage/) into the image rootfs.
	# smoke:  build /usr/bin/soup-smoke and put it in the staging tree.
	iuse="rootfs smoke"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   destdir/usr                  every library: static .a, headers, .pc (prefix=/usr; read
#                                them with pkg-config --define-prefix), CMake package files
#   pkg-config-phoenix           pkg-config over destdir/ + the gtk3_wayland snapshot + the
#                                views below (what a consumer of this port runs)
#   phoenix-aarch64.cross, phoenix-aarch64.cmake   meson cross file / CMake toolchain file
#   deps/sqlite3                 a pkg-config view of the ports sqlite3 (it installs no .pc)
#   bin/                         soup-smoke (unstripped, addr2line) and -stripped (USE smoke)
#   stage/ + stage.MANIFEST      the files for the target rootfs
#   SHA256SUMS
#
# Host tools: meson, ninja, cmake, python3 (libpsl's DAFSA generator), glib-mkenums,
# glib-genmarshal, glib-compile-resources (libsoup).

# name|file|url|sha256 (the anchor, libsoup, is fetched by the framework). libxml2 is the
# same tarball labwc_desktop and atril_wayland build privately.
_wkdeps_pkgs() {
	cat <<'EOF'
brotli|brotli-1.2.0.tar.gz|https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz|816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
woff2|woff2-1.0.2.tar.gz|https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz|add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594
libwebp|libwebp-1.6.0.tar.gz|https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz|e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
nghttp2|nghttp2-1.70.0.tar.xz|https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz|e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
libpsl|libpsl-0.23.3.tar.gz|https://github.com/rockdaboot/libpsl/releases/download/0.23.3/libpsl-0.23.3.tar.gz|93941f85a1e7bd593fa94f299233cb5dfc91cd144fd9a78a6ceb75001c5b03be
libxml2|libxml2-2.15.4.tar.xz|https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz|98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
libxslt|libxslt-1.1.45.tar.xz|https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.45.tar.xz|9acfe68419c4d06a45c550321b3212762d92f41465062ca4ea19e632ee5d216e
glib-networking|glib-networking-2.90.0.tar.xz|https://download.gnome.org/sources/glib-networking/2.90/glib-networking-2.90.0.tar.xz|83a75e3d9c36b66ee86d3281c2fc997816101968a5126ba322b2acb9a74dd8c0
EOF
}

# _wkdeps_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_wkdeps_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "webkit_deps: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "webkit_deps: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# _wkdeps_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name> as its own git
# repository (inside ANOTHER repository -- the buildroot is one -- `git am` would skip
# paths), patches/<name>/*.patch applied by `git am`; re-extracted when the tarball or a
# patch changes.
_wkdeps_extract() {
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
		echo "webkit_deps: apply ${name}/$(basename "${p}")"
		git -C "${dir}" -c user.name=build -c user.email=build@invalid am -q --whitespace=nowarn "${p}"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built"
}

# The link inputs every port links implicitly (libphoenix.a, libgcc.a, libstdc++.a) changed.
# Each package builds under an out/<name>.built stamp, so the framework's default -- delete
# the linked programs and let the build relink them -- would relink soup-smoke but keep
# every package's configure-time libc probes (pipe2, strxfrm_l, tm_gmtoff ... answered NO
# today). Drop the stamps instead: everything is set up and built again (~3 min).
p_relink() {
	rm -f "${PREFIX_PORT_BUILD}/out"/*.built
}

p_prepare() {
	local t
	for t in meson ninja cmake python3 git curl glib-mkenums glib-genmarshal glib-compile-resources; do
		command -v "${t}" >/dev/null || b_die "webkit_deps: host tool ${t} not found"
	done

	_wkdeps_extract libsoup "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
	local n file url sum
	while IFS='|' read -r n file url sum; do
		_wkdeps_extract "${n}" "$(_wkdeps_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_wkdeps_pkgs)
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local S TC gtk_out ossl B
	S="${PREFIX_BUILD%/}/sysroot"
	B="${PREFIX_BUILD%/}"            # the shared ports prefix (sqlite3)
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	gtk_out="$(b_dependency_dir gtk3_wayland)"
	gtk_out="${gtk_out%/}"
	ossl="$(b_dependency_dir openssl)"
	ossl="${ossl%/}"
	local F="${PREFIX_PORT}/files"
	local PHXCC="${gtk_out}/host-bin/phx-gcc"    # drop -pthread, also inside @response files
	local PHXCXX="${gtk_out}/host-bin/phx-g++"
	local GS="${out}/gtk"                  # the GTK snapshot (a symlink tree, see below)
	local X="${I}/destdir"                 # DESTDIR of this build
	local P="${X}/usr"
	local D="${I}/deps"
	local SYSD="${GS}/deps/sys"            # libintl stub, libiconv, resolv (from the GTK build)
	local jobs
	jobs="$(nproc)"

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-g++" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" "${TC}-readelf" \
		"${PHXCC}" "${PHXCXX}" "${gtk_out}/destdir/usr/lib/libgio-2.0.a" "${gtk_out}/phoenix-aarch64.cross" \
		"${ossl}/lib/libssl.a" "${ossl}/lib/pkgconfig/openssl.pc" "${B}/lib/libsqlite3.a" "${B}/include/sqlite3.h"; do
		[ -e "${p}" ] || b_die "webkit_deps: missing ${p}"
	done
	grep -q '^prefix=/usr$' "${gtk_out}/destdir/usr/lib/pkgconfig/glib-2.0.pc" ||
		b_die "webkit_deps: ${gtk_out} is not a /usr-configured GLib build"

	# A fresh work tree (no .built stamps) starts from an empty install.
	if [ ! -f "${out}/install.fresh" ]; then
		rm -rf "${X:?}" "${D:?}" "${I:?}/bin" "${I:?}/stage"
		touch "${out}/install.fresh"
	fi
	mkdir -p "${I}/bin" "${P}/lib/pkgconfig" "${D}" "${out}/obj"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")
	# Every library here parses untrusted network input: -fstack-protector-strong
	# (libphoenix provides __stack_chk_guard and __stack_chk_fail).
	local SSP=-fstack-protector-strong

	# --- the GTK snapshot: a symlink tree of gtk3_wayland's destdir/ and deps/ (as
	# atril_wayland). The .pc files resolve their prefix from where they lie
	# (--define-prefix), so the tree needs its own directories, but the files may be links.
	local stamp t
	stamp="$(sha256sum "${gtk_out}/SHA256SUMS" "${gtk_out}/destdir/usr/lib/libglib-2.0.a" \
		"${gtk_out}/destdir/usr/lib/libgio-2.0.a" "${ossl}/lib/libssl.a" "${ossl}/lib/libcrypto.a" \
		"${B}/lib/libsqlite3.a" | sha256sum | cut -c1-16)"
	if [ "$(cat "${GS}.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		rm -rf "${GS}"
		mkdir -p "${GS}"
		cp -rs "${gtk_out}/destdir" "${gtk_out}/deps" "${GS}/"
		echo "${stamp}" >"${GS}.stamp"
		rm -f "${out}"/*.built   # everything is rebuilt on a new GLib/OpenSSL/sqlite
		echo "webkit_deps: snapshot of ${gtk_out} (stamp ${stamp})"
	fi
	for t in glib-compile-resources glib-compile-schemas glib-mkenums glib-genmarshal gdbus-codegen gobject-query; do
		[ -e "${GS}/destdir/usr/bin/${t}" ] || [ -L "${GS}/destdir/usr/bin/${t}" ] || continue
		command -v "${t}" >/dev/null || continue
		ln -sfn "$(command -v "${t}")" "${GS}/destdir/usr/bin/${t}"
	done
	{ echo "gtk3_wayland: ${gtk_out} (stamp ${stamp})"
	  sha256sum "${GS}"/destdir/usr/lib/lib{glib-2.0,gio-2.0,gobject-2.0}.a | sed "s|${GS}/||"
	  sha256sum "${ossl}"/lib/lib{ssl,crypto}.a "${B}/lib/libsqlite3.a"; } >"${I}/snapshots.txt"

	# sqlite3 (the ports sqlite3 installs no .pc): a view libsoup's dependency('sqlite3')
	# finds
	local sqv
	sqv="$(sed -n 's/^#define SQLITE_VERSION *"\(.*\)"/\1/p' "${B}/include/sqlite3.h")"
	rm -rf "${D}/sqlite3"
	mkdir -p "${D}/sqlite3/include" "${D}/sqlite3/lib/pkgconfig"
	cp -a "${B}/include/sqlite3.h" "${B}/include/sqlite3ext.h" "${D}/sqlite3/include/"
	cp -a "${B}/lib/libsqlite3.a" "${D}/sqlite3/lib/"
	printf '%s\n' "prefix=${D}/sqlite3" "Name: sqlite3" "Description: sqlite3 from the Phoenix ports prefix" \
		"Version: ${sqv}" "Libs: -L\${prefix}/lib -lsqlite3" "Libs.private: -lm" "Cflags: -I\${prefix}/include" \
		>"${D}/sqlite3/lib/pkgconfig/sqlite3.pc"

	local PKGC="${I}/pkg-config-phoenix"
	cat >"${PKGC}" <<EOF
#!/bin/sh
# pkg-config over this port's DESTDIR, the GTK snapshot (GLib), its private ports views,
# sqlite3 and the ports OpenSSL. Every installed .pc says prefix=/usr; --define-prefix takes
# each file's prefix from where it lies.
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig:${GS}/destdir/usr/lib/pkgconfig:${GS}/deps/zlib/lib/pkgconfig:${GS}/deps/libffi/lib/pkgconfig:${D}/sqlite3/lib/pkgconfig:${ossl}/lib/pkgconfig
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
exec /usr/bin/pkg-config --static --define-prefix "\$@"
EOF
	chmod +x "${PKGC}"
	# gtk3_wayland's base cross file (no Wayland compat layer: nothing here is a Wayland
	# client), pointed at the snapshot and this pkg-config; + -fstack-protector-strong,
	# -fmacro-prefix-map (no build-host paths in g_return_if_fail() messages).
	sed -e "s|${gtk_out}/deps|${GS}/deps|g" -e "s|${gtk_out}/destdir|${GS}/destdir|g" \
		-e "s|^pkg-config = .*|pkg-config = '${PKGC}'|" \
		-e "s#^\(c\|cpp\)_args = \[#\1_args = ['${SSP}', '-fmacro-prefix-map=../src/=', '-fmacro-prefix-map=${out}/src/=', '-fmacro-prefix-map=${GS}/destdir/usr/include/=', '-fmacro-prefix-map=${P}/include/=', #" \
		-e "s|^# Generated by .*|# Generated by the phoenix-rtos-ports webkit_deps recipe from gtk3_wayland's phoenix-aarch64.cross|" \
		"${gtk_out}/phoenix-aarch64.cross" >"${I}/phoenix-aarch64.cross"

	# CMake: one root that looks like a /usr tree -- this build's DESTDIR and the views, as
	# symlinks -- so CMake's own Find modules search only target files.
	cat >"${I}/phoenix-aarch64.cmake" <<EOF
# Generated by the phoenix-rtos-ports webkit_deps recipe (aarch64-phoenix, Pi 4).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "${PHXCC}")
set(CMAKE_CXX_COMPILER "${PHXCXX}")
set(CMAKE_AR "${TC}-gcc-ar")
set(CMAKE_RANLIB "${TC}-gcc-ranlib")
set(CMAKE_NM "${TC}-nm")
set(CMAKE_STRIP "${TC}-strip")
set(CMAKE_C_FLAGS_INIT "${TFLAGS[*]} ${SSP} -I${SYSD}/include -fmacro-prefix-map=${out}/src/=")
set(CMAKE_CXX_FLAGS_INIT "${TFLAGS[*]} ${SSP} -I${SYSD}/include -fmacro-prefix-map=${out}/src/=")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${S}/ -B${S}/lib/ -L${SYSD}/lib -Wl,-z,max-page-size=0x1000")
set(CMAKE_FIND_ROOT_PATH "${out}/cmake-root")
# /usr below the find root: the Generic platform has no system prefix of its own, and the
# HINTS Find modules take from pkg-config (absolute DESTDIR paths) do not exist re-rooted
set(CMAKE_PREFIX_PATH /usr)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(PKG_CONFIG_EXECUTABLE "${PKGC}")
EOF

	_refresh_cmake_root() {  # the symlink /usr view, after each install into DESTDIR
		local r="${out}/cmake-root/usr" v
		rm -rf "${out}/cmake-root"
		mkdir -p "${r}/include" "${r}/lib"
		for v in zlib libffi; do
			cp -asf "${GS}/deps/${v}/include/." "${r}/include/"
			cp -asf "${GS}/deps/${v}/lib/." "${r}/lib/"
		done
		cp -asf "${SYSD}/include/." "${r}/include/"
		cp -asf "${SYSD}/lib/." "${r}/lib/"
		if [ -d "${P}/include" ]; then cp -asf "${P}/include/." "${r}/include/"; fi
		cp -asf "${P}/lib/." "${r}/lib/"
		rm -rf "${r}/lib/pkgconfig"   # pkg-config is PKG_CONFIG_EXECUTABLE
	}

	_meson_pkg() {  # name meson-args...
		local name="$1"
		shift
		local bd="${out}/${name}-build" t0
		if [ -f "${out}/${name}.built" ]; then
			echo "webkit_deps: ${name}: up to date"
			return 0
		fi
		t0="$(date +%s)"
		rm -rf "${bd}"
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${I}/phoenix-aarch64.cross" --prefix /usr --sysconfdir /etc \
			--localstatedir /var --libdir lib --buildtype=debugoptimized -Db_staticpic=false -Db_ndebug=if-release \
			--wrap-mode=nodownload "$@" >"${out}/${name}-setup.log" 2>&1 || { tail -40 "${out}/${name}-setup.log"; b_die "webkit_deps: ${name}: meson setup failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${name}-ninja.log" 2>&1 || { grep -E -A3 'error:|undefined reference|multiple definition|FAILED' "${out}/${name}-ninja.log" | cut -c1-400 | head -60; b_die "webkit_deps: ${name}: build failed"; }
		DESTDIR="${X}" ninja -C "${bd}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "webkit_deps: ${name}: install failed"; }
		echo "webkit_deps: ${name}: built in $(( $(date +%s) - t0 )) s ($(grep -c 'warning:' "${out}/${name}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	_cmake_pkg() {  # name cmake-args...
		local name="$1"
		shift
		local bd="${out}/${name}-build" t0
		if [ -f "${out}/${name}.built" ]; then
			echo "webkit_deps: ${name}: up to date"
			return 0
		fi
		t0="$(date +%s)"
		_refresh_cmake_root
		rm -rf "${bd}"
		cmake -S "${out}/src/${name}" -B "${bd}" -G Ninja -DCMAKE_TOOLCHAIN_FILE="${I}/phoenix-aarch64.cmake" \
			-DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DBUILD_SHARED_LIBS=OFF "$@" \
			>"${out}/${name}-setup.log" 2>&1 || { grep -v '^--' "${out}/${name}-setup.log" | tail -40; b_die "webkit_deps: ${name}: cmake failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${name}-ninja.log" 2>&1 || { grep -E -A3 'error:|undefined reference|FAILED' "${out}/${name}-ninja.log" | cut -c1-400 | head -60; b_die "webkit_deps: ${name}: build failed"; }
		DESTDIR="${X}" ninja -C "${bd}" install >"${out}/${name}-install.log" 2>&1 || { tail -20 "${out}/${name}-install.log"; b_die "webkit_deps: ${name}: install failed"; }
		echo "webkit_deps: ${name}: built in $(( $(date +%s) - t0 )) s ($(grep -c 'warning:' "${out}/${name}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	# --- libraries ---
	# brotli: the three libraries, no CLI tool (and so no tests)
	_cmake_pkg brotli -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON
	# woff2 (C++): its CMakeLists predates CMake 3.5. Its Find modules return libbrotlidec/
	# libbrotlienc alone, which links only shared: the static archives also need
	# libbrotlicommon (woff2's own tools fail to link without it).
	_cmake_pkg woff2 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DNOISY_LOGGING=OFF -DCANONICAL_PREFIXES=ON \
		-DBROTLIDEC_INCLUDE_DIRS="${P}/include" -DBROTLIENC_INCLUDE_DIRS="${P}/include" \
		-DBROTLIDEC_LIBRARIES="${P}/lib/libbrotlidec.a;${P}/lib/libbrotlicommon.a" \
		-DBROTLIENC_LIBRARIES="${P}/lib/libbrotlienc.a;${P}/lib/libbrotlicommon.a"
	# libwebp: the libraries (webp, webpdemux, webpmux, sharpyuv); no tools, no extras
	_cmake_pkg libwebp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
		-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
		-DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON -DWEBP_USE_THREAD=ON
	# nghttp2: libnghttp2 only (no apps/examples/HPACK tools/docs/tests)
	_cmake_pkg nghttp2 -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF -DBUILD_TESTING=OFF \
		-DENABLE_FAILMALLOC=OFF
	# libpsl: the list compiled in (DAFSA, generated on the host by python3); no IDNA
	# library at run time (WebKit/libsoup hand it already-ASCII/punycode host names)
	_meson_pkg libpsl -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false
	# libxml2 as atril_wayland builds it, plus the output/HTML/XPath/XInclude parts libxslt
	# and WebCore use (the 2.15 defaults); no zlib/iconv/ICU/HTTP/modules/python
	_meson_pkg libxml2 -Dpython=disabled -Dzlib=disabled -Dicu=disabled -Diconv=disabled \
		-Dhttp=disabled -Dmodules=disabled -Dreadline=disabled -Dhistory=disabled -Ddocs=disabled \
		-Dsax1=enabled -Dcatalog=disabled -Ddebugging=disabled
	# meson writes libxml2's CMake package file with the configured prefix (/usr) spelled
	# out, so CMake consumers (libxslt, WebKit) would look in the build host's /usr/include.
	# Make it relocatable, as libxml2's own CMake build writes it.
	local xc="${P}/lib/cmake/libxml2/libxml2-config.cmake"
	if grep -q '^set(LIBXML2_INCLUDE_DIR */usr/include/libxml2)$' "${xc}"; then
		sed -i -e 's|^set(LIBXML2_INCLUDE_DIR */usr/include/libxml2)$|set(LIBXML2_INCLUDE_DIR    ${CMAKE_CURRENT_LIST_DIR}/../../../include/libxml2)|' \
			-e 's|^set(LIBXML2_LIBRARY_DIR */usr/lib)$|set(LIBXML2_LIBRARY_DIR    ${CMAKE_CURRENT_LIST_DIR}/../..)|' "${xc}"
	fi
	grep -qF 'set(LIBXML2_INCLUDE_DIR    ${CMAKE_CURRENT_LIST_DIR}/../../../include/libxml2)' "${xc}" ||
		b_die "webkit_deps: ${xc}: not relocatable"
	# libxslt + libexslt: no python, no xsltproc, no crypto (libgcrypt), no plugins
	_cmake_pkg libxslt -DLIBXSLT_WITH_PYTHON=OFF -DLIBXSLT_WITH_PROGRAMS=OFF -DLIBXSLT_WITH_TESTS=OFF \
		-DLIBXSLT_WITH_CRYPTO=OFF -DLIBXSLT_WITH_MODULES=OFF -DLIBXSLT_WITH_DEBUGGER=OFF -DLIBXSLT_WITH_PROFILER=OFF
	# glib-networking: the OpenSSL TLS backend only (no GnuTLS, no proxy-resolver modules:
	# GIO's built-in environment-less resolver answers "direct")
	_meson_pkg glib-networking -Dgnutls=disabled -Dopenssl=enabled -Dlibproxy=disabled -Dgnome_proxy=disabled \
		-Denvironment_proxy=disabled -Dtests=false -Dinstalled_tests=false
	# libsoup: HTTP/2 (nghttp2), brotli, libpsl, sqlite3; no GSSAPI/NTLM/sysprof/tests/docs.
	# tls_check=false: the check runs a GIO program at configure time (impossible cross).
	_meson_pkg libsoup -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled -Dtls_check=false \
		-Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled -Ddoc_tests=false -Dtests=false \
		-Dautobahn=disabled -Dinstalled_tests=false -Dsysprof=disabled -Dfuzzing=disabled -Dpkcs11_tests=disabled

	# glib-networking installs gioopenssl.pc next to the module (lib/gio/modules/pkgconfig/),
	# where pkg-config --define-prefix would take lib/gio for the prefix: move it to the
	# common directory (its -L still names lib/gio/modules, where the archive is).
	if [ -f "${P}/lib/gio/modules/pkgconfig/gioopenssl.pc" ]; then
		mv "${P}/lib/gio/modules/pkgconfig/gioopenssl.pc" "${P}/lib/pkgconfig/"
		rmdir "${P}/lib/gio/modules/pkgconfig"
	fi
	grep -q -- '-L${prefix}/lib/gio/modules -lgioopenssl' "${P}/lib/pkgconfig/gioopenssl.pc" ||
		b_die "webkit_deps: gioopenssl.pc does not name lib/gio/modules/libgioopenssl.a"

	# --- verification ---
	local bad=0 a l needle s syms
	for l in brotlicommon brotlidec brotlienc woff2common woff2dec woff2enc webp webpdemux webpmux sharpyuv \
		nghttp2 psl xml2 xslt exslt soup-3.0; do
		[ -f "${P}/lib/lib${l}.a" ] || { echo "webkit_deps: lib${l}.a missing"; bad=1; }
	done
	[ -f "${P}/lib/gio/modules/libgioopenssl.a" ] || { echo "webkit_deps: libgioopenssl.a missing"; bad=1; }
	# the pieces the browser relies on, by symbol: the static TLS registration (+ tlsbase
	# bundled), HTTP/2 + brotli + libpsl inside libsoup, the PSL built in
	syms="$("${TC}-nm" "${P}/lib/gio/modules/libgioopenssl.a" 2>/dev/null)"
	for s in g_io_openssl_load g_tls_backend_openssl_get_type g_tls_connection_base_get_type; do
		grep -qE " T ${s}\$" <<<"${syms}" || { echo "webkit_deps: libgioopenssl.a: ${s} missing"; bad=1; }
	done
	syms="$("${TC}-nm" -u "${P}/lib/libsoup-3.0.a" 2>/dev/null | sort -u)"
	for s in nghttp2_session_client_new2 BrotliDecoderDecompressStream psl_latest sqlite3_open; do
		grep -qE " U ${s}\$" <<<"${syms}" || { echo "webkit_deps: libsoup-3.0.a does not use ${s}"; bad=1; }
	done
	grep -qE ' [TtDdRr] (kDafsa|psl_builtin)$' <<<"$("${TC}-nm" "${P}/lib/libpsl.a")" ||
		{ echo "webkit_deps: libpsl.a has no built-in list"; bad=1; }
	# no build path compiled into the libraries (debug info aside)
	needle="$(basename "$(dirname "${PREFIX_BUILD%/}")")/$(basename "${PREFIX_BUILD%/}")"
	for a in "${P}"/lib/*.a "${P}/lib/gio/modules/libgioopenssl.a"; do
		"${TC}-strip" --strip-debug -o "${out}/nodebug.a" "${a}"
		if grep -qaF "${needle}" "${out}/nodebug.a"; then
			echo "webkit_deps: $(basename "${a}") compiles in a build path: $(grep -ao -- "[[:print:]]*${needle}[[:print:]]*" "${out}/nodebug.a" | head -1)"
			bad=1
		fi
	done
	rm -f "${out}/nodebug.a"

	# --- soup-smoke (USE smoke): everything above in one static program ---
	local BIN="${I}/bin" ST="${I}/stage" und n interp
	rm -rf "${ST}"
	mkdir -p "${ST}"
	if b_use smoke; then
		local mods="libsoup-3.0 gioopenssl libpsl libbrotlienc libbrotlidec libwebp libwebpdemux libwebpmux libwoff2enc libwoff2dec libxslt libexslt"
		# shellcheck disable=2046 # pkg-config output is a word list
		"${PHXCC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror ${SSP} "${TFLAGS[@]}" -I"${SYSD}/include" \
			$("${PKGC}" --cflags ${mods}) -c "${F}/src/soup-smoke.c" -o "${out}/obj/soup-smoke.o"
		# shellcheck disable=2046
		"${PHXCXX}" -O2 -g -std=gnu++17 -Wall -Wextra -Werror ${SSP} "${TFLAGS[@]}" \
			$("${PKGC}" --cflags libwoff2enc libwoff2dec) -c "${F}/src/woff2-roundtrip.cc" -o "${out}/obj/woff2-roundtrip.o"
		# shellcheck disable=2046
		"${PHXCXX}" "${TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,-Map,"${out}/soup-smoke.map" \
			-o "${BIN}/soup-smoke" "${out}/obj/soup-smoke.o" "${out}/obj/woff2-roundtrip.o" -L"${SYSD}/lib" \
			-Wl,--start-group $("${PKGC}" --libs ${mods}) -Wl,--end-group >"${out}/soup-smoke-link.log" 2>&1 ||
			{ grep -v 'warning: .* is not fully supported' "${out}/soup-smoke-link.log" | head -40; b_die "webkit_deps: soup-smoke: link failed"; }
		"${TC}-strip" -o "${BIN}/soup-smoke-stripped" "${BIN}/soup-smoke"
		und="$("${TC}-nm" -u "${BIN}/soup-smoke" || true)"
		n=$(grep -c . <<<"${und}" || true)
		interp="$("${TC}-readelf" -l "${BIN}/soup-smoke" | grep -c INTERP || true)"
		syms="$("${TC}-nm" "${BIN}/soup-smoke")"
		echo "webkit_deps: soup-smoke: nm -u ${n}, PT_INTERP ${interp}; $("${TC}-size" "${BIN}/soup-smoke" | awk 'NR==2 {printf "text %d data %d bss %d", $1, $2, $3}'); stripped $(stat -c %s "${BIN}/soup-smoke-stripped")"
		if [ "${n}" != 0 ] || [ "${interp}" != 0 ]; then head -10 <<<"${und}"; bad=1; fi
		for s in g_io_openssl_load g_tls_backend_openssl_register soup_session_send_and_read_async nghttp2_session_client_new2 \
			BrotliDecoderDecompressStream psl_builtin WebPDecodeRGBA WebPDemuxInternal WebPMuxAssemble xsltApplyStylesheet \
			SSL_CTX_new X509_STORE_set_default_paths; do
			grep -qE " [TtWw] ${s}\$" <<<"${syms}" || { echo "webkit_deps: soup-smoke: symbol ${s} missing"; bad=1; }
		done
		grep -qE ' [TtWw] _ZN5woff217ConvertWOFF2ToTTF' <<<"${syms}" || { echo "webkit_deps: soup-smoke: woff2 decoder missing"; bad=1; }
		# (libstdc++'s own headers name the toolchain build tree in its assertion messages)
		n="$(strings -a "${BIN}/soup-smoke-stripped" | grep -E 'port-sources|versioned-ports|/home/' | grep -vc '/_build/gcc-' || true)"
		echo "webkit_deps: soup-smoke: build-host path strings: ${n}"
		[ "${n}" = 0 ] || bad=1
		install -D -m 755 "${BIN}/soup-smoke-stripped" "${ST}/usr/bin/soup-smoke"
	fi
	[ "${bad}" = 0 ] || b_die "webkit_deps: verification failed"

	{ echo "# webkit_deps port build"
	  (cd "${P}/lib" && sha256sum ./*.a gio/modules/libgioopenssl.a | sed 's|\./||')
	  if b_use smoke; then sha256sum "${BIN}/soup-smoke-stripped" | sed "s|${I}/||"; fi; } >"${I}/SHA256SUMS"
	cat "${I}/SHA256SUMS"

	# --- the staging tree: only soup-smoke (USE smoke); the libraries are link inputs ---
	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs -r sha256sum) >"${I}/stage.MANIFEST"
	echo "webkit_deps: stage: $(wc -l <"${I}/stage.MANIFEST") file(s)"
	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "webkit_deps: staged into ${PREFIX_FS}/root"
	fi
}
