/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: the one libevdev function Weston calls --
 * libevdev_event_code_from_name(), which turns a weston.ini button name
 * ("BTN_MIDDLE") into its code for [libinput] scroll-button. Phoenix has no evdev
 * devices; shims/src/libevdev_phoenix.c knows the mouse button names.
 */
#ifndef WLPHX_LIBEVDEV_H
#define WLPHX_LIBEVDEV_H

#include <linux/input.h>

#ifdef __cplusplus
extern "C" {
#endif

int libevdev_event_code_from_name(unsigned int type, const char *name);

#ifdef __cplusplus
}
#endif

#endif
