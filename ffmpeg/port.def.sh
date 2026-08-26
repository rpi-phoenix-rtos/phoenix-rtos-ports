#!/usr/bin/env bash
:
#shellcheck disable=2034
{
	ports_api=1

	name="ffmpeg"
	version="6.1"
	desc="FFmpeg decode core: static libav{util,codec,format}.a (LGPL, decode-only)"
	cpe23="cpe:2.3:a:ffmpeg:ffmpeg:${version}:*:*:*:*:*:*:*"

	source="https://ffmpeg.org/releases/"
	archive_filename="${name}-${version}.tar.gz"
	src_path="${name}-${version}/"

	size="15802195"
	sha256="938dd778baa04d353163ca5cb06c909c918850055f549205b29b1224e45a5316"

	# Decode-only, LGPL configuration: no --enable-gpl / --enable-nonfree, so the
	# produced archives are LGPL-2.1-or-later (see COPYING.LGPLv2.1). GPL-only
	# codecs/filters are never enabled.
	license="LGPL-2.1-or-later"
	license_file="COPYING.LGPLv2.1"

	conflicts=""
	depends=""

	supports="phoenix>=3.3"
}

# Framework migration of tools/ffmpeg-port/build-ffmpeg-phoenix.py (the E4 decode
# feasibility recipe). This port builds ONLY the reusable static decode libraries
# (libavutil/libavcodec/libavformat) + their headers into the build prefix, so a
# consumer can `depends="ffmpeg"` and link the decode core. The E4 demo/players
# (e4_decode_file.c, e4_play.c, ...) intentionally stay in tools/ffmpeg-port as
# demos, per the master plan ("players stay as demos").
#
# The decode-only, LGPL configure line (asm on, static only, no programs/network/
# docs) is lifted verbatim from the proven python driver. ffmpeg's configure is
# NOT autoconf: it takes --cross-prefix/--cc and its own --enable-* switches.

p_prepare() {
	# ffmpeg's configure link-probes erf/exp2/exp2f/log2f in math.h. Historically
	# the toolchain's *stale* sysroot libphoenix declared but did not define them,
	# so configure set HAVE_*=0 and ffmpeg emitted its own static-inline fallbacks
	# that clashed with libphoenix's non-static prototypes. In the framework build
	# we link against the FRESH PREFIX_BUILD libphoenix (which defines all four), so
	# the probes may already pass; the flip in p_prepare is tolerant either way.
	if [ ! -f "$PREFIX_PORT_WORKDIR/config.h" ]; then
		(cd "$PREFIX_PORT_WORKDIR" && "./configure" \
			--enable-cross-compile \
			--arch=aarch64 \
			--target-os=none \
			--cross-prefix="${CROSS}" \
			--cc="${CC}" \
			--extra-cflags="${CFLAGS}" \
			--extra-ldflags="${LDFLAGS}" \
			--prefix="$PREFIX_PORT_INSTALL" \
			--libdir="$PREFIX_A" \
			--incdir="$PREFIX_H" \
			--disable-everything \
			--enable-decoder=mjpeg,h264,rawvideo,pcm_s16le \
			--enable-parser=h264 \
			--enable-demuxer=mjpeg,wav \
			--enable-protocol=file \
			--enable-asm \
			--disable-programs \
			--disable-network \
			--disable-shared \
			--enable-static \
			--disable-doc)
	fi

	# Flip HAVE_{ERF,EXP2,EXP2F,LOG2F} 0 -> 1 in the generated config.h if configure
	# probed them against a stale libc. No-op (0 flips) when the fresh libphoenix
	# already satisfied the probes -- do NOT fail on a 0 count.
	local cfg="$PREFIX_PORT_WORKDIR/config.h" macro flipped=0
	for macro in HAVE_ERF HAVE_EXP2 HAVE_EXP2F HAVE_LOG2F; do
		if grep -q "^#define ${macro} 0$" "${cfg}"; then
			sed -i "s/^#define ${macro} 0$/#define ${macro} 1/" "${cfg}"
			flipped=$((flipped + 1))
		fi
	done
	echo "config.h: flipped ${flipped} libm HAVE_* flag(s) 0->1 (fresh-libc reconcile)"
}

p_build() {
	# --disable-programs means `make` builds only the enabled libraries.
	make -C "$PREFIX_PORT_WORKDIR"
	# install-libs stages the static archives (libdir) + headers (incdir) only,
	# without any ffmpeg/ffprobe binaries. This is a library port: no b_install of
	# a runtime program (the E4 players live in tools/ffmpeg-port).
	make -C "$PREFIX_PORT_WORKDIR" install-libs
}
