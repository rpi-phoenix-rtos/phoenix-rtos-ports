/* Phoenix-RTOS portability shims for Redis 7.2.x, injected via -include.
 * Each shim is a divergence from Linux/glibc that Phoenix's libc/pthread lacks.
 * Kept out of the Redis tree so the port is a build recipe, not a source fork. */
#ifndef REDIS_PHOENIX_COMPAT_H
#define REDIS_PHOENIX_COMPAT_H

#include <errno.h>
#include <pthread.h>

/* --- errno constants Phoenix's <errno.h> lacks (used only in log/error strings) --- */
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif

/* --- pthread cancellation *type*. Older libphoenix lacked setcanceltype (current
 * libphoenix has it). Redis uses it only in makeThreadKillable() for the
 * crash-report fast-memory-test thread — non-core, so a no-op is acceptable. --- */
#ifndef PTHREAD_CANCEL_DEFERRED
#define PTHREAD_CANCEL_DEFERRED 0
#endif
#ifndef PTHREAD_CANCEL_ASYNCHRONOUS
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#endif
static inline int phoenix_pthread_setcanceltype(int type, int *oldtype) {
    (void)type; if (oldtype) *oldtype = PTHREAD_CANCEL_DEFERRED; return 0;
}
#define pthread_setcanceltype phoenix_pthread_setcanceltype

/* --- pthread_self() is NULL in a fork()ed child. The child runs on a new
 * kernel thread (new tid), but libphoenix's fork() does not re-key the copied
 * main-thread record to it, and pthread_setcancelstate() -- unlike its
 * setcanceltype/testcancel/setspecific siblings -- dereferences the NULL
 * without a check. `--daemonize yes` forks before initServer(), whose
 * makeThreadKillable() then faulted. Threads created after the fork are
 * registered normally, so only the forking (main) thread needs the guard. --- */
static inline int phoenix_pthread_setcancelstate(int state, int *oldstate) {
    if (pthread_self() == (pthread_t)0) {
        if (oldstate) *oldstate = PTHREAD_CANCEL_ENABLE;
        return 0;
    }
    return pthread_setcancelstate(state, oldstate);
}
#define pthread_setcancelstate phoenix_pthread_setcancelstate

/* --- crash-report / watchdog bits debug.c needs (all non-core diagnostics) --- */
#include <sys/time.h>
#include <signal.h>

#ifndef SI_USER
#define SI_USER 0
#endif

/* setitimer / struct itimerval — Phoenix lacks them; the watchdog just won't fire */
#ifndef ITIMER_REAL
#define ITIMER_REAL 0
#endif
#ifndef ITIMER_VIRTUAL
#define ITIMER_VIRTUAL 1
#endif
#ifndef ITIMER_PROF
#define ITIMER_PROF 2
#endif
struct phoenix_itimerval { struct timeval it_interval; struct timeval it_value; };
#define itimerval phoenix_itimerval
static inline int phoenix_setitimer(int which, const struct phoenix_itimerval *nv,
                                    struct phoenix_itimerval *ov) {
    (void)which; (void)nv; (void)ov; return 0;
}
#define setitimer phoenix_setitimer

#endif /* REDIS_PHOENIX_COMPAT_H */
