/* Copyright 2026 Phoenix Systems
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * weston-drm compat: memfd_create() over shmsrv (see compat/include/sys/mman.h
 * and shmsrv/shm_proto.h). One devctl to the server for a fresh id, then
 * open("/shm/<id>", O_RDWR). ftruncate() and mmap() of the descriptor need
 * nothing here: they reach the server (mtTruncate) and the kernel (the export
 * window) directly.
 *
 * Without a running shmsrv this fails with ENOSYS, and weston/libwayland fall
 * back to their own mkostemp() file in $XDG_RUNTIME_DIR -- which works only while
 * a descriptor of the file stays open in some process (see shmsrv.c), so the
 * weston-drm Pi scripts always start shmsrv.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/msg.h>

#include "../../shmsrv/shm_proto.h"

/* One devctl to shmsrv: a fresh object id (never reused). Also the backing of the
 * labwc-drm compat's shm_open() (tools/gpu-lane/labwc-drm/compat). */
int wlphx_shm_create(unsigned int *id)
{
	oid_t oid;
	msg_t msg;
	shmsrv_create_req_t req = { .op = SHMSRV_OP_CREATE, .proto = SHMSRV_PROTO };
	shmsrv_create_rsp_t rsp;

	if (lookup(SHMSRV_NS, NULL, &oid) < 0) {
		errno = ENOSYS;
		return -1;
	}
	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	msg.oid.port = oid.port;
	msg.oid.id = 0;
	memcpy(msg.i.raw, &req, sizeof(req));
	if (msgSend(oid.port, &msg) < 0) {
		errno = ENOSYS;
		return -1;
	}
	memcpy(&rsp, msg.o.raw, sizeof(rsp));
	if (rsp.err < 0) {
		errno = -rsp.err;
		return -1;
	}
	*id = (unsigned int)rsp.id;
	return 0;
}


int memfd_create(const char *name, unsigned int flags)
{
	char path[32];
	unsigned int id;

	(void)name;
	if ((flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL)) != 0u) {
		errno = EINVAL; /* MFD_HUGETLB, MFD_EXEC: not supported */
		return -1;
	}
	if (wlphx_shm_create(&id) < 0) {
		return -1;
	}
	(void)snprintf(path, sizeof(path), SHMSRV_NS "/%u", id);
	return open(path, O_RDWR | (((flags & MFD_CLOEXEC) != 0u) ? O_CLOEXEC : 0));
}
