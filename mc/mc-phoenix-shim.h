/*
 * Phoenix-RTOS compatibility shim for Midnight Commander (force-included via
 * gcc -include). Fills libc gaps mc uses that Phoenix libphoenix lacks.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Witold Bołt
 */
#ifndef MC_PHOENIX_SHIM_H
#define MC_PHOENIX_SHIM_H

#include <pwd.h>

/* libphoenix now provides the passwd-enumeration API (getpwent/setpwent/endpwent
 * in unistd/pwd.c); the local stubs it once needed were removed after the libc
 * gap was filled — redeclaring them here now clashes with <pwd.h>. Same change as
 * the nano port. */

/* P_tmpdir for temp-file fallbacks. */
#ifndef P_tmpdir
#define P_tmpdir "/tmp"
#endif

#endif /* MC_PHOENIX_SHIM_H */
