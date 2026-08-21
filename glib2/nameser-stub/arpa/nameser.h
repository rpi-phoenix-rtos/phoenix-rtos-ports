/*
 * Phoenix-RTOS — minimal <arpa/nameser.h> stub for DNS-aware autotools ports.
 *
 * Phoenix libc ships no resolver headers (arpa/nameser.h, nameser_compat.h);
 * <resolv.h> is an empty placeholder. glib-2.56's configure hard-requires
 * <arpa/nameser.h> to define the DNS class constant C_IN (it compiles a probe
 * "int qclass = C_IN;" and errors out if neither nameser.h nor nameser_compat.h
 * provides it). gio's gresolver also references the common type/class macros.
 *
 * This supplies just the standard BIND ns_class / ns_type constant values
 * (RFC 1035) as plain macros. It carries NO resolver functions — actual DNS
 * resolution on Phoenix would need a real resolver; gio's gresolver is not built
 * for the mc-critical libglib-2.0 and is not exercised here.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef _PHOENIX_ARPA_NAMESER_H
#define _PHOENIX_ARPA_NAMESER_H

/* DNS record classes (ns_class). */
#define C_IN    1   /* Internet */
#define C_CHAOS 3   /* CHAOS net */
#define C_HS    4   /* Hesiod */
#define C_ANY   255 /* wildcard */

/* DNS record types (ns_type), the common subset glib/gio references. */
#define T_A     1   /* host address */
#define T_NS    2   /* authoritative server */
#define T_CNAME 5   /* canonical name */
#define T_SOA   6   /* start of authority */
#define T_PTR   12  /* domain name pointer */
#define T_MX    15  /* mail exchange */
#define T_TXT   16  /* text strings */
#define T_AAAA  28  /* IPv6 address */
#define T_SRV   33  /* service location */
#define T_ANY   255 /* wildcard */

#endif /* _PHOENIX_ARPA_NAMESER_H */
