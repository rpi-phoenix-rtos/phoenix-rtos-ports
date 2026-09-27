/*
 * xorg-drm compat (libphoenix gap): <dlfcn.h> has no RTLD_DEFAULT.  The X
 * server's loader uses dlsym(RTLD_DEFAULT, name) only after the builtin-module
 * tables (loader patch 0002) found nothing; in a static Phoenix program that
 * lookup cannot succeed anyway.  NULL is the value glibc and the BSDs use.
 */
#include_next <dlfcn.h>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif
