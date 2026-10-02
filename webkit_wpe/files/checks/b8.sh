#!/bin/bash
#
# b8.sh -- the browser plan's B8 check (<video> in a page; a webkit_wpe build with USE video),
# one psh command:
#
#     /bin/bash /usr/share/wpe-browser/b8.sh [arms]
#
# One XFCE session (/bin/xfce-session) whose autostart plays the arms in turn, each a fresh
# wpe-browser window on b8.html?src=<clip> with --autoplay=allow, killed after the clip's length
# plus a margin. arms: a comma list of
#
#   h264      the 720p30 H.264 + AAC demo clip (B8_H264_CLIP)
#   hevc      the 1080p30 HEVC + AAC clip on the rpivid block (B8_HEVC_CLIP)
#   hevc-cpu  the same clip on FFmpeg's CPU decoder (FFMPEG_RPIVID=0): the control
#   seek      the H.264 clip with a seek from 10 s to 30 s (b8.html&seek=10:30)
#
# (default h264,hevc,hevc-cpu,seek). Lines of ours start with "B8 ", the page's with "B8PAGE ",
# the browser's with "WPEB ", the media player's with "WPEB-MEDIA ". The coordination repo's
# docs/browser/B8-video.md lists what each must show.
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
SELF=/usr/share/wpe-browser/b8.sh
BROWSER=/usr/bin/wpe-browser
PAGE=file:///usr/share/wpe-browser/b8.html
H264=${B8_H264_CLIP:-/usr/share/video-demo/h264-720p30-aac.mp4}
HEVC=${B8_HEVC_CLIP:-/root/hevc-1080p30-hash.mp4}
export HOME=${HOME:-/root}
export PHX_TRACE_ABORT=1
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export THUNAR_START=${THUNAR_START:-0}   # no file manager window over the browser
export WPE_PHOENIX_MEDIA_STAT_MS=${WPE_PHOENIX_MEDIA_STAT_MS:-2000}

pid=""
pause() {  # pause <seconds>, interruptible by the session's SIGTERM
	sleep "$1" &
	wait $!
}
stop_all() {
	[ -z "${pid}" ] || kill -TERM "${pid}" 2>/dev/null
	wait
	echo "B8 stopped by the session t=${SECONDS}"
	exit 0
}

arm() {  # arm <name> <clip> <seconds> [page query suffix] [env...]
	local name=$1 clip=$2 secs=$3 extra=$4 rc
	shift 4
	if [ ! -f "${clip}" ]; then
		echo "B8 arm=${name} SKIP clip ${clip} missing"
		return
	fi
	echo "B8 arm=${name} start clip=${clip} secs=${secs} env=${*:-none} t=${SECONDS}"
	(
		[ "$#" = 0 ] || export "$@"
		exec "${BROWSER}" --cpu-rendering --autoplay=allow --size=1000x620 "${PAGE}?src=file://${clip}${extra}"
	) &
	pid=$!
	pause "${secs}"
	kill -TERM "${pid}" 2>/dev/null
	wait "${pid}"
	rc=$?
	pid=""
	echo "B8 arm=${name} end rc=${rc} t=${SECONDS}"
	# the device and the decoder block are free again before the next arm
	pause 5
}

inner() {
	trap stop_all TERM
	echo "B8 start t=${SECONDS} arms=${B8_ARMS} display=${WAYLAND_DISPLAY:-unset}"
	local a IFS=,
	for a in ${B8_ARMS}; do
		unset IFS
		case "${a}" in
			h264) arm h264 "${H264}" 75 "" ;;
			hevc) arm hevc "${HEVC}" 50 "" FFMPEG_RPIVID=1 ;;
			hevc-cpu) arm hevc-cpu "${HEVC}" 50 "" FFMPEG_RPIVID=0 ;;
			seek) arm seek "${H264}" 55 "&seek=10:30" ;;
			*) echo "B8 bad arm ${a}" ;;
		esac
	done
	echo "B8 done t=${SECONDS}"
}

if [ -n "${B8_INNER:-}" ]; then
	inner
	exit 0
fi

# --- at psh: the session with the check as its autostart ----------------------------------------
export B8_INNER=1 B8_ARMS=${1:-h264,hevc,hevc-cpu,seek}
export XFCE_AUTOSTART="/bin/bash=${SELF}"
export HOLD=${B8_HOLD:-400}
echo "B8 session hold=${HOLD}s arms=${B8_ARMS}"
/bin/bash /bin/xfce-session
echo "B8 end rc=$?"
