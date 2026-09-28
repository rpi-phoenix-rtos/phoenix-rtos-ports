/*
 * Phoenix-RTOS
 *
 * Xorg-drm: "phxhid", an xf86 input driver for the Phoenix USB HID devices
 *
 * NEW GPU LANE, M4 (docs/gpu-new-lane/M4-xorg-modesetting.md). Phoenix has no evdev
 * and no libinput; usbkbd and usbmouse expose boot-protocol reports on /dev/kbd0 and
 * /dev/mouse0. This driver turns them into X input events -- the protocol handling of
 * the Phoenix kdrive server's input code, rewritten for the xf86 input-driver API:
 *
 *   keyboard  write one 0x01 byte to switch usbkbd to raw 8-byte HID boot reports
 *             ([0] modifier bits, [2..7] up to six usages); each report is diffed
 *             against the previous one into key down/up events; HID usage -> evdev
 *             keycode (phxhid_evdev_map.h) + 8 = the X keycode of the builtin
 *             evdev/pc105/us keymap (xorg-server patch 0003). Autorepeat is XKB's.
 *   mouse     4-byte boot reports: [0] buttons (bit0 left, bit1 right, bit2 middle),
 *             [1] dx, [2] dy (int8), [3] wheel (int8) -> relative motion, buttons
 *             1/3/2, wheel as buttons 4/5.
 *
 * Event source: a 10 ms server timer drains every enabled device with bounded
 * non-blocking reads -- not InputThreadRegisterDev()/xf86AddEnabledDevice(): poll() on
 * these device fds is answered by atPollStatus snapshots on the kernel's 20 ms cycle
 * (E5), so a readiness-driven reader would add 0-20 ms of latency per event for no
 * gain. Xorg-drm is built without the input thread, so the timer runs on the main
 * thread like every other event producer and needs no locking.
 *
 * Configuration (xorg.conf InputDevice):
 *   Driver "phxhid"; Option "Device" "/dev/kbd0" | "/dev/mouse0";
 *   Option "Type" "keyboard" | "mouse" (default: from the device name).
 * A device that cannot be opened is logged and left without events; it never fails
 * the server (a failed virtual core keyboard would abort it).
 *
 * UART-visible lines (ErrorF, graded by the M4 pre-registered cycle):
 *   PHXHID dev=<path> type=<t> open=<ok|errno> raw=<0|1>
 *   PHXHID first <keyboard|mouse> event ...   (once per device type)
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifdef HAVE_XORG_CONFIG_H
#include <xorg-config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/X.h>

#include "xf86.h"
#include "xf86Module.h"
#include "xf86Xinput.h"
#include "xorgVersion.h"
#include "exevents.h"
#include "xserver-properties.h"

#include "phxhid_evdev_map.h"

#define PHXHID_POLL_MS     10
#define PHXHID_MAX_DEVS    4
#define PHXHID_NBUTTONS    7      /* 1-3 buttons, 4/5 wheel, 6/7 unused (hwheel) */
#define PHXHID_READ_GUARD  64     /* reads per device per tick */
#define EVDEV_TO_X         8

typedef enum { PHXHID_KEYBOARD, PHXHID_MOUSE } phxhid_type_t;

typedef struct {
	phxhid_type_t type;
	char *path;
	uint8_t prev[8];          /* keyboard: last report; mouse: prev[0] = buttons */
	unsigned long bytes;
} phxhid_priv_t;

static struct {
	InputInfoPtr on[PHXHID_MAX_DEVS];
	OsTimerPtr timer;
	int first_key, first_ptr;
} phxhid;


static int
usage_present(const uint8_t *rep, uint8_t u)
{
	int i;

	for (i = 2; i < 8; i++)
		if (rep[i] == u)
			return 1;
	return 0;
}


static void
post_key(InputInfoPtr pInfo, unsigned char evdev, int down)
{
	if (evdev == 0)
		return;
	if (!phxhid.first_key) {
		phxhid.first_key = 1;
		ErrorF("PHXHID first keyboard event keycode=%d down=%d\n", evdev + EVDEV_TO_X, down);
	}
	xf86PostKeyboardEvent(pInfo->dev, evdev + EVDEV_TO_X, down);
}


static void
kbd_report(InputInfoPtr pInfo, phxhid_priv_t *p, const uint8_t *rep)
{
	int i;

	for (i = 0; i < 8; i++) {
		int now = (rep[0] >> i) & 1, was = (p->prev[0] >> i) & 1;
		if (now != was)
			post_key(pInfo, hidModEvdev[i], now);
	}
	for (i = 2; i < 8; i++)   /* releases first, then presses */
		if (p->prev[i] >= 0x04 && !usage_present(rep, p->prev[i]))
			post_key(pInfo, hidToEvdev[p->prev[i]], 0);
	for (i = 2; i < 8; i++)
		if (rep[i] >= 0x04 && !usage_present(p->prev, rep[i]))
			post_key(pInfo, hidToEvdev[rep[i]], 1);
	memcpy(p->prev, rep, 8);
}


static void
mouse_report(InputInfoPtr pInfo, phxhid_priv_t *p, const uint8_t *rep)
{
	static const int hid_to_x[3] = { 1, 3, 2 };   /* HID bit order: left, right, middle */
	int dx = (int8_t)rep[1], dy = (int8_t)rep[2], wheel = (int8_t)rep[3];
	int b;

	if (!phxhid.first_ptr) {
		phxhid.first_ptr = 1;
		ErrorF("PHXHID first mouse event dx=%d dy=%d buttons=0x%x wheel=%d\n", dx, dy, rep[0], wheel);
	}
	if (dx != 0 || dy != 0)
		xf86PostMotionEvent(pInfo->dev, Relative, 0, 2, dx, dy);
	for (b = 0; b < 3; b++) {
		int now = (rep[0] >> b) & 1, was = (p->prev[0] >> b) & 1;
		if (now != was)
			xf86PostButtonEvent(pInfo->dev, Relative, hid_to_x[b], now, 0, 0);
	}
	p->prev[0] = rep[0];
	for (; wheel > 0; wheel--) {
		xf86PostButtonEvent(pInfo->dev, Relative, 4, 1, 0, 0);
		xf86PostButtonEvent(pInfo->dev, Relative, 4, 0, 0, 0);
	}
	for (; wheel < 0; wheel++) {
		xf86PostButtonEvent(pInfo->dev, Relative, 5, 1, 0, 0);
		xf86PostButtonEvent(pInfo->dev, Relative, 5, 0, 0, 0);
	}
}


static void
drain(InputInfoPtr pInfo)
{
	phxhid_priv_t *p = pInfo->private;
	const int rep = (p->type == PHXHID_KEYBOARD) ? 8 : 4;
	uint8_t buf[64];
	ssize_t r;
	int off, guard;

	for (guard = 0; guard < PHXHID_READ_GUARD; guard++) {
		r = read(pInfo->fd, buf, sizeof(buf) - (sizeof(buf) % rep));
		if (r <= 0)
			break;
		p->bytes += (unsigned long)r;
		for (off = 0; off + rep <= r; off += rep) {
			if (p->type == PHXHID_KEYBOARD)
				kbd_report(pInfo, p, buf + off);
			else
				mouse_report(pInfo, p, buf + off);
		}
	}
}


static CARD32
phxhid_timer(OsTimerPtr timer, CARD32 now, void *arg)
{
	int i;

	(void)timer;
	(void)now;
	(void)arg;
	for (i = 0; i < PHXHID_MAX_DEVS; i++)
		if (phxhid.on[i] != NULL && phxhid.on[i]->fd >= 0)
			drain(phxhid.on[i]);
	return PHXHID_POLL_MS;
}


static void
track(InputInfoPtr pInfo, int on)
{
	int i, any = 0;

	for (i = 0; i < PHXHID_MAX_DEVS; i++) {
		if (on && phxhid.on[i] == NULL) {
			phxhid.on[i] = pInfo;
			on = 0;
		}
		else if (!on && phxhid.on[i] == pInfo) {
			phxhid.on[i] = NULL;
		}
		any |= (phxhid.on[i] != NULL);
	}
	if (any && phxhid.timer == NULL)
		phxhid.timer = TimerSet(NULL, 0, PHXHID_POLL_MS, phxhid_timer, NULL);
	else if (!any && phxhid.timer != NULL) {
		TimerFree(phxhid.timer);
		phxhid.timer = NULL;
	}
}


static void
ptr_ctrl(DeviceIntPtr dev, PtrCtrl *ctrl)
{
	(void)dev;
	(void)ctrl;
}


/* dix and XKB call the keyboard's bell and control procs unconditionally (ChangeKeyboardControl,
 * indicator updates): usbkbd has neither a bell nor settable LEDs through this path. */
static void
kbd_bell(int percent, DeviceIntPtr dev, void *ctrl, int class)
{
	(void)percent;
	(void)dev;
	(void)ctrl;
	(void)class;
}


static void
kbd_ctrl(DeviceIntPtr dev, KeybdCtrl *ctrl)
{
	(void)dev;
	(void)ctrl;
}


static Bool
init_pointer(DeviceIntPtr dev)
{
	unsigned char map[PHXHID_NBUTTONS + 1];
	Atom btn_labels[PHXHID_NBUTTONS] = { 0 };
	Atom axes_labels[2] = { 0 };
	int i;

	for (i = 0; i <= PHXHID_NBUTTONS; i++)
		map[i] = i;
	btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
	btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
	btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
	btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
	btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
	btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
	btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
	axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
	axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);

	if (!InitPointerDeviceStruct((DevicePtr)dev, map, PHXHID_NBUTTONS, btn_labels, ptr_ctrl,
			GetMotionHistorySize(), 2, axes_labels))
		return FALSE;
	for (i = 0; i < 2; i++) {
		xf86InitValuatorAxisStruct(dev, i, axes_labels[i], -1, -1, 1, 0, 1, Relative);
		xf86InitValuatorDefaults(dev, i);
	}
	return TRUE;
}


static int
open_device(InputInfoPtr pInfo)
{
	phxhid_priv_t *p = pInfo->private;
	int tries, err = 0, raw = 0;
	const unsigned char rawmode = 1;

	/* /dev/kbd0 has a single opener: the console (pl011-tty) holds it in text mode and
	 * lets go asynchronously once the display leaves text mode (rpi4-kms -C) -- retry
	 * briefly to absorb that race, as the kdrive server does. */
	for (tries = 0; tries < 40; tries++) {
		pInfo->fd = open(p->path, ((p->type == PHXHID_KEYBOARD) ? O_RDWR : O_RDONLY) | O_NONBLOCK);
		if (pInfo->fd >= 0)
			break;
		err = errno;
		usleep(25000);
	}
	if (pInfo->fd >= 0 && p->type == PHXHID_KEYBOARD)
		raw = (write(pInfo->fd, &rawmode, 1) == 1);
	if (pInfo->fd >= 0)
		ErrorF("PHXHID dev=%s type=%s open=ok fd=%d raw=%d tries=%d\n", p->path,
			(p->type == PHXHID_KEYBOARD) ? "keyboard" : "mouse", pInfo->fd, raw, tries);
	else
		ErrorF("PHXHID dev=%s type=%s open=%s -- no events from this device\n", p->path,
			(p->type == PHXHID_KEYBOARD) ? "keyboard" : "mouse", strerror(err));
	memset(p->prev, 0, sizeof(p->prev));
	return pInfo->fd;
}


static Bool
phxhid_control(DeviceIntPtr dev, int what)
{
	InputInfoPtr pInfo = dev->public.devicePrivate;
	phxhid_priv_t *p = pInfo->private;

	switch (what) {
		case DEVICE_INIT:
			if (p->type == PHXHID_KEYBOARD)
				return InitKeyboardDeviceStruct(dev, NULL, kbd_bell, kbd_ctrl) ? Success : BadAlloc;
			return init_pointer(dev) ? Success : BadAlloc;

		case DEVICE_ON:
			if (pInfo->fd < 0)
				(void)open_device(pInfo);   /* never fatal, see above */
			track(pInfo, 1);
			dev->public.on = TRUE;
			return Success;

		case DEVICE_OFF:
		case DEVICE_CLOSE:
			track(pInfo, 0);
			if (pInfo->fd >= 0) {
				close(pInfo->fd);
				pInfo->fd = -1;
			}
			dev->public.on = FALSE;
			return Success;

		default:
			return BadValue;
	}
}


static int
phxhid_preinit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	phxhid_priv_t *p;
	char *type;

	(void)drv;
	(void)flags;
	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return BadAlloc;
	p->path = xf86SetStrOption(pInfo->options, "Device", NULL);
	if (p->path == NULL) {
		xf86IDrvMsg(pInfo, X_ERROR, "phxhid: no Option \"Device\"\n");
		free(p);
		return BadValue;
	}
	type = xf86SetStrOption(pInfo->options, "Type", NULL);
	if (type != NULL)
		p->type = (strcmp(type, "mouse") == 0 || strcmp(type, "pointer") == 0) ? PHXHID_MOUSE : PHXHID_KEYBOARD;
	else
		p->type = (strstr(p->path, "mouse") != NULL) ? PHXHID_MOUSE : PHXHID_KEYBOARD;
	free(type);

	pInfo->private = p;
	pInfo->type_name = (p->type == PHXHID_KEYBOARD) ? XI_KEYBOARD : XI_MOUSE;
	pInfo->device_control = phxhid_control;
	pInfo->read_input = NULL;
	pInfo->switch_mode = NULL;
	pInfo->fd = -1;
	xf86IDrvMsg(pInfo, X_INFO, "phxhid %s on %s (timer-drained every %d ms)\n",
		(p->type == PHXHID_KEYBOARD) ? "keyboard" : "mouse", p->path, PHXHID_POLL_MS);
	return Success;
}


static void
phxhid_uninit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
	phxhid_priv_t *p = pInfo->private;

	(void)drv;
	if (p != NULL) {
		free(p->path);
		free(p);
		pInfo->private = NULL;
	}
	xf86DeleteInput(pInfo, flags);   /* DeleteInputDeviceRequest leaves this to UnInit */
}


static InputDriverRec PHXHID = {
	1,
	"phxhid",
	NULL,
	phxhid_preinit,
	phxhid_uninit,
	NULL,
	NULL,
	0,
};


static void *
phxhid_plug(void *module, void *options, int *errmaj, int *errmin)
{
	(void)options;
	(void)errmaj;
	(void)errmin;
	xf86AddInputDriver(&PHXHID, module, 0);
	return module;
}


static XF86ModuleVersionInfo phxhid_vers = {
	"phxhid",
	"Phoenix Systems",
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	1, 0, 0,
	ABI_CLASS_XINPUT,
	ABI_XINPUT_VERSION,
	MOD_CLASS_XINPUT,
	{ 0, 0, 0, 0 }
};

_X_EXPORT XF86ModuleData phxhidModuleData = { &phxhid_vers, phxhid_plug, NULL };
