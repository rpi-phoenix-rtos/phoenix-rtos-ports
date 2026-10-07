#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="video_player"
	version="6.1"
	desc="Video players: ffplay (FFmpeg 6.1, SDL KMSDRM + Wayland), /bin/video-play, gtk-video (GTK 3); HEVC on the Pi 4's rpivid block"

	# The ffmpeg port's release tarball (same archive, same sha256). This port builds its
	# own copy with the PLAYER component set (+ libavfilter, libswscale, libswresample):
	# the ffmpeg port stays the decode-only library port it is.
	source="https://ffmpeg.org/releases/"
	archive_filename="ffmpeg-${version}.tar.gz"
	src_path="ffmpeg-${version}/"

	size="15802195"
	sha256="938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316"

	# FFmpeg configured without --enable-gpl / --enable-nonfree (fftools/ffplay.c,
	# cmdutils.c, opt_common.c and every enabled component: LGPL-2.1-or-later); files/
	# (the glue, gtk-video, the launcher, the clip generator, configuration, the rpivid
	# decoder sources and hevc-rpivid-check): BSD-3-Clause; files/rpivid/patches and
	# files/hls/patches (FFmpeg hunks): LGPL-2.1-or-later as the files they change.
	license="LGPL-2.1-or-later AND BSD-3-Clause"
	license_file="COPYING.LGPLv2.1"

	# NEW GPU LANE: private prefix (nothing here reaches the shared ports prefix).
	conflicts="video_player!=${version}"
	depends="libdrm_phoenix mesa_drm[opengl] sdl2_kmsdrm zlib gtk? ( gtk3_wayland wayland_phoenix )"

	# rootfs   install the staging tree (stage/) into the image rootfs
	# gtk      also gtk-video, the GTK 3 player, and its XFCE menu entry
	# demo     the demo set: the synthetic clips in /usr/share/video-demo (generated at build
	#          time by the HOST ffmpeg, ~61 MB), the video desktop session's labwc configuration
	#          /etc/xdg/labwc-xfce-video/ and a "Video Demo" menu entry
	iuse="rootfs gtk demo"

	supports="phoenix>=3.3"
}

# Ported from the coordination repo's tools/gpu-lane/video-player/ (M10,
# docs/gpu-new-lane/M10-video-player.md): build-ffplay.sh (its --sdl wl link: ONE ffplay with
# SDL's Wayland AND KMSDRM video drivers, sdl2_kmsdrm's link-inputs.txt), gtk-video/build.sh
# and gen-clips.sh. Same configure line, patch, glue, link shape and verification. Copies kept
# identical to the tools files by the coordination repo's scripts/check-gpu-lane-ports-sync.sh:
#   patches/0001            ffplay's opt-in FFPLAY_STATLINE_MS / FFPLAY_AUTOKEYS knobs
#   files/components.sh     the player's FFmpeg component set (LGPL only)
#   files/ffplay_phoenix_glue.c   --wrap=pthread_create: 8 MiB default thread stacks
#   files/gtk-video/gtk-video.c   the GTK 3 player (FFmpeg decode, cairo paint, /dev/audio0)
#   files/gen-clips.sh, files/conf/applications/gtk-video.desktop
# This port's own (derived from the tools' pi/video-play2 and conf/labwc-xfce-m10):
#   files/image/video-play  /bin/video-play: a window under a compositor, else full screen
#   files/image/labwc-xfce-video/ (the video desktop session: the XFCE session's labwc
#   configuration + an autostart that plays a clip), files/image/video-demo.desktop
# and the HEVC hardware decoder (files/rpivid/, port-only: not in tools/gpu-lane):
#   src/         the BCM2711 rpivid block as an hwaccel of FFmpeg's own HEVC decoder that
#                writes system-memory frames: rpivid_hevc.c (the HEVCContext -> block
#                mapping), rpivid_cmd.c (the command buffer, from tools/hevc-decode's proven
#                hevc-m2.c), rpivid_hw.c (MMIO, mailbox clock, IRQ, contiguous memory),
#                rpivid_sand.c (SAND/column-128 -> planar, NEON); copied into libavcodec/
#   patches/     1001: the decoder "hevc_rpivid" = "hevc" + the "rpivid" option (registered
#                first, so avcodec_find_decoder(HEVC) -- ffplay, gtk-video -- picks it); CPU
#                fallback for a stream, a picture or a hardware failure
#   check/       hevc-rpivid-check: hardware vs CPU decode (or a host framemd5) of files or a
#                directory, every frame's md5, timing, one RPIVID-CHECK stream= line per file
#   hosttest/    run.sh: the hwaccel's register programming against the reference player on
#                a register-level mock (host, ASan); not part of the build
# and the HLS demuxer's custom-I/O hunk (files/hls/patches, port-only): 2001 lets hls.c open
#   http(s) segment, key and init-section URLs through a caller's io_open in this
#   --disable-network build (WebKit's media player, webkit_wpe USE video: its loader fetches
#   every playlist and segment); unchanged without AVFMT_FLAG_CUSTOM_IO. 2002: seeking in fMP4
#   playlists (6.1 never resumed after a seek: the mov demuxer's fragment index is keyed by byte
#   position, which the seek restarts at 0) and the target segment's keyframe kept.
# Knobs: FFMPEG_RPIVID=0 (CPU only) | 1 (default: the default tool set) | 2 (every tool), or
# the decoder option -rpivid N; FFMPEG_RPIVID_TOOLS=-amp,+tiles turns single coding tools off
# or on (rpivid_hevc.c tools[]); ffplay -vcodec hevc = the plain CPU decoder.
#
# Installs (${PREFIX_PORT_INSTALL}): bin/ (stripped), prog/ (unstripped, addr2line),
# share/video-player/ (link maps, stage.MANIFEST), ffmpeg/ (the FFmpeg libraries for other
# ports: include/, lib/*.a, lib/pkgconfig/ with prefix-relative paths; webkit_wpe's USE video
# links them; a private prefix, never the shared one), stage/ + stage.MANIFEST (the rootfs files):
#   /usr/bin/ffplay                        ffplay: full screen on KMS from psh, a window on the
#                                          desktop (SDL tries Wayland, then KMSDRM)
#   /usr/bin/hevc-rpivid-check             the rpivid decoder checked against the CPU one
#   /bin/video-play                        picks the mode (SDL_VIDEODRIVER, -fs) by whether a
#                                          compositor socket exists
#   /usr/bin/gtk-video         (gtk)       + /usr/share/applications/gtk-video.desktop
#   /usr/share/video-demo/     (demo)      h264-720p30-aac.mp4, h264-1080p30-aac.mp4,
#                                          hevc-720p30-aac.mp4, vp9-360p-opus.webm;
#                                          /etc/xdg/labwc-xfce-video/{autostart,rc.xml,menu.xml,
#                                          environment}; /usr/share/applications/video-demo.desktop
#
# Relink (libphoenix changed): the framework default is right here -- no link step is
# guarded: p_build compiles the fftools objects and links every program on each run, so
# the deleted programs are always recreated (the configure guard below produces no program).
#
# Host tools: make, and for USE demo an ffmpeg + ffprobe with the libx264, libx265,
# libvpx-vp9, libopus and aac encoders (checked; the clips are data, not reproducible
# byte for byte across host encoder versions).

p_prepare() {
	b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	# the rpivid decoder: its FFmpeg hunks (kept apart from patches/, which mirrors
	# tools/gpu-lane/video-player/patches) and its sources
	PREFIX_PORT_PATCHES="${PREFIX_PORT}/files/rpivid/patches" b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	# the hls demuxer's http(s) URLs over a caller's io_open (WebKit's media player)
	PREFIX_PORT_PATCHES="${PREFIX_PORT}/files/hls/patches" b_port_apply_patches "${PREFIX_PORT_WORKDIR}"
	local f
	for f in "${PREFIX_PORT}"/files/rpivid/src/*.[ch]; do
		cmp -s "${f}" "${PREFIX_PORT_WORKDIR%/}/libavcodec/${f##*/}" || cp "${f}" "${PREFIX_PORT_WORKDIR%/}/libavcodec/"
	done
}

# _vp_fftools <SDL prefix> <object dir>: ffplay.c's objects against that SDL's headers
# (ffmpeg's pattern rule builds them although CONFIG_FFPLAY=no; SDL is never probed by
# configure: its link test would need the whole static Mesa group)
_vp_fftools() {
	local sp="$1" od="$2" o
	rm -f "${VP_FS}"/fftools/*.o
	make -C "${VP_FS}" ECFLAGS="-I${sp}/include/SDL2" fftools/ffplay.o fftools/cmdutils.o fftools/opt_common.o \
		>"${VP_OUT}/ff-fftools-$(basename "${od}").log" 2>&1 ||
		{ tail -40 "${VP_OUT}/ff-fftools-$(basename "${od}").log"; b_die "video_player: fftools compile failed ($(basename "${od}"))"; }
	rm -rf "${od}"
	mkdir -p "${od}"
	for o in ffplay cmdutils opt_common; do
		cp "${VP_FS}/fftools/${o}.o" "${od}/"
	done
}

# _vp_link <name> <object dir> <libSDL2.a> <libgallium.a> <ld flags (one word each)> -- <archives...>
#   the quakespasm-drm shape (tools build-ffplay.sh step 4): C++ driver, -static, --gc-sections,
#   4 KiB pages, libgallium whole-archive, FFmpeg + SDL + Mesa + the rest in one group, the SDL
#   build's --wrap flags, --wrap=pthread_create (the glue) and a 16 MiB main stack
_vp_link() {
	local name="$1" od="$2" sdl="$3" gallium="$4" lflags=() a bin="${VP_OUT}/$1"
	shift 4
	while [ "$#" -gt 0 ] && [ "$1" != -- ]; do lflags+=("$1"); shift; done
	[ "${1:-}" = -- ] || b_die "_vp_link: no -- separator"
	shift
	[ -f "${gallium}" ] || b_die "video_player: missing libgallium (${gallium})"
	for a in "${sdl}" "$@"; do [ -f "${a}" ] || b_die "video_player: missing ${a}"; done
	"${NL_PHXCXX}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 "${lflags[@]}" \
		-Wl,--wrap=pthread_create -Wl,-z,stack-size=16777216 -Wl,-Map,"${bin}.map" -o "${bin}" \
		"${od}/ffplay.o" "${od}/cmdutils.o" "${od}/opt_common.o" "${VP_GLUE_O}" \
		-Wl,--whole-archive "${gallium}" -Wl,--no-whole-archive \
		-Wl,--start-group "${VP_FF_A[@]}" "${sdl}" "$@" -Wl,--end-group -lm \
		>"${VP_OUT}/${name}-link.log" 2>&1 ||
		{ head -60 "${VP_OUT}/${name}-link.log"; b_die "video_player: ${name}: link failed"; }
	"${NL_STRIP}" -o "${bin}.stripped" "${bin}"
}

# _vp_public_configuration <configure args...>: FFMPEG_CONFIGURATION, the configure line
# that `ffplay -buildconf` and every program's banner print, formatted as configure formats
# it but without the build host's paths: a program option keeps the program's name, a flag
# list loses its path-bearing flags (--sysroot, -B, -I, -L)
_vp_public_configuration() {
	local v l r w out=""
	local -a words keep
	for v in "$@"; do
		r="${v#*=}"
		l="${v%"$r"}"
		case "$v" in
		--cc=*/* | --cross-prefix=*/*) r="${r##*/}" ;;
		*=*/*)
			read -ra words <<<"$r"
			keep=()
			for w in "${words[@]}"; do
				case "$w" in */*) ;; *) keep+=("$w") ;; esac
			done
			r="${keep[*]}"
			;;
		*/*) continue ;;
		esac
		case "$r" in *[!A-Za-z0-9_/.+-]*) r="'${r}'" ;; esac
		out="${out# } ${l}${r}"
	done
	printf '%s\n' "${out}"
}

# _vp_verify_ffplay <name>: tools build-ffplay.sh step 5, the checks of both its variants
_vp_verify_ffplay() {
	local name="$1" bin="${VP_OUT}/$1" bad=0 s syms n
	nl_no_undefined "${bin}"
	syms="$("${NL_NM}" "${bin}")"
	local want_syms=(main video_thread audio_thread read_thread sdl_audio_callback __wrap_pthread_create __wrap_mmap
		KMSDRM_CreateDevice SDL_EGL_LoadLibrary ff_hevc_decoder ff_hevc_rpivid_decoder rpivid_hw_decode ff_h264_decoder ff_aac_decoder
		ff_mov_demuxer ff_matroska_demuxer ff_vf_scale ff_af_aresample swr_convert sws_scale v3d_drm_screen_create_renderonly
		SDL_PHOENIX_HID_Poll Wayland_CreateDevice wl_display_connect wl_egl_window_create xkb_context_new __wrap_ioctl
		__wrap_close memfd_create)
	for s in "${want_syms[@]}"; do
		grep -qE " [TtWwDdRr] ${s}\$" <<<"${syms}" || { echo "video_player: ${name}: symbol ${s}: NO"; bad=1; }
	done
	local want_strs=('KMS/DRM Video Driver' '/dev/dri/' 'libdrm-phoenix:' '/dev/audio0' 'EGL_KHR_platform_gbm' 'V3D 4.2'
		'FFPLAY_THREAD_STACK' 'Simple media player' 'ffplay-stat t=' 'FFPLAY_AUTOKEYS' '/dev/kbd0' 'WAYLAND_DISPLAY'
		'xdg_wm_base' 'EGL_KHR_platform_wayland' 'SDL Wayland video driver' 'rpivid: hardware HEVC decode' 'FFMPEG_RPIVID')
	for s in "${want_strs[@]}"; do
		n="$(nl_count_strings "${bin}.stripped" "${s}")"
		[ "${n}" != 0 ] || { echo "video_player: ${name}: string '${s}': 0"; bad=1; }
	done
	nl_forbid_old_lane "${bin}.stripped" phxgl /dev/fb0 RPI4FB_GETMODE
	[ "${bad}" = 0 ] || b_die "video_player: ${name}: verification failed (see above)"
	echo "video_player: ${name}: $(stat -c %s "${bin}") bytes, stripped $(stat -c %s "${bin}.stripped")"
}

p_build() {
	# shellcheck disable=SC1091
	. "${PORT_DEP_libdrm_phoenix}/share/phoenix-newlane/newlane.subr"
	nl_setup "${PREFIX_PORT_BUILD}/nl"

	local F="${PREFIX_PORT}/files" I="${PREFIX_PORT_INSTALL%/}"
	VP_FS="${PREFIX_PORT_WORKDIR%/}"
	VP_OUT="${PREFIX_PORT_BUILD}/out"
	mkdir -p "${VP_OUT}"

	# --- 1. configure: the ffmpeg port's line (static, asm on, no autodetect, no programs, no
	# network) with the player's components. Re-run when the switches or the libphoenix API
	# (its defined symbols: configure probes libc functions) change.
	# shellcheck source=files/components.sh
	. "${F}/components.sh"
	# zlib (FF_COMMON --enable-zlib) comes from the zlib port, seen through a private view:
	# NL_TFLAGS name only the sysroot, and -I/-L on the whole shared prefix would put every
	# port's headers in front of FFmpeg's own. The programs link libz.a from their own
	# inputs (sdl2_kmsdrm's link-inputs.txt tail; gtk3_wayland's pkg-config).
	local ZV="${VP_OUT}/zlib-view" ZD
	ZD="$(b_dependency_dir zlib)"
	ZD="${ZD%/}"
	mkdir -p "${ZV}/include" "${ZV}/lib"
	cp -a "${ZD}/include/zlib.h" "${ZD}/include/zconf.h" "${ZV}/include/"
	cp -a "${ZD}/lib/libz.a" "${ZV}/lib/"
	# -fstack-protector-strong: libphoenix provides __stack_chk_guard and __stack_chk_fail,
	# and the demuxers and decoders parse untrusted files. --extra-cflags reaches the
	# libraries and the fftools objects; FFmpeg's --toolchain=hardened would also make PIE.
	local cfg_args=(--enable-cross-compile --arch=aarch64 --target-os=none --cross-prefix="${NL_CC%gcc}"
		--cc="${NL_CC}" --extra-cflags="${NL_TFLAGS[*]} -O2 -g -fstack-protector-strong -I${ZV}/include" --extra-ldflags="${NL_TFLAGS[*]} -L${ZV}/lib"
		"${FF_COMMON[@]}" --enable-decoder=hevc_rpivid --enable-asm --disable-programs --disable-shared --enable-static)
	local stamp
	stamp="$( { printf '%s\n' "${cfg_args[@]}"; awk '{ print $3 }' <<<"${NL_LIBC_SYMS}" | LC_ALL=C sort -u; } |
		sha256sum | cut -c1-16)"
	if [ ! -f "${VP_FS}/config.h" ] || [ "$(cat "${VP_OUT}/configure.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
		(cd "${VP_FS}" && ./configure "${cfg_args[@]}") >"${VP_OUT}/ff-configure.log" 2>&1 ||
			{ tail -30 "${VP_OUT}/ff-configure.log"; b_die "video_player: ffmpeg configure failed"; }
		# the ffmpeg port's libm reconcile: a probe against a stale libc would set these 0 and
		# ffmpeg's static-inline fallbacks would clash with libphoenix's prototypes
		local macro flipped=0
		for macro in HAVE_ERF HAVE_EXP2 HAVE_EXP2F HAVE_LOG2F; do
			if grep -q "^#define ${macro} 0$" "${VP_FS}/config.h"; then
				sed -i "s/^#define ${macro} 0$/#define ${macro} 1/" "${VP_FS}/config.h"
				flipped=$((flipped + 1))
			fi
		done
		echo "video_player: config.h: flipped ${flipped} libm HAVE_* flag(s) 0->1"
		# FFMPEG_CONFIGURATION is the whole configure line, the toolchain, sysroot and
		# zlib-view paths in it; the libraries and ffplay compile it in. Its public form:
		local pubcfg
		pubcfg="$(_vp_public_configuration "${cfg_args[@]}")"
		case "${pubcfg}" in *[\"\\\|\&]*) b_die "video_player: cannot put this configure line in config.h: ${pubcfg}" ;; esac
		sed -i "s|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION \"${pubcfg}\"|" "${VP_FS}/config.h"
		echo "${stamp}" >"${VP_OUT}/configure.stamp"
	fi
	grep -qE '^#define FFMPEG_CONFIGURATION "[^/]+"$' "${VP_FS}/config.h" ||
		b_die "video_player: config.h: FFMPEG_CONFIGURATION names a path: $(grep '^#define FFMPEG_CONFIGURATION' "${VP_FS}/config.h")"
	local d
	for d in HAVE_PTHREADS HAVE_NEON CONFIG_AVFILTER CONFIG_SWSCALE CONFIG_SWRESAMPLE CONFIG_HEVC_DECODER CONFIG_H264_DECODER \
			CONFIG_AAC_DECODER CONFIG_MOV_DEMUXER CONFIG_SCALE_FILTER CONFIG_ARESAMPLE_FILTER CONFIG_ZLIB \
			CONFIG_MPEG2VIDEO_DECODER CONFIG_AC3_DECODER CONFIG_DCA_DECODER CONFIG_MPEGPS_DEMUXER CONFIG_YADIF_FILTER \
			CONFIG_HEVC_RPIVID_DECODER CONFIG_HLS_DEMUXER; do
		cat "${VP_FS}/config.h" "${VP_FS}/config_components.h" | grep -qE "^#define ${d} 1$" ||
			b_die "video_player: config: ${d} is not 1"
	done
	for d in CONFIG_GPL CONFIG_NONFREE CONFIG_SDL2; do
		grep -qE "^#define ${d} 0$" "${VP_FS}/config.h" || b_die "video_player: config.h: ${d} is not 0"
	done
	# HLS without FFmpeg's network: the hunk's caller opens the URLs (no second HTTP/TLS stack)
	for d in CONFIG_HTTP_PROTOCOL CONFIG_HTTPS_PROTOCOL CONFIG_TLS_PROTOCOL; do
		grep -qE "^#define ${d} 0$" "${VP_FS}/config_components.h" || b_die "video_player: config_components.h: ${d} is not 0"
	done
	grep -q 'a web browser' "${VP_FS}/libavformat/hls.c" || b_die "video_player: libavformat/hls.c lacks the files/hls hunk"

	# --- 2. the libraries, the glue --------------------------------------------------------
	make -C "${VP_FS}" -j"$(nproc)" >"${VP_OUT}/ff-make.log" 2>&1 ||
		{ grep -E -B2 -A6 'error' "${VP_OUT}/ff-make.log" | head -60; b_die "video_player: ffmpeg build failed"; }
	echo "video_player: ffmpeg: $(grep -c 'warning:' "${VP_OUT}/ff-make.log" || true) compiler warning line(s) in this make run"
	VP_FF_A=("${VP_FS}/libavfilter/libavfilter.a" "${VP_FS}/libavformat/libavformat.a" "${VP_FS}/libavcodec/libavcodec.a"
		"${VP_FS}/libswresample/libswresample.a" "${VP_FS}/libswscale/libswscale.a" "${VP_FS}/libavutil/libavutil.a")
	local a
	for a in "${VP_FF_A[@]}"; do [ -f "${a}" ] || b_die "video_player: missing ${a}"; done
	# the libraries for other ports (webkit_wpe USE video): headers, archives, pkg-config files,
	# with the paths made relative to the prefix
	local FFD="${I}/ffmpeg" FFS="${VP_OUT}/ffmpeg-dev"
	rm -rf "${FFD}" "${FFS}"
	make -C "${VP_FS}" DESTDIR="${FFS}" install-libs install-headers >"${VP_OUT}/ff-install.log" 2>&1 ||
		{ tail -20 "${VP_OUT}/ff-install.log"; b_die "video_player: ffmpeg library install failed"; }
	[ -d "${FFS}/usr/local/lib" ] || b_die "video_player: ffmpeg library install: no ${FFS}/usr/local/lib"
	mv "${FFS}/usr/local" "${FFD}"
	sed -i -e 's|^prefix=.*|prefix=/usr/local|' -e 's|^libdir=/usr/local/|libdir=${prefix}/|' \
		-e 's|^includedir=/usr/local/|includedir=${prefix}/|' "${FFD}"/lib/pkgconfig/*.pc
	for a in avformat avcodec swresample swscale avutil; do
		[ -f "${FFD}/lib/lib${a}.a" ] && [ -f "${FFD}/lib/pkgconfig/lib${a}.pc" ] || b_die "video_player: ffmpeg/: lib${a} incomplete"
	done
	grep -q 'ff_hevc_rpivid_decoder' < <("${NL_NM}" "${FFD}/lib/libavcodec.a") || b_die "video_player: ffmpeg/: libavcodec has no hevc_rpivid"
	grep -q 'ff_hls_demuxer' < <("${NL_NM}" "${FFD}/lib/libavformat.a") || b_die "video_player: ffmpeg/: libavformat has no hls demuxer"
	[ -f "${FFD}/include/libavcodec/rpivid_drm.h" ] || b_die "video_player: ffmpeg/: no libavcodec/rpivid_drm.h"
	# hevc_rpivid's zero-copy picture buffers from the V3D render server (files/rpivid/drm):
	# for hevc-rpivid-check -zc here and for players (webkit_wpe), which also link
	# libdrm-phoenix's libdrm.a and its --wrap flags
	local LDP="${PORT_DEP_libdrm_phoenix}"
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${VP_FS}" -I"${LDP}/include" -I"${LDP}/include/libdrm" \
		-c "${F}/rpivid/drm/rpivid_bo_drm.c" -o "${VP_OUT}/rpivid_bo_drm.o"
	rm -f "${FFD}/lib/librpivid_bo_drm.a"
	"${NL_AR}" rcs "${FFD}/lib/librpivid_bo_drm.a" "${VP_OUT}/rpivid_bo_drm.o"
	install -m 644 "${F}/rpivid/drm/rpivid_bo_drm.h" "${FFD}/include/rpivid_bo_drm.h"

	VP_GLUE_O="${VP_OUT}/ffplay_phoenix_glue.o"
	"${NL_CC}" -O2 -g -std=gnu17 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -c "${F}/ffplay_phoenix_glue.c" -o "${VP_GLUE_O}"

	# --- 3. ffplay: SDL KMSDRM + Wayland on Mesa's GL build (EGL on GBM and on Wayland), the
	# group of sdl2_kmsdrm's link-inputs.txt, one item per line in link order (gallium, sdl,
	# mesa-gl..., mesa-es..., tail..., flag...); ffplay links the desktop-GL set (SDL's renderer
	# may pick its OpenGL or its GLES2 back end)
	local SP="${PORT_DEP_sdl2_kmsdrm}" LI wgall wsdl WG=() WF=()
	LI="${SP}/link-inputs.txt"
	[ -f "${LI}" ] || b_die "video_player: no ${LI}"
	wgall="$(awk '$1 == "gallium" { print $2 }' "${LI}")"
	wsdl="$(awk '$1 == "sdl" { print $2 }' "${LI}")"
	[ -n "${wsdl}" ] && [ "${wsdl}" -ef "${SP}/lib/libSDL2.a" ] || b_die "video_player: ${LI}: sdl is not ${SP}/lib/libSDL2.a"
	mapfile -t WG < <(awk '$1 == "mesa-gl" || $1 == "tail" { print $2 }' "${LI}")
	mapfile -t WF < <(awk '$1 == "flag" { print $2 }' "${LI}")
	[ "${#WG[@]}" -gt 10 ] && [ "${#WF[@]}" -gt 0 ] || b_die "video_player: ${LI} is incomplete"
	_vp_fftools "${SP}" "${VP_OUT}/obj"
	_vp_link ffplay "${VP_OUT}/obj" "${wsdl}" "${wgall}" "${WF[@]}" -- "${WG[@]}"
	_vp_verify_ffplay ffplay
	local progs=(ffplay)

	# --- 4. hevc-rpivid-check: the rpivid decoder against the CPU one (libavformat +
	# libavcodec, no SDL); the glue's 8 MiB thread stacks and ffplay's 16 MiB main stack (the
	# CPU decoder runs on the main thread with -t 1 and in the stream probe) -----------------
	# -zc: GPU picture buffers (rpivid_bo_drm.o, libdrm-phoenix and its --wrap flags)
	"${NL_CC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -DRPIVID_CHECK_DRM -I"${VP_FS}" -I"${F}/rpivid/drm" \
		-c "${F}/rpivid/check/hevc-rpivid-check.c" -o "${VP_OUT}/hevc-rpivid-check.o"
	"${NL_CC}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=pthread_create \
		-Wl,--wrap=mmap -Wl,--wrap=ioctl -Wl,--wrap=fcntl -Wl,--wrap=dup -Wl,--wrap=dup2 \
		-Wl,-z,stack-size=16777216 -Wl,-Map,"${VP_OUT}/hevc-rpivid-check.map" -o "${VP_OUT}/hevc-rpivid-check" "${VP_OUT}/hevc-rpivid-check.o" "${VP_GLUE_O}" \
		"${VP_OUT}/rpivid_bo_drm.o" \
		-Wl,--start-group "${VP_FS}/libavformat/libavformat.a" "${VP_FS}/libavcodec/libavcodec.a" "${VP_FS}/libswresample/libswresample.a" \
		"${VP_FS}/libavutil/libavutil.a" "${ZV}/lib/libz.a" "${LDP}/lib/libdrm.a" -Wl,--end-group -lm \
		>"${VP_OUT}/hevc-rpivid-check-link.log" 2>&1 ||
		{ head -40 "${VP_OUT}/hevc-rpivid-check-link.log"; b_die "video_player: hevc-rpivid-check: link failed"; }
	"${NL_STRIP}" -o "${VP_OUT}/hevc-rpivid-check.stripped" "${VP_OUT}/hevc-rpivid-check"
	nl_no_undefined "${VP_OUT}/hevc-rpivid-check"
	local rs rsyms rbad=0
	rsyms="$("${NL_NM}" "${VP_OUT}/hevc-rpivid-check")"
	for rs in main ff_hevc_rpivid_decoder ff_hevc_decoder rpivid_hw_open rpivid_sand8_to_planar rpivid_cmd_slice __wrap_pthread_create \
			rpivid_bo_drm_install rpivid_drm_frame_to_planar rpivid_drm_set_buffer_ops __wrap_mmap __wrap_ioctl drmPrimeHandleToFD; do
		grep -qE " [TtWwDdRr] ${rs}\$" <<<"${rsyms}" || { echo "video_player: hevc-rpivid-check: symbol ${rs}: NO"; rbad=1; }
	done
	for rs in 'RPIVID-CHECK verdict=' 'rpivid: hardware HEVC decode' '/dev/vcmbox' '/tmp/.rpivid.lock' 'RPIVID-CHECK zc frames=' \
			'/dev/dri/renderD128' 'rpivid: drm_prime output'; do
		[ "$(nl_count_strings "${VP_OUT}/hevc-rpivid-check.stripped" "${rs}")" != 0 ] || { echo "video_player: hevc-rpivid-check: string '${rs}': 0"; rbad=1; }
	done
	[ "${rbad}" = 0 ] || b_die "video_player: hevc-rpivid-check: verification failed (see above)"
	progs+=(hevc-rpivid-check)

	# --- 5. gtk-video: GTK 3 Wayland (gtk3_wayland, linked as its gtk3-hello) + the player's
	# FFmpeg libraries + the glue; painted with cairo (no Mesa in this binary) --------------
	if b_use gtk; then
		local GO WLP
		GO="$(b_dependency_dir gtk3_wayland)"
		GO="${GO%/}"
		WLP="$(b_dependency_dir wayland_phoenix)"
		local PHXCC="${GO}/host-bin/phx-gcc" PKGC="${GO}/pkg-config-phoenix" SYSD="${GO}/deps/sys"
		local COMPAT_INC="${WLP%/}/prefix/share/wayland-phoenix/compat/include"
		local GV="${VP_OUT}/gtk-video" GFF=("${VP_FF_A[@]:1}")   # no libavfilter
		for a in "${PHXCC}" "${PKGC}" "${SYSD}/lib" "${COMPAT_INC}"; do
			[ -e "${a}" ] || b_die "video_player: missing ${a}"
		done
		# shellcheck disable=SC2046 # pkg-config output is a word list
		"${PHXCC}" -O2 -g -std=gnu11 -Wall -Wextra -Werror "${NL_TFLAGS[@]}" -I"${SYSD}/include" -I"${COMPAT_INC}" \
			-I"${VP_FS}" $("${PKGC}" --cflags gtk+-3.0 gtk+-wayland-3.0) -c "${F}/gtk-video/gtk-video.c" -o "${VP_OUT}/gtk-video.o"
		# shellcheck disable=SC2046
		"${PHXCC}" "${NL_TFLAGS[@]}" -static -Wl,--gc-sections -Wl,-z,max-page-size=0x1000 -Wl,--wrap=pthread_create \
			-Wl,-Map,"${GV}.map" -o "${GV}" "${VP_OUT}/gtk-video.o" "${VP_GLUE_O}" -L"${SYSD}/lib" \
			-Wl,--start-group "${GFF[@]}" $("${PKGC}" --libs gtk+-3.0 gtk+-wayland-3.0) -Wl,--end-group -lm \
			>"${VP_OUT}/gtk-video-link.log" 2>&1 ||
			{ grep -v 'warning: .* is not fully supported' "${VP_OUT}/gtk-video-link.log" | head -40; b_die "video_player: gtk-video: link failed"; }
		"${NL_STRIP}" -o "${GV}.stripped" "${GV}"
		# verification (tools gtk-video/build.sh)
		nl_no_undefined "${GV}"
		local s syms bad=0 direct
		syms="$("${NL_NM}" "${GV}")"
		for s in main gtk_init gtk_window_fullscreen gdk_wayland_display_get_type avformat_open_input ff_h264_decoder \
				ff_hevc_decoder ff_hevc_rpivid_decoder ff_aac_decoder sws_scale_frame swr_convert __wrap_pthread_create __wrap_close; do
			grep -qE " [TtWwDdRr] ${s}\$" <<<"${syms}" || { echo "video_player: gtk-video: symbol ${s}: NO"; bad=1; }
		done
		for s in 'GTK-VIDEO stat t=' 'GTK_VIDEO_AUTOKEYS' '/dev/audio0' 'media-playback-start' '/org/gtk/libgtk/theme/Adwaita'; do
			[ "$(nl_count_strings "${GV}.stripped" "${s}")" != 0 ] || { echo "video_player: gtk-video: string '${s}': 0"; bad=1; }
		done
		# every thread creator goes through the glue (GLib, libavcodec)
		direct="$("${NL_OBJDUMP}" -d --no-show-raw-insn "${GV}" |
			awk '/^[0-9a-f]+ <[^>]*>:$/ { fn = $2 } /<pthread_create>$/ && /\tb/ { print fn }' | sort -u | tr '\n' ' ')"
		[ "${direct}" = "<__wrap_pthread_create>: " ] ||
			{ echo "video_player: gtk-video: direct callers of the real pthread_create: ${direct:-none}"; bad=1; }
		[ "${bad}" = 0 ] || b_die "video_player: gtk-video: verification failed (see above)"
		progs+=(gtk-video)
	fi

	# --- 6. the demo clips (tools gen-clips.sh, host ffmpeg; data: regenerated only when the
	# generator or the host ffmpeg changes) --------------------------------------------------
	local CL="${VP_OUT}/demo/clips"
	if b_use demo; then
		local encs e hostff
		command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null ||
			b_die "video_player: USE demo needs the host's ffmpeg + ffprobe (the clips are generated at build time)"
		encs="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
		for e in libx264 libx265 libvpx-vp9 libopus aac; do
			grep -qE "^ [A-Z.]{6} ${e} " <<<"${encs}" || b_die "video_player: the host ffmpeg has no ${e} encoder (USE demo)"
		done
		hostff="$(ffmpeg -hide_banner -version 2>/dev/null | head -1)"
		stamp="$( { sha256sum "${F}/gen-clips.sh"; echo "${hostff}"; } | sha256sum | cut -c1-16)"
		if [ "$(cat "${VP_OUT}/demo/clips.stamp" 2>/dev/null || true)" != "${stamp}" ] ||
				[ "$(find "${CL}" -maxdepth 1 -name 'm10-*' 2>/dev/null | wc -l)" != 4 ]; then
			rm -rf "${VP_OUT}/demo"
			mkdir -p "${VP_OUT}/demo"
			bash "${F}/gen-clips.sh" --out "${VP_OUT}/demo" || b_die "video_player: gen-clips.sh failed"
			echo "${stamp}" >"${VP_OUT}/demo/clips.stamp"
		fi
		for e in m10-h264-720p30-aac.mp4 m10-h264-1080p30-aac.mp4 m10-hevc-720p30-aac.mp4 m10-vp9-360p-opus.webm; do
			[ -s "${CL}/${e}" ] || b_die "video_player: clip ${e} not generated"
		done
	fi

	# --- install ------------------------------------------------------------------------------
	local p
	mkdir -p "${I}/bin" "${I}/prog" "${I}/share/video-player"
	for p in "${progs[@]}"; do
		install -m 755 "${VP_OUT}/${p}" "${I}/prog/${p}"
		install -m 755 "${VP_OUT}/${p}.stripped" "${I}/bin/${p}"
		install -m 644 "${VP_OUT}/${p}.map" "${I}/share/video-player/"
	done

	# --- the staging tree (the rootfs files, final names) ------------------------------------
	local ST="${I}/stage" f
	rm -rf "${ST}"
	install -D -m 755 "${I}/bin/ffplay" "${ST}/usr/bin/ffplay"
	install -D -m 755 "${I}/bin/hevc-rpivid-check" "${ST}/usr/bin/hevc-rpivid-check"
	install -D -m 755 "${F}/image/video-play" "${ST}/bin/video-play"
	if b_use gtk; then
		install -D -m 755 "${I}/bin/gtk-video" "${ST}/usr/bin/gtk-video"
		mkdir -p "${ST}/usr/share/applications"
		sed -e 's|^Exec=gtk-video |Exec=/usr/bin/gtk-video |' -e 's|, Phoenix-RTOS M10)|)|' "${F}/conf/applications/gtk-video.desktop" \
			>"${ST}/usr/share/applications/gtk-video.desktop"
		grep -qx 'Exec=/usr/bin/gtk-video %f' "${ST}/usr/share/applications/gtk-video.desktop" ||
			b_die "video_player: gtk-video.desktop: Exec rewrite failed"
	fi
	if b_use demo; then
		# the clips under their plain names (gen-clips.sh writes m10-<name>)
		for f in "${CL}"/m10-*; do install -D -m 644 "${f}" "${ST}/usr/share/video-demo/$(basename "${f#"${CL}"/m10-}")"; done
		# the video desktop session (CONF_DIR=/etc/xdg/labwc-xfce-video)
		for f in rc.xml menu.xml autostart environment; do
			install -D -m 644 "${F}/image/labwc-xfce-video/${f}" "${ST}/etc/xdg/labwc-xfce-video/${f}"
		done
		install -D -m 644 "${F}/image/video-demo.desktop" "${ST}/usr/share/applications/video-demo.desktop"
	fi
	# nothing installed may name a program of the tools' hand-staged sessions
	if grep -rnE '(ffplay-(wl|drm)|video-play)2|-low\b|rpi4-kms-g[0-9]|(xfce-session|foot|fuzzel|labwc)-2|xfce-demo|/usr/share/m10' \
			"${ST}/bin" "${ST}/etc" "${ST}/usr/share/applications" 2>/dev/null; then
		b_die "video_player: an installed file names a program or path this image does not have (above)"
	fi
	(cd "${ST}" && find . -type f -printf '%P\n' | sort | xargs sha256sum) >"${I}/stage.MANIFEST"
	cp "${I}/stage.MANIFEST" "${I}/share/video-player/"
	echo "video_player: stage: $(wc -l <"${I}/stage.MANIFEST") files"

	if b_use rootfs; then
		mkdir -p "${PREFIX_FS}/root"
		cp -a "${ST}/." "${PREFIX_FS}/root/"
		echo "video_player: staged into ${PREFIX_FS}/root"
	fi
}
