#!/bin/bash
#
# game-window.sh -- a GPU game in a WINDOW on the Wayland desktop. The XFCE applications menu
# (Games) runs it; from a terminal of the desktop, or labwc's autostart:
#
#     /bin/bash /bin/game-window.sh <game> [extra game arguments]
#
#   game   quakespasm (qs) | quake2 (q2) | quake3 (q3) | stk, or a preset:
#          quake2-demo  Quake II playing the recorded demo of the pak (q2demo1.dm2)
#          stk-race     SuperTuxKart: an AI race on hacienda (the stk launcher's `race`: four
#                       AI karts, two laps, then the game ends by itself)
#
# The games are the image's only builds of them (quakespasm-drm, quake2, quake3, stk): SDL 2.30
# programs with SDL's Wayland AND KMSDRM video drivers on Mesa's EGL (GBM + Wayland). From psh
# they run full screen on KMS (no compositor socket, SDL falls through to KMSDRM); this script
# asks for the windowed mode instead: SDL_VIDEODRIVER=wayland (an xdg-shell window that labwc
# decorates, keyboard + pointer through wl_seat) and each engine's windowed arguments. The
# compositor composites the window (GLES2 renderer) or scans it out.
#
# Environment knobs:
#   GAME_W, GAME_H  window size (default 1280x720)
#   GAME_SECS       N = ask the game to quit (SIGTERM = SDL_QUIT, its own clean shutdown) after
#                   N seconds; 0 (default) = run until the window is closed or the game is quit
#   GAME_DELAY      seconds to wait before starting (default 0)
#   GAME_ARGS       arguments that replace the per-game defaults below (one word per argument;
#                   psh's `export` cannot give it several: use a preset or the extra arguments)
#   WAYLAND_DISPLAY / XDG_RUNTIME_DIR   as set by labwc for its clients; otherwise the first
#                   socket in /tmp/xdg (the session's XDG_RUNTIME_DIR) is used
#
# While the game runs, $XDG_RUNTIME_DIR/game-window.pid holds its pid (game-window-quit.sh stops
# it that way). Every line of ours starts with "GAME-WINDOW " (grading).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1

GAME=${1:-quakespasm}
[ $# -gt 0 ] && shift
W=${GAME_W:-1280}
H=${GAME_H:-720}
SECS=${GAME_SECS:-0}

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg}
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
	for s in "${XDG_RUNTIME_DIR}"/wayland-[0-9]; do
		[ -e "${s}" ] && WAYLAND_DISPLAY="${s##*/}" && break
	done
fi
export WAYLAND_DISPLAY
export SDL_VIDEODRIVER=wayland
export XKB_DEFAULT_LAYOUT=${XKB_DEFAULT_LAYOUT:-us}
R="${XDG_RUNTIME_DIR}"

# program, its Wayland app_id (labwc's window rules match it) and its windowed arguments
case "${GAME}" in
	quakespasm|qs)
		# the engine itself: its launcher (/usr/bin/quakespasm) asks for 1920x1080 full screen
		# first, and quakespasm takes the FIRST -width/-height
		GAME=quakespasm; BIN=/usr/bin/quakespasm-drm; APP=quakespasm
		DEF=(-window -width "${W}" -height "${H}") ;;
	quake2|q2|quake2-demo)
		# the launcher ram-stages /usr/share/quake2 to /tmp/quake2 and loads the demo1 level
		# (or the caller's +map/+demomap instead); later +set arguments win (yquake2 runs every
		# +set before the first frame)
		BIN=/usr/bin/quake2; APP=quake2
		DEF=(+set vid_fullscreen 0 +set r_mode -1 +set r_customwidth "${W}" +set r_customheight "${H}")
		if [ "${GAME}" = quake2-demo ]; then DEF+=(+demomap q2demo1.dm2); else GAME=quake2; fi ;;
	quake3|q3)
		GAME=quake3; BIN=/usr/bin/quake3; APP=quake3
		DEF=(+set r_fullscreen 0 +set r_mode -1 +set r_customWidth "${W}" +set r_customHeight "${H}" +map q3dm1) ;;
	stk|stk-race)
		# the launcher drops its --screensize default for ours; --windowed is read after --fullscreen
		BIN=/bin/stk; APP=stk
		DEF=(--windowed "--screensize=${W}x${H}")
		[ "${GAME}" = stk-race ] && DEF+=(race) ;;
	*)
		echo "GAME-WINDOW game=${GAME} FAIL unknown game (quakespasm|quake2|quake2-demo|quake3|stk|stk-race)"
		exit 2 ;;
esac
export SDL_VIDEO_WAYLAND_WMCLASS="${APP}"
if [ -n "${GAME_ARGS:-}" ]; then
	# shellcheck disable=SC2206
	DEF=(${GAME_ARGS})
fi

if [ ! -x "${BIN}" ]; then
	echo "GAME-WINDOW game=${GAME} FAIL ${BIN} is not installed"
	exit 1
fi
if [ -z "${WAYLAND_DISPLAY}" ] || [ ! -e "${R}/${WAYLAND_DISPLAY}" ]; then
	echo "GAME-WINDOW game=${GAME} FAIL no Wayland socket in ${R} (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset}): start the desktop first (/bin/xfce-session), or run ${BIN} for full screen"
	exit 1
fi
if [ -e "${R}/game-window.pid" ]; then
	echo "GAME-WINDOW game=${GAME} FAIL another game is running (pid $(cat "${R}/game-window.pid" 2>/dev/null))"
	exit 1
fi
[ "${GAME_DELAY:-0}" -gt 0 ] && sleep "${GAME_DELAY}"

echo "GAME-WINDOW game=${GAME} start bin=${BIN} app_id=${APP} window=${W}x${H} secs=${SECS} display=${WAYLAND_DISPLAY} driver=${SDL_VIDEODRIVER} args=${DEF[*]} $*"
"${BIN}" "${DEF[@]}" "$@" &
pid=$!
echo "${pid}" > "${R}/game-window.pid"
echo "${GAME}" > "${R}/game-window.name"
rm -f "${R}/game-window.result"
t0=${SECONDS}
if [ "${SECS}" -gt 0 ]; then
	# a watchdog in the background: SIGTERM once the time is up (SDL turns it into SDL_QUIT)
	( sleep "${SECS}"; [ -e "${R}/game-window.pid" ] && echo "GAME-WINDOW game=${GAME} time up (${SECS} s): SIGTERM" \
		&& kill -TERM "${pid}" 2>/dev/null ) &
	wd=$!
fi
wait "${pid}"
rc=$?
[ -n "${wd:-}" ] && kill -TERM "${wd}" 2>/dev/null
case "${rc}" in
	0) how="clean exit" ;;
	143) how="killed by SIGTERM (no SDL_QUIT handling)" ;;
	137) how="killed by SIGKILL" ;;
	*) how="exit status ${rc}" ;;
esac
echo "GAME-WINDOW game=${GAME} exited rc=${rc} (${how}) ran_s=$((SECONDS - t0))"
echo "rc=${rc} ran_s=$((SECONDS - t0))" > "${R}/game-window.result"
rm -f "${R}/game-window.pid"
exit "${rc}"
