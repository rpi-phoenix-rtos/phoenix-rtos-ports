/*
 * xorg-drm compat (libphoenix gap): <signal.h> defines no si_code values.
 * The X server compares si_code with SI_USER only to word a log line
 * (os/osinit.c OsSigHandler); 0 is the Linux/BSD value.
 */
#include_next <signal.h>

#ifndef SI_USER
#define SI_USER 0
#endif
