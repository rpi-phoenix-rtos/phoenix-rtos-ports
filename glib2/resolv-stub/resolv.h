/*
 * Phoenix-RTOS — minimal <resolv.h> for the stub libresolv.a.
 *
 * Phoenix libc has no DNS resolver. glib-2.56's configure link-tests res_query()
 * (for gio's gresolver) and errors "not found" if it can't link. This declares
 * the resolver entry points backed by stub implementations (resolv-stub.c) that
 * fail cleanly (return -1). gio's gresolver is NOT built for the mc-critical
 * libglib-2.0, so these are never actually called; they exist only to satisfy
 * the configure link probe. Replace with a real resolver for DNS support.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _PHOENIX_RESOLV_H
#define _PHOENIX_RESOLV_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int res_query(const char *dname, int qclass, int type,
              unsigned char *answer, int anslen);
int res_search(const char *dname, int qclass, int type,
               unsigned char *answer, int anslen);
int res_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _PHOENIX_RESOLV_H */
