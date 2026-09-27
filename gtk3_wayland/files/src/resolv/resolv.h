/*
 * gtk3-wayland: <resolv.h> declarations for GIO (the Phoenix-RTOS header is empty).
 * The functions are stand-ins (resolv_stub.c): DNS record queries (SRV/MX/TXT/NS/SOA
 * through GResolver) fail with h_errno = NO_RECOVERY.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef GTK3WL_RESOLV_H
#define GTK3WL_RESOLV_H

#include <netdb.h>
#include <arpa/nameser.h>

#ifdef __cplusplus
extern "C" {
#endif

int res_init(void);
int res_query(const char *dname, int class_, int type, unsigned char *answer, int anslen);
int dn_expand(const unsigned char *msg, const unsigned char *eom, const unsigned char *src, char *dst, int dstsiz);
int dn_skipname(const unsigned char *src, const unsigned char *eom);

#ifdef __cplusplus
}
#endif

#endif /* GTK3WL_RESOLV_H */
