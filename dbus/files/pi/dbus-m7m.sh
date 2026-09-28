#!/bin/bash
#
# dbus-m7m.sh -- the LOGOUT_CMD of cycle m7m-dbus-peercred (docs/gpu-new-lane/M7-wayland-desktop.md,
# "Stage 8"). Staged as /bin/dbus-m7m.sh and run ON THE Pi by xfce-desktop-2.sh when HOLD is over:
#
#     export LOGOUT_CMD=/bin/dbus-m7m.sh
#     /bin/bash /bin/xfce-session-2
#
# (psh does not strip quotes, so LOGOUT_CMD has to be one word: this script takes no arguments.)
# While the XFCE session is still up, it asks the bus for the credentials of every org.xfce.*
# connection (org.freedesktop.DBus.GetConnectionCredentials), one line per name, then logs out
# the way the default LOGOUT_CMD does (the xfce-demo loginctl stand-in). xfce-desktop-2.sh writes
# its output to /tmp/xfce-logs/logout-cmd.log and prints it at the end ("XFCE log logout-cmd: ").
#
# What the lines mean: ProcessID is there when dbus-daemon read the peer's SO_PEERCRED on that
# connection, whatever mechanism it authenticated with. UnixUserID is there only for a connection
# that authenticated EXTERNAL. The XFCE programs are GDBus clients, and glib has no Phoenix
# credentials case (their EXTERNAL claims uid -1 and is rejected), so they are expected with
# ProcessID and without UnixUserID.
#
# Environment knobs: SEND (default /usr/bin/dbus-send-pc), XFCE_BIN (the loginctl directory,
# default /usr/lib/xfce-demo/bin; /bin/xfce-session exports it). Every line of ours starts with
# "DBUSPC " (grading).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

SEND=${SEND:-/usr/bin/dbus-send-pc}
XFCE_BIN=${XFCE_BIN:-/usr/lib/xfce-demo/bin}

# field KEY REPLY: the uint32 value of KEY in a `dbus-send --print-reply` of
# GetConnectionCredentials, or "absent". The reply holds
#     string "ProcessID"
#     variant             uint32 42
field() {
	local key="$1" line want=0
	while IFS= read -r line; do
		if [ "${want}" = 1 ]; then
			case "${line}" in *uint32*) echo "${line##* }"; return ;; esac
			want=0
		fi
		case "${line}" in *"\"${key}\""*) want=1 ;; esac
	done <<< "$2"
	echo absent
}

echo "DBUSPC start send=${SEND} address=${DBUS_SESSION_BUS_ADDRESS:-unset} t=${SECONDS}"
out="$("${SEND}" --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
	org.freedesktop.DBus.ListNames 2>&1)"
rc=$?
names=""
while IFS= read -r line; do
	case "${line}" in
		*'"org.xfce.'*) line="${line#*\"}"; names="${names} ${line%\"*}" ;;
	esac
done <<< "${out}"
echo "DBUSPC listnames rc=${rc} names=${names# }"

n=0 with_pid=0 with_uid=0
for name in ${names}; do
	[ "${n}" -lt 20 ] || break
	n=$((n + 1))
	reply="$("${SEND}" --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
		org.freedesktop.DBus.GetConnectionCredentials "string:${name}" 2>&1)"
	rc=$?
	pid="$(field ProcessID "${reply}")"
	uid="$(field UnixUserID "${reply}")"
	[ "${pid}" != absent ] && with_pid=$((with_pid + 1))
	[ "${uid}" != absent ] && with_uid=$((with_uid + 1))
	echo "DBUSPC creds name=${name} rc=${rc} pid=${pid} uid=${uid}"
done
echo "DBUSPC summary names=${n} with_pid=${with_pid} with_uid=${with_uid} t=${SECONDS}"

"${XFCE_BIN}/loginctl" terminate-session
echo "DBUSPC logout rc=$? t=${SECONDS}"
