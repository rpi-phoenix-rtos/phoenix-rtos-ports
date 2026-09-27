/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm shim: the subset of the libudev API that Weston's DRM backend and
 * its libbacklight use, over a FIXED device table (shims/src/udev_phoenix.c).
 * Phoenix-RTOS has no udev, no sysfs and no hotplug: the display device is
 * rpi4-kms's /dev/dri/card0, known in advance. Declarations written for this shim
 * (API-compatible with libudev; not systemd's header).
 *
 * Devices: one DRM primary node, subsystem "drm", sysname "card0", devnode
 * /dev/dri/card0, devnum = st_rdev of that node. No parents, no sysfs attributes,
 * no properties (ID_SEAT absent = "seat0"). A monitor never reports an event.
 */
#ifndef WLPHX_LIBUDEV_H
#define WLPHX_LIBUDEV_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udev;
struct udev_device;
struct udev_enumerate;
struct udev_list_entry;
struct udev_monitor;

struct udev *udev_new(void);
struct udev *udev_ref(struct udev *udev);
struct udev *udev_unref(struct udev *udev);

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry);
const char *udev_list_entry_get_name(struct udev_list_entry *entry);
const char *udev_list_entry_get_value(struct udev_list_entry *entry);
#define udev_list_entry_foreach(entry, first) \
	for ((entry) = (first); (entry) != NULL; (entry) = udev_list_entry_get_next(entry))

struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath);
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
	const char *sysname);
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum);
struct udev_device *udev_device_ref(struct udev_device *dev);
struct udev_device *udev_device_unref(struct udev_device *dev);
struct udev *udev_device_get_udev(struct udev_device *dev);
const char *udev_device_get_syspath(struct udev_device *dev);
const char *udev_device_get_sysname(struct udev_device *dev);
const char *udev_device_get_sysnum(struct udev_device *dev);
const char *udev_device_get_devnode(struct udev_device *dev);
const char *udev_device_get_devpath(struct udev_device *dev);
const char *udev_device_get_subsystem(struct udev_device *dev);
const char *udev_device_get_devtype(struct udev_device *dev);
const char *udev_device_get_action(struct udev_device *dev);
dev_t udev_device_get_devnum(struct udev_device *dev);
const char *udev_device_get_property_value(struct udev_device *dev, const char *key);
const char *udev_device_get_sysattr_value(struct udev_device *dev, const char *sysattr);
int udev_device_set_sysattr_value(struct udev_device *dev, const char *sysattr, const char *value);
struct udev_device *udev_device_get_parent(struct udev_device *dev);
struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *dev,
	const char *subsystem, const char *devtype);

struct udev_enumerate *udev_enumerate_new(struct udev *udev);
struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e);
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem);
int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname);
int udev_enumerate_scan_devices(struct udev_enumerate *e);
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e);

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name);
struct udev_monitor *udev_monitor_ref(struct udev_monitor *m);
struct udev_monitor *udev_monitor_unref(struct udev_monitor *m);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
	const char *devtype);
int udev_monitor_enable_receiving(struct udev_monitor *m);
int udev_monitor_get_fd(struct udev_monitor *m);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *m);

#ifdef __cplusplus
}
#endif

#endif
