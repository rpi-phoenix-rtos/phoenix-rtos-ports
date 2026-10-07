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
# equal the CPU decoder's frame of the same pts.
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
	printf '/* host test: the Phoenix-RTOS hardware layer against hosttest/mock.c */\n#define __phoenix__ 1\n#define RPIVID_MMIO_HOOKS 1\n#include "phoenix_shim.h"\n#include "%s/src/rpivid_hw.c"\n' "${rp}" \
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
		# a CPU fallback -- the block refuses its 4th picture (FFMPEG_RPIVID_REFUSE_AT=3), or
		# fails its 6th (MOCK_FAIL_AT=5) --: pictures are dropped up to the next IRAP, then the CPU
		# decoder goes on; every frame that is emitted must be right (by pts against the CPU
		# decoder), in both outputs
		for k in refuse fail; do
			for m in planar zc; do
				if [ "${k}" = refuse ]; then env=(FFMPEG_RPIVID_REFUSE_AT=3); else env=(MOCK_FAIL_AT=5); fi
				o="${work}/${b}.${k}-${m}.out"
				env "${env[@]}" MOCK_GOLDEN="${work}/${b}.golden" MOCK_GOLDEN_SIZE="${w}x${h}" MOCK_LOG=/dev/null \
					"${work}/hevc-rpivid-check" -cpuref -bypts -l 2 -T 1 $([ "${m}" = zc ] && echo -zc) "${c}" >"${o}" 2>&1 || true
				if grep -q 'ERROR: AddressSanitizer' "${o}"; then
					echo "ASAN      ${b} ${k} ${m}"; grep -A12 'ERROR: AddressSanitizer' "${o}" | head -20; exit 1
				fi
				v="$(grep -o 'bypts hw_frames=.*' "${o}" || echo 'bypts none')"
				echo "LOOP-DROP ${b} ${k} ${m}: ${v}"
				if ! grep -Eq 'matched=[0-9]+ wrong=0 unmatched=0 dropped=[1-9][0-9]* hw_used=1 hw_fallback=1' <<<"${v}" ||
						! grep -q 'decoding again from POC' "${o}"; then
					n_loop_bad=$((n_loop_bad + 1))
				fi
			done
		done
	done
	echo "RESULT loop_bad=${n_loop_bad}"
fi
[ "${n_diff}" = 0 ] && [ "${n_loop_bad}" = 0 ]
