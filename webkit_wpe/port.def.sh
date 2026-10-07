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
	#   patches/webkit/0012-0014  JavaScriptCore's JIT on Phoenix (track C's webkit-jit series,
	#                             browser B9): the SA_SIGINFO machine context, concurrent GC, a
	#                             32 MiB RWX executable pool, the EL0 cache flush, WebAssembly off
	#                             at run time; harmless to the LLInt-only build, which they also
	#                             give the real register context for its concurrent collector
	#   patches/webkit/0015       the process model is the launcher's to choose: process swap,
	#                             prewarming, the WebProcess cache's size (WPE_PHOENIX_*)
	#   patches/webkit/0016       frames to the compositor as dma-bufs without GBM, opt-in
	#                             (WPE_PHOENIX_DMABUF=1, wpe-browser --dmabuf); no fence
	#                             descriptors across processes
	#   patches/webkit/0017       ANGLE (WebGL) on Phoenix: platform, TLS, mutex, dlfcn, and the linked
	#                             Mesa EGL instead of dlopen()ing libEGL.so.1 (compiled only
	#                             with USE webgl)
	#   patches/webkit/0018       WPEPlatform Wayland: the event source never prepares a read twice
	#                             (a skipped check() left the UI waiting on itself)
	#   patches/webkit/0019       frame pacing one frame ahead, opt-in (WPE_PHOENIX_FRAME_AHEAD=1,
	#                             wpe-browser --frame-ahead): the UI process answers a frame when it
	#                             is committed, not at the compositor's frame callback
	#   patches/webkit/0020       the frame watch: where each process's frame pipeline stands, for
	#                             the launcher's frame-stall reports (a page that stops drawing
	#                             while every main loop runs)
	#   patches/webkit/0021       WTF: ThreadCondition::timedWait waits on the monotonic clock
	#                             (libphoenix condvars default to CLOCK_MONOTONIC: every WTF
	#                             timed wait lasted until a signal)
	#   patches/webkit/0022       diagnostic: the wait trace (WPE_PHOENIX_WAIT_TRACE: every WTF wait
	#                             of 500 ms or more, its thread and return addresses) and the web
	#                             process's display refreshes in the frame watch (kind no-refresh),
	#                             to name the ~4 s requestAnimationFrame pauses of build 36's B7
	#   patches/webkit/0023       WPEPlatform Wayland: a buffer rendered from inside buffer-rendered
	#                             keeps its frame callback (with 0019 the view stalled for good
	#                             once a frame was held: GPU raster + <video>, build 42)
	#   patches/webkit/0024       diagnostic: the web process's memory pressure monitor logs its
	#                             limits, policy changes and releases ("WPEB-MEMPRESSURE"; the
	#                             build has no release logging)
	#   patches/webkit-video/0030 USE video only: <video>/<audio> without GStreamer, a WebCore
	#                             media player over FFmpeg's libraries (USE_FFMPEG; HEVC on the
	#                             Pi 4's rpivid block through video_player's hevc_rpivid decoder).
	#                             Kept apart so that a build without video has the same tree
	#                             (its new CMake option would change cmakeconfig.h: a full rebuild)
	#   patches/webkit-video/0031 USE video only: native HLS in that player (<video src=*.m3u8>:
	#                             FFmpeg's hls demuxer, every playlist/segment/key through WebKit's
	#                             loader, our variant choice -- HEVC 8-bit <= 1080p30 for rpivid,
	#                             else H.264 <= 720p30 --, AES-128, live and VOD, lazy decoders) and
	#                             MediaCapabilities.decodingInfo() answers (coordination repo
	#                             docs/browser/MSE-DESIGN.md stage 0); needs video_player's hls
	#                             demuxer and its files/hls hunks
	#   patches/webkit-video/0033 USE video only: zero-copy HEVC frames (hevc_rpivid's rpivid_out=drm_prime:
	#                             the block's V3D BOs as AV_PIX_FMT_DRM_PRIME frames, imported once per
	#                             buffer as EGLImages and drawn through GL_TEXTURE_EXTERNAL_OES; Mesa
	#                             de-tiles on the GPU); needs video_player's librpivid_bo_drm.a (else
	#                             it compiles to the upload path); WPE_PHOENIX_MEDIA_ZERO_COPY=0: off
	#                             (coordination repo docs/gpu-new-lane/M10b-video-zero-copy.md)
	#   patches/webkit-mse/0032   USE mse only (needs USE video): Media Source Extensions over the same
	#                             FFmpeg decoders (ENABLE_MEDIA_SOURCE: an MSE engine, MediaSource and
	#                             SourceBuffer backends, a fragmented-MP4 parser; type answers that
	#                             steer adaptive players to HEVC and H.264 <= 720p; MSE-DESIGN.md
	#                             stage 1). Kept apart from webkit-video: turning MSE on changes
	#                             cmakeconfig.h, so a video build without it keeps its tree
	#   patches/webkit-mse/0034   USE mse only: the MSE engine asks for 0033's zero-copy frames too
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
	# video_player (USE video): its ffmpeg/ prefix, FFmpeg 6.1 with the hevc_rpivid decoder.
	depends="gtk3_wayland webkit_deps icu harfbuzz_icu openssl libepoxy mesa_drm wayland_phoenix video? ( video_player )"

	# rootfs: copy the staging tree (stage/) into the image rootfs.
	# checks: also stage the Pi checks (B4 page, probe extension, B6 site list and scripts, the B7
	#      WebGL and animation pages and script).
	# jit: build JavaScriptCore's JIT tiers (Baseline, DFG, FTL, regexp; B9) instead of the LLInt
	#      only. Off: the B5/B6 interpreter build. A JIT build still runs on the LLInt with
	#      JSC_useJIT=false in the environment. Toggling it reconfigures and rebuilds most of
	#      WebKit (~2 h without ccache).
	# release_log: WebKit's RELEASE_LOG compiled in (WEBKIT_DEBUG=ProcessSwapping,Process,Loading
	#      etc. print to stderr). Off: compiled out, as in any Release build. Toggling it rebuilds
	#      nearly all of WebKit (~2 h without ccache).
	# webgl: WebGL (ENABLE_WEBGL: WebKit's bundled ANGLE on Mesa's GLES 3.1, patch 0017;
	#      wpe-browser --webgl turns it on per run). Off: no WebGL, no ANGLE. Toggling it rebuilds
	#      nearly all of WebKit, and ANGLE on top.
	# video: <video> and <audio> (coordination repo docs/browser/B8-video.md): ENABLE_VIDEO with the
	#      FFmpeg media player of patch 0030 (no GStreamer, no Media Source Extensions, no Web
	#      Audio), sound on /dev/audio0. Off: no media, as before. Toggling it rebuilds nearly all
	#      of WebKit (~2 h without ccache hits), as release_log.
	# mse: Media Source Extensions (needs video): ENABLE_MEDIA_SOURCE with patch 0032's FFmpeg MSE
	#      engine (hls.js, dash.js, Shaka, video.js-VHS players; wpe-browser --mse=on|managed|off).
	#      Off: native HLS and plain files only. Toggling it rebuilds nearly all of WebKit (~2 h).
	iuse="rootfs checks jit release_log webgl video mse"

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
#     USE checks: /usr/share/wpe-browser/{b4.html,b6-*,b7*}, /usr/lib/wpe-browser/pi-extensions/
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
	if b_use video; then
		b_port_apply_patches "${PREFIX_PORT_WORKDIR}" webkit-video
	fi
	if b_use mse; then
		b_use video || b_die "webkit_wpe: USE mse needs USE video"
		b_port_apply_patches "${PREFIX_PORT_WORKDIR}" webkit-mse
	fi
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
	local video=0 ffmpeg=""
	if b_use video; then
		video=1
		d="$(b_dependency_dir video_player)" || b_die "webkit_wpe: dependency video_player missing (USE video)"
		ffmpeg="${d%/}/ffmpeg"
		[ -f "${ffmpeg}/lib/libavcodec.a" ] || b_die "webkit_wpe: ${ffmpeg}: no FFmpeg libraries (video_player installs them)"
	fi

	PHX_TREE="${PREFIX_BUILD%/}" PHX_TC="${TC}" \
		PHX_GTK="${dep[gtk3_wayland]}" PHX_WEBKIT_DEPS="${dep[webkit_deps]}" PHX_ICU_PREFIX="${dep[icu]}" \
		PHX_OPENSSL="${dep[openssl]}" PHX_EPOXY="${dep[libepoxy]}" PHX_MESA="${dep[mesa_drm]}" \
		PHX_WAYLAND="${dep[wayland_phoenix]}" WEBKIT_SRC="${PREFIX_PORT_WORKDIR%/}" \
		PHX_WPE_JIT="$(b_use jit && echo 1 || echo 0)" \
		PHX_WPE_RELEASE_LOG="$(b_use release_log && echo 1 || echo 0)" \
		PHX_WPE_WEBGL="$(b_use webgl && echo 1 || echo 0)" \
		PHX_WPE_VIDEO="${video}" PHX_FFMPEG="${ffmpeg}" PHX_WPE_MSE="$(b_use mse && echo 1 || echo 0)" \
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
		for n in b4.html b6.sh b6-sites.txt b6-newwin.html b7.sh b7-webgl.html b7-anim.html; do
			install -D -m 644 "${C}/${n}" "${ST}/usr/share/wpe-browser/${n}"
		done
		if [ "${video}" = 1 ]; then
			for n in b8.html b8.sh b8-stream.sh; do
				install -D -m 644 "${C}/${n}" "${ST}/usr/share/wpe-browser/${n}"
			done
		fi
		install -D -m 755 "${out}/phx-probe-extension.so" "${ST}/usr/lib/wpe-browser/pi-extensions/phx-probe-extension.so"
	fi

	# what the stage must hold: the program with its export table and the B6 shell, the bundle
	local bad=0 s
	for s in 'WPEB t=%.0f ' 'chrome action=go source=%s' 'session persistent data=%s' 'wpeBrowserChrome' \
		'hang-recovery terminate-web-process' 'stall-sample tid=%d' 'WPEB-WEBKIT process-model' 'chrome mode=%s' \
		'gpu raster=%s transport=%s webgl=%s frame-ahead=%d' 'WPEB-WEBKIT swap-chain' 'WPEB-WEBKIT dmabuf-export' \
		'WPEB-WEBKIT frame-pacing' 'frame-watch kind=%s compositor state=%s' 'frame-watch kind=%s backing-store' '%s n=%u report=%u %s' \
		'WPEB-WEBKIT wait-trace pid=%d' ' display-link on=%d link_ms=%lld'; do
		grep -qaF "${s}" "${ST}/usr/bin/wpe-browser" || { echo "webkit_wpe: wpe-browser lacks '${s}'"; bad=1; }
	done
	if [ "${video}" = 1 ]; then
		for s in 'WPEB-MEDIA mono=%llu id=%u %s' 'rpivid: hardware HEVC decode' 'media autoplay=%s' 'hls choose i=%d rule=%s audio=%s' \
			'canplaytype type=%s platform=%s answer=%s' 'capabilities type=%s codec=%s'; do
			grep -qaF "${s}" "${ST}/usr/bin/wpe-browser" || { echo "webkit_wpe: wpe-browser (USE video) lacks '${s}'"; bad=1; }
		done
	fi
	if b_use mse; then
		for s in 'mse append bytes=%zu samples=%u' 'mse init tracks=%zu video=%s' 'media mse=%s managed=%s'; do
			grep -qaF "${s}" "${ST}/usr/bin/wpe-browser" || { echo "webkit_wpe: wpe-browser (USE mse) lacks '${s}'"; bad=1; }
		done
	fi
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
