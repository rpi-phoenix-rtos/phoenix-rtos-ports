#!/bin/bash
#
# game-window.sh -- a GPU game in a WINDOW on the Wayland desktop (M8). Run it from a terminal
# (foot) or a launcher inside the XFCE session (/bin/xfce-session-2), or from labwc's autostart:
#
#     /bin/bash /bin/game-window.sh <game> [extra game arguments]
#
#   game   quakespasm (qs) | quake2 (q2) | quake3 (q3) | stk | simple-egl (a GL test client)
#
# The -wl clones are SDL 2.30 programs with SDL's Wayland video driver: an xdg-shell window that
# labwc decorates, keyboard + pointer through wl_seat, GL/GLES on the V3D through Mesa's EGL
# wayland platform; the compositor composites the window (GLES2 renderer) or scans it out.
#
# Environment knobs:
#   GAME_W, GAME_H  window size (default 1280x720)
#   GAME_SECS       N = ask the game to quit (SIGTERM = SDL_QUIT, its own clean shutdown) after
#                   N seconds; 0 (default) = run until the window is closed or the game is quit
#   GAME_DELAY      seconds to wait before starting (default 0)
#   GAME_ARGS       arguments that replace the per-game defaults below (one word per argument)
#   WAYLAND_DISPLAY / XDG_RUNTIME_DIR   as set by labwc for its clients; otherwise the first
#                   socket in /tmp/xdg (the session's XDG_RUNTIME_DIR) is used
#   SDL_VIDEODRIVER  default wayland (x11 is not built; KMSDRM would take the whole screen)
#
# While the game runs, $XDG_RUNTIME_DIR/game-window.pid holds its pid (game-window-quit.sh stops
# it that way). Every line of ours starts with "M8 " (grading).
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
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
export XKB_DEFAULT_LAYOUT=${XKB_DEFAULT_LAYOUT:-us}
R="${XDG_RUNTIME_DIR}"

# program, its Wayland app_id (labwc's window rules match it) and its windowed arguments
case "${GAME}" in
	quakespasm|qs)
		GAME=quakespasm; BIN=/usr/bin/quakespasm-wl; APP=quakespasm-wl
		DEF=(-window -width "${W}" -height "${H}") ;;
	quake2|q2)
		# the launcher ram-stages /usr/share/quake2 to /tmp/quake2 and plays demo1; later
		# +set arguments win (yquake2 runs every +set before the first frame)
		GAME=quake2; BIN=/usr/bin/quake2-wl; APP=quake2-wl
		DEF=(+set vid_fullscreen 0 +set r_mode -1 +set r_customwidth "${W}" +set r_customheight "${H}") ;;
	quake3|q3)
		GAME=quake3; BIN=/usr/bin/quake3-wl; APP=quake3-wl
		DEF=(+set r_fullscreen 0 +set r_mode -1 +set r_customWidth "${W}" +set r_customHeight "${H}" +map q3dm1) ;;
	stk)
		# stk-wl drops its --screensize default for ours; --windowed is read after --fullscreen
		BIN=/bin/stk-wl; APP=stk-wl
		DEF=(--windowed "--screensize=${W}x${H}") ;;
	simple-egl)
		# weston-simple-egl (M6, libdrm-phoenix low): the smallest GL client, a control that the
		# compositor shows a GPU client window at all
		BIN=/bin/weston-simple-egl-low; APP=org.freedesktop.weston.simple-egl
		DEF=() ;;
	*)
		echo "M8 game=${GAME} FAIL unknown game (quakespasm|quake2|quake3|stk|simple-egl)"
		exit 2 ;;
esac
export SDL_VIDEO_WAYLAND_WMCLASS="${APP}"
if [ -n "${GAME_ARGS:-}" ]; then
	# shellcheck disable=SC2206
	DEF=(${GAME_ARGS})
fi

if [ ! -x "${BIN}" ]; then
	echo "M8 game=${GAME} FAIL ${BIN} not staged"
	exit 1
fi
if [ -z "${WAYLAND_DISPLAY}" ] || [ ! -e "${R}/${WAYLAND_DISPLAY}" ]; then
	echo "M8 game=${GAME} FAIL no Wayland socket in ${R} (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset}): start the desktop first"
	exit 1
fi
if [ -e "${R}/game-window.pid" ]; then
	echo "M8 game=${GAME} FAIL another game is running (pid $(cat "${R}/game-window.pid" 2>/dev/null))"
	exit 1
fi
[ "${GAME_DELAY:-0}" -gt 0 ] && sleep "${GAME_DELAY}"

echo "M8 game=${GAME} start bin=${BIN} app_id=${APP} window=${W}x${H} secs=${SECS} display=${WAYLAND_DISPLAY} driver=${SDL_VIDEODRIVER} args=${DEF[*]} $*"
"${BIN}" "${DEF[@]}" "$@" &
pid=$!
echo "${pid}" > "${R}/game-window.pid"
echo "${GAME}" > "${R}/game-window.name"
rm -f "${R}/game-window.result"
t0=${SECONDS}
if [ "${SECS}" -gt 0 ]; then
	# a watchdog in the background: SIGTERM once the time is up (SDL turns it into SDL_QUIT)
	( sleep "${SECS}"; [ -e "${R}/game-window.pid" ] && echo "M8 game=${GAME} time up (${SECS} s): SIGTERM" \
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
echo "M8 game=${GAME} exited rc=${rc} (${how}) ran_s=$((SECONDS - t0))"
echo "rc=${rc} ran_s=$((SECONDS - t0))" > "${R}/game-window.result"
rm -f "${R}/game-window.pid"
exit "${rc}"
