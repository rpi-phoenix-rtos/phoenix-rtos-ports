#!/bin/bash
#
# game-window-autostart.sh -- the games of the M8 desktop session, one after another, from
# labwc's autostart (/etc/xdg/labwc-xfce-m8/autostart). Not for interactive use: run
# /bin/game-window.sh directly for that.
#
# Environment knobs (psh `export` before /bin/xfce-session-2; labwc passes them on):
#   M8_GAMES   comma-separated list of <game>[:<seconds>] (default quakespasm): each is run
#              through /bin/game-window.sh with GAME_SECS=<seconds> (none = until the session
#              ends), in order; e.g. simple-egl:15,quakespasm
#   M8_DELAY   seconds before the first one (default 15: the panel, the desktop and Thunar
#              load from NFS first)
# The session's LOGOUT_CMD /bin/game-window-quit.sh stops the running game and the list.
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
R=${XDG_RUNTIME_DIR:-/tmp/xdg}
rm -f "${R}/game-window.stop" "${R}/game-window.pid" "${R}/game-window.result"
list=${M8_GAMES:-quakespasm}
echo "M8 autostart games=${list} delay=${M8_DELAY:-15}s display=${WAYLAND_DISPLAY:-unset}"
sleep "${M8_DELAY:-15}"
IFS=, read -r -a items <<< "${list}"
for it in "${items[@]}"; do
	if [ -e "${R}/game-window.stop" ]; then
		echo "M8 autostart stopped before ${it}"
		break
	fi
	g=${it%%:*}
	s=0
	[ "${it}" != "${g}" ] && s=${it#*:}
	GAME_SECS=${s} /bin/bash /bin/game-window.sh "${g}"
	sleep 2
done
echo "M8 autostart done"
