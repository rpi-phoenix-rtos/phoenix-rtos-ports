#!/bin/bash
#
# b7.sh -- the browser plan's B7 checks (GPU compositing, dma-buf frames, WebGL), one psh
# command each:
#
#     /bin/bash /usr/share/wpe-browser/b7.sh anim|webgl|headless
#
#   anim      b7-anim.html (16 composited layers + a repainted block, 60 s each) in four runs,
#             one browser after another in one XFCE session, same page, same boot:
#               A  --cpu-rendering            shm     (today's default: the control)
#               B  GPU raster (Skia Ganesh)   shm
#               C  GPU raster                 --dmabuf
#               D  --cpu-rendering            --dmabuf
#   webgl     b7-webgl.html (20000 triangles, 60 s) in three runs:
#               W0 GPU raster, shm, WebGL off  (the page must say context=none)
#               W1 GPU raster, shm, --webgl
#               W2 GPU raster, --dmabuf, --webgl
#   headless  no session: the B4 checksum with this binary (crc32=c3e96bf3), once as is and once
#             with --dmabuf, which a headless view must refuse (same checksum)
#
# Every browser run prints its own lines (WPEB ..., WPEB-WEBKIT ..., and the page's B7-ANIM /
# B7-WEBGL console lines) between "B7 run=<id> start" and "B7 run=<id> end rc=". The coordination
# repo's docs/browser/B7-gpu-webgl.md lists what each run must print. Lines of ours start "B7 ".
# B7_RUN_SECS (default 95) is how long each run lasts; B7_HOLD the session's.
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
SELF=/usr/share/wpe-browser/b7.sh
BROWSER=/usr/bin/wpe-browser
PAGES=file:///usr/share/wpe-browser
export HOME=${HOME:-/root}
export PHX_TRACE_ABORT=1
export THUNAR_START=${THUNAR_START:-0}   # no file manager window over the browser
unset WEBKIT_SKIA_ENABLE_CPU_RENDERING    # each run says which raster it uses
RUN_SECS=${B7_RUN_SECS:-95}

pids=()
pause() {  # pause <seconds>, interruptible by the session's SIGTERM
	sleep "$1" &
	wait $!
}
stop_all() {
	local p
	for p in "${pids[@]}"; do
		kill -TERM "${p}" 2>/dev/null
	done
	wait
	echo "B7 ${B7_INNER} stopped by the session t=${SECONDS}"
	exit 0
}

# run <id> <url> <browser options...>: one browser for RUN_SECS, then SIGTERM
run() {
	local id=$1 url=$2
	shift 2
	echo "B7 run=${id} start t=${SECONDS} url=${url} args=$*"
	WPE_BROWSER_PRESENT_SECS=5 "${BROWSER}" --size=1280x800 --toolbar=never --ephemeral "$@" "${url}" &
	pids=($!)
	pause "${RUN_SECS}"
	kill -TERM "${pids[0]}" 2>/dev/null
	wait "${pids[0]}"
	echo "B7 run=${id} end rc=$? t=${SECONDS}"
	pause 5
}

# --- inside the session: the autostart item -------------------------------------------------
inner() {
	trap stop_all TERM
	echo "B7 ${B7_INNER} start t=${SECONDS} display=${WAYLAND_DISPLAY:-unset}"
	case "${B7_INNER}" in
		anim)
			local page="${PAGES}/b7-anim.html?mode=both&secs=60"
			run A "${page}" --cpu-rendering
			run B "${page}"
			run C "${page}" --dmabuf
			run D "${page}" --cpu-rendering --dmabuf
			;;
		webgl)
			local page="${PAGES}/b7-webgl.html?secs=60&tris=20000"
			run W0 "${page}"
			run W1 "${page}" --webgl
			run W2 "${page}" --webgl --dmabuf
			;;
	esac
	echo "B7 ${B7_INNER} done t=${SECONDS}"
}

if [ -n "${B7_INNER:-}" ]; then
	inner
	exit 0
fi

# --- at psh ------------------------------------------------------------------------------------
mode=${1:-}
case "${mode}" in
	anim) export B7_INNER=anim XFCE_AUTOSTART="/bin/bash=${SELF}" HOLD=${B7_HOLD:-480} ;;
	webgl) export B7_INNER=webgl XFCE_AUTOSTART="/bin/bash=${SELF}" HOLD=${B7_HOLD:-400} ;;
	headless)
		for id in H0 H1; do
			args=(--headless --cpu-rendering --snapshot=/tmp/b7-${id}.png --timeout=600)
			[ "${id}" = H1 ] && args+=(--dmabuf)
			echo "B7 run=${id} start t=${SECONDS} args=${args[*]}"
			"${BROWSER}" "${args[@]}" /usr/share/wpe-browser/b4.html
			echo "B7 run=${id} end rc=$? t=${SECONDS}"
		done
		echo "B7 headless done"
		exit 0
		;;
	*)
		echo "usage: /bin/bash ${SELF} anim|webgl|headless"
		exit 2
		;;
esac
echo "B7 ${mode} session hold=${HOLD}s autostart=${XFCE_AUTOSTART}"
/bin/bash /bin/xfce-session
echo "B7 ${mode} end rc=$?"
