#!/bin/bash
#
# weston-m6a.sh -- run ON THE Pi at the psh prompt:
#     /bin/bash /bin/weston-m6a.sh <renderer> <client> [input|noinput]
#   renderer  pixman | gl          (pixman: dumb buffers + CPU composition, no Mesa in the
#                                    compositor; gl: GBM/EGL/GLES on V3D)
#   client    shm | egl | none     (weston-simple-shm / weston-simple-egl / compositor only)
#   input     input (default: libinput-phoenix opens /dev/kbd0 + /dev/mouse0) | noinput
#
# First Pi cycle of the new-lane Wayland compositor (docs/gpu-new-lane/M6-wayland.md,
# pre-registered cycle m6a-weston). psh has no '&' and no ';', so this script does the job
# control, as tools/gpu-lane/xorg-drm/pi/xorg-drm-m4a.sh does for Xorg-drm: it starts weston
# in the background, waits for its socket, runs one client, holds, then ends the client and
# sends weston SIGTERM (a clean exit exercises the emulated signalfd and the KMS restore).
#
# Preconditions (earlier psh commands of the same cycle):
#   /bin/rpi4-v3d-async-m3p2 -r 1 -m serial -i
#   /bin/rpi4-kms-gate -G
#   /bin/shmsrv -v                 (memfd_create backing: wl_shm pools, keymaps)
#
# Environment knobs: WESTON (binary), EGL_CLIENT (the egl client binary, default
# /bin/weston-simple-egl; G6 stages its client under a new name), CONF, HOLD (seconds with
# the client up), CLIENT_ARGS,
# SHARED_SCANOUT (1 = run the egl client with mesa-drm 0012's V3D_PHOENIX_SHARED_SCANOUT=1;
# default 0 since G4),
# DRMPHX_TRACE (libdrm-phoenix trace in weston, default 1), WLPHX_TRACE (signal path and
# shutdown steps in weston, "WLPHX ..." lines, default 1; M6 §14).
#
# Every line of ours starts with "WESTONDRM " (grading).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

RENDERER=${1:-pixman}
CLIENT=${2:-shm}
INPUT=${3:-input}
WESTON=${WESTON:-/bin/weston}
EGL_CLIENT=${EGL_CLIENT:-/bin/weston-simple-egl}
CONF=${CONF:-/etc/xdg/weston/weston-drm.ini}
HOLD=${HOLD:-30}
SHARED_SCANOUT=${SHARED_SCANOUT:-0}

export HOME=/root
export PATH=/bin
export XDG_RUNTIME_DIR=/tmp/xdg
export WAYLAND_DISPLAY=wayland-0
export LIBSEAT_BACKEND=noop
if [ "${INPUT}" = noinput ]; then
	export LIBINPUT_PHOENIX_DEVICES=
else
	export LIBINPUT_PHOENIX_DEVICES=/dev/kbd0:keyboard,/dev/mouse0:mouse
fi

mkdir -p "${XDG_RUNTIME_DIR}" 2>/dev/null || export XDG_RUNTIME_DIR=/tmp
chmod 700 "${XDG_RUNTIME_DIR}" 2>/dev/null  # weston only warns about another mode
rm -f "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}.lock"

case "${CLIENT}" in
	shm) CMD="/bin/weston-simple-shm ${CLIENT_ARGS}" ;;
	egl) CMD="${EGL_CLIENT} ${CLIENT_ARGS}" ;;
	none) CMD="" ;;
	*) echo "WESTONDRM FAIL unknown client ${CLIENT}"; exit 2 ;;
esac

echo "WESTONDRM start renderer=${RENDERER} client=${CLIENT} weston=${WESTON} conf=${CONF} hold=${HOLD} shared_scanout=${SHARED_SCANOUT} input=${LIBINPUT_PHOENIX_DEVICES:-none} trace=${DRMPHX_TRACE:-1} wlphx_trace=${WLPHX_TRACE:-1} egl_client=${EGL_CLIENT}"
DRMPHX_TRACE=${DRMPHX_TRACE:-1} WLPHX_TRACE=${WLPHX_TRACE:-1} "${WESTON}" --config="${CONF}" --backend=drm --renderer="${RENDERER}" \
	--shell=kiosk --continue-without-input --idle-time=0 --socket="${WAYLAND_DISPLAY}" &
wpid=$!
echo "WESTONDRM weston pid=${wpid}"

alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}

i=0
while [ ! -e "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" ] && [ "${i}" -lt 60 ] && alive "${wpid}"; do
	sleep 1
	i=$((i + 1))
	# psh-interact ends a command after --idle-secs of UART silence: keep talking
	[ $((i % 10)) -eq 0 ] && echo "WESTONDRM waiting for the socket t=${SECONDS} waited=${i}s"
done
if [ -e "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" ]; then
	echo "WESTONDRM socket=up wait_s=${i} t=${SECONDS}"
else
	alive "${wpid}" && state=running || state=exited
	echo "WESTONDRM socket=missing wait_s=${i} weston=${state} t=${SECONDS}"
	if [ "${state}" = exited ]; then
		wait "${wpid}" 2>/dev/null
		echo "WESTONDRM weston exited rc=$? before its socket appeared"
		echo "WESTONDRM done"
		exit 1
	fi
fi

cpid=""
if [ -n "${CMD}" ]; then
	echo "WESTONDRM client start: ${CMD}"
	if [ "${CLIENT}" = egl ] && [ "${SHARED_SCANOUT}" = 1 ]; then
		# mesa-drm 0012: client buffers from the display device. Off by default since G4
		# (the render node exports); m6d showed it never engaged for a wayland-egl client.
		DRMPHX_TRACE=${DRMPHX_TRACE:-1} V3D_PHOENIX_SHARED_SCANOUT=1 ${CMD} &
	elif [ "${CLIENT}" = egl ]; then
		DRMPHX_TRACE=${DRMPHX_TRACE:-1} ${CMD} &   # the client's PRIME calls on the UART (m6d could not see them)
	else
		${CMD} &
	fi
	cpid=$!
fi
held=0
while [ "${held}" -lt "${HOLD}" ]; do
	sleep 10
	held=$((held + 10))
	c=none
	[ -n "${cpid}" ] && { alive "${cpid}" && c=running || c=exited; }
	w=exited
	alive "${wpid}" && w=running
	echo "WESTONDRM hold t=${SECONDS} held=${held}s weston=${w} client=${c}"
done
if [ -n "${cpid}" ]; then
	kill "${cpid}" 2>/dev/null
	wait "${cpid}" 2>/dev/null
	echo "WESTONDRM client exited rc=$? t=${SECONDS}"
fi

# SIGTERM: weston's signal source (emulated signalfd) -> wl_display_terminate -> clean exit.
kill -TERM "${wpid}" 2>/dev/null
i=0
while alive "${wpid}" && [ "${i}" -lt 15 ]; do
	sleep 1
	i=$((i + 1))
done
if alive "${wpid}"; then
	echo "WESTONDRM weston still up ${i}s after TERM: sending KILL"
	kill -KILL "${wpid}" 2>/dev/null
	sleep 2
fi
wait "${wpid}" 2>/dev/null
echo "WESTONDRM weston exited rc=$? after_term_s=${i} socket=$([ -e "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}" ] && echo left || echo gone) t=${SECONDS}"
echo "WESTONDRM done"
