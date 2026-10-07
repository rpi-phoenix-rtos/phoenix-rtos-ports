#!/bin/bash
#
# b8.sh -- the browser plan's B8 check (<video> in a page; a webkit_wpe build with USE video),
# one psh command:
#
#     /bin/bash /usr/share/wpe-browser/b8.sh [arms]
#     /bin/bash /usr/share/wpe-browser/b8.sh transport [arms]
#
# One XFCE session (/bin/xfce-session) whose autostart plays the arms in turn, each a fresh
# wpe-browser window on b8.html?src=<clip> with --autoplay=allow, killed after the clip's length
# plus a margin. arms: a comma list of
#
#   h264      the 720p30 H.264 + AAC demo clip (B8_H264_CLIP)
#   hevc      the 1080p30 HEVC + AAC clip on the rpivid block (B8_HEVC_CLIP)
#   hevc-cpu  the same clip on FFmpeg's CPU decoder (FFMPEG_RPIVID=0): the control
#   seek      the H.264 clip with a seek from 10 s to 30 s (b8.html&seek=10:30)
#   loop      the HEVC clip on the block with the loop attribute: one wrap, then to the end
#   again     the HEVC clip on the block, played to the end, then a seek to 12 s and to the end again
#
# (default h264,hevc,hevc-cpu,seek; loop and again only when named). Lines of ours start with
# "B8 ", the page's with "B8PAGE ", the browser's with "WPEB ", the media player's with
# "WPEB-MEDIA ". After each arm (transport arms too):
#
#   B8 arm=<a> ended=<n> ended_at=<s> clip=<s>
#
# n = the page's 'ended' events (1 for a clip played to its end, 2 for "again", 0 = the end was
# never signalled), ended_at = the element's currentTime at the first one, clip = the duration
# at loadedmetadata (- when not seen). The coordination repo's docs/browser/B8-video.md lists
# what each must show.
#
# "transport": the frame path's A/B, every arm the H.264 clip in the same window, one knob set
# each (raster, frame transport, frame pacing):
#
#   shm           --cpu-rendering                          (the default before dma-bufs)
#   gpu           Skia's GPU raster, shared memory
#   dmabuf        --cpu-rendering --dmabuf                 (/bin/browser's default)
#   both          GPU raster, --dmabuf
#   shm-ahead     --cpu-rendering --frame-ahead
#   dmabuf-ahead  --cpu-rendering --dmabuf --frame-ahead
#   both-ahead    GPU raster, --dmabuf --frame-ahead
#
# (default shm,gpu,dmabuf,both,shm-ahead,dmabuf-ahead). Each arm's output goes to
# /tmp/b8-<arm>.log and is printed after the arm, then one grading line:
#
#   B8 arm=<a> raster=<cpu|gpu> transport=<shm|dmabuf> ahead=<0|1> painted_fps=<f> presented_fps=<f>
#      shown_fps=<f> painted_pct=<n> window_s=<s> took=<raster>/<transport>/<swap chain>/<pacing>
#
# presented: pictures the player handed to the compositor; painted: those the web process's
# compositor drew (WPEB-MEDIA stat, from 4 s after the start until the clip ends); shown: the
# frames the UI process put on screen (--present-stats=5, the first and the last report left out).
# took= is what the browser says it did (WPEB gpu ..., WPEB-WEBKIT swap-chain / frame-pacing):
# an arm whose took= differs from its knobs measured something else.
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
PRESENT_SECS=5
ARM_SECS=${B8_ARM_SECS:-55}   # transport arms: the 45 s clip after a ~5 s start, then the end

pid=""
EVENTS=/tmp/b8-arm.events
PIDFILE=/tmp/b8-arm.pid
RCFILE=/tmp/b8-arm.rc
pause() {  # pause <seconds>, interruptible by the session's SIGTERM
	sleep "$1" &
	wait $!
}
stop_all() {
	[ -z "${pid}" ] && [ -s "${PIDFILE}" ] && pid=$(cat "${PIDFILE}")
	[ -z "${pid}" ] || kill -TERM "${pid}" 2>/dev/null
	wait
	echo "B8 stopped by the session t=${SECONDS}"
	exit 0
}

summary() {  # summary <arm> [log]: the grading line from the page's lines (default ${EVENTS})
	local line n=0 at=- clip=- file=${2:-${EVENTS}}
	[ -f "${file}" ] || : > "${file}"
	while IFS= read -r line; do
		case "${line}" in
			*"B8PAGE "*" loadedmetadata "*"duration="*)
				clip=${line##*duration=}
				clip=${clip%%[!0-9.]*} ;;
			*"B8PAGE "*" ended current="*)
				n=$((n + 1))
				if [ "${at}" = - ]; then
					at=${line##*ended current=}
					at=${at%%[!0-9.]*}
				fi ;;
		esac
	done < "${file}"
	echo "B8 arm=$1 ended=${n} ended_at=${at:--} clip=${clip:--}"
}

arm() {  # arm <name> <clip> <seconds> [page query suffix] [env...]
	local name=$1 clip=$2 secs=$3 extra=$4 rc tee i
	shift 4
	# a clip is a local file, or an http(s) URL played straight from the network (B8_HEVC_CLIP=
	# https://... : a video-sharing site's file served unchanged, e.g. docs/browser/HEVC-PLATFORMS.md)
	local src="file://${clip}"
	case "${clip}" in
		http://* | https://*) src="${clip}" ;;
		*) if [ ! -f "${clip}" ]; then
			echo "B8 arm=${name} SKIP clip ${clip} missing"
			return
		fi ;;
	esac
	echo "B8 arm=${name} start clip=${clip} secs=${secs} env=${*:-none} t=${SECONDS}"
	rm -f "${EVENTS}" "${PIDFILE}" "${RCFILE}"
	# The browser's lines go to the console as before and, through tee, to ${EVENTS} for the
	# grading line (stderr is line-buffered in every browser process). The subshell keeps the
	# browser's pid and exit status: in a pipeline only the last command's status reaches us.
	(
		[ "$#" = 0 ] || export "$@"
		"${BROWSER}" --cpu-rendering --autoplay=allow --size=1000x620 "${PAGE}?src=${src}${extra}" 2>&1 &
		echo "$!" > "${PIDFILE}"
		wait "$!"
		echo "$?" > "${RCFILE}"
	) | tee "${EVENTS}" &
	tee=$!
	pause "${secs}"
	pid=$(cat "${PIDFILE}" 2>/dev/null)
	[ -z "${pid}" ] || kill -TERM "${pid}" 2>/dev/null
	# the browser's exit, then the web process may print a little longer: tee ends when the last
	# writer closes the pipe
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
	echo "B8 arm=${name} end rc=${rc:-?} t=${SECONDS}"
	summary "${name}"
	# the device and the decoder block are free again before the next arm
	pause 5
}

# F=<the value of "name=" in a log line> (no fork: bash runs this per line)
field() {  # field <line> <name>
	local s=" ${1}"
	F=-
	case "${s}" in
		*" $2="*) s=${s#* $2=}; F=${s%% *} ;;
	esac
}

fps10() {  # fps10 <frames> <ms>: frames per second x10, rounded
	[ "$2" -gt 0 ] && F=$((($1 * 10000 + $2 / 2) / $2)) || F=0
}

grade() {  # grade <arm> <raster> <transport> <ahead> <log>
	local line clock mono pres paint m0=-1 p0 d0 m1 p1 d1 prev=-1 raster=- transport=- chain=- pacing=-
	local reports=() i first=-1 lastnz=-1 shown=0
	while IFS= read -r line; do
		case "${line}" in
			*"WPEB-MEDIA "*" stat clock="*)
				field "${line}" clock; clock=${F%%.*}
				field "${line}" mono; mono=${F}
				field "${line}" presented; pres=${F}
				field "${line}" painted; paint=${F}
				case "${clock}${mono}${pres}${paint}" in *[!0-9]*) continue ;; esac
				if [ "${m0}" -lt 0 ]; then
					[ "${clock}" -ge 4 ] && m0=${mono} p0=${pres} d0=${paint} m1=${mono} p1=${pres} d1=${paint}
				elif [ "${pres}" -gt "${prev}" ]; then
					m1=${mono} p1=${pres} d1=${paint}
				fi
				prev=${pres}
				;;
			*"WPEB t="*" present frames="*)
				field "${line}" frames
				case "${F}" in *[!0-9]*) continue ;; esac
				reports+=("${F}")
				;;
			*"WPEB t="*" gpu raster="*)
				field "${line}" raster; raster=${F}
				field "${line}" transport; transport=${F}
				;;
			*"WPEB-WEBKIT swap-chain "*)
				field "${line}" type; chain=${F}
				;;
			*"WPEB-WEBKIT frame-pacing "*)
				field "${line}" ahead; pacing=ahead${F}
				;;
		esac
	done < "$5"
	if [ "${m0}" -lt 0 ] || [ "${m1}" -le "${m0}" ]; then
		echo "B8 arm=$1 raster=$2 transport=$3 ahead=$4 NO-STATS (no WPEB-MEDIA stat lines while playing) took=${raster}/${transport}/${chain}/${pacing}"
		return
	fi
	local ms=$((m1 - m0)) painted presented shownf pct
	fps10 $((d1 - d0)) "${ms}"; painted=$((F / 10)).$((F % 10))
	fps10 $((p1 - p0)) "${ms}"; presented=$((F / 10)).$((F % 10))
	# the reports between the first and the last with frames (start-up and end: partial), PRESENT_SECS each
	for i in "${!reports[@]}"; do
		[ "${reports[i]}" -gt 0 ] || continue
		[ "${first}" -lt 0 ] && first=${i}
		lastnz=${i}
	done
	for ((i = first + 1; i < lastnz; i++)); do
		shown=$((shown + reports[i]))
	done
	[ $((lastnz - first)) -gt 1 ] && fps10 "${shown}" $(((lastnz - first - 1) * PRESENT_SECS * 1000)) || F=0
	shownf=$((F / 10)).$((F % 10))
	[ $((p1 - p0)) -gt 0 ] && pct=$(((d1 - d0) * 100 / (p1 - p0))) || pct=0
	echo "B8 arm=$1 raster=$2 transport=$3 ahead=$4 painted_fps=${painted} presented_fps=${presented} shown_fps=${shownf}" \
		"painted_pct=${pct} window_s=$((ms / 1000)).$((ms % 1000 / 100)) took=${raster}/${transport}/${chain}/${pacing}"
}

transport_arm() {  # transport_arm <name>
	local name=$1 raster=cpu transport=shm ahead=0 log rc
	local args=(--autoplay=allow --size=1000x620 --present-stats="${PRESENT_SECS}")
	case "${name}" in
		shm) ;;
		gpu) raster=gpu ;;
		dmabuf) transport=dmabuf ;;
		both) raster=gpu transport=dmabuf ;;
		shm-ahead) ahead=1 ;;
		dmabuf-ahead) transport=dmabuf ahead=1 ;;
		both-ahead) raster=gpu transport=dmabuf ahead=1 ;;
		*) echo "B8 bad arm ${name}"; return ;;
	esac
	if [ ! -f "${H264}" ]; then
		echo "B8 arm=${name} SKIP clip ${H264} missing"
		return
	fi
	[ "${raster}" = cpu ] && args+=(--cpu-rendering)
	[ "${transport}" = dmabuf ] && args+=(--dmabuf)
	[ "${ahead}" = 1 ] && args+=(--frame-ahead)
	log=/tmp/b8-${name}.log
	echo "B8 arm=${name} start clip=${H264} secs=${ARM_SECS} args=${args[*]} t=${SECONDS}"
	(
		[ "${raster}" = gpu ] && unset WEBKIT_SKIA_ENABLE_CPU_RENDERING
		unset WPE_PHOENIX_FRAME_AHEAD WPE_BROWSER_FRAME_AHEAD WPE_BROWSER_DMABUF
		exec "${BROWSER}" "${args[@]}" "${PAGE}?src=file://${H264}" > "${log}" 2>&1
	) &
	pid=$!
	pause "${ARM_SECS}"
	kill -TERM "${pid}" 2>/dev/null
	wait "${pid}"
	rc=$?
	pid=""
	cat "${log}"
	echo "B8 arm=${name} end rc=${rc} t=${SECONDS}"
	grade "${name}" "${raster}" "${transport}" "${ahead}" "${log}"
	summary "${name}" "${log}"
	pause 5
}

inner() {
	trap stop_all TERM
	echo "B8 start t=${SECONDS} arms=${B8_ARMS} display=${WAYLAND_DISPLAY:-unset}"
	if [ "${B8_MODE}" = transport ]; then
		local a IFS=,
		for a in ${B8_ARMS}; do
			unset IFS
			transport_arm "${a}"
		done
		echo "B8 done t=${SECONDS}"
		return
	fi
	local a IFS=,
	for a in ${B8_ARMS}; do
		unset IFS
		case "${a}" in
			h264) arm h264 "${H264}" 75 "" ;;
			hevc) arm hevc "${HEVC}" 50 "" FFMPEG_RPIVID=1 ;;
			hevc-cpu) arm hevc-cpu "${HEVC}" 50 "" FFMPEG_RPIVID=0 ;;
			seek) arm seek "${H264}" 55 "&seek=10:30" ;;
			loop) arm loop "${HEVC}" 55 "&loop=1" FFMPEG_RPIVID=1 ;;
			again) arm again "${HEVC}" 45 "&again=12" FFMPEG_RPIVID=1 ;;
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
if [ "${1:-}" = transport ]; then
	# 6 arms of ARM_SECS + 5 s after the desktop's ~15 s: 375 s
	export B8_INNER=1 B8_MODE=transport B8_ARMS=${2:-shm,gpu,dmabuf,both,shm-ahead,dmabuf-ahead}
	export HOLD=${B8_HOLD:-420}
else
	export B8_INNER=1 B8_MODE=gate B8_ARMS=${1:-h264,hevc,hevc-cpu,seek}
	export HOLD=${B8_HOLD:-400}
fi
export XFCE_AUTOSTART="/bin/bash=${SELF}"
echo "B8 session hold=${HOLD}s mode=${B8_MODE} arms=${B8_ARMS}"
/bin/bash /bin/xfce-session
echo "B8 end rc=$?"
