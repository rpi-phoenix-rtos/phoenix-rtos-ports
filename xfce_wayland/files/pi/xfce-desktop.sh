#!/bin/bash
#
# xfce-desktop.sh -- run ON THE Pi at the psh prompt:
#     /bin/bash /bin/xfce-desktop.sh <session> [input|noinput]
#   session  thunar | xfce | none
#            thunar: labwc (without autostart) + Thunar started here, showing /   (m7f-thunar)
#            xfce:   labwc with the XFCE autostart (/etc/xdg/labwc-xfce/autostart: xfdesktop +
#                    xfce4-panel) and Thunar started here once the panel is up    (m7h-xfce)
#            none:   the session bus + xfconfd + labwc only
#   input    input (default: libinput-phoenix opens /dev/kbd0 + /dev/mouse0) | noinput
#
# XFCE 4.20 on labwc, the M7 showcase (docs/gpu-new-lane/M7-wayland-desktop.md). xfwm4 is
# X11-only, so labwc is the window manager; the XFCE programs are GTK 3 Wayland clients
# (the panel and the desktop on wlr-layer-shell). psh has no '&' and no ';': this script
# does the job control:
#   1  dbus-daemon (session bus, /etc/dbus-1/session-phoenix.conf: ANONYMOUS on
#      unix:path=/tmp/dbus-session) in the background, wait for its socket
#   2  xfconfd: first by BUS ACTIVATION (/usr/share/dbus-1/services/org.xfce.Xfconf.service,
#      triggered by a ListChannels call); if that fails, started here directly
#   3  an xfconf write/read round trip through xfconf-query, and the staged Thunar
#      defaults (/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/thunar.xml) read back
#   4  labwc (-C /etc/xdg/labwc-xfce), wait for its socket
#   5  the session's clients; hold with heartbeats that also list the org.xfce.* names
#      on the bus (which XFCE programs registered), until HOLD seconds are over, a logout is
#      requested (the file $XFCE_LOGOUT_FLAG appears: the panel's Log Out through the
#      xfce-demo `loginctl` stand-in) or labwc exits
#   6  stop: `thunar --quit`; `xfce4-panel --quit` and `xfdesktop --quit` (over the bus: the
#      panel saves its layout); SIGTERM to an autostarted foot (its pid in
#      $XDG_RUNTIME_DIR/foot.pid), labwc, xfconfd (its pid from the bus, however it was
#      started: it saves its channels and leaves quietly while the bus is still up), the bus;
#      then the programs' own logs (the autostarted ones write to /tmp/xfce-logs/)
#
# Preconditions: the GPU servers, started at boot: rpi4-v3d-async (render), rpi4-kms -C (display;
# -C hands the console keyboard to labwc), shmsrv (memfd_create/shm_open backing: wl_shm pools,
# keymaps). /bin/xfce-session checks them and runs this script.
#
# Environment knobs: RENDERER (pixman | gles2, default pixman), HOLD (seconds with the
# session up, default 60; 0 = until Log Out or labwc exits), LABWC, CONF_DIR (default
# /etc/xdg/labwc-xfce), XFCE_BIN (a directory of XFCE programs put first on PATH; default none),
# THUNAR / PANEL / XFDESKTOP (default /bin/thunar, /bin/xfce4-panel, /bin/xfdesktop: the
# programs this script starts or asks to quit), THUNAR_DIR (default /), THUNAR_START (0 = no
# Thunar window at start), THUNAR_SECOND (a directory: once the session is up, open it with a
# SECOND `thunar <dir>`, which forwards its command line to the running Thunar over the bus),
# XFCE_CONFIG_DIRS / XFCE_DATA_DIRS (XDG_CONFIG_DIRS / XDG_DATA_DIRS, default /etc/xdg,
# /usr/share), XFCE_HOME (the root of XDG_CONFIG_HOME & co., default /tmp/xfce-home),
# LOGOUT_CMD (run when HOLD is over, default: create $XFCE_LOGOUT_FLAG), TZ (passed through:
# GLib's clock and dates honour it, libphoenix's localtime() does not yet),
# XFCE_AUTOSTART (xfce session: programs to open once the panel is up, by
# /bin/xfce-autostart.sh, which the session's stop closes), XFCONFD (default
# /usr/lib/xfce4/xfconf/xfconfd), ACTIVATION (0 = skip bus activation, start
# xfconfd directly), VERBOSE (labwc -V, default 1), G_DEBUG / G_MESSAGES_DEBUG (passed
# through), GDBUS_DEBUG (1 = G_DBUS_DEBUG=authentication for the XFCE programs).
#
# Every line of ours starts with "XFCE " (grading). GTK/GLib messages look like
# "(thunar:12): Gtk-WARNING **: 12:00:00.000: ..."; wlroots lines "00:00:01.234 [file.c:1] ...".
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

SESSION=${1:-thunar}
INPUT=${2:-input}
RENDERER=${RENDERER:-pixman}
HOLD=${HOLD:-60}
LABWC=${LABWC:-/bin/labwc}
CONF_DIR=${CONF_DIR:-/etc/xdg/labwc-xfce}
THUNAR=${THUNAR:-/bin/thunar}
THUNAR_DIR=${THUNAR_DIR:-/}
THUNAR_START=${THUNAR_START:-1}
THUNAR_SECOND=${THUNAR_SECOND:-}
PANEL=${PANEL:-/bin/xfce4-panel}
XFDESKTOP=${XFDESKTOP:-/bin/xfdesktop}
LOGOUT_CMD=${LOGOUT_CMD:-}
XFCONFD=${XFCONFD:-/usr/lib/xfce4/xfconf/xfconfd}
XFCONF_QUERY=${XFCONF_QUERY:-/bin/xfconf-query}
DAEMON=${DAEMON:-/bin/dbus-daemon}
SEND=${SEND:-/bin/dbus-send}
BUS_CONF=${BUS_CONF:-/etc/dbus-1/session-phoenix.conf}
VERBOSE=${VERBOSE:-1}
SOCK=/tmp/dbus-session
LOGS=/tmp/xfce-logs

export HOME=/root
export PATH=${XFCE_BIN:+${XFCE_BIN}:}/bin:/usr/bin
export XDG_RUNTIME_DIR=/tmp/xdg
export XDG_CONFIG_DIRS=${XFCE_CONFIG_DIRS:-/etc/xdg}   # (knob: the host test points it elsewhere)
export XDG_DATA_DIRS=${XFCE_DATA_DIRS:-/usr/share}
# the panel's Log Out (through the xfce-demo loginctl stand-in) creates this file
export XFCE_LOGOUT_FLAG=${XDG_RUNTIME_DIR}/xfce-logout
# the programs write their settings, caches and state here, not on the NFS root
XFCE_HOME=${XFCE_HOME:-/tmp/xfce-home}
export XDG_CONFIG_HOME=${XFCE_HOME}/config
export XDG_CACHE_HOME=${XFCE_HOME}/cache
export XDG_DATA_HOME=${XFCE_HOME}/data
export XDG_CURRENT_DESKTOP=XFCE
export XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=unix:path=${SOCK}
export GDK_BACKEND=wayland
export GSETTINGS_BACKEND=memory
export GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas
export NO_AT_BRIDGE=1
[ "${GDBUS_DEBUG:-0}" = 1 ] && export G_DBUS_DEBUG=authentication
export LIBSEAT_BACKEND=noop
export WLR_BACKENDS=drm,libinput
# wlroots refuses to start the libinput backend with no devices (m7a-labwc: "libinput initialization
# failed, no input devices"): allow zero devices for noinput runs and for a keyboard the console still holds.
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=${RENDERER}
export WLR_DRM_DEVICES=/dev/dri/card0
export WLR_NO_HARDWARE_CURSORS=1
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
export XKB_DEFAULT_LAYOUT=us
export TERM=xterm-256color
if [ "${INPUT}" = noinput ]; then
	export LIBINPUT_PHOENIX_DEVICES=
else
	export LIBINPUT_PHOENIX_DEVICES=/dev/kbd0:keyboard,/dev/mouse0:mouse
fi

case "${SESSION}" in
	thunar|xfce|none) ;;
	*) echo "XFCE FAIL unknown session ${SESSION}"; exit 2 ;;
esac
case "${RENDERER}" in
	pixman|gles2) ;;
	*) echo "XFCE FAIL unknown renderer ${RENDERER}"; exit 2 ;;
esac

alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}

# print FILE's first N lines as "XFCE log NAME: ..." (bash only)
show_log() {
	local name="$1" f="$2" max="${3:-40}" n=0 line
	[ -f "${f}" ] || { echo "XFCE log ${name}: (no log)"; return; }
	while IFS= read -r line && [ "${n}" -lt "${max}" ]; do
		echo "XFCE log ${name}: ${line}"
		n=$((n + 1))
	done < "${f}"
	echo "XFCE log ${name}: -- ${n} line(s) shown"
}

# the org.xfce.* names owned on the bus, comma-separated (dbus-send ListNames)
bus_names() {
	local out line names=""
	out="$("${SEND}" --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
		org.freedesktop.DBus.ListNames 2>/dev/null)" || { echo "error"; return; }
	while IFS= read -r line; do
		case "${line}" in
			*'"org.xfce.'*) line="${line#*\"}"; names="${names}${line%\"*},";;
		esac
	done <<< "${out}"
	echo "${names%,}"
}

has_name() {
	case ",$(bus_names)," in *",$1,"*) return 0 ;; esac
	return 1
}

# the pid of the process owning bus name $1 (GetConnectionUnixProcessID: the daemon's
# SO_PEERCRED), or nothing
bus_pid() {
	local out line
	out="$("${SEND}" --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
		org.freedesktop.DBus.GetConnectionUnixProcessID "string:$1" 2>/dev/null)" || return 0
	while IFS= read -r line; do
		case "${line}" in
			*'uint32 '*) line="${line##*uint32 }"; echo "${line%% *}"; return 0 ;;
		esac
	done <<< "${out}"
}

# bounded LOG SECS CMD...: run CMD (stdout+stderr to LOG), at most SECS seconds; its exit
# status, or 124 when it had to be killed. (The remote-instance calls below wait for a bus
# reply with no timeout of their own.)
bounded() {
	local log="$1" secs="$2" p i=0
	shift 2
	"$@" > "${log}" 2>&1 &
	p=$!
	while alive "${p}" && [ "${i}" -lt "${secs}" ]; do
		sleep 1
		i=$((i + 1))
	done
	if alive "${p}"; then
		kill -TERM "${p}" 2>/dev/null
		wait "${p}" 2>/dev/null
		return 124
	fi
	wait "${p}"
}

mkdir -p "${XDG_RUNTIME_DIR}" "${XDG_CONFIG_HOME}" "${XDG_CACHE_HOME}" "${XDG_DATA_HOME}" "${LOGS}" 2>/dev/null
chmod 700 "${XDG_RUNTIME_DIR}" 2>/dev/null
rm -f "${XDG_RUNTIME_DIR}"/wayland-* "${XDG_RUNTIME_DIR}/foot.pid" "${SOCK}" "${LOGS}"/*.log "${XFCE_LOGOUT_FLAG}" 2>/dev/null

staged=""
progs="${THUNAR}"
[ "${SESSION}" = xfce ] && progs="${progs} ${PANEL} ${XFDESKTOP}"
for f in "${DAEMON}" "${SEND}" "${BUS_CONF}" "${XFCONFD}" "${XFCONF_QUERY}" "${LABWC}" ${progs} \
		/usr/share/dbus-1/services/org.xfce.Xfconf.service /etc/xdg/xfce4/xfconf/xfce-perchannel-xml/thunar.xml \
		/usr/share/icons/Adwaita/icon-theme.cache /usr/share/icons/hicolor/icon-theme.cache /usr/share/mime/mime.cache \
		/usr/share/glib-2.0/schemas/gschemas.compiled /etc/xdg/gtk-3.0/settings.ini; do
	[ -e "${f}" ] || staged="${staged}${f},"
done
echo "XFCE start session=${SESSION} renderer=${RENDERER} input=${LIBINPUT_PHOENIX_DEVICES:-none} hold=${HOLD} conf=${CONF_DIR} labwc=${LABWC} thunar=${THUNAR} missing=${staged:-none} t=${SECONDS}"
echo "XFCE env PATH=${PATH} XDG_CONFIG_DIRS=${XDG_CONFIG_DIRS} XDG_DATA_DIRS=${XDG_DATA_DIRS} TZ=${TZ:-unset}"

# --- 1: the session bus ---------------------------------------------------------------------
"${DAEMON}" --config-file="${BUS_CONF}" --nofork > "${LOGS}/dbus-daemon.log" 2>&1 &
dpid=$!
i=0
while [ ! -e "${SOCK}" ] && [ "${i}" -lt 60 ] && alive "${dpid}"; do
	sleep 1
	i=$((i + 1))
	[ $((i % 10)) -eq 0 ] && echo "XFCE waiting for the bus socket t=${SECONDS} waited=${i}s"
done
if [ ! -e "${SOCK}" ]; then
	alive "${dpid}" && state=running || state=exited
	echo "XFCE dbus=missing wait_s=${i} daemon=${state}"
	show_log dbus-daemon "${LOGS}/dbus-daemon.log"
	[ "${state}" = running ] && kill -TERM "${dpid}" 2>/dev/null
	echo "XFCE done"
	exit 1
fi
echo "XFCE dbus=up pid=${dpid} wait_s=${i} address=${DBUS_SESSION_BUS_ADDRESS} t=${SECONDS}"

# --- 2: xfconfd -----------------------------------------------------------------------------
xpid=""
via=failed
if [ "${ACTIVATION:-1}" = 1 ]; then
	s0=${SECONDS}
	"${SEND}" --session --print-reply --reply-timeout=30000 --dest=org.xfce.Xfconf /org/xfce/Xfconf \
		org.xfce.Xfconf.ListChannels > "${LOGS}/activation.out" 2>&1
	rc=$?
	echo "XFCE xfconfd activation rc=${rc} took_s=$((SECONDS - s0)) t=${SECONDS}"
	[ "${rc}" = 0 ] || show_log activation "${LOGS}/activation.out" 5
	has_name org.xfce.Xfconf && via=activation
fi
if [ "${via}" != activation ]; then
	"${XFCONFD}" > "${LOGS}/xfconfd.log" 2>&1 &
	xpid=$!
	i=0
	while ! has_name org.xfce.Xfconf && [ "${i}" -lt 30 ] && alive "${xpid}"; do
		sleep 1
		i=$((i + 1))
	done
	has_name org.xfce.Xfconf && via=explicit
	echo "XFCE xfconfd started pid=${xpid} wait_s=${i} t=${SECONDS}"
fi
echo "XFCE xfconfd via=${via} names=$(bus_names) t=${SECONDS}"

# --- 3: xfconf round trip + the staged defaults ------------------------------------------------
"${XFCONF_QUERY}" -c xfce-phx-probe -p /probe -n -t string -s "hello-${SECONDS}" > "${LOGS}/xq-set.out" 2>&1
rc_set=$?
got="$("${XFCONF_QUERY}" -c xfce-phx-probe -p /probe 2>&1)"
rc_get=$?
thumb="$("${XFCONF_QUERY}" -c thunar -p /misc-thumbnail-mode 2>&1)"
rc_thumb=$?
echo "XFCE xfconf set_rc=${rc_set} get_rc=${rc_get} value=${got} thunar_thumbnail_mode=${thumb} (rc ${rc_thumb}) t=${SECONDS}"
[ "${rc_set}" = 0 ] || show_log xfconf-query "${LOGS}/xq-set.out" 5
# (-l lists the channels xfconfd finds on disk: the staged defaults and saved ones; a new
# channel appears only after xfconfd has written it, lazily)
chans=""
while IFS= read -r l; do
	l="${l#"${l%%[! ]*}"}"   # leading blanks
	case "${l}" in Channels:*|"") ;; *) chans="${chans}${l},"; esac
done <<< "$("${XFCONF_QUERY}" -l 2>&1)"
echo "XFCE xfconf channels=${chans%,}"

# --- 4: labwc ---------------------------------------------------------------------------------
conf="${CONF_DIR}"
if [ "${SESSION}" != xfce ]; then
	conf=/tmp/labwc-xfce-conf
	rm -rf "${conf}"
	mkdir -p "${conf}"
	for f in rc.xml menu.xml environment; do
		[ -f "${CONF_DIR}/${f}" ] && cp "${CONF_DIR}/${f}" "${conf}/${f}"
	done
fi
have=""
for f in rc.xml menu.xml autostart environment; do
	[ -f "${conf}/${f}" ] && have="${have}${f},"
done
case "${VERBOSE}" in 0) vflag="" ;; 2) vflag="-d" ;; *) vflag="-V" ;; esac
echo "XFCE labwc start conf=${conf} files=${have%,} t=${SECONDS}"
"${LABWC}" ${vflag} -C "${conf}" &
lpid=$!
sock=""
i=0
while [ -z "${sock}" ] && [ "${i}" -lt 90 ] && alive "${lpid}"; do
	for s in "${XDG_RUNTIME_DIR}"/wayland-[0-9]; do
		[ -e "${s}" ] && sock="${s##*/}"
	done
	[ -n "${sock}" ] && break
	sleep 1
	i=$((i + 1))
	[ $((i % 10)) -eq 0 ] && echo "XFCE waiting for the labwc socket t=${SECONDS} waited=${i}s"
done
if [ -n "${sock}" ]; then
	echo "XFCE labwc socket=up name=${sock} pid=${lpid} wait_s=${i} t=${SECONDS}"
	export WAYLAND_DISPLAY="${sock}"
else
	alive "${lpid}" && state=running || state=exited
	echo "XFCE labwc socket=missing wait_s=${i} labwc=${state} t=${SECONDS}"
fi

# --- 5: the session ---------------------------------------------------------------------------
tpid=""
apid=""
start_thunar() {
	echo "XFCE thunar start: ${THUNAR} ${THUNAR_DIR} t=${SECONDS}"
	"${THUNAR}" "${THUNAR_DIR}" &
	tpid=$!
	echo "XFCE thunar pid=${tpid}"
}
if [ -n "${sock}" ]; then
	case "${SESSION}" in
		thunar) start_thunar ;;
		xfce)
			# the panel and the desktop come from labwc's autostart; Thunar once the panel
			# registered on the bus (or after 60 s)
			i=0
			while ! has_name org.xfce.Panel && [ "${i}" -lt 60 ] && alive "${lpid}"; do
				sleep 5
				i=$((i + 5))
				echo "XFCE waiting for the panel t=${SECONDS} waited=${i}s names=$(bus_names)"
			done
			echo "XFCE session up panel=$(has_name org.xfce.Panel && echo registered || echo missing) t=${SECONDS}"
			[ "${THUNAR_START}" = 1 ] && start_thunar
			if [ -n "${XFCE_AUTOSTART:-}" ]; then
				/bin/bash /bin/xfce-autostart.sh &
				apid=$!
				echo "XFCE autostart pid=${apid} items=${XFCE_AUTOSTART} t=${SECONDS}"
			fi
			;;
	esac
fi

# A second Thunar: GApplication forwards its command line to the running instance over the
# bus (org.gtk.Application.CommandLine) and exits; the running Thunar opens a new window.
# (Only once the first one owns org.xfce.Thunar: started earlier, the second would become
# the primary itself.)
if [ -n "${sock}" ] && [ -n "${THUNAR_SECOND}" ]; then
	i=0
	while ! has_name org.xfce.Thunar && [ "${i}" -lt 60 ]; do
		sleep 2
		i=$((i + 2))
	done
	sleep 3
	s0=${SECONDS}
	bounded "${LOGS}/thunar-second.log" 30 "${THUNAR}" "${THUNAR_SECOND}"
	echo "XFCE thunar second instance dir=${THUNAR_SECOND} rc=$? took_s=$((SECONDS - s0)) waited_for_name_s=${i} names=$(bus_names) t=${SECONDS}"
fi

# Hold until HOLD seconds are over (then LOGOUT_CMD, or the logout file directly), the
# panel's Log Out, or labwc's own exit (its root menu's Exit).
held=0
why=""
while [ -n "${sock}" ] && [ -z "${why}" ]; do
	sleep 5
	held=$((held + 5))
	if [ -e "${XFCE_LOGOUT_FLAG}" ]; then why=logout; break; fi
	if ! alive "${lpid}"; then why=labwc-exited; break; fi
	# a heartbeat every 10 s with a HOLD, every 60 s in an open-ended session
	if [ $((held % ( HOLD > 0 ? 10 : 60 ) )) -eq 0 ]; then
		t=none
		[ -n "${tpid}" ] && { alive "${tpid}" && t=running || t=exited; }
		echo "XFCE hold t=${SECONDS} held=${held}s labwc=running thunar=${t} names=$(bus_names)"
	fi
	if [ "${HOLD}" -gt 0 ] && [ "${held}" -ge "${HOLD}" ]; then
		if [ -n "${LOGOUT_CMD}" ]; then
			${LOGOUT_CMD} > "${LOGS}/logout-cmd.log" 2>&1
			echo "XFCE hold over: ${LOGOUT_CMD} rc=$? t=${SECONDS}"
		else
			: > "${XFCE_LOGOUT_FLAG}"
		fi
		[ -e "${XFCE_LOGOUT_FLAG}" ] && why=logout || why=hold
	fi
done
[ -n "${sock}" ] && echo "XFCE session end reason=${why} held=${held}s t=${SECONDS}"

# --- 6: stop ----------------------------------------------------------------------------------
lup=0
alive "${lpid}" && lup=1
if [ -n "${apid}" ] && alive "${apid}"; then
	# XFCE_AUTOSTART's programs first, while their display is still there
	kill -TERM "${apid}" 2>/dev/null
	i=0
	while alive "${apid}" && [ "${i}" -lt 20 ]; do
		sleep 1
		i=$((i + 1))
	done
	alive "${apid}" && kill -KILL "${apid}" 2>/dev/null
	wait "${apid}" 2>/dev/null
	echo "XFCE autostart stopped rc=$? after_term_s=${i} t=${SECONDS}"
fi
if [ -n "${tpid}" ]; then
	# Thunar saves nothing on SIGTERM; --quit asks the running instance (a remote command
	# line, as THUNAR_SECOND). Without a display the remote cannot start: TERM then.
	rc_q=skipped
	if [ "${lup}" = 1 ]; then
		bounded "${LOGS}/thunar-quit.log" 20 "${THUNAR}" --quit
		rc_q=$?
		i=0
		while alive "${tpid}" && [ "${i}" -lt 10 ]; do
			sleep 1
			i=$((i + 1))
		done
	fi
	alive "${tpid}" && kill -TERM "${tpid}" 2>/dev/null
	wait "${tpid}" 2>/dev/null
	echo "XFCE thunar exited rc=$? quit_rc=${rc_q} t=${SECONDS}"
fi
# The panel and the desktop are labwc's autostart children (no pid here). GTK programs
# abort when their display goes away, and an aborted panel saves nothing: ask them to
# quit over the bus first (each --quit is one more short-lived instance of the program).
if [ "${SESSION}" = xfce ] && [ -n "${sock}" ] && [ "${lup}" = 0 ]; then
	echo "XFCE quit skipped: labwc is gone (the panel and the desktop lost their display) names=$(bus_names) t=${SECONDS}"
elif [ "${SESSION}" = xfce ] && [ -n "${sock}" ]; then
	bounded "${LOGS}/panel-quit.log" 20 "${PANEL}" --quit
	rc_p=$?
	bounded "${LOGS}/xfdesktop-quit.log" 20 "${XFDESKTOP}" --quit
	rc_d=$?
	i=0
	while { has_name org.xfce.Panel || has_name org.xfce.xfdesktop; } && [ "${i}" -lt 15 ]; do
		sleep 1
		i=$((i + 1))
	done
	echo "XFCE quit panel_rc=${rc_p} xfdesktop_rc=${rc_d} wait_s=${i} names=$(bus_names) t=${SECONDS}"
fi
# A terminal the autostart opened (labwc's child, not ours: its pid file): before labwc, or
# it logs Broken-pipe errors when its display goes away
if [ -f "${XDG_RUNTIME_DIR}/foot.pid" ]; then
	fpid="$(cat "${XDG_RUNTIME_DIR}/foot.pid" 2>/dev/null)"
	rm -f "${XDG_RUNTIME_DIR}/foot.pid"
	if [ -n "${fpid}" ] && kill -TERM "${fpid}" 2>/dev/null; then
		i=0
		while kill -0 "${fpid}" 2>/dev/null && [ "${i}" -lt 10 ]; do
			sleep 1
			i=$((i + 1))
		done
		echo "XFCE foot stopped pid=${fpid} after_term_s=${i} t=${SECONDS}"
	fi
fi
if alive "${lpid}"; then
	kill -TERM "${lpid}" 2>/dev/null
	i=0
	while alive "${lpid}" && [ "${i}" -lt 15 ]; do
		sleep 1
		i=$((i + 1))
	done
	if alive "${lpid}"; then
		echo "XFCE labwc still up ${i}s after TERM: sending KILL"
		kill -KILL "${lpid}" 2>/dev/null
		sleep 2
	fi
	wait "${lpid}" 2>/dev/null
	rc=$?
	left=gone
	[ -n "${sock}" ] && [ -e "${XDG_RUNTIME_DIR}/${sock}" ] && left=left
	echo "XFCE labwc exited rc=${rc} after_term_s=${i} socket=${left} t=${SECONDS}"
else
	wait "${lpid}" 2>/dev/null
	echo "XFCE labwc had exited rc=$? t=${SECONDS}"
fi
sleep 3   # autostarted programs notice the display is gone
echo "XFCE after labwc names=$(bus_names) t=${SECONDS}"
# xfconfd before the bus: TERM makes it save and exit; losing the bus first makes it warn
# ("Name org.xfce.Xfconf lost on the message dbus"). Started by activation, it is the
# daemon's child: its pid comes from the bus, and the name going away says it is gone.
xfpid="${xpid}"
[ -n "${xfpid}" ] || xfpid="$(bus_pid org.xfce.Xfconf)"
if [ -n "${xfpid}" ]; then
	kill -TERM "${xfpid}" 2>/dev/null
	i=0
	while has_name org.xfce.Xfconf && [ "${i}" -lt 10 ]; do
		sleep 1
		i=$((i + 1))
	done
	rc=-
	if [ -n "${xpid}" ]; then
		wait "${xpid}" 2>/dev/null
		rc=$?
	fi
	has_name org.xfce.Xfconf && name=left || name=released
	echo "XFCE xfconfd exited pid=${xfpid} via=${via} rc=${rc} after_term_s=${i} name=${name} t=${SECONDS}"
elif has_name org.xfce.Xfconf; then
	echo "XFCE xfconfd pid=unknown (GetConnectionUnixProcessID failed): it stops with the bus t=${SECONDS}"
fi
kill -TERM "${dpid}" 2>/dev/null
i=0
while alive "${dpid}" && [ "${i}" -lt 10 ]; do
	sleep 1
	i=$((i + 1))
done
wait "${dpid}" 2>/dev/null
rc=$?
[ -e "${SOCK}" ] && left=left || left=gone
echo "XFCE dbus exited rc=${rc} after_term_s=${i} socket=${left} t=${SECONDS}"
for f in "${LOGS}"/*.log; do
	[ -e "${f}" ] || continue
	n="${f##*/}"
	show_log "${n%.log}" "${f}" 40
done
# settings written by xfconfd (per-channel XML under XDG_CONFIG_HOME)
xmls=""
for f in "${XDG_CONFIG_HOME}"/xfce4/xfconf/xfce-perchannel-xml/*.xml; do
	[ -e "${f}" ] && xmls="${xmls}${f##*/},"
done
echo "XFCE saved channels=${xmls%,}"
echo "XFCE done"
