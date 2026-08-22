/*
 * Phoenix-RTOS compatibility shim for xedit's bundled Lisp interpreter.
 * Force-included (gcc -include) into the xedit build. Phoenix's libphoenix lacks
 * a few legacy Unix-isms that the Lisp `time`/math builtins use; stub them so
 * xedit builds. The affected Lisp builtins (interval timers) are not needed by
 * the editor itself.
 */
#ifndef XEDIT_PHOENIX_SHIM_H
#define XEDIT_PHOENIX_SHIM_H

#include <sys/time.h>
/* Several Lisp math TUs (mp/mpi.c, ...) call copysign/isfinite/... without
 * including <math.h>; pull it in up front so they are declared, not implicit. */
#include <math.h>
/* Phoenix libm provides neither a copysign prototype nor the symbol, so define
 * it inline via the GCC builtin (no libm dependency, satisfies mp/mpi.c at both
 * compile and link). */
#ifndef copysign
static inline double pl_copysign(double x, double y) { return __builtin_copysign(x, y); }
#define copysign(x, y) pl_copysign((x), (y))
#endif

/* Interval timers (lisp/time.c): not implemented on Phoenix — provide the type,
 * the ITIMER_* selectors, and no-op get/set so the Lisp `time` builtin links. */
#ifndef ITIMER_REAL
struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

static inline int setitimer(int which, const struct itimerval *nv, struct itimerval *ov)
{
	(void)which;
	(void)nv;
	(void)ov;
	return 0;
}

static inline int getitimer(int which, struct itimerval *cv)
{
	(void)which;
	if (cv != 0) {
		cv->it_interval.tv_sec = 0;
		cv->it_interval.tv_usec = 0;
		cv->it_value.tv_sec = 0;
		cv->it_value.tv_usec = 0;
	}
	return 0;
}
#endif /* ITIMER_REAL */

#endif /* XEDIT_PHOENIX_SHIM_H */
