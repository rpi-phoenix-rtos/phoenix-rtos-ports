/*
 * xorg-drm compat (libphoenix gap): no O_NOFOLLOW.  Used by the X server's
 * lock-file code (os/utils.c LockServer) to refuse a symlinked lock file; 0
 * degrades that to a plain open (single-user system, /tmp owned by the one
 * user).
 */
#include_next <fcntl.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
