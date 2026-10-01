/* rpivid host test shim (SPDX-License-Identifier: BSD-3-Clause) */
#ifndef RPIVID_SHIM_MSG_H
#define RPIVID_SHIM_MSG_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t port; uint64_t id; } oid_t;
enum { mtOpen = 0, mtClose, mtRead, mtWrite, mtTruncate, mtDevCtl };
typedef struct {
	int type;
	oid_t oid;
	struct { const void *data; size_t size; uint8_t raw[64]; } i;
	struct { void *data; size_t size; int err; struct { oid_t dev; } lookup; uint8_t raw[64]; } o;
} msg_t;
int lookup(const char *name, oid_t *file, oid_t *dev);
int msgSend(uint32_t port, msg_t *m);
#endif
