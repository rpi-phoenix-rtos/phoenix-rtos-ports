#!/bin/bash
#
# xfce-autostart.sh -- open programs on the XFCE desktop without keyboard or mouse, in order.
# /bin/xfce-desktop.sh runs it once the panel is up when XFCE_AUTOSTART is set; not for
# interactive use. From psh, before the session:
#
#     export XFCE_AUTOSTART=atril:40,quake3:90,video HOLD=300
#     /bin/bash /bin/xfce-session
#
# XFCE_AUTOSTART is a comma-separated list of <item>[=<arg>][:<seconds>] (psh's `export`
# cannot give a value with spaces, so there are none). An item with <seconds> runs for that
# long, is then closed, and the next item starts; an item without runs until the session ends
# and the next one starts XFCE_AUTOSTART_GAP seconds later.
#
#   atril[=<pdf>]        the PDF reader on <pdf> (default /usr/share/doc/phoenix/sample.pdf);
#   atril-fs[=<pdf>]     the same full screen;  atril-pres[=<pdf>]  as a presentation
#   video[=<clip>]       /bin/video-play in a window (default: VIDEO_CLIP, else the H.264 720p
#                        demo clip); FFPLAY_AUTOKEYS & co. pass through from the environment
#   gtk-video[=<clip>]   the GTK video player
#   quakespasm quake2 quake2-demo quake3 stk stk-race
#                        a game in a window (/bin/game-window.sh; qs, q2, q3 also work)
#   foot                 a terminal;  mc  Midnight Commander in a terminal
#   thunar[=<dir>]       a Thunar window on <dir> (default /)
#   appfinder            the application finder
#   sleep:<seconds>      a pause
#   /<path>[=<arg>]      any program, with at most one argument
#
# Environment knobs: XFCE_AUTOSTART_DELAY seconds before the first item (default 5),
# XFCE_AUTOSTART_GAP seconds between items without <seconds> (default 3). SIGTERM (the
# session's stop) closes what is still open.
#
# Every line of ours starts with "XFCE-AUTOSTART " (grading).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
R=${XDG_RUNTIME_DIR:-/tmp/xdg}
PDF=/usr/share/doc/phoenix/sample.pdf
CLIP=${VIDEO_CLIP:-/usr/share/video-demo/h264-720p30-aac.mp4}
GAP=${XFCE_AUTOSTART_GAP:-3}
pids=()

alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}

stop_all() {
	local p
	echo "XFCE-AUTOSTART stop: closing what is still open"
	# a game: game-window.sh waits for it; SIGTERM is SDL_QUIT, the game's clean exit
	[ -f "${R}/game-window.pid" ] && kill -TERM "$(cat "${R}/game-window.pid")" 2>/dev/null
	for p in "${pids[@]}"; do
		kill -TERM "${p}" 2>/dev/null
	done
	wait
	echo "XFCE-AUTOSTART done (stopped)"
	exit 0
}
trap stop_all TERM

# cmd <item> <arg>: the command of an item, one word per line in the array CMD
cmd() {
	local item=$1 arg=$2
	case "${item}" in
		atril) CMD=(/usr/bin/atril "${arg:-${PDF}}") ;;
		atril-fs) CMD=(/usr/bin/atril --fullscreen "${arg:-${PDF}}") ;;
		atril-pres) CMD=(/usr/bin/atril --presentation "${arg:-${PDF}}") ;;
		video) CMD=(/bin/bash /bin/video-play "${arg:-${CLIP}}") ;;
		gtk-video) CMD=(/usr/bin/gtk-video --autoexit "${arg:-${CLIP}}") ;;
		quakespasm|qs|quake2|q2|quake2-demo|quake3|q3|stk|stk-race) CMD=(/bin/bash /bin/game-window.sh "${item}") ;;
		foot) CMD=(/bin/foot) ;;
		mc) CMD=(/bin/foot -e /bin/mc) ;;
		thunar) CMD=(/bin/thunar "${arg:-/}") ;;
		appfinder) CMD=(/bin/xfce4-appfinder) ;;
		/*) CMD=("${item}"); [ -n "${arg}" ] && CMD+=("${arg}") ;;
		*) return 1 ;;
	esac
	return 0
}

echo "XFCE-AUTOSTART start items=${XFCE_AUTOSTART} delay=${XFCE_AUTOSTART_DELAY:-5}s gap=${GAP}s display=${WAYLAND_DISPLAY:-unset}"
sleep "${XFCE_AUTOSTART_DELAY:-5}" &
wait $!
IFS=, read -r -a items <<< "${XFCE_AUTOSTART}"
for it in "${items[@]}"; do
	[ -n "${it}" ] || continue
	secs=0
	case "${it}" in *:*) secs=${it##*:}; it=${it%:*} ;; esac
	arg=""
	case "${it}" in *=*) arg=${it#*=}; it=${it%%=*} ;; esac
	case "${secs}" in ''|*[!0-9]*) echo "XFCE-AUTOSTART skip ${it}: '${secs}' is not a number of seconds"; continue ;; esac
	if [ "${it}" = sleep ]; then
		echo "XFCE-AUTOSTART sleep ${secs}s t=${SECONDS}"
		sleep "${secs}" &
		wait $!
		continue
	fi
	if ! cmd "${it}" "${arg}"; then
		echo "XFCE-AUTOSTART skip ${it}: unknown item"
		continue
	fi
	if [ ! -e "${CMD[0]}" ] || { [ "${CMD[0]}" = /bin/bash ] && [ ! -e "${CMD[1]}" ]; }; then
		echo "XFCE-AUTOSTART skip ${it}: ${CMD[*]} is not installed"
		continue
	fi
	case "${it}" in
		quakespasm|qs|quake2|q2|quake2-demo|quake3|q3|stk|stk-race)
			# game-window.sh's own watchdog ends the game (SDL_QUIT) after GAME_SECS
			echo "XFCE-AUTOSTART open ${it} secs=${secs} t=${SECONDS}: GAME_SECS=${secs} ${CMD[*]}"
			GAME_SECS=${secs} "${CMD[@]}" &
			p=$!
			pids+=("${p}")
			if [ "${secs}" -gt 0 ]; then
				wait "${p}"
				echo "XFCE-AUTOSTART closed ${it} rc=$? t=${SECONDS}"
				continue
			fi
			;;
		video)
			# ffplay's own -t ends the clip after <seconds> (-autoexit)
			[ "${secs}" -gt 0 ] && CMD+=(-t "${secs}")
			echo "XFCE-AUTOSTART open ${it} secs=${secs} t=${SECONDS}: ${CMD[*]}"
			"${CMD[@]}" &
			p=$!
			pids+=("${p}")
			if [ "${secs}" -gt 0 ]; then
				wait "${p}"
				echo "XFCE-AUTOSTART closed ${it} rc=$? t=${SECONDS}"
				continue
			fi
			;;
		*)
			echo "XFCE-AUTOSTART open ${it} secs=${secs} t=${SECONDS}: ${CMD[*]}"
			"${CMD[@]}" &
			p=$!
			pids+=("${p}")
			if [ "${secs}" -gt 0 ]; then
				i=0
				while alive "${p}" && [ "${i}" -lt "${secs}" ]; do
					sleep 1 &
					wait $!
					i=$((i + 1))
				done
				alive "${p}" && kill -TERM "${p}" 2>/dev/null
				wait "${p}" 2>/dev/null
				echo "XFCE-AUTOSTART closed ${it} rc=$? after ${i}s t=${SECONDS}"
				continue
			fi
			;;
	esac
	sleep "${GAP}" &
	wait $!
done
echo "XFCE-AUTOSTART all items started t=${SECONDS}"
# stay until the session ends, so its SIGTERM can close what is still open
while [ -n "$(jobs -rp)" ]; do
	sleep 5 &
	wait $!
done
echo "XFCE-AUTOSTART done t=${SECONDS}"
