#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="webkit_wpe"
	version="2.54.0"
	desc="WPE WebKit 2.54 web browser: /usr/bin/wpe-browser (one static multi-call program), /bin/browser, the XFCE menu entry"

	# WPE WebKit (PORT=WPE, the WPEPlatform API: Wayland + headless displays) cross-built STATIC
	# as ONE program, wpe-browser: the UI process, the WebProcess and the NetworkProcess are the
	# same ELF, dispatched on WPE_PHOENIX_PROCESS_ROLE (coordination repo docs/browser/PLAN.md,
	# decision 2; tools/browser/wpe/README.md explains the configuration and the Pi checks).
	#
	#   patches/webkit/0001-0005  OS(PHOENIX) for WTF, JavaScriptCore (LLInt, no JIT) and mimalloc
	#                             (the browser plan's track C, the jsc shell)
	#   patches/webkit/0006-0011  WPE on Phoenix: OpenSSL for PAL's digests, a static libWebKit,
	#                             the multi-call process lookup, SharedMemory and wl_shm over
	#                             shmsrv (memfd_create), WTF's platform sources, build fixes,
	#                             the disk cache's files written with write() (no write-back
	#                             of file mappings on Phoenix)
	#   patches/webkit/0015       the process model is the launcher's to choose: process swap,
	#                             prewarming, the WebProcess cache's size (WPE_PHOENIX_*)
	#   files/build-wpe.sh        the build (also run by tools/browser/wpe/build.sh for scratch
	#                             builds): host ruby if missing, a private dependency prefix,
	#                             the libphoenix compat objects, CMake + ninja, the link checks
	#   files/launcher/           the program: role dispatch, the browser shell (address bar
	#                             overlay, navigation, persistent session, test knobs)
	#   files/share/              /bin/browser, the .desktop entry, the start page
	#   files/checks/             the Pi checks (USE checks): the B4 page, a web process
	#                             extension, the B6 site list and scripts
	#
	# The build is big: ~8500 ninja steps, ~2 h at -j8 on the 16-thread / 29 GB build host, and
	# ~15 GB of objects. So its build directory is NOT the framework's per-port work directory,
	# which a clean removes (a dependency's recipe change cleans every port that depends on it):
	# it is ${PREFIX_BUILD}/webkit_wpe-build, and every input reaches it by content -- the
	# patched source tree (rsync -c, so an unchanged re-extracted file keeps its mtime), the
	# dependency prefix (the same), the static link closure (a hash: any changed archive relinks
	# the program). ninja then rebuilds exactly what changed; a dependency rebuilt to the same
	# headers costs a relink (~1 min). Removing that directory forces a full build. When the
	# host has ccache it is used (PHX_CCACHE=0 turns it off): `ccache -M 20G` holds one build.
	#
	# Memory: an image build (rebuild-rpi4b-fast.sh) holds the host's heavy-build lock for its
	# whole run, so this compile never runs beside a scratch WebKit build (those go through
	# scripts/heavy-build.sh); -j is min(8, MemAvailable / 2 GB).
	source="https://wpewebkit.org/releases/"
	archive_filename="wpewebkit-${version}.tar.xz"
	src_path="wpewebkit-${version}/"

	size="46202080"
	sha256="efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452"

	# WebKit: WebCore/JavaScriptCore LGPL-2.0/2.1-or-later and BSD-2-Clause (Apple), bundled
	# third-party code under their own permissive licenses (Skia BSD-3, mimalloc MIT, ...);
	# files/: BSD-3-Clause (ours).
	license="LGPL-2.1-or-later AND BSD-2-Clause AND BSD-3-Clause"
	license_file="Source/WebCore/LICENSE-LGPL-2.1"

	# As webkit_deps: GLib/GIO is gtk3_wayland's 2.88, never the ports glib2 2.56 (and the
	# private versioned-ports prefix).
	conflicts="glib2>=0.0"
	# gtk3_wayland: GLib 2.88 + its private views (zlib, libffi, libpng, libjpeg, freetype,
	# fontconfig, expat); webkit_deps: libsoup 3 + glib-networking (OpenSSL), psl, nghttp2,
	# brotli, woff2, webp, xml2, xslt, sqlite3; icu + harfbuzz_icu: ICU 78 and hb-icu;
	# openssl: PAL's digests and the TLS backend; libepoxy + mesa_drm: EGL/GLES (the WebProcess
	# composites with GLES even for CPU raster); wayland_phoenix: Wayland, xkbcommon, the
	# memfd_create()-over-shmsrv compat library.
	depends="gtk3_wayland webkit_deps icu harfbuzz_icu openssl libepoxy mesa_drm wayland_phoenix"

	# rootfs: copy the staging tree (stage/) into the image rootfs.
	# checks: also stage the Pi checks (B4 page, probe extension, B6 site list and scripts).
	# release_log: WebKit's RELEASE_LOG compiled in (WEBKIT_DEBUG=ProcessSwapping,Process,Loading
	#      etc. print to stderr). Off: compiled out, as in any Release build. Toggling it rebuilds
	#      nearly all of WebKit (~2 h without ccache).
	iuse="rootfs checks release_log"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   bin/wpe-browser          unstripped (addr2line the pc of a fault dump here)
#   stage/ + stage.MANIFEST  the files for the target rootfs:
#     /usr/bin/wpe-browser                                          the program (stripped)
#     /usr/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so   dlopen()ed by every WebProcess
#     /bin/browser                                                  the desktop launcher (bash)
#     /usr/share/applications/wpe-browser.desktop                   XFCE menu: Internet
#     /usr/share/wpe-browser/start.html                             the start page
#     USE checks: /usr/share/wpe-browser/{b4.html,b6-*}, /usr/lib/wpe-browser/pi-extensions/
#   SHA256SUMS
#
# Host tools: cmake, ninja, perl, python3, gperf, gcc, git, curl, rsync, pkg-config,
# wayland-scanner, glib-mkenums, glib-compile-resources, gdbus-codegen; ruby (built from
# source into the build directory when the host has none).

_wpe_out() { echo "${PREFIX_BUILD%/}/webkit_wpe-build"; }

# The libraries every port links changed (port_manager "relinking"): build-wpe.sh hashes its
# link inputs and relinks by itself; dropping the program here makes that explicit.
p_relink() {
	rm -f "$(_wpe_out)/webkit-build/bin/wpe-browser"
}

p_prepare() {
	local t
	for t in cmake ninja perl python3 gperf gcc git curl rsync pkg-config wayland-scanner glib-mkenums \
		glib-compile-resources gdbus-codegen; do
		command -v "${t}" >/dev/null || b_die "webkit_wpe: host tool ${t} not found"
	done
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}" webkit
}

p_build() {
	local F="${PREFIX_PORT}/files" I="${PREFIX_PORT_INSTALL%/}" out TC n d
	out="$(_wpe_out)"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	local -A dep
	for n in gtk3_wayland webkit_deps icu openssl libepoxy mesa_drm wayland_phoenix; do
		d="$(b_dependency_dir "${n}")" || b_die "webkit_wpe: dependency ${n} missing"
		dep[${n}]="${d%/}"
	done

	PHX_TREE="${PREFIX_BUILD%/}" PHX_TC="${TC}" \
		PHX_GTK="${dep[gtk3_wayland]}" PHX_WEBKIT_DEPS="${dep[webkit_deps]}" PHX_ICU_PREFIX="${dep[icu]}" \
		PHX_OPENSSL="${dep[openssl]}" PHX_EPOXY="${dep[libepoxy]}" PHX_MESA="${dep[mesa_drm]}" \
		PHX_WAYLAND="${dep[wayland_phoenix]}" WEBKIT_SRC="${PREFIX_PORT_WORKDIR%/}" \
		PHX_WPE_RELEASE_LOG="$(b_use release_log && echo 1 || echo 0)" \
		"${F}/build-wpe.sh" --out "${out}" --dl "${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane" \
		--src-copy -j 8 || b_die "webkit_wpe: build-wpe.sh failed"

	local ST="${I}/stage" S="${F}/share" C="${F}/checks"
	rm -rf "${ST}"
	mkdir -p "${I}/bin"
	cp -f "${out}/wpe-browser" "${I}/bin/wpe-browser"
	install -D -m 755 "${out}/wpe-browser-stripped" "${ST}/usr/bin/wpe-browser"
	install -D -m 755 "${out}/libWPEInjectedBundle.so" "${ST}/usr/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
	install -D -m 755 "${S}/browser" "${ST}/bin/browser"
	install -D -m 644 "${S}/wpe-browser.desktop" "${ST}/usr/share/applications/wpe-browser.desktop"
	install -D -m 644 "${S}/start.html" "${ST}/usr/share/wpe-browser/start.html"
	if b_use checks; then
		for n in b4.html b6.sh b6-sites.txt b6-newwin.html; do
			install -D -m 644 "${C}/${n}" "${ST}/usr/share/wpe-browser/${n}"
		done
		install -D -m 755 "${out}/phx-probe-extension.so" "${ST}/usr/lib/wpe-browser/pi-extensions/phx-probe-extension.so"
	fi

	# what the stage must hold: the program with its export table and the B6 shell, the bundle
	local bad=0 s
	for s in 'WPEB t=%.0f ' 'chrome action=go source=%s' 'session persistent data=%s' 'wpeBrowserChrome' \
		'hang-recovery terminate-web-process' 'stall-sample tid=%d' 'WPEB-WEBKIT process-model' 'chrome mode=%s'; do
		grep -qaF "${s}" "${ST}/usr/bin/wpe-browser" || { echo "webkit_wpe: wpe-browser lacks '${s}'"; bad=1; }
	done
	"${TC}-readelf" -dW "${ST}/usr/lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so" | grep -q '(HASH)' ||
		{ echo "webkit_wpe: the injected bundle has no DT_HASH"; bad=1; }
	[ "${bad}" = 0 ] || b_die "webkit_wpe: stage verification failed"

	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs -r sha256sum) >"${I}/stage.MANIFEST"
	sha256sum "${I}/bin/wpe-browser" "${ST}/usr/bin/wpe-browser" | sed "s|${I}/||" >"${I}/SHA256SUMS"
	echo "webkit_wpe: stage: $(wc -l <"${I}/stage.MANIFEST") file(s), wpe-browser $(stat -c %s "${ST}/usr/bin/wpe-browser") bytes"
	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "webkit_wpe: staged into ${PREFIX_FS}/root"
	fi
}
