/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: libevdev_event_code_from_name() for the names Weston's
 * weston.ini accepts as [libinput] scroll-button (see shims/include/libevdev/
 * libevdev.h). Unknown names and types answer -1, as libevdev does.
 */

#include <string.h>

#include <libevdev/libevdev.h>

static const struct {
	const char *name;
	int code;
} names[] = {
	{ "BTN_LEFT", BTN_LEFT },
	{ "BTN_RIGHT", BTN_RIGHT },
	{ "BTN_MIDDLE", BTN_MIDDLE },
	{ "BTN_SIDE", BTN_SIDE },
	{ "BTN_EXTRA", BTN_EXTRA },
	{ "BTN_FORWARD", BTN_FORWARD },
	{ "BTN_BACK", BTN_BACK },
	{ "BTN_TASK", BTN_TASK },
};

int libevdev_event_code_from_name(unsigned int type, const char *name)
{
	size_t i;

	if ((type != EV_KEY) || (name == NULL)) {
		return -1;
	}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(names[i].name, name) == 0) {
			return names[i].code;
		}
	}
	return -1;
}
