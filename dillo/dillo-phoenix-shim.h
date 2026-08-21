/*
 * Phoenix-RTOS — force-included compatibility shim for cross-building the Dillo
 * web browser (dillo-browser/dillo 3.2.0) for aarch64-phoenix (#53).
 *
 * Dillo is a C/C++ FLTK 1.3 X11 client. Phoenix's libphoenix/libc covers the
 * sockets/poll/select/fork/exec/getaddrinfo/pthread/iconv surface Dillo needs
 * (verified present in the sysroot), so this shim is deliberately tiny: it only
 * patches the handful of POSIX-isms Dillo assumes that Phoenix's headers do not
 * expose. Add to it ONLY when a real cross-compile gap surfaces; do not
 * speculatively stub.
 *
 * Included via -include on Dillo's CFLAGS/CXXFLAGS (see build-dillo.sh).
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef DILLO_PHOENIX_SHIM_H
#define DILLO_PHOENIX_SHIM_H

/*
 * Phoenix libm provides round()/roundf() but NOT the C99 rint()/rintf().
 * Dillo's dw/style.cc border-drawing calls rint() for pixel rounding. Alias it
 * to round() (the one-ULP difference at exact .5 boundaries is irrelevant for
 * integer pixel coordinates) — same approach as the FLTK shim. This must come
 * before the standard headers so it patches every translation unit.
 */
#include <math.h>
#ifndef rint
#define rint(x) round(x)
#endif
#ifndef rintf
#define rintf(x) roundf(x)
#endif

/*
 * Dillo's dpip/IO layer and dns.c expect the BSD/glibc socket + resolver macros.
 * Phoenix's lwip-backed <sys/socket.h>/<netdb.h> provide the functions, but a
 * few feature-test guards differ. Pull the standard headers in early so any
 * macro fixups below see the real declarations.
 */
#include <sys/types.h>

/*
 * AI_ADDRCONFIG / AI_NUMERICSERV: Dillo's dns.c passes getaddrinfo() hints with
 * AI_* flags. lwip's <netdb.h> defines the core ones (AI_PASSIVE, AI_CANONNAME,
 * AI_NUMERICHOST) but may omit AI_ADDRCONFIG/AI_NUMERICSERV. Defining them to 0
 * is harmless (they are hints, not requirements) and lets dns.c compile.
 */
#include <netdb.h>
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif
#ifndef AI_V4MAPPED
#define AI_V4MAPPED 0
#endif

#endif /* DILLO_PHOENIX_SHIM_H */
