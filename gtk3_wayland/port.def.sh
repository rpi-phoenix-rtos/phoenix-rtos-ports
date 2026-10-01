#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="gtk3_wayland"
	version="3.24.52"
	desc="GTK 3.24 (Wayland backend only) + GLib 2.88/GIO, pango 1.54, cairo 1.18, gdk-pixbuf, atk, gtk-layer-shell"

	# Aggregate port: GTK 3 with ONLY the Wayland GDK backend and the libraries it
	# needs that the ports prefix lacks or has too old, as the coordination repo's
	# tools/gpu-lane/gtk3-wayland/build.sh --usr builds them (M7 stage 2):
	#
	#   pcre2 10.47 (BSD-3)          GLib >= 2.74 needs PCRE2; ports has only PCRE1
	#   GLib 2.88.3 (LGPL) + GIO     ports glib2 is 2.56.4 without GIO
	#   fribidi 1.0.16 (LGPL)        pango, GTK
	#   atk 2.38.0 (LGPL)            GTK 3 still links ATK (no at-spi bridge)
	#   gdk-pixbuf 2.42.12 (LGPL)    PNG + JPEG + GIF loaders built in (no loader modules)
	#   harfbuzz 14.4.0 (MIT)        = the ports release, rebuilt with meson: no C++ runtime
	#   pango 1.54.0 (LGPL)          the last pango that accepts the ports fontconfig 2.14
	#   cairo 1.18.4 (LGPL/MPL)      ports cairo 1.16 lacks PDF/PS surfaces and cairo-gobject
	#   GTK 3.24.52 (LGPL)           Wayland only; no X11/broadway, introspection, print
	#                                backends, colord/cloudproviders/tracker
	#   gtk-layer-shell 0.10.1 (LGPL-3) layer-shell for GTK 3 (xfce4-panel, xfdesktop)
	#   libepoxy 1.5.10 (MIT)        static-EGL dispatch (patches/libepoxy, as the
	#                                xorg-drm build), with files/gtkphx_noegl.c as its
	#                                default EGL: GTK here draws with cairo/wl_shm only
	#   gtk3-hello                   the M7 test program (files/src/gtk3-hello.c)
	#
	# Everything is configured for the TARGET's paths (--prefix /usr --sysconfdir /etc
	# --localstatedir /var) and installed with DESTDIR into this port's prefix; the
	# XFCE port builds on exactly that layout (see "install layout" below).
	source="https://download.gnome.org/sources/gtk/3.24/"
	archive_filename="gtk-${version}.tar.xz"
	src_path="gtk-${version}/"

	size="13578032"
	sha256="80931fa472a77b9a164f6740e3c0b444fac6770054632d35a7ff9d679e5e7b9f"

	# GTK/GLib/pango/atk/gdk-pixbuf/fribidi: LGPL-2.1-or-later; gtk-layer-shell:
	# LGPL-3.0-or-later; cairo: LGPL-2.1-only OR MPL-1.1; pcre2: BSD-3-Clause WITH
	# PCRE2-exception; harfbuzz, libepoxy: MIT; files/egl-include (the Khronos EGL/KHR
	# headers as Mesa 26.2 installs them): Apache-2.0 and MIT (eglext_angle.h
	# BSD-3-Clause); files/src: BSD-3-Clause (ours).
	license="LGPL-2.1-or-later AND LGPL-3.0-or-later AND (LGPL-2.1-only OR MPL-1.1) AND BSD-3-Clause WITH PCRE2-exception AND MIT AND Apache-2.0 AND BSD-3-Clause"
	license_file="COPYING"

	# A real conflict: this port carries its own GLib 2.88, and a program that also
	# linked the ports glib2 (2.56) would get two GLibs. The conflict also gives the
	# port its own versioned-ports/<name>-<version>/ prefix, so none of these
	# libraries reaches the shared _build/<target>/{lib,include} of the image's ports.
	conflicts="glib2>=0.0"
	# From the shared ports prefix, through private per-library views: zlib, libffi,
	# expat/freetype/fontconfig (xorg_fonts), pixman 0.42 (xorg_libs), libpng16,
	# libjpeg, libiconv. The Wayland client stack + compat: wayland_phoenix.
	depends="wayland_phoenix xorg_fonts xorg_libs libpng libjpeg libffi zlib libiconv"

	# rootfs: also copy the staging tree (stage/) into the image rootfs.
	# demos:  put the test programs in the staging tree too: gtk3-hello, gtk3-demo,
	#         gtk3-widget-factory (built and verified either way; nothing ships them).
	iuse="rootfs demos"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}), = the tools build's --usr <out> dir, which
# is what xfce_wayland reads:
#   destdir/usr/                 every library: static .a, headers, *.pc (prefix=/usr;
#                                pkg-config-phoenix runs with --define-prefix)
#   deps/                        the private ports views + the Wayland/epoxy snapshots
#   phoenix-aarch64{,-wl,-gtk}.cross, pkg-config-phoenix, host-bin/phx-{gcc,g++}
#   data/glib-2.0/schemas/       gschemas.compiled (host-compiled)
#   bin/                         gtk3-hello, gtk3-demo, gtk3-widget-factory (+ -stripped)
#   stage/ + stage.MANIFEST      the files for the target rootfs (new names only; the
#                                bin/ programs only with USE demos)
#   SHA256SUMS
#
# Host tools: meson, ninja, cmake, wayland-scanner 1.24.0, python3, and GLib's host
# tools (glib-compile-resources, glib-mkenums, glib-genmarshal, gdbus-codegen,
# glib-compile-schemas; a 2.88 series host matches the target GLib).

# name|file|url|sha256 (the anchor, gtk, is fetched by the framework). name = the
# directory under src/ and under patches/ (libepoxy keeps the xorg-drm build's
# versioned directory name: meson's relative source paths end up in the objects).
_gtk3wl_pkgs() {
	cat <<'EOF'
pcre2|pcre2-10.47.tar.bz2|https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.tar.bz2|47fe8c99461250d42f89e6e8fdaeba9da057855d06eb7fc08d9ca03fd08d7bc7
glib|glib-2.88.3.tar.xz|https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz|ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b
fribidi|fribidi-1.0.16.tar.xz|https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz|1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
atk|atk-2.38.0.tar.xz|https://download.gnome.org/sources/atk/2.38/atk-2.38.0.tar.xz|ac4de2a4ef4bd5665052952fe169657e65e895c5057dffb3c2a810f6191a0c36
gdk-pixbuf|gdk-pixbuf-2.42.12.tar.xz|https://download.gnome.org/sources/gdk-pixbuf/2.42/gdk-pixbuf-2.42.12.tar.xz|b9505b3445b9a7e48ced34760c3bcb73e966df3ac94c95a148cb669ab748e3c7
harfbuzz|harfbuzz-14.4.0.tar.xz|https://github.com/harfbuzz/harfbuzz/releases/download/14.4.0/harfbuzz-14.4.0.tar.xz|2357ed966c6ced7bfa720b0640c0231065af01158fbea215093ffa15aed44371
pango|pango-1.54.0.tar.xz|https://download.gnome.org/sources/pango/1.54/pango-1.54.0.tar.xz|8a9eed75021ee734d7fc0fdf3a65c3bba51dfefe4ae51a9b414a60c70b2d1ed8
cairo|cairo-1.18.4.tar.xz|https://cairographics.org/releases/cairo-1.18.4.tar.xz|445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb
gtk-layer-shell|gtk-layer-shell-0.10.1.tar.gz|https://github.com/wmww/gtk-layer-shell/archive/refs/tags/v0.10.1.tar.gz|88c3a3e0a5300532f3d368d5df64838a87f1fb85273f22d41df0a6b8d0ec59c6
libepoxy-1.5.10|libepoxy-1.5.10.tar.gz|https://github.com/anholt/libepoxy/archive/refs/tags/1.5.10.tar.gz|a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15
EOF
}

# _gtk3wl_fetch <file> <url> <sha256>: the tarball in the port directory (gitignored),
# else ${PHOENIX_DISTFILES:-~/.phoenix-distfiles}/newlane/, else downloaded (and cached
# there). Verified before use.
_gtk3wl_fetch() {
	local file="$1" url="$2" sum="$3"
	local cache="${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane"
	local f="${PREFIX_PORT}/${file}"
	if [ ! -f "${f}" ]; then
		f="${cache}/${file}"
		if [ ! -f "${f}" ]; then
			mkdir -p "${cache}"
			curl -sSfL --retry 3 -o "${f}.part" "${url}" || b_die "gtk3_wayland: download failed: ${url}"
			mv "${f}.part" "${f}"
		fi
	fi
	echo "${sum}  ${f}" | sha256sum -c --quiet - || b_die "gtk3_wayland: ${file}: sha256 mismatch (${f})"
	echo "${f}"
}

# _gtk3wl_extract <name> <tarball-or-dir> <sha256>: ${out}/src/<name>, as its own git
# repository (inside ANOTHER repository -- the buildroot is one -- git would apply
# relative to that repository's root and silently skip every path), with
# patches/<name>/*.patch applied by `git am` (each keeps its message, so `git
# format-patch` in the tree regenerates patches/<name>/ unchanged). Re-extracted when
# the tarball or a patch changes.
_gtk3wl_extract() {
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
		echo "gtk3_wayland: apply ${name}/$(basename "${p}")"
		git -C "${dir}" -c user.name=build -c user.email=build@invalid am -q --whitespace=nowarn "${p}"
	done
	echo "${stamp}" >"${dir}.stamp"
	rm -f "${out}/${name}.built"
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
	for t in meson ninja cmake wayland-scanner python3 git curl glib-compile-resources glib-mkenums glib-genmarshal \
		gdbus-codegen glib-compile-schemas; do
		command -v "${t}" >/dev/null || b_die "gtk3_wayland: host tool ${t} not found"
	done
	[ "$(wayland-scanner --version 2>&1 | awk '{print $2}')" = "1.24.0" ] ||
		b_die "gtk3_wayland: host wayland-scanner is not 1.24.0"

	_gtk3wl_extract gtk "${PREFIX_PORT_WORKDIR%/}" "${sha256}"
	local n file url sum
	while IFS='|' read -r n file url sum; do
		_gtk3wl_extract "${n}" "$(_gtk3wl_fetch "${file}" "${url}" "${sum}")" "${sum}"
	done < <(_gtk3wl_pkgs)
}

p_build() {
	local out="${PREFIX_PORT_BUILD}/out"
	local I="${PREFIX_PORT_INSTALL%/}"
	local B S TC WLP
	B="${PREFIX_BUILD%/}"            # the shared ports prefix (the dependencies above)
	S="${PREFIX_BUILD%/}/sysroot"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	WLP="$(b_dependency_dir wayland_phoenix)"
	local wl_src_prefix="${WLP%/}/prefix"
	local F="${PREFIX_PORT}/files"
	local COMPAT_INC="${wl_src_prefix}/share/wayland-phoenix/compat/include"   # M6: epoll/timerfd/signalfd/memfd...
	# (mesa-drm's compat/include is NOT used: libphoenix has had what it shims since
	# build 10, and its duplicate declarations break -Werror=redundant-decls builds such
	# as pango's.)
	local DESTDIR_="${I}/destdir"
	local P="${DESTDIR_}/usr"      # where the installed files are, on the build host
	local PREFIX_ARGS=(--prefix /usr --sysconfdir /etc --localstatedir /var)
	local INSTALL_ENV=(env DESTDIR="${DESTDIR_}")
	local CMAKE_PREFIX=/usr
	local PKGC_DEFINE_PREFIX=" --define-prefix"
	local D="${I}/deps"
	local SYSD="${D}/sys"   # libintl stub + libiconv + resolv: on every compile/link of this
	                        # build (meson's builtin intl/iconv dependencies look for them as
	                        # system libraries)
	local jobs
	jobs="$(nproc)"

	local p
	for p in "${S}/lib/libphoenix.a" "${TC}-gcc" "${TC}-gcc-ar" "${TC}-nm" "${TC}-strip" \
		"${B}/lib/libfontconfig.a" "${B}/lib/libfreetype.a" \
		"${B}/lib/libpixman-1.a" "${B}/lib/libpng16.a" "${B}/lib/libjpeg.a" "${B}/lib/libffi.a" "${B}/lib/libexpat.a" \
		"${B}/lib/libz.a" "${B}/lib/libiconv.a" \
		"${wl_src_prefix}/lib/libwayland-client.a" "${wl_src_prefix}/lib/libwlphx-compat.a" \
		"${wl_src_prefix}/lib/libxkbcommon.a" "${F}/egl-include/EGL/egl.h"; do
		[ -e "${p}" ] || b_die "gtk3_wayland: missing ${p}"
	done

	# A fresh work tree (no .built stamps) starts from an empty install: nothing from an
	# earlier build may linger in destdir/.
	if [ ! -f "${out}/install.fresh" ]; then
		rm -rf "${DESTDIR_:?}" "${D:?}" "${I:?}/data" "${I:?}/bin" "${I:?}/stage" "${I:?}/host-bin"
		touch "${out}/install.fresh"
	fi
	mkdir -p "${P}/lib/pkgconfig" "${P}/include" "${D}" "${SYSD}/include" "${SYSD}/lib" "${out}/bin" "${out}/obj"
	local TFLAGS=(-mcpu=cortex-a72 -mtune=cortex-a72 -mstrict-align -mno-outline-atomics -ffunction-sections -fdata-sections
		--sysroot="${S}/" -B"${S}/lib/")

	# The compiler wrappers: the aarch64-phoenix gcc rejects -pthread (GLib's .pc files
	# and meson's threads dependency add it; Phoenix pthreads live in libphoenix, so
	# dropping it is exact), also inside @response files (meson writes one for long link
	# lines, e.g. gtk3-demo's). (tools/gpu-lane/gtk3-wayland/bin/phx-gcc)
	# They are installed (host-bin/): the cross files, which xfce_wayland reuses, name them.
	local cc
	mkdir -p "${I}/host-bin"
	for cc in gcc g++; do
		cat >"${I}/host-bin/phx-${cc}" <<EOF
#!/bin/sh
# Generated by the phoenix-rtos-ports gtk3_wayland recipe: ${TC}-${cc} without -pthread.
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
		chmod +x "${I}/host-bin/phx-${cc}"
	done
	local PHXCC="${I}/host-bin/phx-gcc" PHXCXX="${I}/host-bin/phx-g++"

	# --- private views of the ports prefix ---
	# view name version "cflags-subdirs" "libs" "requires" "requires.private" inc:<path>|lib:<path>...
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
				*) cf="${cf} -I\${prefix}/include/${s}" ;;
			esac
		done
		printf '%s\n' "prefix=${D}/${name}" "Name: ${name}" "Description: ${name} from the Phoenix ports prefix" \
			"Version: ${ver}" "Requires: ${req}" "Requires.private: ${reqp}" "Libs: -L\${prefix}/lib ${libs}" \
			"Cflags:${cf}" >"${D}/${name}/lib/pkgconfig/${name}.pc"
	}
	_pc_alias() {  # alias-name view version requires
		printf '%s\n' "Name: $1" "Description: alias within the $2 view" "Version: $3" "Requires: $4" "Libs:" "Cflags:" \
			>"${D}/$2/lib/pkgconfig/$1.pc"
	}
	_pcver() { sed -n 's/^Version: *//p' "${B}/lib/pkgconfig/$1.pc"; }

	# --- meson cross files + pkg-config ---
	_write_cross() {
		local pkgc="${I}/pkg-config-phoenix" v libdirs=""
		for v in zlib libffi expat pixman-1 libpng16 libjpeg freetype2 fontconfig wayland epoxy; do
			libdirs="${libdirs}:${D}/${v}/lib/pkgconfig"
		done
		cat >"${pkgc}" <<EOF
#!/bin/sh
# pkg-config restricted to this build's prefix, the private ports views and the snapshots.
export PKG_CONFIG_LIBDIR=${P}/lib/pkgconfig:${P}/share/pkgconfig${libdirs}:${D}/wayland/share/pkgconfig
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config --static${PKGC_DEFINE_PREFIX} "\$@"
EOF
		chmod +x "${pkgc}"
		# -fmacro-prefix-map: __FILE__ of an installed header names it relative to the
		# include dir, not by its build-host path. GTK's gtkwidget.c includes GLib's
		# gobject/gobjectnotifyqueue.c, whose g_return_if_fail() messages put that path
		# into every program linking libgtk-3.a.
		local pmap="'-fmacro-prefix-map=${P}/include/='"
		local flags="'--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections', '-I${SYSD}/include', ${pmap}"
		local lflags="'--sysroot=${S}/', '-B${S}/lib/', '-L${SYSD}/lib', '-Wl,-z,max-page-size=0x1000'"
		# GTK and gtk-layer-shell (Wayland clients) additionally see the M6 compat layer
		# (memfd_create over shmsrv, epoll & co.) -- never GLib: its configure would find
		# the emulated eventfd/epoll headers and build its main loop on them.
		# (-I the Wayland snapshot too: GTK's configure looks for <linux/input.h> -- the
		# M6 shim over FreeBSD's evdev codes -- with the base flags only.)
		local flags_wl="'--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections', '-I${SYSD}/include', '-I${COMPAT_INC}', '-I${D}/wayland/include', ${pmap}"
		local lflags_wl="${lflags}, '-Wl,-u,__wrap_close', '-Wl,-u,__wrap_write', '${D}/wayland/lib/libwlphx-compat.a', '-Wl,--wrap=close', '-Wl,--wrap=write'"
		# GTK itself: the wl flags + the built-in keymap header (GTK patch 0003)
		local flags_gtk="${flags_wl}, '-DGDK_WAYLAND_BUILTIN_XKB_KEYMAP_H=\"${D}/gdk_builtin_keymap.h\"'"
		local c f l
		for c in "" -wl -gtk; do
			f="${flags}"
			l="${lflags}"
			if [ "${c}" = -wl ]; then f="${flags_wl}"; l="${lflags_wl}"; fi
			if [ "${c}" = -gtk ]; then f="${flags_gtk}"; l="${lflags_wl}"; fi
			cat >"${I}/phoenix-aarch64${c}.cross" <<EOF
# Generated by the phoenix-rtos-ports gtk3_wayland recipe (aarch64-phoenix, Pi 4).
[binaries]
c = '${PHXCC}'
cpp = '${PHXCXX}'
ar = '${TC}-gcc-ar'
nm = '${TC}-nm'
strip = '${TC}-strip'
objcopy = '${TC}-objcopy'
pkg-config = '${pkgc}'
glib-compile-resources = '$(command -v glib-compile-resources)'
glib-compile-schemas = '$(command -v glib-compile-schemas)'
glib-mkenums = '$(command -v glib-mkenums)'
glib-genmarshal = '$(command -v glib-genmarshal)'
gdbus-codegen = '$(command -v gdbus-codegen)'
wayland-scanner = '$(command -v wayland-scanner)'

[host_machine]
system = 'phoenix'
cpu_family = 'aarch64'
cpu = 'cortex-a72'
endian = 'little'

[properties]
needs_exe_wrapper = true
# GLib's run-time probes (cross answers). The printf family is libphoenix's: GLib's
# gnulib replacement needs frexpl(), which libphoenix lacks. libphoenix's vsnprintf
# is C99 (returns the full length for a short buffer); positional %1\$s arguments
# appear only in translations, and this build has none (no NLS).
have_c99_vsnprintf = true
have_c99_snprintf = true
have_unix98_printf = true
growing_stack = false
va_val_copy = true
have_strlcpy = true
have_proc_self_cmdline = false

[built-in options]
c_args = [${f}]
cpp_args = [${f}]
c_link_args = [${l}]
cpp_link_args = [${l}]
default_library = 'static'
EOF
		done
	}

	_meson_pkg() {  # [--cross <file>] name builddir-name meson-args...
		local cross="${I}/phoenix-aarch64.cross"
		if [ "$1" = --cross ]; then cross="$2"; shift 2; fi
		local name="$1" bname="$2"
		shift 2
		local bd="${out}/${bname}"
		if [ -f "${out}/${name}.built" ]; then
			echo "gtk3_wayland: ${name}: up to date"
			return 0
		fi
		rm -rf "${bd}"
		meson setup "${bd}" "${out}/src/${name}" --cross-file "${cross}" "${PREFIX_ARGS[@]}" \
			--libdir lib --buildtype=debugoptimized -Db_staticpic=false -Db_ndebug=if-release --wrap-mode=nodownload "$@" \
			>"${out}/${bname}-setup.log" 2>&1 || { tail -40 "${out}/${bname}-setup.log"; b_die "gtk3_wayland: ${name}: meson setup failed"; }
		ninja -C "${bd}" -j"${jobs}" >"${out}/${bname}-ninja.log" 2>&1 || { grep -E -A8 'error|FAILED' "${out}/${bname}-ninja.log" | head -80; b_die "gtk3_wayland: ${name}: build failed"; }
		"${INSTALL_ENV[@]}" ninja -C "${bd}" install >"${out}/${bname}-install.log" 2>&1 || { tail -20 "${out}/${bname}-install.log"; b_die "gtk3_wayland: ${name}: install failed"; }
		echo "gtk3_wayland: ${name}: built ($(grep -c 'warning:' "${out}/${bname}-ninja.log" || true) warning line(s))"
		touch "${out}/${name}.built"
	}

	# GLib's .pc files name its tools under ${bindir} (target binaries here); consumers
	# that read them (pkg-config --variable) must get the host's tools.
	_fix_glib_pc() {
		local pc v
		for pc in "${P}/lib/pkgconfig/glib-2.0.pc" "${P}/lib/pkgconfig/gio-2.0.pc"; do
			[ -f "${pc}" ] || continue
			for v in glib_genmarshal gobject_query glib_mkenums glib_compile_schemas glib_compile_resources gdbus_codegen; do
				sed -i "s|^${v}=.*|${v}=$(command -v "$(echo "${v}" | tr _ -)" || echo /bin/false)|" "${pc}"
			done
		done
	}

	# --- dependency views (ports prefix) + snapshots ---
	_view zlib "$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' "${B}/include/zlib.h")" . -lz "" "" inc:zlib.h inc:zconf.h lib:libz.a
	_view libffi "$(_pcver libffi)" . -lffi "" "" inc:ffi.h inc:ffitarget.h lib:libffi.a
	_view expat "$(_pcver expat)" . -lexpat "" "" inc:expat.h inc:expat_config.h inc:expat_external.h lib:libexpat.a
	_view pixman-1 "$(_pcver pixman-1)" pixman-1 -lpixman-1 "" "" inc:pixman-1 lib:libpixman-1.a
	_view libpng16 "$(_pcver libpng16)" libpng16 -lpng16 "" zlib inc:libpng16 lib:libpng16.a
	_pc_alias libpng libpng16 "$(_pcver libpng16)" libpng16
	_view libjpeg "$(_pcver libjpeg)" . -ljpeg "" "" inc:jpeglib.h inc:jconfig.h inc:jerror.h inc:jmorecfg.h lib:libjpeg.a
	_view freetype2 "$(_pcver freetype2)" freetype2 -lfreetype "" "" inc:freetype2 lib:libfreetype.a
	_view fontconfig "$(_pcver fontconfig)" . -lfontconfig freetype2 expat inc:fontconfig lib:libfontconfig.a
	# libiconv + the libintl stub: system-library style (meson's builtin deps look there)
	cp "${B}/include/iconv.h" "${B}/include/libcharset.h" "${B}/include/localcharset.h" "${SYSD}/include/"
	cp "${B}/lib/libiconv.a" "${SYSD}/lib/"
	cp "${F}/src/intl/libintl.h" "${SYSD}/include/"
	"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${SYSD}/include" \
		-c "${F}/src/intl/intl_stub.c" -o "${out}/obj/intl_stub.o"
	rm -f "${SYSD}/lib/libintl.a"
	"${TC}-gcc-ar" rcs "${SYSD}/lib/libintl.a" "${out}/obj/intl_stub.o"
	# resolver headers + stand-ins for GIO's DNS record queries (files/src/resolv/)
	mkdir -p "${SYSD}/include/arpa"
	cp "${F}/src/resolv/resolv.h" "${SYSD}/include/"
	cp "${F}/src/resolv/arpa/nameser.h" "${SYSD}/include/arpa/"
	"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${SYSD}/include" \
		-c "${F}/src/resolv/resolv_stub.c" -o "${out}/obj/resolv_stub.o"
	rm -f "${SYSD}/lib/libresolv.a"
	"${TC}-gcc-ar" rcs "${SYSD}/lib/libresolv.a" "${out}/obj/resolv_stub.o"

	# The Wayland client stack (snapshot of wayland_phoenix).
	rm -rf "${D}/wayland"
	mkdir -p "${D}/wayland/lib/pkgconfig" "${D}/wayland/share"
	cp -a "${wl_src_prefix}/include" "${D}/wayland/"
	local l pc
	for l in libwayland-client.a libwayland-cursor.a libwayland-egl.a libxkbcommon.a libwlphx-compat.a; do
		cp -a "${wl_src_prefix}/lib/${l}" "${D}/wayland/lib/"
	done
	cp -a "${wl_src_prefix}/share/pkgconfig" "${wl_src_prefix}/share/wayland-protocols" "${D}/wayland/share/"
	if [ -d "${wl_src_prefix}/share/wayland" ]; then
		cp -a "${wl_src_prefix}/share/wayland" "${D}/wayland/share/"
	fi
	for pc in wayland-client wayland-cursor wayland-egl wayland-egl-backend xkbcommon wlphx-compat; do
		sed "s|${wl_src_prefix}|${D}/wayland|g" "${wl_src_prefix}/lib/pkgconfig/${pc}.pc" >"${D}/wayland/lib/pkgconfig/${pc}.pc"
	done
	sed -i "s|${wl_src_prefix}|${D}/wayland|g" "${D}/wayland/share/pkgconfig/wayland-protocols.pc"

	# libepoxy 1.5.10 with the static-EGL dispatch patch, built as the xorg-drm build
	# builds it (its cross file: E7 wrappers, tree sysroot, xorg-drm's compat headers;
	# files/epoxy-compat) against the Khronos EGL headers (files/egl-include, with an
	# egl.pc equal to Mesa 26.2's in everything epoxy's meson reads from it).
	local DP="${out}/epoxy-prefix"
	if [ ! -f "${DP}/lib/libepoxy.a" ]; then
		local EPOXY_SRC="${out}/src/libepoxy-1.5.10" EPC="${out}/epoxy-egl"
		rm -rf "${out}/epoxy-build" "${DP}" "${EPC}"
		mkdir -p "${EPC}/lib/pkgconfig"
		printf '%s\n' "prefix=${F}/egl-include" "includedir=\${prefix}" "" "Name: egl" \
			"Description: Khronos EGL headers (as installed by Mesa 26.2)" "Version: 26.2.0" \
			"Libs: -pthread -lm" "Cflags: -I\${includedir} -pthread -DXXH_FORCE_ALIGN_CHECK=0 -DXXH_FORCE_MEMORY_ACCESS=0" \
			>"${EPC}/lib/pkgconfig/egl.pc"
		cat >"${out}/epoxy-pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR=${EPC}/lib/pkgconfig
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
exec /usr/bin/pkg-config --static "\$@"
EOF
		chmod +x "${out}/epoxy-pkg-config"
		local eflags="'--sysroot=${S}/', '-B${S}/lib/', '-mcpu=cortex-a72', '-mtune=cortex-a72', '-mstrict-align', '-mno-outline-atomics', '-ffunction-sections', '-fdata-sections', '-I${F}/epoxy-compat/include'"
		local elflags="'--sysroot=${S}/', '-B${S}/lib/', '-L${B}/lib', '-Wl,-z,max-page-size=0x1000'"
		cat >"${out}/epoxy.cross" <<EOF
# Generated by the phoenix-rtos-ports gtk3_wayland recipe (libepoxy; aarch64-phoenix, Pi 4).
[binaries]
c = '${PHXCC}'
cpp = '${PHXCXX}'
ar = '${TC}-gcc-ar'
nm = '${TC}-nm'
strip = '${TC}-strip'
objcopy = '${TC}-objcopy'
pkg-config = '${out}/epoxy-pkg-config'

[host_machine]
system = 'phoenix'
cpu_family = 'aarch64'
cpu = 'cortex-a72'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = [${eflags}]
cpp_args = [${eflags}]
c_link_args = [${elflags}]
cpp_link_args = [${elflags}]
default_library = 'static'
EOF
		meson setup "${out}/epoxy-build" "${EPOXY_SRC}" --cross-file "${out}/epoxy.cross" \
			--prefix "${DP}" --libdir lib --buildtype=debugoptimized -Db_ndebug=true -Db_staticpic=false \
			-Dglx=no -Degl=yes -Dx11=false -Dtests=false -Ddocs=false \
			>"${out}/epoxy-setup.log" 2>&1 || { tail -30 "${out}/epoxy-setup.log"; b_die "gtk3_wayland: libepoxy: meson setup failed"; }
		ninja -C "${out}/epoxy-build" >"${out}/epoxy-ninja.log" 2>&1 || { grep -B2 -A6 -E 'error|FAILED' "${out}/epoxy-ninja.log" | head -40; b_die "gtk3_wayland: libepoxy: build failed"; }
		ninja -C "${out}/epoxy-build" install >"${out}/epoxy-install.log" 2>&1 || { tail -20 "${out}/epoxy-install.log"; b_die "gtk3_wayland: libepoxy: install failed"; }
	fi
	"${TC}-nm" "${DP}/lib/libepoxy.a" | grep -q ' T epoxy_static_proc_address$' ||
		b_die "gtk3_wayland: libepoxy.a has no epoxy_static_proc_address (the static-EGL patch)"
	# ... and the snapshot GTK builds on: libepoxy + the EGL headers it includes + the
	# no-EGL stand-in (files/src/gtkphx_noegl.c) as its default EGL.
	rm -rf "${D}/epoxy"
	mkdir -p "${D}/epoxy/lib/pkgconfig" "${D}/epoxy/include"
	cp -a "${DP}/include/epoxy" "${D}/epoxy/include/"
	cp -a "${F}/egl-include/EGL" "${F}/egl-include/KHR" "${D}/epoxy/include/"
	cp -a "${DP}/lib/libepoxy.a" "${D}/epoxy/lib/"
	"${TC}-gcc" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -c "${F}/src/gtkphx_noegl.c" -o "${out}/obj/gtkphx_noegl.o"
	rm -f "${D}/epoxy/lib/libgtkphx-noegl.a"
	"${TC}-gcc-ar" rcs "${D}/epoxy/lib/libgtkphx-noegl.a" "${out}/obj/gtkphx_noegl.o"
	printf '%s\n' "prefix=${D}/epoxy" "epoxy_has_glx=0" "epoxy_has_egl=1" "epoxy_has_wgl=0" "Name: epoxy" \
		"Description: libepoxy 1.5.10 (xorg-drm static-EGL build) + no-EGL stand-in" "Version: 1.5.10" \
		"Libs: -L\${prefix}/lib -lepoxy -lgtkphx-noegl" "Cflags: -I\${prefix}/include -DEGL_NO_X11" \
		>"${D}/epoxy/lib/pkgconfig/epoxy.pc"
	# GDK's default keymap before (or without) a wl_keyboard: wayland_phoenix's baked
	# evdev/pc105/us keymap. GTK patch 0003 includes it when
	# GDK_WAYLAND_BUILTIN_XKB_KEYMAP_H names this header.
	local km="${WLP%/}/keymap-us.xkb"
	grep -q 'xkb_keymap' "${km}" 2>/dev/null || b_die "gtk3_wayland: ${km} is not a keymap"
	cp "${km}" "${D}/keymap-us.xkb"
	python3 - "${D}/keymap-us.xkb" "${D}/gdk_builtin_keymap.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
with open(dst, 'w') as f:
    f.write('/* Generated by tools/gpu-lane/gtk3-wayland/build.sh from keymap-us.xkb\n')
    f.write(' * (xkbcli compile-keymap --rules evdev --model pc105 --layout us). */\n')
    f.write('static const char gdk_wayland_builtin_xkb_keymap[] =\n')
    for line in open(src).read().splitlines():
        f.write('\t"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n')
    f.write('\t;\n')
PY
	{ echo "wayland: ${wl_src_prefix}"; echo "epoxy: ${DP}"; echo "EGL headers: ${F}/egl-include"
	  echo "keymap: ${km}"; sha256sum "${D}/keymap-us.xkb" | sed "s|${D}/||"
	  sha256sum "${D}"/wayland/lib/*.a "${D}/epoxy/lib/libepoxy.a" | sed "s|${D}/||"; } >"${I}/snapshots.txt"

	_write_cross

	# --- libraries ---
	if [ ! -f "${out}/pcre2.built" ]; then
		rm -rf "${out}/pcre2-build"
		cmake -S "${out}/src/pcre2" -B "${out}/pcre2-build" -G Ninja \
			-DCMAKE_SYSTEM_NAME=Generic -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
			-DCMAKE_C_COMPILER="${TC}-gcc" -DCMAKE_AR="${TC}-gcc-ar" -DCMAKE_RANLIB="${TC}-gcc-ranlib" \
			-DCMAKE_C_FLAGS="${TFLAGS[*]} -O2 -g" -DCMAKE_EXE_LINKER_FLAGS="--sysroot=${S}/ -B${S}/lib/" \
			-DCMAKE_INSTALL_PREFIX="${CMAKE_PREFIX}" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
			-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF \
			-DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_SUPPORT_JIT=OFF -DPCRE2_SUPPORT_UNICODE=ON -DPCRE2_BUILD_PCRE2GREP=OFF \
			-DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_LIBBZ2=OFF -DPCRE2_SUPPORT_LIBZ=OFF -DPCRE2_SUPPORT_LIBEDIT=OFF \
			-DPCRE2_SUPPORT_LIBREADLINE=OFF -DPCRE2_STATIC_PIC=OFF \
			>"${out}/pcre2-setup.log" 2>&1 || { tail -30 "${out}/pcre2-setup.log"; b_die "gtk3_wayland: pcre2: cmake failed"; }
		"${INSTALL_ENV[@]}" ninja -C "${out}/pcre2-build" -j"${jobs}" install >"${out}/pcre2-ninja.log" 2>&1 ||
			{ grep -E -A6 'error|FAILED' "${out}/pcre2-ninja.log" | head -40; b_die "gtk3_wayland: pcre2: build failed"; }
		[ -f "${P}/lib/pkgconfig/libpcre2-8.pc" ] || b_die "gtk3_wayland: pcre2 installed no libpcre2-8.pc"
		touch "${out}/pcre2.built"
	fi

	_meson_pkg glib glib-build -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled -Dxattr=false \
		-Dlibelf=disabled -Dsysprof=disabled -Dintrospection=disabled -Dtests=false -Dinstalled_tests=false \
		-Ddocumentation=false -Dman-pages=disabled -Ddtrace=disabled -Dsystemtap=disabled \
		-Dbsymbolic_functions=false -Dglib_debug=disabled -Dfile_monitor_backend=auto
	_fix_glib_pc

	_meson_pkg fribidi fribidi-build -Ddocs=false -Dbin=false -Dtests=false
	_meson_pkg atk atk-build -Dintrospection=false -Ddocs=false
	# PNG + JPEG + GIF (animated too) loaders built in; sniffing by loader signatures,
	# not GIO. GIF is in-tree C with no library. TIFF needs a libtiff port (none).
	# -Dothers (bmp/ico/ani/pnm/tga/xpm/xbm/icns/qtif) stays off: upstream turned it
	# off by default as "weakly maintained", and these parsers would read every
	# untrusted file Thunar thumbnails.
	_meson_pkg gdk-pixbuf gdk-pixbuf-build -Dpng=enabled -Djpeg=enabled -Dtiff=disabled -Dgif=enabled \
		-Dothers=disabled -Dbuiltin_loaders=png,jpeg,gif -Dintrospection=disabled -Dman=false -Dgtk_doc=false \
		-Ddocs=false -Dtests=false -Dinstalled_tests=false -Dgio_sniffing=false -Drelocatable=false

	# The same release as the ports HarfBuzz, rebuilt: the ports (CMake) objects reference
	# __gxx_personality_v0, so every C program linking them would need libstdc++ (whose
	# hypotf stub then collides with libphoenix libm). HarfBuzz's meson build is
	# -fno-exceptions/-fno-rtti and links from C with no C++ runtime; it also gives hb-glib.
	_meson_pkg harfbuzz harfbuzz-build -Dfreetype=enabled -Dglib=enabled -Dgobject=disabled -Dcairo=disabled \
		-Dchafa=disabled -Dpng=disabled -Dzlib=disabled -Dicu=disabled -Dgraphite=disabled -Dgraphite2=disabled \
		-Dfontations=disabled -Dharfrust=disabled -Dkbts=disabled -Dwasm=disabled -Draster=disabled \
		-Dvector=disabled -Dgpu=disabled -Dgpu_demo=disabled -Dsubset=disabled -Dtests=disabled \
		-Dintrospection=disabled -Ddocs=disabled -Ddoc_tests=false -Dutilities=disabled -Dbenchmark=disabled
	local und
	und="$("${TC}-nm" -u "${P}/lib/libharfbuzz.a" | grep -E '__gxx_personality|_Znw|_Zdl|__cxa' || true)"
	[ -z "${und}" ] || { sort -u <<<"${und}" | head; b_die "gtk3_wayland: libharfbuzz.a still needs the C++ runtime"; }

	# cairo 1.18 rebuilt here: the ports cairo 1.16 has no PDF/PS surfaces (GTK's print
	# operation code includes cairo-pdf.h/cairo-ps.h unconditionally) and no cairo-gobject.
	_meson_pkg cairo cairo-build -Dfontconfig=enabled -Dfreetype=enabled -Dpng=enabled -Dzlib=enabled \
		-Dglib=enabled -Dxcb=disabled -Dxlib=disabled -Dxlib-xcb=disabled -Dquartz=disabled -Ddwrite=disabled \
		-Dtee=disabled -Dtests=disabled -Dlzo=disabled -Dgtk2-utils=disabled -Dspectre=disabled \
		-Dsymbol-lookup=disabled -Dgtk_doc=false

	_meson_pkg pango pango-build -Dintrospection=disabled -Dgtk_doc=false -Ddocumentation=false \
		-Dbuild-testsuite=false -Dbuild-examples=false -Dfontconfig=enabled -Dfreetype=enabled -Dcairo=enabled \
		-Dlibthai=disabled -Dxft=disabled -Dsysprof=disabled

	_meson_pkg --cross "${I}/phoenix-aarch64-gtk.cross" gtk gtk-build -Dx11_backend=false -Dwayland_backend=true \
		-Dbroadway_backend=false -Dwin32_backend=false -Dquartz_backend=false -Dxinerama=no -Dcloudproviders=false \
		-Dprofiler=false -Dtracker3=false -Dprint_backends=none -Dcolord=no -Dgtk_doc=false -Dman=false \
		-Dintrospection=false -Ddemos=true -Dexamples=false -Dtests=false -Dinstalled_tests=false \
		-Dbuiltin_immodules=all

	_meson_pkg --cross "${I}/phoenix-aarch64-wl.cross" gtk-layer-shell gtk-layer-shell-build -Dexamples=false \
		-Ddocs=false -Dtests=false -Dintrospection=false -Dvapi=false

	# GSettings schemas (GTK's org.gtk.Settings.*, GLib's): compiled on the host (the
	# format is architecture-independent) for /usr/share/glib-2.0/schemas.
	rm -rf "${I}/data/glib-2.0/schemas"
	mkdir -p "${I}/data/glib-2.0/schemas"
	cp "${P}"/share/glib-2.0/schemas/*.xml "${I}/data/glib-2.0/schemas/"
	glib-compile-schemas --strict "${I}/data/glib-2.0/schemas"

	# --- programs ---
	# gtk3-hello: linked by hand with --gc-sections (meson's links of GTK's own programs
	# keep every section); the whole static closure comes from the installed .pc files.
	local PKGC="${I}/pkg-config-phoenix" BIN="${I}/bin"
	mkdir -p "${BIN}"
	# shellcheck disable=2046 # pkg-config output is a word list
	"${PHXCC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${TFLAGS[@]}" -I"${SYSD}/include" -I"${COMPAT_INC}" \
		$("${PKGC}" --cflags gtk+-3.0 gtk+-wayland-3.0 gtk-layer-shell-0) -c "${F}/src/gtk3-hello.c" -o "${out}/obj/gtk3-hello.o"
	# shellcheck disable=2046
	"${PHXCC}" "${TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,-Map,"${out}/gtk3-hello.map" \
		-o "${BIN}/gtk3-hello" "${out}/obj/gtk3-hello.o" -L"${SYSD}/lib" -Wl,--start-group \
		$("${PKGC}" --libs gtk-layer-shell-0 gtk+-3.0 gtk+-wayland-3.0) -Wl,--end-group >"${out}/gtk3-hello-link.log" 2>&1 ||
		{ grep -v 'warning: .* is not fully supported' "${out}/gtk3-hello-link.log" | head -40; b_die "gtk3_wayland: gtk3-hello: link failed"; }
	"${TC}-strip" -o "${BIN}/gtk3-hello-stripped" "${BIN}/gtk3-hello"
	# GTK's own demos, as meson linked them
	local d b
	for d in gtk-demo/gtk3-demo widget-factory/gtk3-widget-factory; do
		b="$(basename "${d}")"
		cp "${out}/gtk-build/demos/${d}" "${BIN}/${b}"
		"${TC}-strip" -o "${BIN}/${b}-stripped" "${BIN}/${b}"
	done

	# --- verification (the tools build's gate) ---
	local bad=0 o n interp syms x11 s m strs
	for o in gtk3-hello gtk3-demo gtk3-widget-factory; do
		und="$("${TC}-nm" -u "${BIN}/${o}" || true)"
		n=$(grep -c . <<<"${und}" || true)
		interp="$("${TC}-readelf" -l "${BIN}/${o}" | grep -c INTERP || true)"
		syms="$("${TC}-nm" "${BIN}/${o}")"
		x11="$(grep -cE ' (XOpenDisplay|XInternAtom|xcb_connect|gdk_x11_display_get_type|_gdk_broadway_display_open)$' <<<"${syms}" || true)"
		echo "gtk3_wayland: ${o}: nm -u ${n}, PT_INTERP ${interp}, X11/broadway symbols ${x11}; $("${TC}-size" "${BIN}/${o}" | awk 'NR==2 {printf "text %d data %d bss %d", $1, $2, $3}')"
		if [ "${n}" != 0 ] || [ "${interp}" != 0 ] || [ "${x11}" != 0 ]; then head -10 <<<"${und}"; bad=1; fi
		for s in gdk_wayland_display_get_type _gdk_wayland_display_open memfd_create __wrap_close eglGetProcAddress \
			wl_display_connect xkb_keymap_new_from_string g_vfs_get_local pango_cairo_font_map_get_default \
			gdk_pixbuf_new_from_file _gdk_pixbuf__gif_fill_vtable; do
			grep -qE " [TtWw] ${s}\$" <<<"${syms}" || { echo "gtk3_wayland: ${o}: symbol ${s} missing"; bad=1; }
		done
	done
	strs="$(strings -a "${BIN}/gtk3-hello-stripped")"
	n="$(grep -c '/org/gtk/libgtk/theme/Adwaita' <<<"${strs}" || true)"
	m="$(grep -cE ' [Tt] _gtk_register_resource$' <<<"$("${TC}-nm" "${BIN}/gtk3-hello")" || true)"
	echo "gtk3_wayland: gtk3-hello: GTK resource bundle: paths ${n}, _gtk_register_resource ${m}"
	if [ "${n}" = 0 ] || [ "${m}" != 1 ]; then bad=1; fi
	n="$(grep -c 'Using the built-in XKB keymap' <<<"${strs}" || true)"
	m="$(grep -c 'xkb_keymap {' <<<"${strs}" || true)"
	echo "gtk3_wayland: gtk3-hello: built-in XKB keymap message ${n}, keymap text ${m} (GTK patch 0003)"
	if [ "${n}" = 0 ] || [ "${m}" = 0 ]; then bad=1; fi
	n="$(grep -cE ' [Tt] (gtk_layer_init_for_window|gtk_layer_is_supported)$' <<<"$("${TC}-nm" "${BIN}/gtk3-hello")" || true)"
	echo "gtk3_wayland: gtk-layer-shell linked into gtk3-hello: ${n}/2 symbols"
	if [ ! -f "${P}/lib/libgtk-layer-shell.a" ] || [ ! -f "${P}/lib/pkgconfig/gtk-layer-shell-0.pc" ] || [ "${n}" != 2 ]; then bad=1; fi
	for s in 'GTK3HELLO' 'gdk-wayland' 'wayland-0' 'Adwaita' '/shm'; do
		n=$(grep -cF -- "${s}" <<<"${strs}" || true)
		echo "gtk3_wayland: gtk3-hello strings '${s}': ${n}"
		[ "${n}" != 0 ] || bad=1
	done
	{ echo "# gtk3_wayland port build"; (cd "${BIN}" && sha256sum ./*-stripped | sed 's|\./||')
	  (cd "${I}" && sha256sum data/glib-2.0/schemas/gschemas.compiled); } >"${I}/SHA256SUMS"
	cat "${I}/SHA256SUMS"
	# no build path compiled into this build's libraries (the debug info, stripped from
	# the shipped programs, names the work tree by design)
	local a needle
	needle="$(basename "$(dirname "${PREFIX_BUILD%/}")")/$(basename "${PREFIX_BUILD%/}")"
	for a in "${P}"/lib/*.a; do
		"${TC}-strip" --strip-debug -o "${out}/nodebug.a" "${a}"
		if grep -qaF "${needle}" "${out}/nodebug.a"; then
			echo "gtk3_wayland: $(basename "${a}") compiles in a build path: $(grep -ao -- "[[:print:]]*${needle}[[:print:]]*" "${out}/nodebug.a" | head -1)"
			bad=1
		fi
	done
	rm -f "${out}/nodebug.a"
	[ "${bad}" = 0 ] || b_die "gtk3_wayland: verification failed"

	# --- the staging tree: the compiled schemas, the settings and (USE demos) the GTK
	# test programs. The programs above are the build's own link check; they are not
	# part of the image. ---
	local ST="${I}/stage"
	rm -rf "${ST}"
	if b_use demos; then
		for o in gtk3-hello gtk3-demo gtk3-widget-factory; do
			install -D -m 755 "${BIN}/${o}-stripped" "${ST}/bin/${o}"
		done
	fi
	install -D -m 644 "${I}/data/glib-2.0/schemas/gschemas.compiled" "${ST}/usr/share/glib-2.0/schemas/gschemas.compiled"
	install -D -m 644 "${F}/conf/settings.ini" "${ST}/etc/xdg/gtk-3.0/settings.ini"
	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "gtk3_wayland: staged $(wc -l <"${I}/stage.MANIFEST") file(s) into ${PREFIX_FS}/root"
	fi
}
