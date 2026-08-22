/*
 * Phoenix-RTOS compatibility shim for GNU nano (force-included via gcc -include).
 * Fills two small libc gaps nano uses; the bundled-gnulib-free nano 2.2.x needs
 * nothing more.
 */
#ifndef NANO_PHOENIX_SHIM_H
#define NANO_PHOENIX_SHIM_H

#include <pwd.h>

/* Phoenix <stdio.h> lacks P_tmpdir (used for nano's mkstemp temp files). */
#ifndef P_tmpdir
#define P_tmpdir "/tmp"
#endif

/* libphoenix now provides the passwd-enumeration API (getpwent/setpwent/endpwent
 * in unistd/pwd.c); the local stubs it once needed were removed after the libc
 * gap was filled — redeclaring them here now clashes with <pwd.h>. */

#endif /* NANO_PHOENIX_SHIM_H */
