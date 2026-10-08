#!/usr/bin/env bash
#
# rpivid host test: does the FFmpeg hwaccel (hevc_rpivid) program the rpivid block exactly
# as the reference player that was proved bit-exact on the Pi 4 does?
#
#   files/rpivid/hosttest/run.sh --coord <coordination repo> [--work <dir>] [--tarball <ffmpeg-6.1.tar.gz>]
#                                [--threads N] [clip...]
#
# Builds, for the host (x86, ASan):
#   - FFmpeg 6.1 with this port's rpivid patch and sources, the hardware layer (src/rpivid_hw.c)
#     compiled for Phoenix-RTOS against mock.c (a register-level stand-in for the block);
#   - check/hevc-rpivid-check against it;
#   - the reference: the coordination repo's tools/hevc-decode/hevc-m2.c (hevc-play) against
#     the same mock (its register accessors routed to the mock, its frame pacing off, one pass).
# Then, per clip (default: every .265/.mp4 in tools/hevc-decode/testdata), both decoders run
# on the mock -- the hwaccel on the proven tool set (FFMPEG_RPIVID_TOOLS=none), the subset the
# reference implements -- and their canonical register logs (mock.c) are compared:
#   SAME      the hwaccel drives the block exactly like the reference: every command-buffer
#             entry, every phase-2 register, every slice's bitstream bytes
#   PREFIX    the same for every picture the reference decoded before it gave up (its own
#             parser's limits), the hwaccel going on
#   DIFF      the first differing lines are shown
#   REF-SKIP  the reference player rejects the clip (outside its subset): hwaccel-only log
#   CPU       hevc_rpivid chose the CPU decoder (its "rpivid: CPU decode:" line says why)
# A clip given on the command line is also decoded with -threads N (default 4) to show that
# frame threading does not change what the block is told.
#
# --loop (clips given on the command line, closed GOP: the display index is derived from the
# POC and the IDRs): the mock "decodes" each picture by writing the host
# ffmpeg's decode of it, SAND tiled, into the output buffers; hevc-rpivid-check (-l 2, all
# tools) then compares the hwaccel's frames with the CPU decoder's: the output path --
# de-tiling 8/10 bit, cropping, frame order, 1 and N threads -- must be BIT-EXACT. Then the
# same with zero-copy output (-zc: DRM_PRIME frames in the NV12_COL128 single-buffer layout,
# read back on the CPU; 1 and N slice threads; frames held as a compositor holds them) --
# every 8-bit clip BIT-EXACT too; and a forced CPU fallback (FFMPEG_RPIVID_REFUSE_AT=3: the
# block refuses its 4th picture; MOCK_FAIL_AT=5: it fails its 6th): pictures are dropped up to
# the next IRAP, then the CPU decoder goes on, and every frame emitted, in either output, must
# equal the CPU decoder's frame of the same pts. Also per clip: the interrupt path
# (MOCK_ISR_RACE=1), BIT-EXACT with no stale completion. Then, on the first 8-bit clip: a block
# that stops responding (MOCK_WEDGE_AT: phase 2 never finishes) -- the process records it, its
# clock goes off, no register is touched after; the next process does not touch the block at
# all; FFMPEG_RPIVID_RESET=1 tries it once (a clock off/on), and only once; a marker from an
# earlier boot is ignored -- and a zero-copy buffer beyond the block's reach (MOCK_PA_HIGH_SIZE):
# the stream keeps system-memory frames, still decoded by the block.
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rp="$(cd "${here}/.." && pwd)"
port="$(cd "${rp}/../.." && pwd)"
coord="" work="${TMPDIR:-/tmp}/rpivid-hosttest" tarball="${port}/ffmpeg-6.1.tar.gz" threads=4 loop=0
clips=()
while [ $# -gt 0 ]; do
	case "$1" in
		--coord) coord="$2"; shift ;;
		--work) work="$2"; shift ;;
		--tarball) tarball="$2"; shift ;;
		--threads) threads="$2"; shift ;;
		--loop) loop=1 ;;
		-h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) clips+=("$1") ;;
	esac
	shift
done
[ -n "${coord}" ] && [ -f "${coord}/tools/hevc-decode/hevc-m2.c" ] || { echo "run.sh: --coord <coordination repo> (with tools/hevc-decode)" >&2; exit 2; }
[ -f "${tarball}" ] || { echo "run.sh: no ${tarball} (--tarball)" >&2; exit 2; }
hd="${coord}/tools/hevc-decode"
mkdir -p "${work}"
san=(-g -O1 -fsanitize=address -fno-omit-frame-pointer)
# FFmpeg keeps its own -O3. --disable-inline-asm: under ASan, configure takes ASan's
# "__odr_asan." symbols for the extern prefix, and the x86 CABAC inline asm then reads
# the wrong tables (the CPU decoder's output is garbage; the hwaccel path has no CABAC)
ffsan=(-g -fsanitize=address -fno-omit-frame-pointer)

# --- host FFmpeg with the patch -------------------------------------------------------------
src="${work}/ffmpeg-6.1" bld="${work}/build"
stamp="$( { cat "${rp}"/patches/*.patch "${rp}"/src/*; echo "${ffsan[*]} noinlineasm"; } | sha256sum | cut -c1-16)"
if [ "$(cat "${work}/ffmpeg.stamp" 2>/dev/null || true)" != "${stamp}" ]; then
	rm -rf "${src}" "${bld}"
	mkdir -p "${src}" "${bld}"
	tar xzf "${tarball}" -C "${src}" --strip-components=1
	for p in "${rp}"/patches/*.patch; do patch -d "${src}" -p1 -s <"${p}"; done
	cp "${rp}"/src/*.[ch] "${src}/libavcodec/"
	printf '/* host test: the Phoenix-RTOS hardware layer against hosttest/mock.c */\n#define __phoenix__ 1\n#define RPIVID_MMIO_HOOKS 1\nconst char *rpivid_dead_path(void);\n#define RPIVID_DEAD_PATH rpivid_dead_path()\n#include "phoenix_shim.h"\n#include "%s/src/rpivid_hw.c"\n' "${rp}" \
		>"${src}/libavcodec/rpivid_hw.c"
	(cd "${bld}" && "${src}/configure" --disable-everything --enable-decoder=hevc,hevc_rpivid --enable-demuxer=hevc,mov \
		--enable-parser=hevc --enable-protocol=file --enable-bsf=hevc_mp4toannexb --disable-programs --disable-doc \
		--enable-static --disable-shared --disable-x86asm --disable-inline-asm --enable-pthreads --disable-autodetect \
		--extra-cflags="-I${here}/shim -I${rp}/src ${ffsan[*]}" --extra-ldflags=-fsanitize=address) >"${work}/configure.log" 2>&1 ||
		{ tail -20 "${work}/configure.log"; exit 1; }
	grep -q '^#define CONFIG_HEVC_RPIVID_DECODER 1$' "${bld}/config_components.h" || { echo "run.sh: hevc_rpivid not enabled" >&2; exit 1; }
	make -C "${bld}" -j"$(nproc)" >"${work}/make.log" 2>&1 || { grep -E -A5 'error' "${work}/make.log" | head -40; exit 1; }
	echo "${stamp}" >"${work}/ffmpeg.stamp"
fi
if grep -E 'rpivid[a-z_]*\.c:[0-9]+:[0-9]+: warning' "${work}/make.log"; then
	echo "run.sh: warnings in the rpivid sources (above)"
fi

# --- the tools ------------------------------------------------------------------------------
cc=(gcc -std=gnu11 -Wall -Wextra "${san[@]}")
"${cc[@]}" -I"${here}/shim" -I"${rp}/src" -c "${here}/mock.c" -o "${work}/mock.o"
"${cc[@]}" -I"${src}" -I"${bld}" "${rp}/check/hevc-rpivid-check.c" "${work}/mock.o" \
	"${bld}/libavformat/libavformat.a" "${bld}/libavcodec/libavcodec.a" "${bld}/libavutil/libavutil.a" -lpthread -lm \
	-o "${work}/hevc-rpivid-check"
sed -e 's|^static inline uint32_t rd(const volatile void \*a) { return .*|uint32_t m2_rd(const volatile void *a); static inline uint32_t rd(const volatile void *a) { return m2_rd(a); }|' \
	-e 's|^static inline void wr(volatile void \*a, uint32_t v) { .*|void m2_wr(volatile void *a, uint32_t v); static inline void wr(volatile void *a, uint32_t v) { m2_wr(a, v); }|' \
	-e 's|^\tconst int passes = (nslices >= 8) ? 2 : 8;|\tconst int passes = 1;|' \
	-e '/^static void pace_frame(void)$/{n;s|^{$|{ return;|}' \
	-e 's|__asm__ volatile("dsb sy" ::: "memory");|__sync_synchronize();|' \
	"${hd}/hevc-m2.c" >"${work}/hevc-m2-host.c"
grep -q 'm2_rd(a)' "${work}/hevc-m2-host.c" && grep -q 'm2_wr(a, v)' "${work}/hevc-m2-host.c" &&
	grep -q 'const int passes = 1;' "${work}/hevc-m2-host.c" && grep -q '^{ return;' "${work}/hevc-m2-host.c" ||
	{ echo "run.sh: hevc-m2.c changed shape; update the sed lines" >&2; exit 1; }
"${cc[@]}" -Wno-unused-parameter -Wno-unused-function -DPLAY_TOOL -include "${here}/shim/phoenix_shim.h" -I"${here}/shim" -I"${hd}" \
	"${work}/hevc-m2-host.c" "${hd}/hevc_parse.c" "${hd}/hevc_mp4.c" "${work}/mock.o" -lpthread -o "${work}/hevc-play-ref"

# --- compare --------------------------------------------------------------------------------
if [ "${#clips[@]}" = 0 ]; then
	mapfile -t clips < <(ls "${hd}"/testdata/*.265 "${hd}"/testdata/*.mp4 2>/dev/null)
	mt=0
else
	mt=1
fi
export ASAN_OPTIONS=detect_leaks=0
# the block's cross-process state (rpivid_hw.c's /tmp/.rpivid.dead on the Pi)
dead="${work}/rpivid.dead"
export MOCK_DEAD_PATH="${dead}"
rm -f "${dead}"
n_same=0 n_diff=0 n_skip=0 n_cpu=0
for c in "${clips[@]}"; do
	b="$(basename "${c}")" ref="${work}/${b}.ref.log" hw="${work}/${b}.hw.log"
	MOCK_LOG="${ref}" "${work}/hevc-play-ref" "${c}" >"${work}/${b}.ref.out" 2>&1 || true
	# the proven tool set only (FFMPEG_RPIVID_TOOLS=none): the reference implements that
	# subset, and e.g. ignores PPS deblocking offsets that the hwaccel sends (idr64.265)
	FFMPEG_RPIVID_TOOLS=none MOCK_LOG="${hw}" "${work}/hevc-rpivid-check" -hw "${c}" >"${work}/${b}.hw.out" 2>&1 || true
	touch "${ref}" "${hw}"
	if grep -q 'ERROR: AddressSanitizer' "${work}/${b}.hw.out"; then
		echo "ASAN      ${b}"; grep -A12 'ERROR: AddressSanitizer' "${work}/${b}.hw.out" | head -20; exit 1
	fi
	npic="$(grep -c '^PIC ' "${hw}" 2>/dev/null || true)"
	if grep -q 'rpivid: CPU decode:' "${work}/${b}.hw.out"; then
		echo "CPU       ${b}: $(grep -o 'rpivid: CPU decode: .*' "${work}/${b}.hw.out" | head -1)"
		n_cpu=$((n_cpu + 1))
	elif [ "${npic}" = 0 ] && grep -q 'continuing on the CPU decoder' "${work}/${b}.hw.out"; then
		echo "CPU       ${b}: $(grep -o 'rpivid: picture POC [0-9-]*: .*' "${work}/${b}.hw.out" | head -1)"
		n_cpu=$((n_cpu + 1))
	elif ! grep -q '^PIC ' "${ref}" 2>/dev/null; then
		echo "REF-SKIP  ${b}: ${npic} pictures on the hwaccel; reference: $(grep -E 'rejected|only|no |FAILED' "${work}/${b}.ref.out" | head -1)"
		n_skip=$((n_skip + 1))
	elif cmp -s "${ref}" "${hw}"; then
		echo "SAME      ${b}: ${npic} pictures, $(grep -c '^C ' "${hw}") command entries"
		n_same=$((n_same + 1))
	elif head -c "$(stat -c %s "${ref}")" "${hw}" | cmp -s - "${ref}" && [ "$(tail -c 1 "${ref}" | od -An -c | tr -d ' ')" = '\n' ]; then
		echo "PREFIX    ${b}: the reference stopped after $(grep -c '^PIC ' "${ref}") pictures ($(grep -E 'not in DPB|rejected|FAILED' "${work}/${b}.ref.out" | head -1)), identical up to there; the hwaccel decoded ${npic}"
		n_same=$((n_same + 1))
	else
		echo "DIFF      ${b}: ref $(grep -c '^PIC ' "${ref}") pictures, hwaccel ${npic}"
		{ diff "${ref}" "${hw}" || true; } | head -12 | sed 's/^/          /'
		n_diff=$((n_diff + 1))
	fi
	if [ "${mt}" = 1 ]; then
		FFMPEG_RPIVID_TOOLS=none MOCK_LOG="${hw}.mt" "${work}/hevc-rpivid-check" -hw -T "${threads}" "${c}" >"${work}/${b}.hw-mt.out" 2>&1 || true
		touch "${hw}.mt"
		if grep -q 'ERROR: AddressSanitizer' "${work}/${b}.hw-mt.out"; then
			echo "ASAN      ${b} (${threads} threads)"; grep -A12 'ERROR: AddressSanitizer' "${work}/${b}.hw-mt.out" | head -20; exit 1
		fi
		if cmp -s "${hw}" "${hw}.mt"; then
			echo "          ${b}: the same with ${threads} frame threads"
		else
			echo "          ${b}: DIFFERENT with ${threads} frame threads"; { diff "${hw}" "${hw}.mt" || true; } | head -6
			n_diff=$((n_diff + 1))
		fi
	fi
done
echo "RESULT same=${n_same} diff=${n_diff} ref-skip=${n_skip} cpu=${n_cpu}"

n_loop_bad=0
if [ "${loop}" = 1 ]; then
	for c in "${clips[@]}"; do
		b="$(basename "${c}")"
		IFS=, read -r w h pf < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,pix_fmt -of csv=p=0 "${c}")
		ffmpeg -nostdin -v error -y -i "${c}" -f rawvideo -pix_fmt "${pf}" "${work}/${b}.golden"
		for t in 1 "${threads}"; do
			MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${w}x${h}" MOCK_LOG=/dev/null \
				"${work}/hevc-rpivid-check" -cpuref -l 2 -T "${t}" "${c}" >"${work}/${b}.loop${t}.out" 2>&1 || true
			# hw_used=1: a hardware pass that fell back to the CPU (e.g. the block's lock file held
			# by a concurrent run) would be bit-exact against the CPU pass trivially
			v="$(grep -o 'verdict=[A-Z-]* frames=[0-9]* bad=[0-9]* first_bad=[0-9-]* hw_used=[0-9]' "${work}/${b}.loop${t}.out" || echo 'verdict=none')"
			echo "LOOP      ${b} threads=${t}: ${v}"
			case "${v}" in *BIT-EXACT*hw_used=1) ;; *) n_loop_bad=$((n_loop_bad + 1)) ;; esac
		done
		# the interrupt path (MOCK_ISR_RACE=1: a handler registered, running inside the waiter's
		# controller read: both see one completion): still BIT-EXACT, no completion left stale
		MOCK_ISR_RACE=1 MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${w}x${h}" MOCK_LOG=/dev/null \
			"${work}/hevc-rpivid-check" -cpuref -l 2 -T 1 "${c}" >"${work}/${b}.isr.out" 2>&1 || true
		if grep -q 'ERROR: AddressSanitizer' "${work}/${b}.isr.out"; then
			echo "ASAN      ${b} isr"; grep -A12 'ERROR: AddressSanitizer' "${work}/${b}.isr.out" | head -20; exit 1
		fi
		v="$(grep -o 'verdict=[A-Z-]* frames=[0-9]* bad=[0-9]* first_bad=[0-9-]* hw_used=[0-9]' "${work}/${b}.isr.out" || echo 'verdict=none')"
		z="$(grep -o 'completion by interrupt\|[0-9]* stale completions' "${work}/${b}.isr.out" | tr '\n' ' ')"
		echo "LOOP-ISR  ${b}: ${v}; ${z}"
		case "${v} ${z}" in *BIT-EXACT*hw_used=1*"completion by interrupt"*" 0 stale completions"*) ;; *) n_loop_bad=$((n_loop_bad + 1)) ;; esac
		# zero copy: 8-bit only (10-bit streams keep system-memory frames: nothing new to see)
		[ "${pf}" = yuv420p ] || continue
		for t in 1 "${threads}"; do
			MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${w}x${h}" MOCK_LOG=/dev/null \
				"${work}/hevc-rpivid-check" -cpuref -zc -l 2 -T "${t}" "${c}" >"${work}/${b}.zc${t}.out" 2>&1 || true
			if grep -q 'ERROR: AddressSanitizer' "${work}/${b}.zc${t}.out"; then
				echo "ASAN      ${b} zc threads=${t}"; grep -A12 'ERROR: AddressSanitizer' "${work}/${b}.zc${t}.out" | head -20; exit 1
			fi
			v="$(grep -o 'verdict=[A-Z-]* frames=[0-9]* bad=[0-9]* first_bad=[0-9-]* hw_used=[0-9] hw_fallback=[0-9]' "${work}/${b}.zc${t}.out" || echo 'verdict=none')"
			z="$(grep -o 'zc frames=[0-9]* drm_prime=[0-9]* buffers=[a-z]* held_checked=[0-9]* held_bad=[0-9]*' "${work}/${b}.zc${t}.out" | head -1 || true)"
			echo "LOOP-ZC   ${b} threads=${t}: ${v}; ${z:-no zc line}"
			if ! grep -Eq 'BIT-EXACT.*hw_used=1 hw_fallback=0' <<<"${v}" || ! grep -Eq 'drm_prime=[1-9][0-9]* .*held_bad=0$' <<<"${z}"; then
				n_loop_bad=$((n_loop_bad + 1))
			fi
		done
		# a CPU fallback -- the block refuses its 4th picture (FFMPEG_RPIVID_REFUSE_AT=3, once), or
		# fails its 6th (MOCK_FAIL_AT=5) --: pictures are dropped up to the next IRAP, where the
		# block is taken again (retakes=1; with FFMPEG_RPIVID_RETRIES=0, "noretry", it stays off
		# and the CPU decoder goes on); every frame that is emitted must be right (by pts against
		# the CPU decoder), in both outputs. MOCK_GOLDEN_IDR: the IDRs' display indices, since the
		# mock does not see the dropped pictures
		idrs="$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame -of csv=p=0 "${c}" | awk '$1 == 1 { print NR - 1 }' | paste -sd, -)"
		for k in refuse fail noretry; do
			for m in planar zc; do
				case "${k}" in
					refuse) env=(FFMPEG_RPIVID_REFUSE_AT=3) want_r=1 ;;
					fail) env=(MOCK_FAIL_AT=5) want_r=1 ;;
					noretry) env=(FFMPEG_RPIVID_REFUSE_AT=3 FFMPEG_RPIVID_RETRIES=0) want_r=0 ;;
				esac
				o="${work}/${b}.${k}-${m}.out"
				env "${env[@]}" MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${w}x${h}" MOCK_GOLDEN_IDR="${idrs}" MOCK_LOG=/dev/null \
					"${work}/hevc-rpivid-check" -cpuref -bypts -l 2 -T 1 $([ "${m}" = zc ] && echo -zc) "${c}" >"${o}" 2>&1 || true
				if grep -q 'ERROR: AddressSanitizer' "${o}"; then
					echo "ASAN      ${b} ${k} ${m}"; grep -A12 'ERROR: AddressSanitizer' "${o}" | head -20; exit 1
				fi
				v="$(grep -o 'bypts hw_frames=.*' "${o}" || echo 'bypts none')"
				r="$(grep -c 'back on the block from POC' "${o}" || true)"
				echo "LOOP-DROP ${b} ${k} ${m}: ${v} retakes=${r}"
				if ! grep -Eq 'matched=[0-9]+ wrong=0 unmatched=0 dropped=[1-9][0-9]* hw_used=1 hw_fallback=1' <<<"${v}" ||
						! grep -q 'decoding again from POC' "${o}" || [ "${r}" != "${want_r}" ]; then
					n_loop_bad=$((n_loop_bad + 1))
				fi
			done
		done
	done

	# --- a block that stops responding, and one out of reach ---
	wc="" ww="" wh=""
	for c in "${clips[@]}"; do
		IFS=, read -r w h pf < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,pix_fmt -of csv=p=0 "${c}")
		if [ "${pf}" = yuv420p ]; then wc="${c}" ww="${w}" wh="${h}"; break; fi
	done
	if [ -n "${wc}" ]; then
		b="$(basename "${wc}")"
		idrs="$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame -of csv=p=0 "${wc}" | awk '$1 == 1 { print NR - 1 }' | paste -sd, -)"
		# run <name> <expect regex for the output, in order> -- <env...>: one decoder process
		wrun() {
			local name="$1" mode="$2"; shift 2
			local o="${work}/${b}.wedge-${name}.out"
			env "$@" MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${ww}x${wh}" MOCK_GOLDEN_IDR="${idrs}" \
				MOCK_LOG="${work}/${b}.wedge-${name}.mock" "${work}/hevc-rpivid-check" -cpuref -l 2 -T 1 ${mode} "${wc}" >"${o}" 2>&1 || true
			if grep -q 'ERROR: AddressSanitizer' "${o}"; then
				echo "ASAN      ${b} wedge ${name}"; grep -A12 'ERROR: AddressSanitizer' "${o}" | head -20; exit 1
			fi
		}
		# wcheck <name> <ok 0/1> <what>: one verdict line
		wcheck() {
			echo "LOOP-WEDGE ${b} $1: $([ "$2" = 1 ] && echo ok || echo BAD) -- $3"
			[ "$2" = 1 ] || n_loop_bad=$((n_loop_bad + 1))
		}
		has() { grep -Eq -- "$2" "$1"; }
		o="${work}/${b}.wedge" m="${work}/${b}.wedge"
		bypts_ok='matched=[0-9]+ wrong=0 unmatched=0 dropped=[1-9][0-9]* hw_used=1 hw_fallback=1'

		# 1. phase 2 of the 4th picture never finishes: recorded, clock off, nothing after
		rm -f "${dead}"
		wrun stop -bypts MOCK_WEDGE_AT=3
		ok=1
		has "${o}-stop.out" "${bypts_ok}" && has "${o}-stop.out" 'timeout in phase 2 .*not used again until reboot \(clock switched off\)' &&
			has "${o}-stop.out" 'CPU decode: the block stopped responding earlier in this process' && has "${m}-stop.mock" '^MOCK clock off$' &&
			! has "${m}-stop.mock" 'register access with the clock off' && has "${dead}" '^wedged pid=[0-9]+ t=[0-9]+ phase 2 timed out' || ok=0
		wcheck stop "${ok}" "$(grep -o 'bypts hw_frames=[^ ]* .*' "${o}-stop.out" | cut -d' ' -f1-7); marker: $(cut -c1-60 "${dead}" 2>/dev/null)"

		# 2. the next process: CPU decode at once, no mailbox call, no register access
		wrun next ''
		ok=1
		has "${o}-next.out" 'CPU decode: the block stopped responding earlier in this boot' && has "${o}-next.out" 'verdict=BIT-EXACT .*hw_used=0' &&
			! has "${m}-next.mock" '^(PIC |MOCK )' || ok=0
		wcheck next "${ok}" "$(grep -o 'CPU decode: .\{0,60\}' "${o}-next.out" | head -1)"

		# 3. FFMPEG_RPIVID_RESET=1: a clock off/on, the block decodes again, the marker goes
		wrun reset '' FFMPEG_RPIVID_RESET=1
		ok=1
		has "${o}-reset.out" 'clock switched off and on' && has "${o}-reset.out" 'works again after the clock reset' &&
			has "${o}-reset.out" 'verdict=BIT-EXACT .*hw_used=1 hw_fallback=0' && [ ! -e "${dead}" ] &&
			[ "$(grep -c '^MOCK clock' "${m}-reset.mock")" = 2 ] && ! has "${m}-reset.mock" 'register access with the clock off' || ok=0
		wcheck reset "${ok}" "$(grep -o 'verdict=[A-Z-]* frames=[0-9]* bad=[0-9]*' "${o}-reset.out"); marker $([ -e "${dead}" ] && echo kept || echo removed)"

		# 4. a reset that does not help: still recorded, as tried
		echo "wedged pid=1 t=0 (the host test)" >"${dead}"
		wrun resetfail -bypts FFMPEG_RPIVID_RESET=1 MOCK_WEDGE_AT=0
		ok=1
		has "${o}-resetfail.out" "${bypts_ok}" && has "${dead}" '^reset-tried .*after a clock reset' || ok=0
		wcheck resetfail "${ok}" "marker: $(cut -c1-80 "${dead}" 2>/dev/null)"

		# 5. and is not tried again
		wrun once '' FFMPEG_RPIVID_RESET=1
		ok=1
		has "${o}-once.out" 'CPU decode: the block stopped responding earlier in this boot' && ! has "${o}-once.out" 'FFMPEG_RPIVID_RESET=1 tries' &&
			! has "${m}-once.mock" '^(PIC |MOCK )' || ok=0
		wcheck once "${ok}" "$(grep -o 'CPU decode: .\{0,60\}' "${o}-once.out" | head -1)"

		# 6. a marker from an earlier boot (a later time than now) is ignored and removed
		echo "wedged pid=1 t=999999999999 (an earlier boot)" >"${dead}"
		wrun oldboot ''
		ok=1
		has "${o}-oldboot.out" 'verdict=BIT-EXACT .*hw_used=1 hw_fallback=0' && [ ! -e "${dead}" ] || ok=0
		wcheck oldboot "${ok}" "$(grep -o 'verdict=[A-Z-]* frames=[0-9]* bad=[0-9]*' "${o}-oldboot.out")"

		# 7. zero copy with the picture buffer beyond the block's reach: system-memory frames,
		# still from the block
		zs="$(grep -o 'column height [0-9]*, [0-9]* bytes per picture' "${work}/${b}.zc1.out" | head -1 | awk '{ print $4 }')"
		if [ -n "${zs}" ]; then
			wrun reach -zc MOCK_PA_HIGH_SIZE=$(( (zs + 4095) / 4096 * 4096 ))
			ok=1
			has "${o}-reach.out" "drm_prime output: no contiguous memory the block reaches .*: system-memory frames for this stream" &&
				has "${o}-reach.out" 'verdict=BIT-EXACT .*hw_used=1 hw_fallback=0' && has "${o}-reach.out" 'zc frames=[0-9]+ drm_prime=0 ' || ok=0
			wcheck reach "${ok}" "$(grep -o 'drm_prime output: .\{0,70\}' "${o}-reach.out" | head -1)"
		else
			wcheck reach 0 "no zero-copy size in ${b}.zc1.out"
		fi
		rm -f "${dead}"
	fi
	echo "RESULT loop_bad=${n_loop_bad}"
fi
[ "${n_diff}" = 0 ] && [ "${n_loop_bad}" = 0 ]
