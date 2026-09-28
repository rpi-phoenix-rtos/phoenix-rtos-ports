/*
 * gtk3-wayland: <arpa/nameser.h> for GIO's resolver code (Phoenix-RTOS has none).
 *
 * DNS wire-format constants (RFC 1035 and later type registrations), the message
 * header layout and the BIND byte-order helpers GIO's gthreadedresolver.c parses
 * answers with. No resolver lives here: resolv_stub.c makes every DNS record query
 * fail cleanly (plain host name lookups go through getaddrinfo(), not this).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef GTK3WL_ARPA_NAMESER_H
#define GTK3WL_ARPA_NAMESER_H

#include <stdint.h>
#include <sys/types.h>

#define NS_PACKETSZ 512
#define NS_MAXDNAME 1025
#define NS_HFIXEDSZ 12
#define NS_QFIXEDSZ 4
#define NS_INT32SZ 4
#define NS_INT16SZ 2
#define PACKETSZ NS_PACKETSZ
#define MAXDNAME NS_MAXDNAME
#define HFIXEDSZ NS_HFIXEDSZ
#define QFIXEDSZ NS_QFIXEDSZ

typedef enum __ns_class {
	ns_c_invalid = 0, ns_c_in = 1, ns_c_chaos = 3, ns_c_hs = 4, ns_c_none = 254, ns_c_any = 255, ns_c_max = 65536
} ns_class;

typedef enum __ns_type {
	ns_t_invalid = 0, ns_t_a = 1, ns_t_ns = 2, ns_t_md = 3, ns_t_mf = 4, ns_t_cname = 5, ns_t_soa = 6,
	ns_t_mb = 7, ns_t_mg = 8, ns_t_mr = 9, ns_t_null = 10, ns_t_wks = 11, ns_t_ptr = 12, ns_t_hinfo = 13,
	ns_t_minfo = 14, ns_t_mx = 15, ns_t_txt = 16, ns_t_rp = 17, ns_t_afsdb = 18, ns_t_x25 = 19,
	ns_t_isdn = 20, ns_t_rt = 21, ns_t_nsap = 22, ns_t_nsap_ptr = 23, ns_t_sig = 24, ns_t_key = 25,
	ns_t_px = 26, ns_t_gpos = 27, ns_t_aaaa = 28, ns_t_loc = 29, ns_t_nxt = 30, ns_t_eid = 31,
	ns_t_nimloc = 32, ns_t_srv = 33, ns_t_atma = 34, ns_t_naptr = 35, ns_t_kx = 36, ns_t_cert = 37,
	ns_t_a6 = 38, ns_t_dname = 39, ns_t_sink = 40, ns_t_opt = 41, ns_t_apl = 42, ns_t_tkey = 249,
	ns_t_tsig = 250, ns_t_ixfr = 251, ns_t_axfr = 252, ns_t_mailb = 253, ns_t_maila = 254, ns_t_any = 255,
	ns_t_zxfr = 256, ns_t_max = 65536
} ns_type;

#define C_IN ns_c_in
#define C_CHAOS ns_c_chaos
#define C_HS ns_c_hs
#define C_ANY ns_c_any
#define T_A ns_t_a
#define T_NS ns_t_ns
#define T_CNAME ns_t_cname
#define T_SOA ns_t_soa
#define T_PTR ns_t_ptr
#define T_MX ns_t_mx
#define T_TXT ns_t_txt
#define T_AAAA ns_t_aaaa
#define T_SRV ns_t_srv
#define T_ANY ns_t_any

/* The fixed 12-byte message header (little-endian bit-field order: AArch64). */
typedef struct {
	unsigned id : 16;
	unsigned rd : 1;
	unsigned tc : 1;
	unsigned aa : 1;
	unsigned opcode : 4;
	unsigned qr : 1;
	unsigned rcode : 4;
	unsigned cd : 1;
	unsigned ad : 1;
	unsigned unused : 1;
	unsigned ra : 1;
	unsigned qdcount : 16;
	unsigned ancount : 16;
	unsigned nscount : 16;
	unsigned arcount : 16;
} HEADER;

#define NS_GET16(s, cp) \
	do { \
		const unsigned char *t_cp_ = (const unsigned char *)(cp); \
		(s) = (uint16_t)(((uint16_t)t_cp_[0] << 8) | (uint16_t)t_cp_[1]); \
		(cp) += NS_INT16SZ; \
	} while (0)

#define NS_GET32(l, cp) \
	do { \
		const unsigned char *t_cp_ = (const unsigned char *)(cp); \
		(l) = ((uint32_t)t_cp_[0] << 24) | ((uint32_t)t_cp_[1] << 16) | ((uint32_t)t_cp_[2] << 8) | (uint32_t)t_cp_[3]; \
		(cp) += NS_INT32SZ; \
	} while (0)

#define GETSHORT NS_GET16
#define GETLONG NS_GET32

#endif /* GTK3WL_ARPA_NAMESER_H */
