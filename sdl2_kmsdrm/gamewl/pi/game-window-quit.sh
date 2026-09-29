#!/bin/bash
#
# game-window-quit.sh -- LOGOUT_CMD of the M8 desktop session (/bin/xfce-session-2 with
# `export LOGOUT_CMD=/bin/game-window-quit.sh`): when HOLD is over, quit the running game the
# clean way first -- SIGTERM, which SDL turns into SDL_QUIT, the event the engines answer with
# their own shutdown -- wait for it to exit, then log out as the panel's Log Out button does.
# psh does not strip quotes, so LOGOUT_CMD must be one word: this script takes no arguments.
# Its output reaches the UART through the session's "XFCE log logout-cmd:" lines.
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
R=${XDG_RUNTIME_DIR:-/tmp/xdg}
: > "${R}/game-window.stop"      # no further game from game-window-autostart.sh
if [ -f "${R}/game-window.pid" ]; then
	pid=$(cat "${R}/game-window.pid")
	name=$(cat "${R}/game-window.name" 2>/dev/null)
	echo "M8 quit: SIGTERM to ${name} pid=${pid}"
	kill -TERM "${pid}"
	i=0
	# game-window.sh removes the pid file once the game has exited
	while [ -f "${R}/game-window.pid" ] && [ "${i}" -lt 30 ]; do
		sleep 1
		i=$((i + 1))
	done
	if [ -f "${R}/game-window.pid" ]; then
		echo "M8 quit: ${name} still running after ${i}s: SIGKILL"
		kill -KILL "${pid}" 2>/dev/null
		sleep 2
	fi
	echo "M8 quit: ${name} gone after ${i}s result=$(cat "${R}/game-window.result" 2>/dev/null || echo none)"
else
	echo "M8 quit: no game running"
fi
/usr/lib/xfce-demo/bin/loginctl terminate-session
echo "M8 quit: logout requested rc=$?"
