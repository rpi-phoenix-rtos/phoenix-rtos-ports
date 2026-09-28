/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - the seam between upstream xf86drm.c and the Phoenix backend
 *
 * Upstream libdrm reaches the kernel through ioctl() on /dev/dri character
 * devices and identifies devices by major/minor and sysfs. On Phoenix the DRM
 * nodes are served by two userspace servers (rpi4-kms, rpi4-v3d-async) that
 * speak their own message protocols, and there is no sysfs. The patch set adds
 * one `#ifdef __phoenix__` call per affected public function of xf86drm.c; each
 * lands here. Nothing else of upstream libdrm changes behaviour.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _XF86DRM_PHOENIX_H_
#define _XF86DRM_PHOENIX_H_

#include <stdint.h>
#include <sys/types.h>

#include "xf86drm.h"


/* drmIoctl(): every DRM request, marshalled to the node's server.
 * Returns 0, or -1 with errno (the ioctl() convention drmIoctl keeps). */
int drm_phoenix_ioctl(int fd, unsigned long request, void *arg);

/* Device identity: a static list of two platform devices (vc4 = rpi4-kms,
 * v3d = rpi4-v3d-async), node names resolved at call time. */
int drm_phoenix_get_devices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
int drm_phoenix_get_device2(int fd, uint32_t flags, drmDevicePtr *device);
int drm_phoenix_get_device_from_devid(dev_t find_rdev, uint32_t flags, drmDevicePtr *device);
int drm_phoenix_node_type_from_devid(dev_t devid);
int drm_phoenix_node_type_from_fd(int fd);
char *drm_phoenix_device_name_from_fd(int fd);
char *drm_phoenix_minor_name_for_fd(int fd, int type);
int drm_phoenix_open_minor(int minor, int type);

#endif /* _XF86DRM_PHOENIX_H_ */
