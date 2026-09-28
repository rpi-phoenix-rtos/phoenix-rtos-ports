#!/bin/bash
#
# m7b-colors.sh -- runs INSIDE foot on the Pi (labwc-desktop.sh ... colors): draws what
# the m7b-foot HDMI snapshots grade, with no keyboard needed. Its output goes to foot's
# pty, not the UART.
#   1. a 24-bit colour bar (SGR 48;2;r;g;b), red -> green -> blue ramps
#   2. Unicode beyond Latin-1: Polish, Greek, Cyrillic, box drawing, arrows, a check mark
#   3. the 16 ANSI colours, bold/italic/underline
# then it holds the window for HOLD seconds (default 120).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%

HOLD=${HOLD:-120}
printf 'foot on Phoenix-RTOS: TERM=%s\n\n' "${TERM}"
printf '24-bit: '
for ((i = 0; i < 256; i += 4)); do   # 64 cells: fits foot's 80 columns
	printf '\033[48;2;%d;%d;%dm ' "$((255 - i))" "${i}" 0
done
printf '\033[0m\n        '
for ((i = 0; i < 256; i += 4)); do
	printf '\033[48;2;%d;%d;%dm ' 0 "$((255 - i))" "${i}"
done
printf '\033[0m\n\n'
printf 'Unicode: zażółć gęślą jaźń  αβγδε ΩΣΠ  привет  ─┼─ ┌┐└┘ ║═╬  → ← ↑ ↓  ✓ ✗ € °\n\n'
for c in 30 31 32 33 34 35 36 37; do printf '\033[%dm#%d\033[0m ' "${c}" "${c}"; done
printf '\n'
for c in 90 91 92 93 94 95 96 97; do printf '\033[%dm#%d\033[0m ' "${c}" "${c}"; done
printf '\n\033[1mbold\033[0m \033[3mitalic\033[0m \033[4munderline\033[0m \033[7mreverse\033[0m\n\n'
printf 'holding %ss\n' "${HOLD}"
sleep "${HOLD}"
