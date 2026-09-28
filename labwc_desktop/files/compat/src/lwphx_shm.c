/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * labwc-drm compat: shm_open()/shm_unlink() over shmsrv.
 *
 * libphoenix has no POSIX shared memory. wlroots (util/shm.c) is the user: it
 * creates a uniquely named object (O_CREAT|O_EXCL), optionally opens it a second
 * time read-only (the keymap's read-only descriptor for clients), and unlinks the
 * name at once. A name therefore only has to be visible inside the creating
 * process: a small table maps names to shmsrv object ids, and every open is
 * open("/shm/<id>"), the same object the server keeps alive while any
 * descriptor of it is open. After shm_unlink() the object lives on in its
 * descriptors and mappings, as with POSIX.
 *
 * Limits: names are per process (another process cannot shm_open() them), at most
 * LWPHX_SHM_NAMES live names, the mode is ignored (shmsrv objects are 0600).
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define LWPHX_SHM_NAMES   32
#define LWPHX_SHM_NAMELEN 64
#ifndef LWPHX_SHM_NS
#define LWPHX_SHM_NS      "/shm" /* shmsrv (weston-drm/shmsrv/shm_proto.h); host tests override it */
#endif

static struct {
	char name[LWPHX_SHM_NAMELEN];
	unsigned int id;
	int used;
} shm_names[LWPHX_SHM_NAMES];

static pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;


static int shm_name_ok(const char *name)
{
	size_t len;

	if ((name == NULL) || (name[0] != '/') || (strchr(name + 1, '/') != NULL)) {
		errno = EINVAL;
		return 0;
	}
	len = strlen(name);
	if ((len < 2u) || (len >= LWPHX_SHM_NAMELEN)) {
		errno = (len < 2u) ? EINVAL : ENAMETOOLONG;
		return 0;
	}
	return 1;
}


static int shm_find(const char *name)
{
	int i;

	for (i = 0; i < LWPHX_SHM_NAMES; i++) {
		if ((shm_names[i].used != 0) && (strcmp(shm_names[i].name, name) == 0)) {
			return i;
		}
	}
	return -1;
}


static int shm_open_id(unsigned int id, int oflag)
{
	char path[96];
	int acc = oflag & O_ACCMODE;

	if ((acc != O_RDONLY) && (acc != O_RDWR)) {
		errno = EINVAL;
		return -1;
	}
	(void)snprintf(path, sizeof(path), LWPHX_SHM_NS "/%u", id);
	/* POSIX: shm_open() descriptors are FD_CLOEXEC */
	return open(path, acc | O_CLOEXEC);
}


int shm_open(const char *name, int oflag, mode_t mode)
{
	unsigned int id;
	int i, slot, fd;

	(void)mode;
	if (!shm_name_ok(name)) {
		return -1;
	}
	pthread_mutex_lock(&shm_lock);
	i = shm_find(name);
	if (i >= 0) {
		if (((oflag & O_CREAT) != 0) && ((oflag & O_EXCL) != 0)) {
			pthread_mutex_unlock(&shm_lock);
			errno = EEXIST;
			return -1;
		}
		id = shm_names[i].id;
		pthread_mutex_unlock(&shm_lock);
		/* O_TRUNC of an existing object is not supported (wlroots never asks) */
		return shm_open_id(id, oflag);
	}
	if ((oflag & O_CREAT) == 0) {
		pthread_mutex_unlock(&shm_lock);
		errno = ENOENT;
		return -1;
	}
	for (slot = 0; (slot < LWPHX_SHM_NAMES) && (shm_names[slot].used != 0); slot++) {
	}
	if (slot == LWPHX_SHM_NAMES) {
		pthread_mutex_unlock(&shm_lock);
		errno = ENFILE;
		return -1;
	}
	if (wlphx_shm_create(&id) < 0) {
		pthread_mutex_unlock(&shm_lock);
		return -1; /* ENOSYS: no shmsrv */
	}
	fd = shm_open_id(id, oflag);
	if (fd >= 0) {
		strcpy(shm_names[slot].name, name);
		shm_names[slot].id = id;
		shm_names[slot].used = 1;
	}
	pthread_mutex_unlock(&shm_lock);
	return fd;
}


int shm_unlink(const char *name)
{
	int i;

	if (!shm_name_ok(name)) {
		return -1;
	}
	pthread_mutex_lock(&shm_lock);
	i = shm_find(name);
	if (i >= 0) {
		shm_names[i].used = 0;
	}
	pthread_mutex_unlock(&shm_lock);
	if (i < 0) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}
