/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: USB HID boot-keyboard usage -> evdev keycode, from the table
 * the new lane's Xorg input driver uses (tools/gpu-lane/xorg-drm/src/
 * phxhid_evdev_map.h: FreeBSD's evdev_usb_scancodes[], BSD-2). A translation
 * unit of its own: that header defines the KEY_* names itself, which must not
 * meet <linux/input.h>'s.
 */

#include "phxhid_evdev_map.h"

unsigned int libinput_phoenix_hid_key(unsigned int usage);
unsigned int libinput_phoenix_hid_modifier(unsigned int bit);

unsigned int libinput_phoenix_hid_key(unsigned int usage)
{
	return (usage < 256u) ? hidToEvdev[usage] : 0u;
}


unsigned int libinput_phoenix_hid_modifier(unsigned int bit)
{
	return (bit < 8u) ? hidModEvdev[bit] : 0u;
}
