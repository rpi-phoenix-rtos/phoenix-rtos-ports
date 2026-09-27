/*
 * Phoenix-RTOS
 *
 * libdrm-phoenix - PROPOSED additions to the rpi4-v3d-async and rpi4-kms wire
 * protocols (M3 server work list)
 *
 * Nothing here changes an existing opcode or struct of v3da_proto.h or
 * kms_proto.h. Each item is either a request layout for an opcode the protocol
 * already reserves (the server answers -ENOSYS today), or a NEW opcode number
 * appended after the last one of its group. libdrm-phoenix speaks each item only
 * when the server's protocol version (HELLO) says it exists; until then the DRM
 * call fails with ENOSYS locally. When a server implements an item, move its
 * definitions into that server's *_proto.h, bump the protocol version there,
 * and delete them here.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _DRM_PHOENIX_EXT_H_
#define _DRM_PHOENIX_EXT_H_

#include <stdint.h>


/* ------------------------------------------------------------------------- */
/* rpi4-v3d-async                                                             */
/* ------------------------------------------------------------------------- */

/*
 * V3DA_OP_BO_IMPORT (21): IMPLEMENTED in M3 part 2 - the request layout
 * (v3da_bo_import_req_t) and its semantics moved to v3da_proto.h, additively
 * inside proto 2 (a part-2 server answers the opcode -ENOSYS, so no version bump
 * was needed and libdrm-phoenix sends it unconditionally). Only the namespace
 * names stay here; their values are V3DA_IMPORT_NS_*.
 */
#ifndef V3DA_HAVE_BO_IMPORT
#error "v3da_proto.h predates BO_IMPORT (M3 part 2): build against the current tools/gpu-lane/v3d-async"
#endif

enum drmphx_ns {
	DRMPHX_NS_KMSBUF = V3DA_IMPORT_NS_KMSBUF,   /* "/kmsbuf/<id>"  (rpi4-kms dumb buffers) */
	DRMPHX_NS_V3DBUF = V3DA_IMPORT_NS_V3DBUF    /* "/v3dbuf/<id>"  (rpi4-v3d-async BOs, V3DA_OP_BO_EXPORT, proto 3) */
};

/*
 * V3DA_OP_BO_EXPORT (22): IMPLEMENTED for gap G4 (M6) - the opcode, V3DA_BUF_NS
 * ("/v3dbuf") and the semantics moved to v3da_proto.h, which bumped the protocol
 * to 3 (V3DA_PROTO_BO_EXPORT). libdrm-phoenix HELLOs with 3, falls back to 2 on a
 * proto-2 server and answers PRIME_HANDLE_TO_FD of a non-imported BO with ENOSYS
 * there, as before.
 */
#ifndef V3DA_HAVE_BO_EXPORT
#error "v3da_proto.h predates BO_EXPORT (G4): build against the current tools/gpu-lane/v3d-async"
#endif

/*
 * V3DA_OP_BO_LAST_FENCE (23) and V3DA_OP_BO_ATTACH_FENCE (24): IMPLEMENTED for gap
 * G6 (cross-process implicit sync) - the opcodes, v3da_bo_sync_req_t /
 * v3da_bo_fences_resp_t and the semantics moved to v3da_proto.h, which bumped the
 * protocol to 4 (V3DA_PROTO_BO_SYNC). They serve DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE
 * and the flip gate of a buffer another process renders (the cross-process
 * producers the old V3DA_OP_BO_LAST_FENCE_EXT note here asked for). libdrm-phoenix
 * uses them only when the server's HELLO says 4 or more; against an older server
 * the dma-buf ioctls answer ENOTTY and foreign flips go ungated, as before.
 */
#ifndef V3DA_HAVE_BO_SYNC
#error "v3da_proto.h predates BO_LAST_FENCE (G6): build against the current tools/gpu-lane/v3d-async"
#endif

/*
 * V3DA_BO_LOWMEM (BO_CREATE flag, proto 5, V3DA_PROTO_BO_LOWMEM): IMPLEMENTED - a
 * BO the firmware plane may scan out is placed below 1 GiB (v3da_proto.h). On the
 * DRM side it is a Phoenix-only DRM_IOCTL_V3D_CREATE_BO flag (Linux v3d defines no
 * create flags and requires 0): Mesa v3d sets it for PIPE_BIND_SCANOUT resources on
 * Phoenix (mesa-drm patch 0016, which carries the same value). libdrm-phoenix sends
 * V3DA_BO_LOWMEM for it only when the server's HELLO says 5 or more, and drops it
 * silently otherwise (the pre-proto-5 placement: anywhere). Any other flag bit is
 * EINVAL, as on Linux. Never link Mesa objects built with patch 0016 against a
 * libdrm.a from before this flag: that library answers the flag EINVAL (Mesa then
 * retries without it, so the program works, but its scan-out BOs land anywhere).
 */
#define DRM_PHOENIX_V3D_CREATE_BO_SCANOUT (1u << 31)

#ifndef V3DA_HAVE_BO_LOWMEM
#error "v3da_proto.h predates BO_CREATE placement (V3DA_BO_LOWMEM): build against the current tools/gpu-lane/v3d-async"
#endif

/*
 * STILL OPEN (G6b): syncobj and sync-file DESCRIPTORS across processes
 * (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD without EXPORT_SYNC_FILE, and sync files passed
 * over SCM_RIGHTS: linux-explicit-synchronization, wp_linux_drm_syncobj, DRI3 1.4,
 * vkGetSemaphoreFdKHR to another process). NEW ops, V3DA proto >= 6: a
 * "/v3dsync/<id>" namespace whose descriptors name a server syncobj (opaque fd) or
 * a frozen fence (sync file); poll() on it = atPollStatus against the fence page.
 * libdrm-phoenix emulates sync files in-process meanwhile (implicit sync, G6, needs
 * none of this).
 */
#define V3DA_OP_SYNCOBJ_EXPORT_EXT  55u
#define V3DA_OP_SYNCOBJ_FDIMPORT_EXT 56u


/* ------------------------------------------------------------------------- */
/* rpi4-kms                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * KMS_OP_PRIME_IMPORT (38): IMPLEMENTED for gap G7 (M6 section 16) - the opcode,
 * kms_prime_import_req_t (the v3da_bo_import_req_t byte layout) and the semantics
 * moved to kms_proto.h, which bumped the protocol to 2 (KMS_PROTO_PRIME_IMPORT).
 * libdrm-phoenix HELLOs with 2, falls back to KMS_PROTO_BASE (1) on a proto-1
 * server and answers a foreign card0 import with ENOSYS there, as before.
 */
#ifndef KMS_HAVE_PRIME_IMPORT
#error "kms_proto.h predates PRIME_IMPORT (G7): build against the current tools/gpu-lane/kms"
#endif


#endif /* _DRM_PHOENIX_EXT_H_ */
