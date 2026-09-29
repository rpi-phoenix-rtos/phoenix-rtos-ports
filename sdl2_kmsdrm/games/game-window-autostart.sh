#!/bin/bash
#
# game-window-autostart.sh -- the games of the games desktop session, one after another, from
# labwc's autostart (/etc/xdg/labwc-xfce-games/autostart). Not for interactive use: the XFCE
# menu entries (Games) and /bin/game-window.sh are.
#
# Environment knobs (psh `export` before /bin/xfce-session; labwc passes them on):
#   GAME_LIST   comma-separated list of <game>[:<seconds>] (default quakespasm): each is run
#               through /bin/game-window.sh with GAME_SECS=<seconds> (none = until the session
#               ends), in order; e.g. quake2:60,stk:90
#   GAME_LIST_DELAY  seconds before the first one (default 15: the panel, the desktop and
#               Thunar load first)
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
list=${GAME_LIST:-quakespasm}
echo "GAME-WINDOW autostart games=${list} delay=${GAME_LIST_DELAY:-15}s display=${WAYLAND_DISPLAY:-unset}"
sleep "${GAME_LIST_DELAY:-15}"
IFS=, read -r -a items <<< "${list}"
for it in "${items[@]}"; do
	if [ -e "${R}/game-window.stop" ]; then
		echo "GAME-WINDOW autostart stopped before ${it}"
		break
	fi
	g=${it%%:*}
	s=0
	[ "${it}" != "${g}" ] && s=${it#*:}
	GAME_SECS=${s} /bin/bash /bin/game-window.sh "${g}"
	sleep 2
done
echo "GAME-WINDOW autostart done"
