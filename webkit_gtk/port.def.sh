#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="webkit_gtk"
	version="2.54.0"
	desc="WebKitGTK 2.54 desktop web browser with tabs and downloads: /usr/bin/webkit-browser (one static multi-call program), the XFCE menu entry"

	# WebKitGTK (PORT=GTK, GTK 3, the Wayland target only) cross-built STATIC as ONE program,
	# webkit-browser: the UI process (WebKit's MiniBrowser/gtk window -- tabs, downloads bar, find,
	# zoom -- with our own main), the WebProcess and the NetworkProcess are the same ELF,
	# dispatched on WPE_PHOENIX_PROCESS_ROLE, exactly as webkit_wpe's wpe-browser (coordination repo
	# docs/browser/B10-WEBKITGTK.md explains the choice, the configuration and the Pi gate).
	#
	#   patches/webkit/, webkit-video/, webkit-mse/   SYMLINKS to webkit_wpe's patches: OS(PHOENIX)
	#                     for WTF/JavaScriptCore/mimalloc, the multi-call processes, shared memory,
	#                     the JIT, ANGLE, the dma-buf frame export, the FFmpeg media player, native
	#                     HLS, MSE, zero-copy video. The same release (2.54.0) as the WPE tarball;
	#                     files/build-gtk.sh applies them with the WPE-only paths excluded
	#                     (WPEPlatform/, UIProcess/wpe/, *PlatformWPE.cmake, OptionsWPE.cmake: not in
	#                     this tarball) and fails when webkit_wpe has a patch not linked here
	#   patches/webkit-gtk/0101       the GTK side of 0006/0008: OpenSSL instead of libgcrypt +
	#                                 libtasn1, static libwebkit2gtk, JavaScriptCore as an OBJECT
	#                                 library, WTF's Phoenix sources, the multi-call executable path
	#   patches/webkit-gtk/0102       MiniBrowser/gtk: build-time hooks for the start URL and the
	#                                 location entry's text-to-URI rule
	#   patches/webkit-gtk/0103       GTK 3 GLib API build fixes (a missing include, a libdrm-only
	#                                 call in webkit://gpu); not Phoenix-specific
	#   patches/webkit-gtk/0104       DMABufBuffer without GBM: the GTK UI process imports the web
	#                                 process's dma-buf frames as EGLImages (USE_GBM=OFF)
	#   patches/webkit-gtk/0105       the GTK UI process's paint watch: where each web-process frame
	#                                 spends its time between arriving and GDK's swap (the UI half
	#                                 of shared patch 0020 for GTK; webkit-browser --present-stats)
	#   patches/webkit-gtk/0106       one frame ahead (opt-in, webkit-browser --frame-ahead): FrameDone
	#                                 when a frame is received, so the web process renders the next
	#                                 frame while GTK paints this one (the GTK side of WPE's 0019)
	#   patches/webkit-gtk/0107       opaque frames (opt-in, webkit-browser --opaque-frames): an
	#                                 opaque view's dma-buf frames imported as XB24, so GDK draws
	#                                 them without uploading and blending the window below the view
	#   patches/webkit-gtk-video/0130 USE video only: USE_FFMPEG for PORT=GTK (0030's CMake side)
	#   files/build-gtk.sh            the build (build-wpe.sh's stages for PORT=GTK)
	#   files/launcher/               the program: role dispatch, desktop defaults, downloads,
	#                                 MiniBrowser's window sources
	#   files/share/                  the .desktop entry, the start page
	#   files/checks/                 the B10 gate page (USE checks)
	#   files/cmake/, files/compat/   symlinks to webkit_wpe's
	#
	# The build is as big as webkit_wpe's (~2900 objects; 50 min to ~2 h at -j8 cold) and ccache
	# shares nothing with it (PORT changes cmakeconfig.h, which every object includes). Its build
	# directory is ${PREFIX_BUILD}/webkit_gtk-build, outside the framework's work directory, fed
	# by content as webkit_wpe's (see that port's notes).
	source="https://webkitgtk.org/releases/"
	archive_filename="webkitgtk-${version}.tar.xz"
	src_path="webkitgtk-${version}/"

	size="49710944"
	sha256="846fd19ccedbae1dbfe904f26dbf2d68a800a33a50caf2ad5222c8dcb3f25682"

	# WebKit: WebCore/JavaScriptCore LGPL-2.0/2.1-or-later and BSD-2-Clause (Apple, Igalia: also
	# MiniBrowser/gtk), bundled third-party code under their own permissive licenses; files/:
	# BSD-3-Clause (ours).
	license="LGPL-2.1-or-later AND BSD-2-Clause AND BSD-3-Clause"
	license_file="Source/WebCore/LICENSE-LGPL-2.1"

	# As webkit_wpe: GLib/GIO is gtk3_wayland's 2.88, never the ports glib2 2.56.
	conflicts="glib2>=0.0"
	# webkit_wpe's dependencies, plus GTK 3 itself (gtk3_wayland: GTK, GDK, pango, cairo, atk,
	# gdk-pixbuf, fribidi) and Mesa's wayland variant: the UI process composites each page in
	# GDK's EGL context on the Wayland platform (the web process uses the surfaceless one).
	depends="gtk3_wayland webkit_deps icu harfbuzz_icu openssl libepoxy mesa_drm[wayland] wayland_phoenix video? ( video_player )"

	# rootfs: copy the staging tree (stage/) into the image rootfs.
	# jit webgl video mse release_log: as webkit_wpe's (JavaScriptCore's JIT tiers, WebGL through
	#      ANGLE, <video>/<audio> over FFmpeg with HEVC on rpivid, Media Source Extensions, WebKit's
	#      RELEASE_LOG). Each toggles a near-full WebKit rebuild.
	# checks: also stage the B10 gate page, /usr/share/webkit-browser/checks/b10.html (drop-downs,
	#      pickers, dialogs, a self-contained 1 MiB download).
	iuse="rootfs checks jit release_log webgl video mse"

	supports="phoenix>=3.3"
}

# Install layout (${PREFIX_PORT_INSTALL}):
#   bin/webkit-browser       unstripped (addr2line the pc of a fault dump here)
#   stage/ + stage.MANIFEST  the files for the target rootfs:
#     /usr/bin/webkit-browser                                              the program (stripped)
#     /usr/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so   dlopen()ed by every WebProcess
#     /usr/share/applications/webkit-browser.desktop                       XFCE menu: Internet
#     /usr/share/webkit-browser/start.html                                 the start page
#     USE checks: /usr/share/webkit-browser/checks/b10.html                the B10 gate page
#   SHA256SUMS
#
# Host tools: as webkit_wpe.

_wk_out() { echo "${PREFIX_BUILD%/}/webkit_gtk-build"; }

# The libraries every port links changed (port_manager "relinking"): build-gtk.sh hashes its
# link inputs and relinks by itself; dropping the program here makes that explicit.
p_relink() {
	rm -f "$(_wk_out)/webkit-build/bin/webkit-browser"
}

# the build script's view of the USE flags
_wk_env() {
	echo "PHX_WK_JIT=$(b_use jit && echo 1 || echo 0)"
	echo "PHX_WK_RELEASE_LOG=$(b_use release_log && echo 1 || echo 0)"
	echo "PHX_WK_WEBGL=$(b_use webgl && echo 1 || echo 0)"
	echo "PHX_WK_VIDEO=$(b_use video && echo 1 || echo 0)"
	echo "PHX_WK_MSE=$(b_use mse && echo 1 || echo 0)"
}

p_prepare() {
	local t
	for t in cmake ninja perl python3 gperf gcc git curl rsync pkg-config wayland-scanner glib-mkenums \
		glib-compile-resources gdbus-codegen; do
		command -v "${t}" >/dev/null || b_die "webkit_gtk: host tool ${t} not found"
	done
	if b_use mse && ! b_use video; then
		b_die "webkit_gtk: USE mse needs USE video"
	fi
	# the patch series onto the framework's extracted tarball (with the WPE-only exclusions; the
	# framework's own b_port_apply_patches cannot exclude paths)
	env $(_wk_env) WEBKIT_SRC="${PREFIX_PORT_WORKDIR%/}" PHX_TREE="${PREFIX_BUILD%/}" PHX_TC=unused \
		"${PREFIX_PORT}/files/build-gtk.sh" --out "$(_wk_out)" --stage patch || b_die "webkit_gtk: patching failed"
}

p_build() {
	local F="${PREFIX_PORT}/files" I="${PREFIX_PORT_INSTALL%/}" out TC n d
	out="$(_wk_out)"
	# shellcheck disable=SC2153 # CROSS: the framework environment (aarch64-phoenix-)
	TC="$(dirname "$(command -v "${CROSS}gcc")")/${CROSS%-}"
	local -A dep
	for n in gtk3_wayland webkit_deps icu openssl libepoxy mesa_drm wayland_phoenix; do
		d="$(b_dependency_dir "${n}")" || b_die "webkit_gtk: dependency ${n} missing"
		dep[${n}]="${d%/}"
	done
	local ffmpeg=""
	if b_use video; then
		d="$(b_dependency_dir video_player)" || b_die "webkit_gtk: dependency video_player missing (USE video)"
		ffmpeg="${d%/}/ffmpeg"
		[ -f "${ffmpeg}/lib/libavcodec.a" ] || b_die "webkit_gtk: ${ffmpeg}: no FFmpeg libraries (video_player installs them)"
	fi

	env $(_wk_env) PHX_TREE="${PREFIX_BUILD%/}" PHX_TC="${TC}" \
		PHX_GTK="${dep[gtk3_wayland]}" PHX_WEBKIT_DEPS="${dep[webkit_deps]}" PHX_ICU_PREFIX="${dep[icu]}" \
		PHX_OPENSSL="${dep[openssl]}" PHX_EPOXY="${dep[libepoxy]}" PHX_MESA="${dep[mesa_drm]}" \
		PHX_WAYLAND="${dep[wayland_phoenix]}" WEBKIT_SRC="${PREFIX_PORT_WORKDIR%/}" PHX_FFMPEG="${ffmpeg}" \
		"${F}/build-gtk.sh" --out "${out}" --dl "${PHOENIX_DISTFILES:-${HOME}/.phoenix-distfiles}/newlane" \
		--src-copy -j 8 || b_die "webkit_gtk: build-gtk.sh failed"

	local ST="${I}/stage" S="${F}/share"
	rm -rf "${ST}"
	mkdir -p "${I}/bin"
	cp -f "${out}/webkit-browser" "${I}/bin/webkit-browser"
	install -D -m 755 "${out}/webkit-browser-stripped" "${ST}/usr/bin/webkit-browser"
	install -D -m 755 "${out}/libwebkit2gtkinjectedbundle.so" \
		"${ST}/usr/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so"
	install -D -m 644 "${S}/webkit-browser.desktop" "${ST}/usr/share/applications/webkit-browser.desktop"
	install -D -m 644 "${S}/start.html" "${ST}/usr/share/webkit-browser/start.html"
	if b_use checks; then
		install -D -m 644 "${F}/checks/b10.html" "${ST}/usr/share/webkit-browser/checks/b10.html"
	fi

	# what the stage must hold: the program with its log lines and export table, the bundle
	local bad=0 s
	for s in 'WKGB t=%.0f %s' 'download finished uri=%s' 'ui window shown gdk_gl=%s' 'WPEB-WEBKIT swap-chain pid=%d' \
		'WPEB-WEBKIT process-model' 'Disabled hardware acceleration because GTK failed to initialize GL' 'gdk-gl ok use_es=%d version=%d.%d' 'egl-probe platform_wayland=%d' 'egl-early wayland=1 client_ext=' 'b10-r6' \
		'gtk-paint %s' 'frame-watch-web pid=%d' 'frame-watch-ui %s' 'WPEB-WEBKIT gtk-paint import pid=%d' 'WPEB-WEBKIT frame-pacing pid=%d ahead=%d opaque=%d'; do
		grep -qaF "${s}" "${ST}/usr/bin/webkit-browser" || { echo "webkit_gtk: webkit-browser lacks '${s}'"; bad=1; }
	done
	"${TC}-readelf" -dW "${ST}/usr/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so" | grep -q '(HASH)' ||
		{ echo "webkit_gtk: the injected bundle has no DT_HASH"; bad=1; }
	[ "${bad}" = 0 ] || b_die "webkit_gtk: stage verification failed"

	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs -r sha256sum) >"${I}/stage.MANIFEST"
	sha256sum "${I}/bin/webkit-browser" "${ST}/usr/bin/webkit-browser" | sed "s|${I}/||" >"${I}/SHA256SUMS"
	echo "webkit_gtk: stage: $(wc -l <"${I}/stage.MANIFEST") file(s), webkit-browser $(stat -c %s "${ST}/usr/bin/webkit-browser") bytes"
	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "webkit_gtk: staged into ${PREFIX_FS}/root"
	fi
}
