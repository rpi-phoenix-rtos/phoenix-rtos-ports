#!/bin/bash
#
# b6.sh -- the browser plan's B6 checks ("a usable browser"), one psh command each:
#
#     /bin/bash /usr/share/wpe-browser/b6.sh sites|persist|persist-warm|soak|keys|keys-hid
#
#   sites    /bin/browser with its start page, then Wikipedia, GitHub, a DuckDuckGo search,
#            Stack Overflow and BBC News, one window after another (XFCE_AUTOSTART items)
#   persist  cookies + HTTP disk cache: clears $HOME/.local/share/wpe-browser and
#            $HOME/.cache/wpe-browser, loads Wikipedia twice in two runs, lists the files after each
#   persist-warm  after a reboot following `persist`: one more run on what the disk kept
#   soak     30 minutes: the five sites in turn, one per minute (--cycle), the memory footprint
#            of every process and the system's free RAM every 5 minutes, shmsrv's stats as often;
#            the probe extension names the web process of every document (WPEB-EXT lines).
#            B6_SOAK_ARGS: more browser options (the process model: --process-cache=N,
#            --prewarm, --no-process-swap, --hang-recovery=S, --stall-secs=S);
#            B6_WEBKIT_DEBUG: WebKit log channels (WEBKIT_DEBUG; a release_log build only)
#   keys     the chrome driven by synthetic keys (WPE_BROWSER_AUTO): the address bar, a new
#            window request, a search, back, forward, reload, stop, cancel, home, quit
#   keys-hid the same path end to end: HID boot reports appended to /tmp/kbd-inject, which
#            labwc reads as a keyboard (xfce-desktop.sh INPUT_EXTRA, ports branch
#            xfce-browser-launcher): address bar, go, back, quit
#
# Each mode starts the XFCE session (/bin/xfce-session) with the check as its autostart and ends
# it by itself (HOLD: the mode's default, B6_HOLD=<seconds> overrides it; a HOLD exported earlier
# at psh is not used). The tools/browser/wpe/README.md "B6" section of the coordination repo lists
# the expected lines. Lines of ours start with "B6 " (and the browser's with "WPEB ").
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

exec 2>&1
SELF=/usr/share/wpe-browser/b6.sh
BROWSER=/usr/bin/wpe-browser
export HOME=${HOME:-/root}
DATA=${HOME}/.local/share/wpe-browser
CACHE=${HOME}/.cache/wpe-browser
WIKI=https://en.wikipedia.org/wiki/Phoenix-RTOS
export PHX_TRACE_ABORT=1
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export THUNAR_START=${THUNAR_START:-0}   # no file manager window over the browser

pids=()
alive() {
	local p
	for p in $(jobs -rp); do
		[ "${p}" = "$1" ] && return 0
	done
	return 1
}
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
	echo "B6 ${B6_INNER} stopped by the session t=${SECONDS}"
	exit 0
}

# what the persistent session left on disk
files() {
	local n=0 blobs=0 checked=0 nonzero=0 f cookies=missing rows=- kb
	shopt -s nullglob globstar
	for f in "${CACHE}"/**; do
		[ -f "${f}" ] && n=$((n + 1))
	done
	# bodies larger than a page are blobs (WebKitCache/Version N/Blobs); read() sees what is on
	# the disk: a blob of zeros never got its bytes (a file mapping is not written back on
	# Phoenix; WebKit patch 0011 writes them with write())
	for f in "${CACHE}"/WebKitCache/*/Blobs/*; do
		[ -f "${f}" ] || continue
		blobs=$((blobs + 1))
		[ "${checked}" -lt 20 ] || continue
		checked=$((checked + 1))
		[ "$(tr -d '\000' < "${f}" | wc -c)" -gt 0 ] && nonzero=$((nonzero + 1))
	done
	if [ -f "${DATA}/cookies.sqlite" ]; then
		cookies="$(wc -c < "${DATA}/cookies.sqlite")"
		# libsoup's SoupCookieJarDB table
		rows="$(sqlite3 "${DATA}/cookies.sqlite" "SELECT count(*) FROM moz_cookies;" 2>&1)"
	fi
	kb="$(du -sk "${CACHE}" 2>/dev/null)"
	echo "B6 persist files run=$1 cookies_bytes=${cookies} cookie_rows=${rows} cache_files=${n} cache_kb=${kb%%[[:space:]]*} blobs=${blobs} blobs_nonzero=${nonzero}/${checked} data=$(ls -A "${DATA}" 2>/dev/null | tr '\n' ',')"
}

# --- keys-hid: 8-byte HID boot keyboard reports (modifiers, 0, usage...) into the inject file ---
INJECT=/tmp/kbd-inject
hid() {  # hid <modifier byte hex> <usage hex>: a press and a release
	printf "\\x$1\\x00\\x$2\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00" >> "${INJECT}"
}
hid_key() {  # hid_key [ctrl+|alt+|shift+]...<Return|Escape|Left|Right|Home|F5|a-z>
	local spec=$1 mods=0 u
	while :; do
		case "${spec}" in
			ctrl+*) mods=$((mods | 1)); spec=${spec#ctrl+} ;;
			shift+*) mods=$((mods | 2)); spec=${spec#shift+} ;;
			alt+*) mods=$((mods | 4)); spec=${spec#alt+} ;;
			*) break ;;
		esac
	done
	case "${spec}" in
		Return) u=28 ;; Escape) u=29 ;; BackSpace) u=2a ;; Right) u=4f ;; Left) u=50 ;; Home) u=4a ;; F5) u=3e ;;
		[a-z]) printf -v u '%02x' $(( $(printf '%d' "'${spec}") - 97 + 4 )) ;;
		*) echo "B6 keys-hid bad key ${spec}"; return ;;
	esac
	echo "B6 keys-hid key=$1 t=${SECONDS}"
	hid "$(printf '%02x' "${mods}")" "${u}"
}
hid_type() {  # hid_type <text>: letters, digits, space and . / - : _ on a US layout
	local text=$1 c n i
	echo "B6 keys-hid type=${text} t=${SECONDS}"
	for ((i = 0; i < ${#text}; i++)); do
		c=${text:i:1}
		printf -v n '%d' "'${c}"
		case "${c}" in
			[a-z]) hid 00 "$(printf '%02x' $((n - 97 + 4)))" ;;
			[A-Z]) hid 02 "$(printf '%02x' $((n - 65 + 4)))" ;;
			[1-9]) hid 00 "$(printf '%02x' $((n - 49 + 0x1e)))" ;;
			0) hid 00 27 ;;
			' ') hid 00 2c ;;
			-) hid 00 2d ;;
			_) hid 02 2d ;;
			.) hid 00 37 ;;
			/) hid 00 38 ;;
			:) hid 02 33 ;;
			*) echo "B6 keys-hid cannot type '${c}'" ;;
		esac
	done
}

shm_stats() {
	echo "B6 soak shm t=${SECONDS} $(/bin/shmsrv -s 2>&1)"
}

# --- inside the session: the autostart item -------------------------------------------------
inner() {
	trap stop_all TERM
	echo "B6 ${B6_INNER} start t=${SECONDS} home=${HOME} display=${WAYLAND_DISPLAY:-unset}"
	case "${B6_INNER}" in
		persist | persist-warm)
			local runs="1 2"
			if [ "${B6_INNER}" = persist ]; then
				echo "B6 persist clear ${DATA} ${CACHE}"
				rm -rf "${DATA}" "${CACHE}"
			else
				runs=3   # after a reboot: what the disk kept
			fi
			files 0
			for run in ${runs}; do
				echo "B6 persist run=${run} start url=${WIKI} t=${SECONDS}"
				"${BROWSER}" --cpu-rendering "${WIKI}" &
				pids=($!)
				pause "${B6_PERSIST_SECS:-100}"
				kill -TERM "${pids[0]}" 2>/dev/null
				wait "${pids[0]}"
				echo "B6 persist run=${run} end rc=$? t=${SECONDS}"
				files "${run}"
				pause 5
			done
			;;
		soak)
			local secs=${B6_SOAK_SECS:-1800} every=${B6_SOAK_STAT_SECS:-300} p args
			# shellcheck disable=SC2206 # options, split on spaces
			args=(${B6_SOAK_ARGS:-})
			[ -z "${B6_WEBKIT_DEBUG:-}" ] || export WEBKIT_DEBUG="${B6_WEBKIT_DEBUG}"
			echo "B6 soak args=${B6_SOAK_ARGS:-none} webkit_debug=${B6_WEBKIT_DEBUG:-none}"
			shm_stats
			WPE_BROWSER_CYCLE=/usr/share/wpe-browser/b6-sites.txt WPE_BROWSER_CYCLE_SECS=${B6_SOAK_CYCLE_SECS:-60} \
				WPE_BROWSER_RSS_SECS=${every} "${BROWSER}" --cpu-rendering \
				--web-extensions=/usr/lib/wpe-browser/pi-extensions "${args[@]}" &
			p=$!
			pids=("${p}")
			while [ "${SECONDS}" -lt "${secs}" ] && alive "${p}"; do
				pause "${every}"
				shm_stats
			done
			alive "${p}" && echo "B6 soak browser alive after ${SECONDS}s" || echo "B6 soak browser EXITED early t=${SECONDS}"
			kill -TERM "${p}" 2>/dev/null
			wait "${p}"
			echo "B6 soak browser rc=$? t=${SECONDS}"
			shm_stats
			;;
		keys)
			# <seconds from start>:key:<keys> | :type:<text>; the timings leave the LLInt time to
			# load each page (Wikipedia ~22 s on B5)
			local steps=(
				"20:key:ctrl+l" "22:type:/usr/share/wpe-browser/b6-newwin.html" "24:key:Return"
				"36:key:Return"
				"50:key:ctrl+l" "52:type:en.wikipedia.org/wiki/Phoenix-RTOS" "56:key:Return"
				"110:key:ctrl+l" "112:type:phoenix rtos microkernel" "116:key:Return"
				"170:key:alt+Left"
				"220:key:alt+Right"
				"270:key:F5" "271:key:Escape"
				"290:key:ctrl+l" "322:key:Escape"
				"330:key:alt+Home"
				"360:key:ctrl+q"
			)
			local IFS=,
			WPE_BROWSER_AUTO="${steps[*]}" "${BROWSER}" --cpu-rendering &
			unset IFS
			pids=($!)
			wait "${pids[0]}"
			echo "B6 keys browser rc=$? t=${SECONDS}"
			;;
		keys-hid)
			"${BROWSER}" --cpu-rendering &
			pids=($!)
			# libinput-phoenix writes its raw-mode byte when it opens the file: the handshake, and
			# what keeps the reports 8-byte aligned (they start at offset 1)
			local i=0
			while [ ! -s "${INJECT}" ] && [ "${i}" -lt 60 ]; do
				pause 1
				i=$((i + 1))
			done
			echo "B6 keys-hid inject file $( [ -s "${INJECT}" ] && echo opened || echo NOT-opened ) after ${i}s"
			pause 30
			hid_key ctrl+l
			pause 2
			hid_type en.wikipedia.org/wiki/Phoenix-RTOS
			pause 2
			hid_key Return
			pause 60
			hid_key alt+Left
			pause 30
			hid_key ctrl+q
			wait "${pids[0]}"
			echo "B6 keys-hid browser rc=$? t=${SECONDS}"
			;;
	esac
	echo "B6 ${B6_INNER} done t=${SECONDS}"
}

if [ -n "${B6_INNER:-}" ]; then
	inner
	exit 0
fi

# --- at psh: the session with the check as its autostart ----------------------------------------
mode=${1:-}
item="/bin/bash=${SELF}"
case "${mode}" in
	sites)
		export WPE_BROWSER_RSS_SECS=60 WPE_PHOENIX_SHM_LOG=1
		export XFCE_AUTOSTART="/bin/bash=/bin/browser:90,/usr/bin/wpe-browser=${WIKI}:200,/usr/bin/wpe-browser=https://github.com/phoenix-rtos/phoenix-rtos-kernel:240,/usr/bin/wpe-browser=phoenix-rtos:150,/usr/bin/wpe-browser=https://stackoverflow.com/questions/tagged/rtos:240,/usr/bin/wpe-browser=https://www.bbc.com/news:300"
		export HOLD=${B6_HOLD:-1320}
		;;
	persist) export B6_INNER=persist XFCE_AUTOSTART="${item}" HOLD=${B6_HOLD:-360} ;;
	persist-warm) export B6_INNER=persist-warm XFCE_AUTOSTART="${item}" HOLD=${B6_HOLD:-240} ;;
	soak) export B6_INNER=soak XFCE_AUTOSTART="${item}" HOLD=${B6_HOLD:-1920} ;;
	keys) export B6_INNER=keys XFCE_AUTOSTART="${item}" HOLD=${B6_HOLD:-420} ;;
	keys-hid)
		# an empty file before the session: libinput-phoenix opens it once labwc starts
		rm -f "${INJECT}"
		: > "${INJECT}"
		export B6_INNER=keys-hid XFCE_AUTOSTART="${item}" HOLD=${B6_HOLD:-260} INPUT_EXTRA=${INJECT}:keyboard
		;;
	*)
		echo "usage: /bin/bash ${SELF} sites|persist|persist-warm|soak|keys|keys-hid"
		exit 2
		;;
esac
echo "B6 ${mode} session hold=${HOLD}s autostart=${XFCE_AUTOSTART}"
/bin/bash /bin/xfce-session
echo "B6 ${mode} end rc=$?"
