#!/bin/bash
#
# labwc-desktop.sh -- run ON THE Pi at the psh prompt:
#     /bin/bash /bin/labwc-desktop.sh <renderer> <client> [input|noinput]
# (staged for m7a/m7b as /bin/labwc-desktop.sh; this version adds the desktop and fuzzel
# clients and is staged as /bin/labwc-desktop-m7c.sh -- the m7a/m7b commands behave the
# same with it; m7c selects its configuration with CONF_DIR=/etc/xdg/labwc-m7c)
#   renderer  pixman | gles2       (WLR_RENDERER: pixman = dumb buffers + CPU composition,
#                                   no Mesa in the compositor; gles2 = GBM/EGL/GLES on V3D)
#   client    shm | foot | colors | mc | autostart | desktop | fuzzel | none
#                                  shm:  weston-simple-shm (the M6 wl_shm client), started here
#                                  foot: /bin/foot (an interactive bash), started here
#                                  colors: foot running /bin/m7b-colors.sh (24-bit colour bar,
#                                          Unicode line; needs no keyboard)
#                                  mc:   foot running Midnight Commander in / (the "Files" menu entry)
#                                  autostart: labwc's own autostart (CONF_DIR/autostart)
#                                  desktop: labwc's autostart (m7c: swaybg + foot) AND the
#                                           fuzzel launcher started here
#                                  fuzzel: the fuzzel launcher only
#                                  none: the compositor only
#   input     input (default: libinput-phoenix opens /dev/kbd0 + /dev/mouse0) | noinput
#
# The M7 desktop (docs/gpu-new-lane/M7-wayland-desktop.md in the coordination repo): labwc
# (wlroots 0.20) with the DRM + libinput backends. psh has no '&' and no ';', so this
# script does the job control, as weston-drm/pi/weston-m6a.sh does for Weston: it starts
# labwc in the background, waits for its socket, runs one client, holds, then ends the
# client and sends labwc SIGTERM (a clean exit exercises the emulated signalfd and the KMS
# restore).
#
# Preconditions: the GPU servers, started at boot: rpi4-v3d-async (render), rpi4-kms (display;
# its -C hands the console keyboard to labwc), shmsrv (memfd_create/shm_open backing: wl_shm
# pools, keymaps).
#
# FOOT, FUZZEL, SWAYBG (client binaries, default /bin/foot, /bin/fuzzel, /bin/swaybg; with
# another value the configuration copy in /tmp/labwc-conf has /bin/foot etc. replaced in
# rc.xml, menu.xml and autostart, so labwc's own launches use it too -- e.g. the m7b2
# rebuilds /bin/foot-2, /bin/fuzzel-2 with /bin/labwc-2),
# Environment knobs: LABWC (binary; /bin/tinywl = wlroots' tinywl instead), CONF_DIR (labwc -C, default
# /etc/xdg/labwc = the m7a/m7b configuration, /etc/xdg/labwc-m7c = m7c's; for any
# client other than autostart the script uses a copy without the autostart file), HOLD
# (seconds with the client up), CLIENT_ARGS, VERBOSE (labwc -V = info, default 1; 2 = -d),
# DRM_DEVICES (WLR_DRM_DEVICES, default /dev/dri/card0; empty = udev enumeration),
# HW_CURSORS (1 = let wlroots use the cursor plane; default 0 = WLR_NO_HARDWARE_CURSORS=1),
# ATOMIC (0 = WLR_DRM_NO_ATOMIC=1, wlroots' legacy KMS path; default 1), NO_MODIFIERS (1 =
# WLR_DRM_NO_MODIFIERS=1: plain ADDFB2 without modifiers; default 0),
# FOOT_LOG (foot --log-level for the foot/colors/mc clients, default info),
# DRMPHX_TRACE (libdrm-phoenix trace in labwc, default 1), WLPHX_TRACE (signal path, default 1).
#
# Every line of ours starts with "LABWC " (grading). wlroots' own lines look like
# "00:00:01.234 [backend/drm/drm.c:123] ..." (possibly wrapped in ANSI colour codes).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

RENDERER=${1:-pixman}
CLIENT=${2:-shm}
INPUT=${3:-input}
LABWC=${LABWC:-/bin/labwc}
CONF_DIR=${CONF_DIR:-/etc/xdg/labwc}
HOLD=${HOLD:-30}
VERBOSE=${VERBOSE:-1}
FOOT_LOG=${FOOT_LOG:-info}
FOOT=${FOOT:-/bin/foot}
FUZZEL=${FUZZEL:-/bin/fuzzel}
SWAYBG=${SWAYBG:-/bin/swaybg}
HW_CURSORS=${HW_CURSORS:-0}

export HOME=/root
export PATH=/bin:/usr/bin
export XDG_RUNTIME_DIR=/tmp/xdg
export XDG_CONFIG_DIRS=/etc/xdg
export LIBSEAT_BACKEND=noop
export WLR_BACKENDS=drm,libinput
# wlroots refuses to start the libinput backend with no devices (m7a-labwc: "libinput initialization
# failed, no input devices"): allow zero devices for noinput runs and for a keyboard the console still holds.
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=${RENDERER}
export WLR_DRM_DEVICES=${DRM_DEVICES-/dev/dri/card0}
[ -z "${WLR_DRM_DEVICES}" ] && unset WLR_DRM_DEVICES
[ "${HW_CURSORS}" = 1 ] || export WLR_NO_HARDWARE_CURSORS=1
[ "${ATOMIC:-1}" = 0 ] && export WLR_DRM_NO_ATOMIC=1
[ "${NO_MODIFIERS:-0}" = 1 ] && export WLR_DRM_NO_MODIFIERS=1
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
export XKB_DEFAULT_LAYOUT=us
export TERM=xterm-256color
if [ "${INPUT}" = noinput ]; then
	export LIBINPUT_PHOENIX_DEVICES=
else
	export LIBINPUT_PHOENIX_DEVICES=/dev/kbd0:keyboard,/dev/mouse0:mouse
fi

case "${RENDERER}" in
	pixman|gles2) ;;
	*) echo "LABWC FAIL unknown renderer ${RENDERER}"; exit 2 ;;
esac
case "${CLIENT}" in
	shm) CMD="/bin/weston-simple-shm ${CLIENT_ARGS}" ;;
	foot) CMD="${FOOT} --log-level=${FOOT_LOG} ${CLIENT_ARGS}" ;;
	colors) CMD="${FOOT} --log-level=${FOOT_LOG} ${CLIENT_ARGS} -e /bin/bash /bin/m7b-colors.sh" ;;
	mc) CMD="${FOOT} --log-level=${FOOT_LOG} ${CLIENT_ARGS} -e /bin/mc /" ;;
	fuzzel|desktop) CMD="${FUZZEL} --log-level=${FOOT_LOG} --terminal=${FOOT} ${CLIENT_ARGS}" ;;
	autostart|none) CMD="" ;;
	*) echo "LABWC FAIL unknown client ${CLIENT}"; exit 2 ;;
esac

mkdir -p "${XDG_RUNTIME_DIR}" 2>/dev/null || export XDG_RUNTIME_DIR=/tmp
chmod 700 "${XDG_RUNTIME_DIR}" 2>/dev/null
rm -f "${XDG_RUNTIME_DIR}"/wayland-* 2>/dev/null

# Only the autostart and desktop clients run labwc's autostart file: the others use a
# copy of the configuration without it, so exactly one client is up.
# Other client binaries (FOOT/FUZZEL/SWAYBG) also mean a copy: their paths are replaced in
# it with bash alone (the export has no sed).
files="rc.xml menu.xml environment"
{ [ "${CLIENT}" = autostart ] || [ "${CLIENT}" = desktop ]; } && files="${files} autostart"
conf="${CONF_DIR}"
if [ "${CLIENT}" != autostart ] && [ "${CLIENT}" != desktop ] || [ "${FOOT}" != /bin/foot ] ||
		[ "${FUZZEL}" != /bin/fuzzel ] || [ "${SWAYBG}" != /bin/swaybg ]; then
	conf=/tmp/labwc-conf
	rm -rf "${conf}"
	mkdir -p "${conf}"
	for f in ${files}; do
		[ -f "${CONF_DIR}/${f}" ] || continue
		c="$(< "${CONF_DIR}/${f}")"
		c="${c//\/bin\/foot /${FOOT} }"
		c="${c//\/bin\/foot\"/${FOOT}\"}"
		c="${c//\/bin\/fuzzel\"/${FUZZEL}\"}"
		c="${c//\/bin\/fuzzel /${FUZZEL} }"
		c="${c//\/bin\/swaybg /${SWAYBG} }"
		printf '%s\n' "${c}" > "${conf}/${f}"
	done
fi
have=""
for f in rc.xml menu.xml autostart environment; do
	[ -f "${conf}/${f}" ] && have="${have}${f},"
done

case "${VERBOSE}" in
	0) vflag="" ;;
	2) vflag="-d" ;;
	*) vflag="-V" ;;
esac

echo "LABWC start renderer=${RENDERER} client=${CLIENT} labwc=${LABWC} foot=${FOOT} fuzzel=${FUZZEL} swaybg=${SWAYBG} conf=${conf} files=${have%,} hold=${HOLD} input=${LIBINPUT_PHOENIX_DEVICES:-none} drm_devices=${WLR_DRM_DEVICES:-udev} hw_cursors=${HW_CURSORS} atomic=${ATOMIC:-1} no_modifiers=${NO_MODIFIERS:-0} verbose=${VERBOSE} trace=${DRMPHX_TRACE:-1} wlphx_trace=${WLPHX_TRACE:-1}"
# LABWC=/bin/tinywl: wlroots' own minimal compositor (no pango/GLib/libxml2, no config):
# the fallback that separates wlroots from labwc's text stack
case "${LABWC##*/}" in
	tinywl*) largs="" ;;
	*) largs="${vflag} -C ${conf}" ;;
esac
DRMPHX_TRACE=${DRMPHX_TRACE:-1} WLPHX_TRACE=${WLPHX_TRACE:-1} "${LABWC}" ${largs} &
lpid=$!
echo "LABWC labwc pid=${lpid}"

alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}

# labwc takes the first free wayland-N (wayland-0 after the clean-up above)
sock=""
i=0
while [ -z "${sock}" ] && [ "${i}" -lt 90 ] && alive "${lpid}"; do
	for s in "${XDG_RUNTIME_DIR}"/wayland-[0-9]; do
		[ -e "${s}" ] && sock="${s##*/}"
	done
	[ -n "${sock}" ] && break
	sleep 1
	i=$((i + 1))
	# psh-interact ends a command after --idle-secs of UART silence: keep talking
	[ $((i % 10)) -eq 0 ] && echo "LABWC waiting for the socket t=${SECONDS} waited=${i}s"
done
if [ -n "${sock}" ]; then
	echo "LABWC socket=up name=${sock} wait_s=${i} t=${SECONDS}"
	export WAYLAND_DISPLAY="${sock}"
else
	alive "${lpid}" && state=running || state=exited
	echo "LABWC socket=missing wait_s=${i} labwc=${state} t=${SECONDS}"
	if [ "${state}" = exited ]; then
		wait "${lpid}" 2>/dev/null
		echo "LABWC labwc exited rc=$? before its socket appeared"
		echo "LABWC done"
		exit 1
	fi
fi

cpid=""
if [ -n "${CMD}" ] && [ -n "${sock}" ]; then
	echo "LABWC client start: ${CMD}"
	${CMD} &
	cpid=$!
	echo "LABWC client pid=${cpid}"
fi
held=0
while [ "${held}" -lt "${HOLD}" ]; do
	sleep 10
	held=$((held + 10))
	c=none
	[ -n "${cpid}" ] && { alive "${cpid}" && c=running || c=exited; }
	l=exited
	alive "${lpid}" && l=running
	echo "LABWC hold t=${SECONDS} held=${held}s labwc=${l} client=${c}"
done
if [ -n "${cpid}" ]; then
	kill "${cpid}" 2>/dev/null
	wait "${cpid}" 2>/dev/null
	echo "LABWC client exited rc=$? t=${SECONDS}"
fi

# SIGTERM: labwc's signal source (emulated signalfd) -> wl_display_terminate -> clean exit
# (autostart children are orphans of labwc's double fork; they lose the display and exit).
kill -TERM "${lpid}" 2>/dev/null
i=0
while alive "${lpid}" && [ "${i}" -lt 15 ]; do
	sleep 1
	i=$((i + 1))
done
if alive "${lpid}"; then
	echo "LABWC labwc still up ${i}s after TERM: sending KILL"
	kill -KILL "${lpid}" 2>/dev/null
	sleep 2
fi
wait "${lpid}" 2>/dev/null
rc=$?
left=gone
[ -n "${sock}" ] && [ -e "${XDG_RUNTIME_DIR}/${sock}" ] && left=left
echo "LABWC labwc exited rc=${rc} after_term_s=${i} socket=${left} t=${SECONDS}"
echo "LABWC done"
