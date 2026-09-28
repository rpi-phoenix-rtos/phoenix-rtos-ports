#!/usr/bin/env bash
#
# Check that an SDL clone links the frame-pacing order of KMSDRM_GLES_SwapWindow (sdl2-drm
# patches/0009, docs/gpu-new-lane/frame-pacing.md): the frame is submitted (eglSwapBuffers, lock
# the new front buffer, KMSDRM_FBFromBO) BEFORE the wait for the previous page flip. Stock SDL
# 2.30.12 waits first, which locks a GPU-heavy game to 30.00 fps at 60 Hz.
#
# The patch adds no string, so the check reads the code: in the objdump of <KMSDRM_GLES_SwapWindow>
# the first direct call of KMSDRM_WaitPageflip must come after the call of KMSDRM_FBFromBO (stock:
# WaitPageflip first; FBFromBO is not inlined in either order, and the function keeps its second
# WaitPageflip call, SDL_VIDEO_DOUBLE_BUFFER's, after the flip).
#
# Usage: gamedrm/check-swap-order.sh <linked ELF>...   (unstripped: the stripped copies have no symbols)
# Env:   GAMEDRM_OBJDUMP  the aarch64-phoenix objdump (default: <repo>/.toolchain/...; the ports
#        copy of this script, sdl2_kmsdrm/gamedrm/, is run by the port recipes with it set)
# Prints one `swap-order <elf>: submit-first|wait-first|...` line per ELF; exit 1 unless every
# ELF is submit-first.
#
# Copyright 2026 Phoenix Systems
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${here}/../../.." && pwd)"
objdump="${GAMEDRM_OBJDUMP:-${repo_root}/.toolchain/aarch64-phoenix/bin/aarch64-phoenix-objdump}"
[ "$#" -ge 1 ] || { echo "usage: check-swap-order.sh <elf>..." >&2; exit 2; }
[ -x "${objdump}" ] || { echo "check-swap-order: no ${objdump}" >&2; exit 2; }

rc=0
for elf in "$@"; do
	if [ ! -f "${elf}" ]; then echo "swap-order ${elf}: MISSING"; rc=1; continue; fi
	body="$("${objdump}" -d --no-show-raw-insn --disassemble=KMSDRM_GLES_SwapWindow "${elf}" 2>/dev/null \
		| awk '/^[0-9a-f]+ <KMSDRM_GLES_SwapWindow>:$/{f=1; next} f && /^$/{exit} f')"
	if [ -z "${body}" ]; then echo "swap-order ${elf}: NO KMSDRM_GLES_SwapWindow"; rc=1; continue; fi
	fb="$(grep -nE '\sbl\s+[0-9a-f]+ <KMSDRM_FBFromBO>$' <<< "${body}" | head -1 | cut -d: -f1)"
	wait="$(grep -nE '\sbl\s+[0-9a-f]+ <KMSDRM_WaitPageflip>$' <<< "${body}" | head -1 | cut -d: -f1)"
	nwait="$(grep -cE '\sbl\s+[0-9a-f]+ <KMSDRM_WaitPageflip>$' <<< "${body}" || true)"
	if [ -z "${fb}" ] || [ -z "${wait}" ]; then
		echo "swap-order ${elf}: UNKNOWN (FBFromBO call at insn ${fb:-none}, WaitPageflip at ${wait:-none})"; rc=1
	elif [ "${fb}" -lt "${wait}" ]; then
		echo "swap-order ${elf}: submit-first (FBFromBO at insn ${fb}, first of ${nwait} WaitPageflip at ${wait})"
	else
		echo "swap-order ${elf}: wait-first (stock; first WaitPageflip at insn ${wait}, FBFromBO at ${fb})"; rc=1
	fi
done
exit "${rc}"
