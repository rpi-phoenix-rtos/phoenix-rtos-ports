#!/usr/bin/env bash
#
# M10 test clips, generated with the HOST ffmpeg from synthetic sources only (lavfi testsrc2,
# mandelbrot, sine): no third-party content, nothing to license. Written to <out>/clips/ and
# NOT committed (a few MB each). Stage them (coordinator) under /usr/share/m10/ on the live
# export -- a new directory.
#
#   m10-h264-720p30-aac.mp4    45 s  1280x720 30 fps H.264 Main + AAC-LC 44.1 kHz stereo (the CPU demo)
#   m10-h264-1080p30-aac.mp4   30 s  1920x1080 30 fps H.264 High + AAC (CPU at 1080p: expect drops)
#   m10-hevc-720p30-aac.mp4    30 s  1280x720 30 fps HEVC Main in the rpivid subset + AAC (decoded
#                                    on the rpivid block by FFmpeg's hevc_rpivid)
#   m10-vp9-360p-opus.webm     20 s  640x360 30 fps VP9 + Opus 48 kHz (a second codec family and
#                                    a 48 kHz track: aresample + SDL's converter to 44.1 kHz)
#
# The picture carries testsrc2's frame counter and a timestamp overlay, so an HDMI snapshot
# names the frame on screen; the audio is a 440 Hz tone with a beep every second (sine's
# beep_factor), so a/v drift is audible.
#
# Usage: tools/gpu-lane/video-player/gen-clips.sh [--out <dir>]
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${here}/build-out"
while [ $# -gt 0 ]; do
	case "$1" in
		--out) shift; out="${1:?--out needs a directory}" ;;
		-h|--help) sed -n '2,24p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "gen-clips.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
c="${out}/clips"
mkdir -p "${c}"
ff() { ffmpeg -nostdin -y -hide_banner -loglevel error "$@"; }
tone() { echo "sine=frequency=440:beep_factor=4:sample_rate=$1:duration=$2"; }

# 1: the CPU demo clip
ff -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=45" -f lavfi -i "$(tone 44100 45)" \
	-c:v libx264 -profile:v main -preset medium -crf 23 -g 60 -pix_fmt yuv420p \
	-c:a aac -b:a 128k -ac 2 -ar 44100 -movflags +faststart -shortest "${c}/m10-h264-720p30-aac.mp4"
# 2: 1080p H.264
ff -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=30" -f lavfi -i "$(tone 44100 30)" \
	-c:v libx264 -profile:v high -preset medium -crf 23 -g 60 -pix_fmt yuv420p \
	-c:a aac -b:a 128k -ac 2 -ar 44100 -movflags +faststart -shortest "${c}/m10-h264-1080p30-aac.mp4"
# 3: HEVC in the rpivid-verified subset (tools/hevc-decode/transcode-for-phoenix.sh's x265 set)
ff -f lavfi -i "mandelbrot=size=1280x720:rate=30" -f lavfi -i "$(tone 44100 30)" -t 30 \
	-vf "format=yuv420p" -c:v libx265 -profile:v main -preset medium -crf 26 \
	-x265-params "wpp=1:no-amp=1:deblock=0,0:no-open-gop=1:bframes=0:ref=1:vbv-maxrate=4000:vbv-bufsize=8000:log-level=none" -tag:v hvc1 \
	-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv \
	-c:a aac -b:a 128k -ac 2 -ar 44100 -movflags +faststart "${c}/m10-hevc-720p30-aac.mp4"
# 4: VP9 + Opus
ff -f lavfi -i "testsrc2=size=640x360:rate=30:duration=20" -f lavfi -i "$(tone 48000 20)" \
	-c:v libvpx-vp9 -b:v 1M -row-mt 1 -deadline good -cpu-used 4 -pix_fmt yuv420p \
	-c:a libopus -b:a 96k -ac 2 -shortest "${c}/m10-vp9-360p-opus.webm"

for f in "${c}"/m10-*; do
	printf '%s  %s bytes  %s\n' "$(sha256sum "${f}" | cut -c1-16)" "$(stat -c %s "${f}")" "$(basename "${f}")"
	ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,sample_rate,channels:format=duration \
		-of compact=p=0:nk=0 "${f}" | sed 's/^/    /'
done
