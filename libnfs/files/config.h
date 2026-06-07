/*
 * Static config.h for the Phoenix-RTOS libnfs port (aarch64a72-generic-rpi4b).
 *
 * libnfs normally generates this via autoconf (./configure) or CMake feature
 * probes. Neither can run here: the dev host has no autotools, and a cross
 * target cannot execute the probe binaries that autoconf/CMake link to detect
 * features. So we hand-write the HAVE_* set, verified header-by-header against
 * the Phoenix aarch64-phoenix sysroot
 * (.toolchain/aarch64-phoenix/aarch64-phoenix/usr/include).
 *
 * Each define below was confirmed present (header file exists, or symbol/struct
 * member exists) in that sysroot. Anything NOT present is left undefined so
 * libnfs takes its portable fallback path.
 *
 * Single-threaded build (NFS sync API only): HAVE_PTHREAD / HAVE_MULTITHREADING
 * are intentionally NOT defined (T0/T1/T2). MT support is a later stage (plan
 * section 8) and would add HAVE_MULTITHREADING + a native thread shim.
 */

#ifndef LIBNFS_PHOENIX_CONFIG_H
#define LIBNFS_PHOENIX_CONFIG_H

/* ---- headers present in the Phoenix sysroot ---- */
#define HAVE_ARPA_INET_H 1      /* htonl->htobe32 / ntohl->be32toh; without this
                                   socket.c gets implicit-decl of htonl/ntohl */
#define HAVE_POLL_H 1           /* poll() is the only wait primitive in the sync loop */
#define HAVE_SYS_UIO_H 1        /* writev() in rpc_write_to_socket */
#define HAVE_UNISTD_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1    /* TCP_NODELAY */
#define HAVE_NET_IF_H 1
#define HAVE_NETDB_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_PWD_H 1            /* getpwnam used only by example utils, not the .a;
                                   header is present so the define is honest */
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UTIME_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_UTSNAME_H 1

/* ---- features / struct members verified in the sysroot ---- */
#define HAVE_SOCKADDR_STORAGE 1 /* sys/socket.h:33 defines struct sockaddr_storage;
                                   libnfs-private.h:105 redefines it ONLY when this
                                   is unset -> must set to avoid "redefinition" */
#define HAVE_CLOCK_GETTIME 1    /* time.h:104 declares clock_gettime */
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1 /* sys/stat.h uses st_mtim.tv_sec */
#define MAJOR_IN_SYSMACROS 1    /* major()/minor()/makedev() in <sys/sysmacros.h> */

/*
 * ---- small OS-seam shim ----
 *
 * SOL_TCP: a Linux-ism libnfs uses in set_tcp_sockopt() (socket.c:196) as the
 * setsockopt level for TCP_NODELAY. Phoenix (like POSIX/BSD) only provides
 * IPPROTO_TCP (netinet/in.h:59), which is the portable equivalent for that
 * call. Alias it so socket.c compiles unchanged. Guarded so it never clashes if
 * a future Phoenix sys header starts defining SOL_TCP itself.
 */
#include <netinet/in.h>
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

/*
 * htonl/ntohl/htons/ntohs live in <arpa/inet.h> on Phoenix (-> htobe32/be32toh),
 * NOT in <netinet/in.h> as on glibc/Linux. Several libnfs sources (e.g.
 * nfs_v4.c) include only <netinet/in.h> and then call ntohl/htonl, which would
 * be implicit-declared on Phoenix. Pulling arpa/inet.h in via config.h (which
 * every .c includes first) makes the byte-order functions visible everywhere
 * without patching each source. socket.c already includes it under
 * HAVE_ARPA_INET_H; this just guarantees the other units see it too.
 */
#include <arpa/inet.h>

/*
 * CLOCK_MONOTONIC_COARSE: a Linux-specific low-overhead clock libnfs reads in
 * rpc_current_time()/rpc_current_time_us() (init.c:78,90) under HAVE_CLOCK_GETTIME.
 * It is purely a perf hint (coarser, cheaper than CLOCK_MONOTONIC); the portable
 * CLOCK_MONOTONIC (time.h:29) is a correct, if slightly more expensive, stand-in.
 * Alias it so init.c compiles unchanged.
 */
#include <time.h>
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

/*
 * O_NOFOLLOW: POSIX open() flag that Phoenix's <fcntl.h> does not define.
 * libnfs uses it only as a *caller-supplied bit* it tests against its own
 * open-flags word (nfs_v3.c:335, nfs_v4.c:2527) to decide whether to refuse a
 * symlink (-ELOOP). It is never passed to the host open(). Defining it as a
 * spare bit (Phoenix uses O_* bits up to 0x10000, see phoenix/posix-fcntl.h)
 * keeps the symlink-NOFOLLOW guard functional and consistent for any Phoenix
 * caller of nfs_open(): a caller that doesn't set the bit gets follow
 * behaviour, one that sets this same bit gets NOFOLLOW. The exact numeric value
 * is internal to libnfs's flag word, so any non-colliding bit is correct.
 */
#include <fcntl.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif

/*
 * Errno values Phoenix's small errno set (phoenix/errno.h) does not define but
 * libnfs needs for its NFS-status -> errno mapping (nfs.c nfsstat3_to_errno(),
 * nfs4 equivalents) and its symlink-loop guard. These are returned to the
 * libnfs caller; the numeric values match the standard Linux/asm-generic errno
 * numbers so a Phoenix consumer comparing against POSIX names behaves sanely.
 * If Phoenix later adds these to its errno.h, the #ifndef guards defer to it.
 */
#include <errno.h>
#ifndef ELOOP
#define ELOOP 40        /* Too many symbolic links encountered */
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 39    /* Directory not empty */
#endif
#ifndef ESTALE
#define ESTALE 116      /* Stale file handle */
#endif

/*
 * ---- intentionally NOT defined (absent in sysroot or not wanted for T0) ----
 *
 * HAVE_SYS_FILIO_H / HAVE_SYS_SOCKIO_H : absent -> set_nonblocking() falls back
 *     from ioctl(FIONBIO) to fcntl(F_SETFL,O_NONBLOCK), which Phoenix supports.
 * HAVE_SOCKADDR_LEN     : Phoenix sockaddr has no sa_len member.
 * HAVE_SO_BINDTODEVICE  : not in Phoenix sys/socket.h.
 * HAVE_SYS_VFS_H        : absent (statvfs.h used instead).
 * HAVE_DLFCN_H / HAVE_FUSE_H / HAVE_DISPATCH_DISPATCH_H : absent.
 * HAVE_STDATOMIC_H      : only needed for the MT build; omitted for sync T0.
 * HAVE_PTHREAD / HAVE_MULTITHREADING : single-threaded sync build (see header).
 * HAVE_LIBKRB5 / HAVE_TALLOC_TEVENT / HAVE_TLS / HAVE_GNUTLS_* : krb5/tls/talloc
 *     not built (krb5-wrapper.c excluded from the object set).
 * HAVE_LIBNSL / HAVE_LIBSOCKET : sockets are in libc, no separate -lsocket/-lnsl.
 */

#endif /* LIBNFS_PHOENIX_CONFIG_H */
