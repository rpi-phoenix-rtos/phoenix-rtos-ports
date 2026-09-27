/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: on Linux <sys/ioctl.h> declares struct winsize and the
 * TIOC*WINSZ requests (foot sizes its pty with them); libphoenix has them in
 * <termios.h>.
 */
#include_next <sys/ioctl.h>
#include <termios.h>
