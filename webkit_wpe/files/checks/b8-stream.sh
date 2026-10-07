#!/bin/bash
#
# b8-stream.sh -- streaming video in the browser, stage 0 (native HLS; coordination repo
# docs/browser/MSE-DESIGN.md §10.1), one psh command:
#
#     /bin/bash /usr/share/wpe-browser/b8-stream.sh [arms]
#
# The pages and the ladders come from the host's media server (coordination repo
# tools/browser/media/: gen-ladders.sh, serve-media.py on 10.42.0.1:8091, which copies its pages
# into the media root); B8S_BASE overrides the server. One XFCE session (/bin/xfce-session)
# whose autostart plays the arms in turn, each a fresh wpe-browser window (--autoplay=allow,
# CPU raster as b8.sh; B8S_BROWSER_ARGS replaces the flags) killed after its time. arms: a comma
# list of
#
#   hevc-fmp4    the HEVC/H.264 fMP4 ladder: must choose HEVC 1080p (rule=hevc8) on rpivid
#   hevc-ts      the same in MPEG-TS segments
#   h264-only    H.264 1080p/720p/360p: the 720p cap (rule=h264)
#   main10       HEVC Main10 1080p + H.264 720p: H.264 720p (rule=h264)
#   main10-cpu   the same with the Main10 variant forced (WPE_PHOENIX_HLS_VARIANT=B8S_MAIN10_INDEX,
#                default 0): the measured CPU-conversion gap
#   seek         hevc-fmp4 with a seek to 40 s at 10 s
#   byterange    H.264 720p from one file by EXT-X-BYTERANGE
#   aes          H.264 720p MPEG-TS, AES-128
#   audio-only   AAC/Opus, no pictures
#   live         a live window over hevc-fmp4 (serve-media.py /live/), 90 s
#   live-disc    a live window over hevc-ts with EXT-X-DISCONTINUITY every 10 segments, 90 s
#   hlsjs        b8-hlsjs.html (hls.js): with no MSE it must take the native branch
#   memory       b8-hls-memory.html: seven idle players, then one plays (no decoder before play)
#
# (default hevc-fmp4,h264-only,main10,seek,aes,live: ~530 s, inside one 540 s psh command; the
# rest in a second run, e.g. hevc-ts,byterange,main10-cpu,audio-only,live-disc). Lines of ours start with
# "B8S ", the pages' with "B8HLS" (and the other pages' tags), the player's with "WPEB-MEDIA ".
# After each arm one grading line:
#
#   B8S arm=<a> choose=<rule>:<variant> video=<decoder> hw=<0|1|-> fps=<last playing stat>
#       dropped=<n> rebuffers=<n> segments=<opened>/<in playlist> done=<page result>
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
SELF=/usr/share/wpe-browser/b8-stream.sh
BROWSER=/usr/bin/wpe-browser
BASE=${B8S_BASE:-http://10.42.0.1:8091}
export HOME=${HOME:-/root}
export PHX_TRACE_ABORT=1
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export THUNAR_START=${THUNAR_START:-0}
export WPE_PHOENIX_MEDIA_STAT_MS=${WPE_PHOENIX_MEDIA_STAT_MS:-2000}
read -ra BROWSER_ARGS <<< "${B8S_BROWSER_ARGS:---cpu-rendering --autoplay=allow --size=1000x620}"

pid=""
LOG=/tmp/b8s-arm.log
PIDFILE=/tmp/b8s-arm.pid
RCFILE=/tmp/b8s-arm.rc
pause() {  # pause <seconds>, interruptible by the session's SIGTERM
	sleep "$1" &
	wait $!
}
stop_all() {
	[ -z "${pid}" ] && [ -s "${PIDFILE}" ] && pid=$(cat "${PIDFILE}")
	[ -z "${pid}" ] || kill -TERM "${pid}" 2>/dev/null
	wait
	echo "B8S stopped by the session t=${SECONDS}"
	exit 0
}

# F=<the value of "name=" in a log line> (no fork: bash runs this per line)
field() {  # field <line> <name>
	local s=" ${1}"
	F=-
	case "${s}" in
		*" $2="*) s=${s#* $2=}; F=${s%% *} ;;
	esac
}

grade() {  # grade <arm>: the grading line from ${LOG}
	local line choose=- video=- hw=- fps=- dropped=- rebuffers=- segments=- done=- paused
	[ -f "${LOG}" ] || : > "${LOG}"
	while IFS= read -r line; do
		case "${line}" in
			*"WPEB-MEDIA "*" hls choose i="*)
				field "${line}" rule; choose=${F}
				field "${line}" i; choose=${choose}:${F} ;;
			*"WPEB-MEDIA "*" decoder video="*)
				field "${line}" video; video=${F} ;;
			*"WPEB-MEDIA "*" stat clock="*)
				field "${line}" paused; paused=${F}
				field "${line}" hw; hw=${F}
				field "${line}" dropped; dropped=${F}
				field "${line}" rebuffers; [ "${F}" = - ] || rebuffers=${F}
				field "${line}" segments; [ "${F}" = - ] || segments=${F}
				if [ "${paused}" = 0 ]; then
					field "${line}" fps; fps=${F}
				fi ;;
			*"-DONE result="*)
				field "${line}" result; done=${F} ;;
		esac
	done < "${LOG}"
	echo "B8S arm=$1 choose=${choose} video=${video} hw=${hw} fps=${fps} dropped=${dropped} rebuffers=${rebuffers}" \
		"segments=${segments} done=${done}"
}

arm() {  # arm <name> <url> <seconds> [env...]
	local name=$1 url=$2 secs=$3 rc tee i
	shift 3
	echo "B8S arm=${name} start url=${url} secs=${secs} env=${*:-none} t=${SECONDS}"
	rm -f "${LOG}" "${PIDFILE}" "${RCFILE}"
	(
		[ "$#" = 0 ] || export "$@"
		"${BROWSER}" "${BROWSER_ARGS[@]}" "${url}" 2>&1 &
		echo "$!" > "${PIDFILE}"
		wait "$!"
		echo "$?" > "${RCFILE}"
	) | tee "${LOG}" &
	tee=$!
	pause "${secs}"
	pid=$(cat "${PIDFILE}" 2>/dev/null)
	[ -z "${pid}" ] || kill -TERM "${pid}" 2>/dev/null
	i=0
	while [ ! -s "${RCFILE}" ] && [ "${i}" -lt 60 ]; do
		pause 1
		i=$((i + 1))
	done
	i=0
	while kill -0 "${tee}" 2>/dev/null && [ "${i}" -lt 10 ]; do
		pause 1
		i=$((i + 1))
	done
	kill -TERM "${tee}" 2>/dev/null
	wait "${tee}" 2>/dev/null
	pid=""
	rc=$(cat "${RCFILE}" 2>/dev/null)
	echo "B8S arm=${name} end rc=${rc:-?} t=${SECONDS}"
	grade "${name}"
	# the device and the decoder block are free again before the next arm
	pause 5
}

hls() {  # hls <src path> [extra query]: the native-HLS page on a ladder
	echo "${BASE}/b8-hls.html?run=${RUN}&src=$1$2"
}

inner() {
	trap stop_all TERM
	echo "B8S start t=${SECONDS} arms=${B8S_ARMS} base=${BASE} display=${WAYLAND_DISPLAY:-unset}"
	local a IFS=,
	for a in ${B8S_ARMS}; do
		unset IFS
		RUN=${a}
		case "${a}" in
			hevc-fmp4) arm "${a}" "$(hls /ladders/hevc-fmp4/master.m3u8 '&stop=60')" 85 ;;
			hevc-ts) arm "${a}" "$(hls /ladders/hevc-ts/master.m3u8 '&stop=60')" 85 ;;
			h264-only) arm "${a}" "$(hls /ladders/h264-only/master.m3u8 '&stop=60')" 85 ;;
			main10) arm "${a}" "$(hls /ladders/hevc-main10/master.m3u8 '&stop=40')" 65 ;;
			main10-cpu) arm "${a}" "$(hls /ladders/hevc-main10/master.m3u8 '&stop=40')" 65 WPE_PHOENIX_HLS_VARIANT="${B8S_MAIN10_INDEX:-0}" ;;
			seek) arm "${a}" "$(hls /ladders/hevc-fmp4/master.m3u8 '&seek=40@10&stop=40')" 65 ;;
			byterange) arm "${a}" "$(hls /ladders/byterange/master.m3u8 '&stop=40')" 65 ;;
			aes) arm "${a}" "$(hls /ladders/aes/master.m3u8 '&stop=40')" 65 ;;
			audio-only) arm "${a}" "$(hls /ladders/audio-only/master.m3u8 '&stop=30')" 55 ;;
			live) arm "${a}" "$(hls /live/hevc-fmp4/master.m3u8 '&stop=90')" 115 ;;
			live-disc) arm "${a}" "$(hls %2Flive%2Fhevc-ts%2Fmaster.m3u8%3Fdisc%3D10 '&stop=90')" 115 ;;
			hlsjs) arm "${a}" "${BASE}/b8-hlsjs.html?run=${RUN}&src=/ladders/hevc-fmp4/master.m3u8&stop=30" 55 ;;
			memory) arm "${a}" "${BASE}/b8-hls-memory.html?run=${RUN}&src=/ladders/hevc-fmp4/master.m3u8" 100 ;;
			*) echo "B8S bad arm ${a}" ;;
		esac
	done
	echo "B8S done t=${SECONDS}"
}

if [ -n "${B8S_INNER:-}" ]; then
	inner
	exit 0
fi

# --- at psh: the session with the check as its autostart ----------------------------------------
export B8S_INNER=1 B8S_ARMS=${1:-hevc-fmp4,h264-only,main10,seek,aes,live}
# the arms' seconds + 5 s each after the desktop's ~15 s (the default set: ~525 s)
export HOLD=${B8S_HOLD:-540}
export XFCE_AUTOSTART="/bin/bash=${SELF}"
echo "B8S session hold=${HOLD}s arms=${B8S_ARMS} base=${BASE}"
/bin/bash /bin/xfce-session
echo "B8S end rc=$?"
