#!/bin/bash
#
# dbus-m7f.sh -- run ON THE Pi at the psh prompt:
#     /bin/bash /bin/dbus-m7f.sh [anon|external]
#   anon      /etc/dbus-1/session-phoenix.conf           (ANONYMOUS only; stage 1, default)
#   external  /etc/dbus-1/session-phoenix-external.conf  (EXTERNAL then ANONYMOUS: on today's
#             kernel EXTERNAL must be REJECTED and both clients must fall back to ANONYMOUS)
#
# Pre-registered cycle m7f-dbus (docs/gpu-new-lane/M7-wayland-desktop.md, "D-Bus session
# bus"). psh has no '&', ';' or '|', so this script does the job control, as weston-m6a.sh
# does: it starts dbus-daemon in the background (--nofork), waits for its socket, runs the
# clients, stops the daemon with SIGTERM and prints a summary.
#
#   1  dbus-send  ListNames                  (libdbus method call + reply)
#   2  dbus-send  Peer.Ping                  (the bus driver answers)
#   3  dbus-send  GetConnectionCredentials   (the bus reports its own pid)
#   4  dbus-monitor + dbus-send signal       (the bus routes a signal between two clients)
#   5  gdbus call ListNames                  (GIO's GDBus, if ${GDBUS} exists: skipped otherwise)
#   6  auth summary from the daemon's DBUS_VERBOSE log (anonymous / external / no-credentials)
#
# Environment knobs: DAEMON, SEND, MONITOR, GDBUS (binaries), CONF (overrides the arm),
# CLIENT_VERBOSE=1 (dbus-send's own DBUS_VERBOSE to a file, "Trying mechanism" counted).
#
# Every line of ours starts with "DBUSPHX " (grading).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

ARM=${1:-anon}
DAEMON=${DAEMON:-/bin/dbus-daemon}
SEND=${SEND:-/bin/dbus-send}
MONITOR=${MONITOR:-/bin/dbus-monitor}
GDBUS=${GDBUS:-/bin/gdbus}
case "${ARM}" in
	anon) CONF=${CONF:-/etc/dbus-1/session-phoenix.conf} ;;
	external) CONF=${CONF:-/etc/dbus-1/session-phoenix-external.conf} ;;
	*) echo "DBUSPHX FAIL unknown arm ${ARM}"; exit 2 ;;
esac

SOCK=/tmp/dbus-session
LOG=/tmp/dbus-m7f.daemon.log
ADDR=/tmp/dbus-m7f.addr
MON=/tmp/dbus-m7f.monitor.out
export HOME=/root
export PATH=/bin
export DBUS_SESSION_BUS_ADDRESS=unix:path=${SOCK}

# count lines of FILE containing TEXT (bash only: no grep dependency)
count() {
	local n=0 line
	[ -f "$1" ] || { echo 0; return; }
	while IFS= read -r line; do
		case "${line}" in *"$2"*) n=$((n + 1)) ;; esac
	done < "$1"
	echo "${n}"
}

alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}

rm -f "${SOCK}" "${LOG}" "${ADDR}" "${MON}" /tmp/dbus-m7f.*.out /tmp/dbus-m7f.*.log
echo "DBUSPHX start arm=${ARM} conf=${CONF} daemon=${DAEMON} address=${DBUS_SESSION_BUS_ADDRESS} t=${SECONDS}"
[ -f "${CONF}" ] || { echo "DBUSPHX FAIL no ${CONF}"; echo "DBUSPHX done"; exit 1; }

# The daemon's verbose trace goes to a file (it would flood the UART): step 6 reads it.
DBUS_VERBOSE=1 "${DAEMON}" --config-file="${CONF}" --nofork --print-address=1 > "${ADDR}" 2> "${LOG}" &
dpid=$!
echo "DBUSPHX daemon pid=${dpid}"

i=0
while [ ! -e "${SOCK}" ] && [ "${i}" -lt 60 ] && alive "${dpid}"; do
	sleep 1
	i=$((i + 1))
	[ $((i % 10)) -eq 0 ] && echo "DBUSPHX waiting for the socket t=${SECONDS} waited=${i}s"
done
if [ ! -e "${SOCK}" ]; then
	alive "${dpid}" && state=running || state=exited
	echo "DBUSPHX socket=missing wait_s=${i} daemon=${state} t=${SECONDS}"
	if [ "${state}" = exited ]; then
		wait "${dpid}"
		echo "DBUSPHX daemon exited rc=$? before its socket appeared; last log lines:"
		n=0
		while IFS= read -r line; do n=$((n + 1)); [ "${n}" -le 40 ] && echo "DBUSPHX log: ${line}"; done < "${LOG}"
	else
		kill -TERM "${dpid}" 2>/dev/null
	fi
	echo "DBUSPHX done"
	exit 1
fi
sleep 1  # --print-address is written right after the socket appears
read -r addr < "${ADDR}"
echo "DBUSPHX socket=up wait_s=${i} printed_address=${addr:-none} t=${SECONDS}"

sv() {  # label args... : dbus-send, reply to the UART, rc summarised
	local label="$1" rc
	shift
	if [ "${CLIENT_VERBOSE:-0}" = 1 ]; then
		DBUS_VERBOSE=1 "${SEND}" --session --print-reply "$@" 2> "/tmp/dbus-m7f.${label}.log"
		rc=$?
		echo "DBUSPHX ${label} client_mechanisms_tried=$(count "/tmp/dbus-m7f.${label}.log" 'Trying mechanism')"
	else
		"${SEND}" --session --print-reply "$@"
		rc=$?
	fi
	echo "DBUSPHX ${label} rc=${rc} t=${SECONDS}"
}

# 1-3: method calls on the bus driver
sv listnames --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames
sv ping --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.Peer.Ping
sv busid --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId
sv creds --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetConnectionCredentials \
	string:org.freedesktop.DBus

# 4: client-to-client routing: dbus-monitor sees a signal dbus-send emits
"${MONITOR}" --session "type='signal',interface='org.phoenix.M7f'" > "${MON}" 2>&1 &
mpid=$!
sleep 3
"${SEND}" --session --type=signal /org/phoenix/M7f org.phoenix.M7f.Hello string:m7f-routed
echo "DBUSPHX signal sent rc=$? t=${SECONDS}"
sleep 3
kill -TERM "${mpid}" 2>/dev/null
wait "${mpid}" 2>/dev/null
echo "DBUSPHX monitor seen_member=$(count "${MON}" 'member=Hello') seen_payload=$(count "${MON}" 'm7f-routed') t=${SECONDS}"

# 5: GDBus (GIO) client
if [ -x "${GDBUS}" ]; then
	G_DBUS_DEBUG=authentication "${GDBUS}" call --session --dest org.freedesktop.DBus \
		--object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames > /tmp/dbus-m7f.gdbus.out 2>&1
	rc=$?
	echo "DBUSPHX gdbus rc=${rc} reply_has_bus=$(count /tmp/dbus-m7f.gdbus.out "'org.freedesktop.DBus'") tried_external=$(count /tmp/dbus-m7f.gdbus.out "Trying mechanism 'EXTERNAL'") tried_anonymous=$(count /tmp/dbus-m7f.gdbus.out "Trying mechanism 'ANONYMOUS'") t=${SECONDS}"
	n=0
	while IFS= read -r line; do
		case "${line}" in *org.freedesktop.DBus*|*"Trying mechanism"*|*rror*) n=$((n + 1)); [ "${n}" -le 20 ] && echo "DBUSPHX gdbus: ${line}" ;; esac
	done < /tmp/dbus-m7f.gdbus.out
else
	echo "DBUSPHX gdbus=absent (${GDBUS}): GIO client not staged yet"
fi

# stop the bus: SIGTERM -> the reload/exit pipe -> clean exit, the server unlinks its socket
kill -TERM "${dpid}" 2>/dev/null
i=0
while alive "${dpid}" && [ "${i}" -lt 15 ]; do
	sleep 1
	i=$((i + 1))
done
if alive "${dpid}"; then
	echo "DBUSPHX daemon still up ${i}s after TERM: sending KILL"
	kill -KILL "${dpid}" 2>/dev/null
	sleep 2
fi
wait "${dpid}" 2>/dev/null
echo "DBUSPHX daemon exited rc=$? after_term_s=${i} socket=$([ -e "${SOCK}" ] && echo left || echo gone) t=${SECONDS}"

# 6: how the daemon authenticated its clients -- ONE pass over the verbose log (thousands of
# lines on an NFS /tmp): the interesting lines are printed as they are found, and a progress
# line every 1000 lines keeps the UART talking (psh-interact cuts a command after 20 s of silence)
echo "DBUSPHX scanning the daemon log t=${SECONDS}"
anon=0 ext=0 noc=0 lines=0 n=0
while IFS= read -r line; do
	lines=$((lines + 1))
	[ $((lines % 1000)) -eq 0 ] && echo "DBUSPHX scanned ${lines} log lines t=${SECONDS}"
	case "${line}" in
		*"authenticated client as anonymous"*) anon=$((anon + 1)) ;;
		*"authenticated client based on socket credentials"*) ext=$((ext + 1)) ;;
		*"no credentials, mechanism EXTERNAL can't authenticate"*) noc=$((noc + 1)) ;;
		*"Credentials:"*|*"Failed to"*|*"Error"*|*"Unknown"*|*"rejected"*)
			n=$((n + 1))
			[ "${n}" -le 12 ] && echo "DBUSPHX log: ${line}"
			;;
	esac
done < "${LOG}"
echo "DBUSPHX auth anonymous=${anon} external=${ext} external_no_credentials=${noc} log_lines=${lines} t=${SECONDS}"
echo "DBUSPHX done"
