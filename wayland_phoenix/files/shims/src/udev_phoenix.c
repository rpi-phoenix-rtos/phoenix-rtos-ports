/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: libudev over a fixed device table (see shims/include/libudev.h).
 */

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>

#include <libudev.h>

struct udev_known {
	const char *syspath;
	const char *sysname;
	const char *sysnum;
	const char *subsystem;
	const char *devtype;
	const char *devnode;
};

/* The Pi 4's display device (rpi4-kms). The render node (rpi4-v3d-async) is
 * not listed: Weston needs only the KMS device, and Mesa finds the render node
 * through libdrm-phoenix's own device list (M3 section 2.9). */
static const struct udev_known known[] = {
	{ "/sys/devices/platform/gpu/drm/card0", "card0", "0", "drm", "drm_minor", "/dev/dri/card0" },
};
#define NKNOWN (sizeof(known) / sizeof(known[0]))

struct udev {
	int refs;
};

struct udev_device {
	int refs;
	struct udev *udev;
	const struct udev_known *k;
};

struct udev_list_entry {
	struct udev_list_entry *next;
	const char *name;
};

#define MAX_MATCH 4

struct udev_enumerate {
	int refs;
	struct udev *udev;
	const char *subsystem[MAX_MATCH];
	char *sysname[MAX_MATCH];
	int nsub, nname;
	struct udev_list_entry entries[NKNOWN];
	struct udev_list_entry *first;
};

struct udev_monitor {
	int refs;
	struct udev *udev;
	int fd[2]; /* a socket that never turns readable: there is no hotplug */
};


struct udev *udev_new(void)
{
	struct udev *u = calloc(1, sizeof(*u));

	if (u != NULL) {
		u->refs = 1;
	}
	return u;
}


struct udev *udev_ref(struct udev *udev)
{
	if (udev != NULL) {
		udev->refs++;
	}
	return udev;
}


struct udev *udev_unref(struct udev *udev)
{
	if ((udev != NULL) && (--udev->refs == 0)) {
		free(udev);
	}
	return NULL;
}


struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry)
{
	return (entry != NULL) ? entry->next : NULL;
}


const char *udev_list_entry_get_name(struct udev_list_entry *entry)
{
	return (entry != NULL) ? entry->name : NULL;
}


const char *udev_list_entry_get_value(struct udev_list_entry *entry)
{
	(void)entry;
	return NULL;
}


static struct udev_device *device_new(struct udev *udev, const struct udev_known *k)
{
	struct udev_device *d = calloc(1, sizeof(*d));

	if (d == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	d->refs = 1;
	d->udev = udev_ref(udev);
	d->k = k;
	return d;
}


struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath)
{
	size_t i;

	for (i = 0; (syspath != NULL) && (i < NKNOWN); i++) {
		if (strcmp(known[i].syspath, syspath) == 0) {
			return device_new(udev, &known[i]);
		}
	}
	errno = ENODEV;
	return NULL;
}


struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
	const char *sysname)
{
	size_t i;

	for (i = 0; (subsystem != NULL) && (sysname != NULL) && (i < NKNOWN); i++) {
		if ((strcmp(known[i].subsystem, subsystem) == 0) && (strcmp(known[i].sysname, sysname) == 0)) {
			return device_new(udev, &known[i]);
		}
	}
	errno = ENODEV;
	return NULL;
}


struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum)
{
	size_t i;
	struct stat st;

	for (i = 0; (type == 'c') && (i < NKNOWN); i++) {
		if ((stat(known[i].devnode, &st) == 0) && (st.st_rdev == devnum)) {
			return device_new(udev, &known[i]);
		}
	}
	errno = ENODEV;
	return NULL;
}


struct udev_device *udev_device_ref(struct udev_device *dev)
{
	if (dev != NULL) {
		dev->refs++;
	}
	return dev;
}


struct udev_device *udev_device_unref(struct udev_device *dev)
{
	if ((dev != NULL) && (--dev->refs == 0)) {
		udev_unref(dev->udev);
		free(dev);
	}
	return NULL;
}


struct udev *udev_device_get_udev(struct udev_device *dev)
{
	return (dev != NULL) ? dev->udev : NULL;
}


const char *udev_device_get_syspath(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->syspath : NULL;
}


const char *udev_device_get_sysname(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->sysname : NULL;
}


const char *udev_device_get_sysnum(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->sysnum : NULL;
}


const char *udev_device_get_devnode(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->devnode : NULL;
}


const char *udev_device_get_devpath(struct udev_device *dev)
{
	return (dev != NULL) ? (dev->k->syspath + strlen("/sys")) : NULL;
}


const char *udev_device_get_subsystem(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->subsystem : NULL;
}


const char *udev_device_get_devtype(struct udev_device *dev)
{
	return (dev != NULL) ? dev->k->devtype : NULL;
}


const char *udev_device_get_action(struct udev_device *dev)
{
	(void)dev;
	return NULL;
}


dev_t udev_device_get_devnum(struct udev_device *dev)
{
	struct stat st;

	/* On Phoenix st_rdev of a device node is its server's port (M3 section 2.9). */
	if ((dev == NULL) || (stat(dev->k->devnode, &st) != 0)) {
		return 0;
	}
	return st.st_rdev;
}


const char *udev_device_get_property_value(struct udev_device *dev, const char *key)
{
	(void)dev;
	(void)key;
	return NULL; /* no ID_SEAT (= "seat0"), no HOTPLUG, no WL_OUTPUT */
}


const char *udev_device_get_sysattr_value(struct udev_device *dev, const char *sysattr)
{
	(void)dev;
	(void)sysattr;
	return NULL; /* no sysfs */
}


int udev_device_set_sysattr_value(struct udev_device *dev, const char *sysattr, const char *value)
{
	(void)dev;
	(void)sysattr;
	(void)value;
	return -ENOENT;
}


struct udev_device *udev_device_get_parent(struct udev_device *dev)
{
	(void)dev;
	return NULL;
}


struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *dev,
	const char *subsystem, const char *devtype)
{
	(void)dev;
	(void)subsystem;
	(void)devtype;
	return NULL; /* platform device: no PCI parent, so no boot_vga */
}


struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
	struct udev_enumerate *e = calloc(1, sizeof(*e));

	if (e != NULL) {
		e->refs = 1;
		e->udev = udev_ref(udev);
	}
	return e;
}


struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e)
{
	if (e != NULL) {
		e->refs++;
	}
	return e;
}


struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e)
{
	int i;

	if ((e != NULL) && (--e->refs == 0)) {
		for (i = 0; i < e->nname; i++) {
			free(e->sysname[i]);
		}
		udev_unref(e->udev);
		free(e);
	}
	return NULL;
}


int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem)
{
	if ((e == NULL) || (subsystem == NULL) || (e->nsub == MAX_MATCH)) {
		return -EINVAL;
	}
	e->subsystem[e->nsub++] = subsystem; /* callers pass string literals */
	return 0;
}


int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname)
{
	if ((e == NULL) || (sysname == NULL) || (e->nname == MAX_MATCH)) {
		return -EINVAL;
	}
	e->sysname[e->nname] = strdup(sysname);
	if (e->sysname[e->nname] == NULL) {
		return -ENOMEM;
	}
	e->nname++;
	return 0;
}


int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
	struct udev_list_entry **tail;
	size_t i;
	int j, ok;

	if (e == NULL) {
		return -EINVAL;
	}
	tail = &e->first;
	*tail = NULL;
	for (i = 0; i < NKNOWN; i++) {
		ok = (e->nsub == 0);
		for (j = 0; j < e->nsub; j++) {
			ok |= (strcmp(known[i].subsystem, e->subsystem[j]) == 0);
		}
		if (ok && (e->nname > 0)) {
			ok = 0;
			for (j = 0; j < e->nname; j++) {
				ok |= (fnmatch(e->sysname[j], known[i].sysname, 0) == 0);
			}
		}
		if (ok) {
			e->entries[i].name = known[i].syspath;
			e->entries[i].next = NULL;
			*tail = &e->entries[i];
			tail = &e->entries[i].next;
		}
	}
	return 0;
}


struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
	return (e != NULL) ? e->first : NULL;
}


struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name)
{
	struct udev_monitor *m;

	(void)name;
	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		return NULL;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, m->fd) < 0) {
		free(m);
		return NULL;
	}
	(void)fcntl(m->fd[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(m->fd[1], F_SETFD, FD_CLOEXEC);
	m->refs = 1;
	m->udev = udev_ref(udev);
	return m;
}


struct udev_monitor *udev_monitor_ref(struct udev_monitor *m)
{
	if (m != NULL) {
		m->refs++;
	}
	return m;
}


struct udev_monitor *udev_monitor_unref(struct udev_monitor *m)
{
	if ((m != NULL) && (--m->refs == 0)) {
		close(m->fd[0]);
		close(m->fd[1]);
		udev_unref(m->udev);
		free(m);
	}
	return NULL;
}


int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
	const char *devtype)
{
	(void)m;
	(void)subsystem;
	(void)devtype;
	return 0;
}


int udev_monitor_enable_receiving(struct udev_monitor *m)
{
	return (m != NULL) ? 0 : -EINVAL;
}


int udev_monitor_get_fd(struct udev_monitor *m)
{
	return (m != NULL) ? m->fd[0] : -1;
}


struct udev_device *udev_monitor_receive_device(struct udev_monitor *m)
{
	(void)m;
	errno = EAGAIN;
	return NULL;
}
