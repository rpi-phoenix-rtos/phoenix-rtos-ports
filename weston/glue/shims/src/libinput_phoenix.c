/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: libinput-phoenix -- the libinput API (upstream libinput.h,
 * MIT, fetched by build.sh) over Phoenix-RTOS's USB HID devices: usbkbd
 * /dev/kbd0 (raw 8-byte boot reports after writing 0x01) and usbmouse
 * /dev/mouse0 (4-byte boot reports), the same devices and report handling as the
 * new lane's Xorg driver phxhid (tools/gpu-lane/xorg-drm/src/phxhid.c).
 *
 * Model:
 *  - libinput_udev_assign_seat() opens the configured devices through the
 *    caller's open_restricted() (Weston: its launcher -> libseat noop -> open());
 *    a device that cannot be opened yet (the console holds /dev/kbd0 until the
 *    display leaves text mode) is retried from libinput_dispatch() once a second.
 *  - A reader thread polls the open devices every LIPHX_POLL_MS with non-blocking
 *    reads (device descriptors ride the kernel's 20 ms poll cycle, M3 G12),
 *    turns reports into events on a queue and wakes the caller through the
 *    descriptor libinput_get_fd() returns (a socketpair: immediate wake-up).
 *  - Events: DEVICE_ADDED, KEYBOARD_KEY (evdev codes), POINTER_MOTION (relative,
 *    unaccelerated = accelerated), POINTER_BUTTON (BTN_LEFT/RIGHT/MIDDLE),
 *    POINTER_AXIS (wheel, 15 degrees per detent). No touch, tablet, gesture or
 *    switch devices: those accessors exist for the linker and answer "none".
 *  - Configuration: every device answers "not available" (no acceleration
 *    profiles, tapping, calibration, ...); setters return UNSUPPORTED.
 *
 * Environment: LIBINPUT_PHOENIX_DEVICES = comma-separated path:kind list
 * (kind = keyboard|mouse; default "/dev/kbd0:keyboard,/dev/mouse0:mouse"; empty
 * = no devices, a display-only compositor).
 * Log lines start with "LIBINPUT-PHX " through the caller's log handler.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>

#include <libinput.h>
#include <linux/input.h>

#define LIPHX_POLL_MS    8
#define LIPHX_MAX_DEVS   4
#define LIPHX_RETRY_MS   1000
#define LIPHX_MAX_EVENTS 1024 /* queue bound: a stalled compositor loses input, not memory */

unsigned int libinput_phoenix_hid_key(unsigned int usage);
unsigned int libinput_phoenix_hid_modifier(unsigned int bit);

enum liphx_kind { LIPHX_KEYBOARD, LIPHX_MOUSE };

struct libinput_seat {
	int refs;
	struct libinput *li;
	void *user_data;
};

struct libinput_device {
	int refs;
	struct libinput *li;
	enum liphx_kind kind;
	char *path;
	char *sysname;
	int fd;            /* -1 while not open */
	int added;         /* DEVICE_ADDED queued */
	int open_failures; /* logged once */
	uint8_t prev[8];   /* keyboard: last report; mouse: prev[0] = buttons */
	void *user_data;
	unsigned long reports;
};

struct libinput_event {
	struct libinput_event *next;
	enum libinput_event_type type;
	struct libinput_device *dev;
	uint64_t time_usec;
	uint32_t code;     /* key or button */
	uint32_t state;    /* key/button state */
	uint32_t seat_count;
	double dx, dy;
	double axis_v;     /* vertical scroll, degrees */
	int32_t discrete_v;
};

/* The per-type event structs of the API are the one event record. */
struct libinput_event_keyboard { struct libinput_event base; };
struct libinput_event_pointer { struct libinput_event base; };
struct libinput_event_touch { struct libinput_event base; };
struct libinput_event_tablet_tool { struct libinput_event base; };

struct libinput {
	int refs;
	const struct libinput_interface *iface;
	void *user_data;
	libinput_log_handler log;
	enum libinput_log_priority prio;
	struct libinput_seat seat;
	struct libinput_device *devs[LIPHX_MAX_DEVS];
	int ndevs;
	int wake[2];
	pthread_t thread;
	int thread_up;
	volatile int stop;
	volatile int suspended;
	pthread_mutex_t lock;   /* the queue, device fds */
	struct libinput_event *head, *tail;
	int nqueued;
	uint64_t last_retry_usec;
	uint32_t keys_down;     /* seat-wide pressed key count (one keyboard) */
	uint32_t buttons_down;
};


static uint64_t now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}


static void liphx_log(struct libinput *li, enum libinput_log_priority prio, const char *fmt, ...)
{
	va_list ap;

	if ((li->log == NULL) || (prio < li->prio)) {
		return;
	}
	va_start(ap, fmt);
	li->log(li, prio, fmt, ap);
	va_end(ap);
}


static void wake(struct libinput *li)
{
	char c = 1;

	(void)write(li->wake[1], &c, 1); /* non-blocking: a full socket already means "wake" */
}


/* Under li->lock. */
static struct libinput_event *event_new(struct libinput *li, struct libinput_device *dev,
	enum libinput_event_type type)
{
	struct libinput_event *e;

	if (li->nqueued >= LIPHX_MAX_EVENTS) {
		return NULL;
	}
	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		return NULL;
	}
	e->type = type;
	e->dev = libinput_device_ref(dev);
	e->time_usec = now_usec();
	if (li->tail != NULL) {
		li->tail->next = e;
	}
	else {
		li->head = e;
	}
	li->tail = e;
	li->nqueued++;
	return e;
}


static void post_key(struct libinput *li, struct libinput_device *d, unsigned int code, int down)
{
	struct libinput_event *e;

	if (code == 0u) {
		return;
	}
	if (down) {
		li->keys_down++;
	}
	else if (li->keys_down > 0u) {
		li->keys_down--;
	}
	e = event_new(li, d, LIBINPUT_EVENT_KEYBOARD_KEY);
	if (e != NULL) {
		e->code = code;
		e->state = down ? LIBINPUT_KEY_STATE_PRESSED : LIBINPUT_KEY_STATE_RELEASED;
		e->seat_count = li->keys_down;
	}
}


static int usage_present(const uint8_t *rep, uint8_t u)
{
	int i;

	for (i = 2; i < 8; i++) {
		if (rep[i] == u) {
			return 1;
		}
	}
	return 0;
}


static void kbd_report(struct libinput *li, struct libinput_device *d, const uint8_t *rep)
{
	int i;

	for (i = 0; i < 8; i++) {
		int now = (rep[0] >> i) & 1, was = (d->prev[0] >> i) & 1;
		if (now != was) {
			post_key(li, d, libinput_phoenix_hid_modifier((unsigned)i), now);
		}
	}
	for (i = 2; i < 8; i++) { /* releases first, then presses */
		if ((d->prev[i] >= 0x04) && !usage_present(rep, d->prev[i])) {
			post_key(li, d, libinput_phoenix_hid_key(d->prev[i]), 0);
		}
	}
	for (i = 2; i < 8; i++) {
		if ((rep[i] >= 0x04) && !usage_present(d->prev, rep[i])) {
			post_key(li, d, libinput_phoenix_hid_key(rep[i]), 1);
		}
	}
	memcpy(d->prev, rep, 8);
}


static void mouse_report(struct libinput *li, struct libinput_device *d, const uint8_t *rep)
{
	static const uint32_t hid_to_btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
	int dx = (int8_t)rep[1], dy = (int8_t)rep[2], wheel = (int8_t)rep[3];
	struct libinput_event *e;
	int b;

	if ((dx != 0) || (dy != 0)) {
		e = event_new(li, d, LIBINPUT_EVENT_POINTER_MOTION);
		if (e != NULL) {
			e->dx = dx;
			e->dy = dy;
		}
	}
	for (b = 0; b < 3; b++) {
		int now = (rep[0] >> b) & 1, was = (d->prev[0] >> b) & 1;
		if (now == was) {
			continue;
		}
		if (now) {
			li->buttons_down++;
		}
		else if (li->buttons_down > 0u) {
			li->buttons_down--;
		}
		e = event_new(li, d, LIBINPUT_EVENT_POINTER_BUTTON);
		if (e != NULL) {
			e->code = hid_to_btn[b];
			e->state = now ? LIBINPUT_BUTTON_STATE_PRESSED : LIBINPUT_BUTTON_STATE_RELEASED;
			e->seat_count = li->buttons_down;
		}
	}
	d->prev[0] = rep[0];
	if (wheel != 0) {
		/* HID: positive = away from the user (up); libinput: positive = down */
		e = event_new(li, d, LIBINPUT_EVENT_POINTER_AXIS);
		if (e != NULL) {
			e->axis_v = -15.0 * wheel;
			e->discrete_v = -wheel;
		}
		/* libinput >= 1.19 sends SCROLL_WHEEL in addition to the deprecated AXIS
		 * event; wlroots reads only the former, Weston only the latter. */
		e = event_new(li, d, LIBINPUT_EVENT_POINTER_SCROLL_WHEEL);
		if (e != NULL) {
			e->axis_v = -15.0 * wheel;
			e->discrete_v = -wheel;
		}
	}
}


static void *reader(void *arg)
{
	struct libinput *li = arg;
	uint8_t buf[64];
	int i, off, guard, queued, missing;
	ssize_t r;
	uint64_t last_nudge = now_usec();

	while (!li->stop) {
		queued = 0;
		missing = 0;
		pthread_mutex_lock(&li->lock);
		for (i = 0; i < li->ndevs; i++) {
			struct libinput_device *d = li->devs[i];
			const int rep = (d->kind == LIPHX_KEYBOARD) ? 8 : 4;
			if (d->fd < 0) {
				missing = 1;
				continue;
			}
			for (guard = 0; guard < 64; guard++) {
				r = read(d->fd, buf, sizeof(buf) - (sizeof(buf) % (size_t)rep));
				if (r <= 0) {
					break;
				}
				if (li->suspended) {
					continue;
				}
				for (off = 0; off + rep <= r; off += rep) {
					d->reports++;
					if (d->kind == LIPHX_KEYBOARD) {
						kbd_report(li, d, buf + off);
					}
					else {
						mouse_report(li, d, buf + off);
					}
				}
			}
		}
		queued = (li->head != NULL);
		pthread_mutex_unlock(&li->lock);
		/* A device not open yet is retried by libinput_dispatch() on the caller's
		 * thread (open_restricted is the caller's code): make it run once a second. */
		if (missing && (now_usec() - last_nudge >= (uint64_t)LIPHX_RETRY_MS * 1000u)) {
			last_nudge = now_usec();
			queued = 1;
		}
		if (queued) {
			wake(li);
		}
		usleep(LIPHX_POLL_MS * 1000);
	}
	return NULL;
}


/* Main thread only (open_restricted is the caller's code). */
static void try_open(struct libinput *li, struct libinput_device *d)
{
	int fd;
	uint8_t rawmode = 1;

	fd = li->iface->open_restricted(d->path, ((d->kind == LIPHX_KEYBOARD) ? O_RDWR : O_RDONLY) | O_NONBLOCK,
		li->user_data);
	if ((fd < 0) && (d->kind == LIPHX_MOUSE)) {
		/* libseat's noop backend opens O_RDWR whatever is asked; usbmouse may refuse writers */
		fd = open(d->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (fd < 0) {
		if (d->open_failures++ == 0) {
			liphx_log(li, LIBINPUT_LOG_PRIORITY_INFO, "LIBINPUT-PHX dev=%s kind=%s open=failed errno=%d (retrying every %d ms)\n",
				d->path, (d->kind == LIPHX_KEYBOARD) ? "keyboard" : "mouse", errno, LIPHX_RETRY_MS);
		}
		return;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	if ((d->kind == LIPHX_KEYBOARD) && (write(fd, &rawmode, 1) != 1)) {
		liphx_log(li, LIBINPUT_LOG_PRIORITY_ERROR, "LIBINPUT-PHX dev=%s raw mode refused errno=%d\n", d->path, errno);
	}
	pthread_mutex_lock(&li->lock);
	d->fd = fd;
	if (!d->added && (event_new(li, d, LIBINPUT_EVENT_DEVICE_ADDED) != NULL)) {
		d->added = 1;
	}
	pthread_mutex_unlock(&li->lock);
	liphx_log(li, LIBINPUT_LOG_PRIORITY_INFO, "LIBINPUT-PHX dev=%s kind=%s open=ok fd=%d\n", d->path,
		(d->kind == LIPHX_KEYBOARD) ? "keyboard" : "mouse", fd);
}


static int add_device(struct libinput *li, const char *path, enum liphx_kind kind)
{
	struct libinput_device *d;
	const char *base;

	if (li->ndevs == LIPHX_MAX_DEVS) {
		return -1;
	}
	d = calloc(1, sizeof(*d));
	if (d == NULL) {
		return -1;
	}
	d->refs = 1;
	d->li = li;
	d->kind = kind;
	d->fd = -1;
	d->path = strdup(path);
	base = strrchr(path, '/');
	d->sysname = strdup((base != NULL) ? base + 1 : path);
	if ((d->path == NULL) || (d->sysname == NULL)) {
		free(d->path);
		free(d->sysname);
		free(d);
		return -1;
	}
	li->devs[li->ndevs++] = d;
	return 0;
}


struct libinput *libinput_udev_create_context(const struct libinput_interface *interface, void *user_data,
	struct udev *udev)
{
	struct libinput *li;

	(void)udev;
	if ((interface == NULL) || (interface->open_restricted == NULL) || (interface->close_restricted == NULL)) {
		return NULL;
	}
	li = calloc(1, sizeof(*li));
	if (li == NULL) {
		return NULL;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, li->wake) < 0) {
		free(li);
		return NULL;
	}
	(void)fcntl(li->wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(li->wake[1], F_SETFL, O_NONBLOCK);
	(void)fcntl(li->wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(li->wake[1], F_SETFD, FD_CLOEXEC);
	pthread_mutex_init(&li->lock, NULL);
	li->refs = 1;
	li->iface = interface;
	li->user_data = user_data;
	li->prio = LIBINPUT_LOG_PRIORITY_ERROR;
	li->seat.refs = 1;
	li->seat.li = li;
	return li;
}


int libinput_udev_assign_seat(struct libinput *li, const char *seat_id)
{
	const char *cfg = getenv("LIBINPUT_PHOENIX_DEVICES");
	char *list, *tok, *save = NULL, *colon;
	int i;

	if ((seat_id == NULL) || (strcmp(seat_id, "seat0") != 0) || (li->ndevs != 0)) {
		return -1;
	}
	list = strdup((cfg != NULL) ? cfg : "/dev/kbd0:keyboard,/dev/mouse0:mouse");
	if (list == NULL) {
		return -1;
	}
	for (tok = strtok_r(list, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
		colon = strrchr(tok, ':');
		if (colon == NULL) {
			continue;
		}
		*colon = '\0';
		(void)add_device(li, tok, (strcmp(colon + 1, "mouse") == 0) ? LIPHX_MOUSE : LIPHX_KEYBOARD);
	}
	free(list);
	for (i = 0; i < li->ndevs; i++) {
		try_open(li, li->devs[i]);
	}
	li->last_retry_usec = now_usec();
	if ((li->ndevs > 0) && (pthread_create(&li->thread, NULL, reader, li) == 0)) {
		li->thread_up = 1;
	}
	liphx_log(li, LIBINPUT_LOG_PRIORITY_INFO, "LIBINPUT-PHX seat=%s devices=%d reader=%d\n", seat_id, li->ndevs,
		li->thread_up);
	if (li->head != NULL) {
		wake(li);
	}
	return 0;
}


struct libinput *libinput_ref(struct libinput *li)
{
	li->refs++;
	return li;
}


struct libinput *libinput_unref(struct libinput *li)
{
	int i;
	struct libinput_event *e;

	if ((li == NULL) || (--li->refs > 0)) {
		return li;
	}
	li->stop = 1;
	if (li->thread_up) {
		pthread_join(li->thread, NULL);
	}
	while ((e = li->head) != NULL) {
		li->head = e->next;
		libinput_device_unref(e->dev);
		free(e);
	}
	for (i = 0; i < li->ndevs; i++) {
		if (li->devs[i]->fd >= 0) {
			li->iface->close_restricted(li->devs[i]->fd, li->user_data);
			li->devs[i]->fd = -1;
		}
		libinput_device_unref(li->devs[i]);
	}
	close(li->wake[0]);
	close(li->wake[1]);
	pthread_mutex_destroy(&li->lock);
	free(li);
	return NULL;
}


int libinput_get_fd(struct libinput *li)
{
	return li->wake[0];
}


int libinput_dispatch(struct libinput *li)
{
	char buf[64];
	int i;
	uint64_t now;

	while (read(li->wake[0], buf, sizeof(buf)) > 0) {
	}
	now = now_usec();
	if (now - li->last_retry_usec >= (uint64_t)LIPHX_RETRY_MS * 1000u) {
		li->last_retry_usec = now;
		for (i = 0; i < li->ndevs; i++) {
			if (li->devs[i]->fd < 0) {
				try_open(li, li->devs[i]);
			}
		}
	}
	return 0;
}


struct libinput_event *libinput_get_event(struct libinput *li)
{
	struct libinput_event *e;

	pthread_mutex_lock(&li->lock);
	e = li->head;
	if (e != NULL) {
		li->head = e->next;
		if (li->head == NULL) {
			li->tail = NULL;
		}
		li->nqueued--;
		e->next = NULL;
	}
	pthread_mutex_unlock(&li->lock);
	return e;
}


void *libinput_get_user_data(struct libinput *li)
{
	return li->user_data;
}


void libinput_suspend(struct libinput *li)
{
	li->suspended = 1;
}


int libinput_resume(struct libinput *li)
{
	li->suspended = 0;
	return 0;
}


void libinput_log_set_handler(struct libinput *li, libinput_log_handler log_handler)
{
	li->log = log_handler;
}


void libinput_log_set_priority(struct libinput *li, enum libinput_log_priority priority)
{
	li->prio = priority;
}


const char *libinput_seat_get_logical_name(struct libinput_seat *seat)
{
	(void)seat;
	return "default";
}


/* ------------------------------------------------------------------------- */
/* events                                                                    */

void libinput_event_destroy(struct libinput_event *event)
{
	if (event != NULL) {
		libinput_device_unref(event->dev);
		free(event);
	}
}


enum libinput_event_type libinput_event_get_type(struct libinput_event *event)
{
	return event->type;
}


struct libinput *libinput_event_get_context(struct libinput_event *event)
{
	return event->dev->li;
}


struct libinput_device *libinput_event_get_device(struct libinput_event *event)
{
	return event->dev;
}


struct libinput_event_keyboard *libinput_event_get_keyboard_event(struct libinput_event *event)
{
	return (event->type == LIBINPUT_EVENT_KEYBOARD_KEY) ? (struct libinput_event_keyboard *)event : NULL;
}


struct libinput_event_pointer *libinput_event_get_pointer_event(struct libinput_event *event)
{
	return ((event->type >= LIBINPUT_EVENT_POINTER_MOTION) && (event->type <= LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS)) ?
		(struct libinput_event_pointer *)event :
		NULL;
}


struct libinput_event_touch *libinput_event_get_touch_event(struct libinput_event *event)
{
	(void)event;
	return NULL;
}


struct libinput_event_tablet_tool *libinput_event_get_tablet_tool_event(struct libinput_event *event)
{
	(void)event;
	return NULL;
}


uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *event)
{
	return event->base.code;
}


enum libinput_key_state libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *event)
{
	return (enum libinput_key_state)event->base.state;
}


uint32_t libinput_event_keyboard_get_seat_key_count(struct libinput_event_keyboard *event)
{
	return event->base.seat_count;
}


uint64_t libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *event)
{
	return event->base.time_usec;
}


uint64_t libinput_event_pointer_get_time_usec(struct libinput_event_pointer *event)
{
	return event->base.time_usec;
}


double libinput_event_pointer_get_dx(struct libinput_event_pointer *event)
{
	return event->base.dx;
}


double libinput_event_pointer_get_dy(struct libinput_event_pointer *event)
{
	return event->base.dy;
}


double libinput_event_pointer_get_dx_unaccelerated(struct libinput_event_pointer *event)
{
	return event->base.dx;
}


double libinput_event_pointer_get_dy_unaccelerated(struct libinput_event_pointer *event)
{
	return event->base.dy;
}


double libinput_event_pointer_get_absolute_x_transformed(struct libinput_event_pointer *event, uint32_t width)
{
	(void)event;
	(void)width;
	return 0.0; /* no absolute pointers */
}


double libinput_event_pointer_get_absolute_y_transformed(struct libinput_event_pointer *event, uint32_t height)
{
	(void)event;
	(void)height;
	return 0.0;
}


uint32_t libinput_event_pointer_get_button(struct libinput_event_pointer *event)
{
	return event->base.code;
}


enum libinput_button_state libinput_event_pointer_get_button_state(struct libinput_event_pointer *event)
{
	return (enum libinput_button_state)event->base.state;
}


uint32_t libinput_event_pointer_get_seat_button_count(struct libinput_event_pointer *event)
{
	return event->base.seat_count;
}


int libinput_event_pointer_has_axis(struct libinput_event_pointer *event, enum libinput_pointer_axis axis)
{
	return ((event->base.type == LIBINPUT_EVENT_POINTER_AXIS) || (event->base.type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)) &&
		(axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
}


double libinput_event_pointer_get_axis_value(struct libinput_event_pointer *event, enum libinput_pointer_axis axis)
{
	return (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ? event->base.axis_v : 0.0;
}


double libinput_event_pointer_get_axis_value_discrete(struct libinput_event_pointer *event,
	enum libinput_pointer_axis axis)
{
	return (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ? (double)event->base.discrete_v : 0.0;
}


enum libinput_pointer_axis_source libinput_event_pointer_get_axis_source(struct libinput_event_pointer *event)
{
	(void)event;
	return LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;
}


/* No touch devices: the accessors exist for the linker. */
uint64_t libinput_event_touch_get_time_usec(struct libinput_event_touch *event)
{
	return event->base.time_usec;
}


int32_t libinput_event_touch_get_seat_slot(struct libinput_event_touch *event)
{
	(void)event;
	return -1;
}


double libinput_event_touch_get_x_transformed(struct libinput_event_touch *event, uint32_t width)
{
	(void)event;
	(void)width;
	return 0.0;
}


double libinput_event_touch_get_y_transformed(struct libinput_event_touch *event, uint32_t height)
{
	(void)event;
	(void)height;
	return 0.0;
}


/* No tablets: never constructed. */
struct libinput_tablet_tool *libinput_event_tablet_tool_get_tool(struct libinput_event_tablet_tool *event)
{
	(void)event;
	return NULL;
}


#define LIPHX_TABLET_INT(name) \
	int name(struct libinput_event_tablet_tool *event) \
	{ \
		(void)event; \
		return 0; \
	}
LIPHX_TABLET_INT(libinput_event_tablet_tool_x_has_changed)
LIPHX_TABLET_INT(libinput_event_tablet_tool_y_has_changed)
LIPHX_TABLET_INT(libinput_event_tablet_tool_pressure_has_changed)
LIPHX_TABLET_INT(libinput_event_tablet_tool_distance_has_changed)
LIPHX_TABLET_INT(libinput_event_tablet_tool_tilt_x_has_changed)
LIPHX_TABLET_INT(libinput_event_tablet_tool_tilt_y_has_changed)

#define LIPHX_TABLET_DOUBLE(name) \
	double name(struct libinput_event_tablet_tool *event) \
	{ \
		(void)event; \
		return 0.0; \
	}
LIPHX_TABLET_DOUBLE(libinput_event_tablet_tool_get_pressure)
LIPHX_TABLET_DOUBLE(libinput_event_tablet_tool_get_distance)
LIPHX_TABLET_DOUBLE(libinput_event_tablet_tool_get_tilt_x)
LIPHX_TABLET_DOUBLE(libinput_event_tablet_tool_get_tilt_y)


double libinput_event_tablet_tool_get_x_transformed(struct libinput_event_tablet_tool *event, uint32_t width)
{
	(void)event;
	(void)width;
	return 0.0;
}


double libinput_event_tablet_tool_get_y_transformed(struct libinput_event_tablet_tool *event, uint32_t height)
{
	(void)event;
	(void)height;
	return 0.0;
}


uint32_t libinput_event_tablet_tool_get_time(struct libinput_event_tablet_tool *event)
{
	return (uint32_t)(event->base.time_usec / 1000u);
}


enum libinput_tablet_tool_proximity_state libinput_event_tablet_tool_get_proximity_state(
	struct libinput_event_tablet_tool *event)
{
	(void)event;
	return LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT;
}


enum libinput_tablet_tool_tip_state libinput_event_tablet_tool_get_tip_state(struct libinput_event_tablet_tool *event)
{
	(void)event;
	return LIBINPUT_TABLET_TOOL_TIP_UP;
}


uint32_t libinput_event_tablet_tool_get_button(struct libinput_event_tablet_tool *event)
{
	(void)event;
	return 0;
}


enum libinput_button_state libinput_event_tablet_tool_get_button_state(struct libinput_event_tablet_tool *event)
{
	(void)event;
	return LIBINPUT_BUTTON_STATE_RELEASED;
}


enum libinput_tablet_tool_type libinput_tablet_tool_get_type(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return LIBINPUT_TABLET_TOOL_TYPE_PEN;
}


uint64_t libinput_tablet_tool_get_tool_id(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return 0;
}


uint64_t libinput_tablet_tool_get_serial(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return 0;
}


#define LIPHX_TOOL_INT(name) \
	int name(struct libinput_tablet_tool *tool) \
	{ \
		(void)tool; \
		return 0; \
	}
LIPHX_TOOL_INT(libinput_tablet_tool_has_pressure)
LIPHX_TOOL_INT(libinput_tablet_tool_has_distance)
LIPHX_TOOL_INT(libinput_tablet_tool_has_tilt)
LIPHX_TOOL_INT(libinput_tablet_tool_is_unique)


void *libinput_tablet_tool_get_user_data(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return NULL;
}


void libinput_tablet_tool_set_user_data(struct libinput_tablet_tool *tool, void *user_data)
{
	(void)tool;
	(void)user_data;
}


/* ------------------------------------------------------------------------- */
/* devices                                                                   */

struct libinput_device *libinput_device_ref(struct libinput_device *device)
{
	if (device != NULL) {
		(void)__atomic_add_fetch(&device->refs, 1, __ATOMIC_RELAXED); /* the reader thread queues events too */
	}
	return device;
}


struct libinput_device *libinput_device_unref(struct libinput_device *device)
{
	if ((device != NULL) && (__atomic_sub_fetch(&device->refs, 1, __ATOMIC_ACQ_REL) == 0)) {
		free(device->path);
		free(device->sysname);
		free(device);
	}
	return NULL;
}


void libinput_device_set_user_data(struct libinput_device *device, void *user_data)
{
	device->user_data = user_data;
}


void *libinput_device_get_user_data(struct libinput_device *device)
{
	return device->user_data;
}


const char *libinput_device_get_name(struct libinput_device *device)
{
	return (device->kind == LIPHX_KEYBOARD) ? "Phoenix USB keyboard" : "Phoenix USB mouse";
}


const char *libinput_device_get_sysname(struct libinput_device *device)
{
	return device->sysname;
}


const char *libinput_device_get_output_name(struct libinput_device *device)
{
	(void)device;
	return NULL;
}


unsigned int libinput_device_get_id_product(struct libinput_device *device)
{
	(void)device;
	return 0;
}


unsigned int libinput_device_get_id_vendor(struct libinput_device *device)
{
	(void)device;
	return 0;
}


struct libinput_seat *libinput_device_get_seat(struct libinput_device *device)
{
	return &device->li->seat;
}


struct udev_device *libinput_device_get_udev_device(struct libinput_device *device)
{
	(void)device;
	return NULL;
}


int libinput_device_has_capability(struct libinput_device *device, enum libinput_device_capability capability)
{
	return ((device->kind == LIPHX_KEYBOARD) && (capability == LIBINPUT_DEVICE_CAP_KEYBOARD)) ||
		((device->kind == LIPHX_MOUSE) && (capability == LIBINPUT_DEVICE_CAP_POINTER));
}


void libinput_device_led_update(struct libinput_device *device, enum libinput_led leds)
{
	(void)device;
	(void)leds; /* usbkbd exposes no LED control */
}


/* Configuration: nothing is configurable. */
int libinput_device_config_tap_get_finger_count(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_tap_set_enabled(struct libinput_device *device,
	enum libinput_config_tap_state enable)
{
	(void)device;
	return (enable == LIBINPUT_CONFIG_TAP_DISABLED) ? LIBINPUT_CONFIG_STATUS_SUCCESS :
		LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_status libinput_device_config_tap_set_drag_enabled(struct libinput_device *device,
	enum libinput_config_drag_state enable)
{
	(void)device;
	(void)enable;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_status libinput_device_config_tap_set_drag_lock_enabled(struct libinput_device *device,
	enum libinput_config_drag_lock_state enable)
{
	(void)device;
	(void)enable;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_calibration_has_matrix(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_calibration_set_matrix(struct libinput_device *device,
	const float matrix[6])
{
	(void)device;
	(void)matrix;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


static int identity(float matrix[6])
{
	static const float id[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };

	memcpy(matrix, id, sizeof(id));
	return 0;
}


int libinput_device_config_calibration_get_matrix(struct libinput_device *device, float matrix[6])
{
	(void)device;
	return identity(matrix);
}


int libinput_device_config_calibration_get_default_matrix(struct libinput_device *device, float matrix[6])
{
	(void)device;
	return identity(matrix);
}


int libinput_device_config_accel_is_available(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_accel_set_speed(struct libinput_device *device, double speed)
{
	(void)device;
	(void)speed;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


uint32_t libinput_device_config_accel_get_profiles(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_accel_set_profile(struct libinput_device *device,
	enum libinput_config_accel_profile profile)
{
	(void)device;
	(void)profile;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_scroll_has_natural_scroll(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_scroll_set_natural_scroll_enabled(struct libinput_device *device,
	int enable)
{
	(void)device;
	(void)enable;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_left_handed_is_available(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_left_handed_set(struct libinput_device *device, int left_handed)
{
	(void)device;
	(void)left_handed;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


uint32_t libinput_device_config_scroll_get_methods(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_scroll_set_method(struct libinput_device *device,
	enum libinput_config_scroll_method method)
{
	(void)device;
	return (method == LIBINPUT_CONFIG_SCROLL_NO_SCROLL) ? LIBINPUT_CONFIG_STATUS_SUCCESS :
		LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_status libinput_device_config_scroll_set_button(struct libinput_device *device, uint32_t button)
{
	(void)device;
	(void)button;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_middle_emulation_is_available(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_middle_emulation_set_enabled(struct libinput_device *device,
	enum libinput_config_middle_emulation_state enable)
{
	(void)device;
	(void)enable;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_dwt_is_available(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_dwt_set_enabled(struct libinput_device *device,
	enum libinput_config_dwt_state enable)
{
	(void)device;
	(void)enable;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


int libinput_device_config_rotation_is_available(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_status libinput_device_config_rotation_set_angle(struct libinput_device *device,
	unsigned int degrees_cw)
{
	(void)device;
	(void)degrees_cw;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


/*
 * wlroots (labwc-drm, M7): the scroll-wheel API of libinput >= 1.19, the device
 * identity, and accessors for event kinds this library never produces (gestures,
 * switches, tablet pads, the newer tablet tool axes) -- they exist for the linker.
 */
double libinput_event_pointer_get_scroll_value(struct libinput_event_pointer *event, enum libinput_pointer_axis axis)
{
	return (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ? event->base.axis_v : 0.0;
}


double libinput_event_pointer_get_scroll_value_v120(struct libinput_event_pointer *event, enum libinput_pointer_axis axis)
{
	return (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) ? 120.0 * (double)event->base.discrete_v : 0.0;
}


unsigned int libinput_device_get_id_bustype(struct libinput_device *device)
{
	(void)device;
	return 0x03u; /* BUS_USB: usbkbd, usbmouse */
}


int libinput_device_get_size(struct libinput_device *device, double *width, double *height)
{
	(void)device;
	(void)width;
	(void)height;
	return -1; /* no absolute devices */
}


struct libinput_event_gesture *libinput_event_get_gesture_event(struct libinput_event *event)
{
	(void)event;
	return NULL;
}


struct libinput_event_switch *libinput_event_get_switch_event(struct libinput_event *event)
{
	(void)event;
	return NULL;
}


struct libinput_event_tablet_pad *libinput_event_get_tablet_pad_event(struct libinput_event *event)
{
	(void)event;
	return NULL;
}


#define LIPHX_NEVER(type, name, argtype) \
	type name(argtype *arg) \
	{ \
		(void)arg; \
		return (type)0; \
	}
LIPHX_NEVER(uint64_t, libinput_event_gesture_get_time_usec, struct libinput_event_gesture)
LIPHX_NEVER(int, libinput_event_gesture_get_finger_count, struct libinput_event_gesture)
LIPHX_NEVER(int, libinput_event_gesture_get_cancelled, struct libinput_event_gesture)
LIPHX_NEVER(double, libinput_event_gesture_get_dx, struct libinput_event_gesture)
LIPHX_NEVER(double, libinput_event_gesture_get_dy, struct libinput_event_gesture)
LIPHX_NEVER(double, libinput_event_gesture_get_scale, struct libinput_event_gesture)
LIPHX_NEVER(double, libinput_event_gesture_get_angle_delta, struct libinput_event_gesture)
LIPHX_NEVER(uint64_t, libinput_event_switch_get_time_usec, struct libinput_event_switch)
LIPHX_NEVER(enum libinput_switch, libinput_event_switch_get_switch, struct libinput_event_switch)
LIPHX_NEVER(enum libinput_switch_state, libinput_event_switch_get_switch_state, struct libinput_event_switch)
LIPHX_NEVER(uint64_t, libinput_event_tablet_pad_get_time_usec, struct libinput_event_tablet_pad)
LIPHX_NEVER(uint32_t, libinput_event_tablet_pad_get_button_number, struct libinput_event_tablet_pad)
LIPHX_NEVER(enum libinput_button_state, libinput_event_tablet_pad_get_button_state, struct libinput_event_tablet_pad)
LIPHX_NEVER(unsigned int, libinput_event_tablet_pad_get_mode, struct libinput_event_tablet_pad)
LIPHX_NEVER(struct libinput_tablet_pad_mode_group *, libinput_event_tablet_pad_get_mode_group, struct libinput_event_tablet_pad)
LIPHX_NEVER(unsigned int, libinput_event_tablet_pad_get_ring_number, struct libinput_event_tablet_pad)
LIPHX_NEVER(double, libinput_event_tablet_pad_get_ring_position, struct libinput_event_tablet_pad)
LIPHX_NEVER(enum libinput_tablet_pad_ring_axis_source, libinput_event_tablet_pad_get_ring_source, struct libinput_event_tablet_pad)
LIPHX_NEVER(unsigned int, libinput_event_tablet_pad_get_strip_number, struct libinput_event_tablet_pad)
LIPHX_NEVER(double, libinput_event_tablet_pad_get_strip_position, struct libinput_event_tablet_pad)
LIPHX_NEVER(enum libinput_tablet_pad_strip_axis_source, libinput_event_tablet_pad_get_strip_source, struct libinput_event_tablet_pad)
LIPHX_NEVER(uint64_t, libinput_event_tablet_tool_get_time_usec, struct libinput_event_tablet_tool)
LIPHX_NEVER(double, libinput_event_tablet_tool_get_dx, struct libinput_event_tablet_tool)
LIPHX_NEVER(double, libinput_event_tablet_tool_get_dy, struct libinput_event_tablet_tool)
LIPHX_NEVER(double, libinput_event_tablet_tool_get_rotation, struct libinput_event_tablet_tool)
LIPHX_NEVER(double, libinput_event_tablet_tool_get_slider_position, struct libinput_event_tablet_tool)
LIPHX_NEVER(double, libinput_event_tablet_tool_get_wheel_delta, struct libinput_event_tablet_tool)
LIPHX_NEVER(int, libinput_event_tablet_tool_rotation_has_changed, struct libinput_event_tablet_tool)
LIPHX_NEVER(int, libinput_event_tablet_tool_slider_has_changed, struct libinput_event_tablet_tool)
LIPHX_NEVER(int, libinput_event_tablet_tool_wheel_has_changed, struct libinput_event_tablet_tool)
LIPHX_NEVER(int, libinput_tablet_tool_has_rotation, struct libinput_tablet_tool)
LIPHX_NEVER(int, libinput_tablet_tool_has_slider, struct libinput_tablet_tool)
LIPHX_NEVER(int, libinput_tablet_tool_has_wheel, struct libinput_tablet_tool)
LIPHX_NEVER(unsigned int, libinput_tablet_pad_mode_group_get_index, struct libinput_tablet_pad_mode_group)
LIPHX_NEVER(unsigned int, libinput_tablet_pad_mode_group_get_num_modes, struct libinput_tablet_pad_mode_group)
LIPHX_NEVER(int, libinput_device_tablet_pad_get_num_buttons, struct libinput_device)
LIPHX_NEVER(int, libinput_device_tablet_pad_get_num_rings, struct libinput_device)
LIPHX_NEVER(int, libinput_device_tablet_pad_get_num_strips, struct libinput_device)
LIPHX_NEVER(int, libinput_device_tablet_pad_get_num_mode_groups, struct libinput_device)


struct libinput_tablet_tool *libinput_tablet_tool_ref(struct libinput_tablet_tool *tool)
{
	return tool;
}


struct libinput_tablet_tool *libinput_tablet_tool_unref(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return NULL;
}


struct libinput_tablet_pad_mode_group *libinput_device_tablet_pad_get_mode_group(struct libinput_device *device,
	unsigned int index)
{
	(void)device;
	(void)index;
	return NULL;
}


struct libinput_tablet_pad_mode_group *libinput_tablet_pad_mode_group_ref(struct libinput_tablet_pad_mode_group *group)
{
	return group;
}


struct libinput_tablet_pad_mode_group *libinput_tablet_pad_mode_group_unref(struct libinput_tablet_pad_mode_group *group)
{
	(void)group;
	return NULL;
}


#define LIPHX_GROUP_HAS(name) \
	int name(struct libinput_tablet_pad_mode_group *group, unsigned int n) \
	{ \
		(void)group; \
		(void)n; \
		return 0; \
	}
LIPHX_GROUP_HAS(libinput_tablet_pad_mode_group_has_button)
LIPHX_GROUP_HAS(libinput_tablet_pad_mode_group_has_ring)
LIPHX_GROUP_HAS(libinput_tablet_pad_mode_group_has_strip)


/*
 * labwc (M7) reads each device's configuration defaults and applies its
 * rc.xml <libinput> settings. usbkbd/usbmouse have none of these features:
 * defaults are "off", setters other than the default are unsupported.
 */
enum libinput_config_accel_profile libinput_device_config_accel_get_default_profile(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_ACCEL_PROFILE_NONE;
}


double libinput_device_config_accel_get_default_speed(struct libinput_device *device)
{
	(void)device;
	return 0.0;
}


uint32_t libinput_device_config_click_get_methods(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}


enum libinput_config_click_method libinput_device_config_click_get_default_method(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
}


enum libinput_config_status libinput_device_config_click_set_method(struct libinput_device *device,
	enum libinput_config_click_method method)
{
	(void)device;
	return (method == LIBINPUT_CONFIG_CLICK_METHOD_NONE) ? LIBINPUT_CONFIG_STATUS_SUCCESS : LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_dwt_state libinput_device_config_dwt_get_default_enabled(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_DWT_DISABLED;
}


int libinput_device_config_left_handed_get_default(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_middle_emulation_state libinput_device_config_middle_emulation_get_default_enabled(
	struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
}


uint32_t libinput_device_config_scroll_get_default_button(struct libinput_device *device)
{
	(void)device;
	return 0;
}


enum libinput_config_scroll_method libinput_device_config_scroll_get_default_method(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
}


int libinput_device_config_scroll_get_natural_scroll_enabled(struct libinput_device *device)
{
	(void)device;
	return 0;
}


int libinput_device_config_scroll_get_default_natural_scroll_enabled(struct libinput_device *device)
{
	(void)device;
	return 0;
}


uint32_t libinput_device_config_send_events_get_modes(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED; /* 0: no other mode */
}


uint32_t libinput_device_config_send_events_get_default_mode(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}


enum libinput_config_status libinput_device_config_send_events_set_mode(struct libinput_device *device, uint32_t mode)
{
	(void)device;
	return (mode == LIBINPUT_CONFIG_SEND_EVENTS_ENABLED) ? LIBINPUT_CONFIG_STATUS_SUCCESS : LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_tap_state libinput_device_config_tap_get_default_enabled(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_TAP_DISABLED;
}


enum libinput_config_tap_button_map libinput_device_config_tap_get_default_button_map(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_TAP_MAP_LRM;
}


enum libinput_config_status libinput_device_config_tap_set_button_map(struct libinput_device *device,
	enum libinput_config_tap_button_map map)
{
	(void)device;
	(void)map;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}


enum libinput_config_drag_state libinput_device_config_tap_get_default_drag_enabled(struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_DRAG_DISABLED;
}


enum libinput_config_drag_lock_state libinput_device_config_tap_get_default_drag_lock_enabled(
	struct libinput_device *device)
{
	(void)device;
	return LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
}


struct libinput_device_group *libinput_device_get_device_group(struct libinput_device *device)
{
	(void)device;
	return NULL; /* only tablets are grouped (labwc: pad <-> tablet) */
}


int libinput_tablet_tool_config_pressure_range_is_available(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return 0;
}


double libinput_tablet_tool_config_pressure_range_get_minimum(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return 0.0;
}


double libinput_tablet_tool_config_pressure_range_get_maximum(struct libinput_tablet_tool *tool)
{
	(void)tool;
	return 1.0;
}


enum libinput_config_status libinput_tablet_tool_config_pressure_range_set(struct libinput_tablet_tool *tool,
	double minimum, double maximum)
{
	(void)tool;
	(void)minimum;
	(void)maximum;
	return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}
