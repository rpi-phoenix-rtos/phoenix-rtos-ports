/*
 * Phoenix-RTOS browser track C (JavaScriptCore) -- local compat, not part of libphoenix.
 *
 * Gaps WTF/JSC need that libphoenix does not have yet. Each one is listed for track B
 * (docs/browser/PLAN.md, B1) and must be deleted here once libphoenix provides it:
 *
 *   pthread_getattr_np()  TODO(browser-B1)  stack bounds of the CALLING thread (WTF StackBounds)
 *   sem_*()               TODO(browser-B1)  unnamed POSIX semaphores (see include/semaphore.h)
 *   madvise()             TODO(browser-B1)  advice accepted, nothing done (see include/sys/mman.h)
 *   msync()               TODO(browser-B1)  ENOSYS: no shared file mappings (P21)
 *
 * build.sh probes the sysroot's libphoenix and compiles only the ones it lacks
 * (-DPHX_COMPAT_<NAME>=1), so the same tree builds before and after they land there.
 *
 * And link-time hooks, not gaps: _malloc_init() and the _malloc_fork*() trio (below).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <pthread.h>
#if PHX_COMPAT_SEM
#include <semaphore.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


/* mimalloc replaces malloc/free/realloc/calloc/posix_memalign/... in this binary (WebKit's
 * Source/bmalloc/mimalloc/CMakeLists.txt, MI_OVERRIDE on Phoenix). libphoenix's startup
 * (misc/init.c) still calls _malloc_init(), which lives in the same archive member as
 * libphoenix's malloc (stdlib/malloc_dl.c): resolving it here keeps that member, and its
 * duplicate malloc/free, out of the link. mimalloc initialises itself on first use.
 * Weak, because helper programs of the build (LLIntSettingsExtractor) link this object without
 * mimalloc: there libphoenix's malloc_dl.o is pulled for malloc() and its _malloc_init() wins. */
__attribute__((weak)) void _malloc_init(void)
{
}


/* Same reason, since libphoenix 19e9713 (P24): fork() (unistd/sys.c) takes and releases
 * libphoenix's heap lock around the copy through these three, which also live in
 * malloc_dl.o. With mimalloc as the allocator there is no libphoenix heap to protect. */
__attribute__((weak)) void _malloc_forkPrepare(void)
{
}


__attribute__((weak)) void _malloc_forkParent(void)
{
}


__attribute__((weak)) void _malloc_forkChild(void)
{
}


/* Triage aid: with PHX_TRACE_ABORT=1 in the environment, a SIGABRT (abort(), a failed
 * RELEASE_ASSERT on this target) prints the interrupted pc/lr and the frame-pointer chain to
 * stderr before the default action, e.g. `PHX-ABORT pc=0x... lr=0x...` then `PHX-ABORT fp#N
 * ret=0x...`. Resolve with `addr2line -e <out>/jsc`. Silent and inert otherwise. */
static void phx_putHex(const char *tag, unsigned long v)
{
	char buf[48];
	size_t n = strlen(tag);
	int i;

	memcpy(buf, tag, n);
	buf[n++] = '0';
	buf[n++] = 'x';
	for (i = 60; i >= 0; i -= 4) {
		buf[n++] = "0123456789abcdef"[(v >> i) & 0xf];
	}
	buf[n++] = '\n';
	(void)write(2, buf, n);
}


static unsigned long phx_peekAddr;


static void phx_abortTrace(int sig, siginfo_t *info, void *ctx)
{
	const ucontext_t *uc = ctx;
	const unsigned long *fp = (const unsigned long *)uc->uc_mcontext.regs[29];
	int depth;

	(void)info;
	phx_putHex("PHX-ABORT pc=", uc->uc_mcontext.pc);
	phx_putHex("PHX-ABORT lr=", uc->uc_mcontext.regs[30]);
	phx_putHex("PHX-ABORT x0=", uc->uc_mcontext.regs[0]);
	phx_putHex("PHX-ABORT x1=", uc->uc_mcontext.regs[1]);
	phx_putHex("PHX-ABORT x2=", uc->uc_mcontext.regs[2]);
	if (phx_peekAddr != 0UL) {
		/* PHX_TRACE_PEEK=<hex address>: eight words there, e.g. a lock's handle */
		const unsigned long *w = (const unsigned long *)phx_peekAddr;
		for (depth = 0; depth < 8; depth++) {
			phx_putHex("PHX-ABORT peek=", w[depth]);
		}
	}
	/* Stop at the first frame pointer that is not a user (39-bit) stack address. */
	for (depth = 0; depth < 24 && fp != NULL && ((unsigned long)fp & 0xf) == 0 && ((unsigned long)fp >> 39) == 0; depth++) {
		phx_putHex("PHX-ABORT ret=", fp[1]);
		if ((const unsigned long *)fp[0] <= fp) {
			break;
		}
		fp = (const unsigned long *)fp[0];
	}
	signal(sig, SIG_DFL);
	raise(sig);
}


__attribute__((constructor)) static void phx_abortTraceInit(void)
{
	struct sigaction sa;
	const char *on = getenv("PHX_TRACE_ABORT");

	if (on == NULL || on[0] != '1') {
		return;
	}
	on = getenv("PHX_TRACE_PEEK");
	if (on != NULL) {
		phx_peekAddr = strtoul(on, NULL, 16);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = phx_abortTrace;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGABRT, &sa, NULL);
}


#if PHX_COMPAT_PTHREAD_GETATTR_NP
/* pthread_getattr_np(): only for pthread_self(), which is all WTF asks (StackBounds.cpp).
 *
 * libphoenix keeps each pthread's stack in a private record (pthread/pthread.c) and the kernel
 * maps the main thread's stack itself (proc/process.c, process_load), so neither is reachable
 * through a public API. Every stack is its own anonymous map entry, though, so the entry that
 * holds a local variable of the caller IS the caller's stack: meminfo() lists the process's map
 * entries (as `psh mem <pid>` does). A pthread guard page is a separate PROT_NONE entry below
 * the stack (pthread_create mprotects it), so it is not included. */
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr)
{
	meminfo_t info;
	entryinfo_t *map = NULL, *grown;
	int mapsz = 32, i, err = ENOMEM;
	uintptr_t here = (uintptr_t)&info;

	if (attr == NULL) {
		return EINVAL;
	}
	if (!pthread_equal(thread, pthread_self())) {
		return ENOTSUP;
	}

	for (;;) {
		grown = realloc(map, (size_t)mapsz * sizeof(*map));
		if (grown == NULL) {
			free(map);
			return ENOMEM;
		}
		map = grown;
		memset(&info, 0, sizeof(info));
		info.page.mapsz = -1;
		info.maps.mapsz = -1;
		info.entry.kmapsz = -1;
		info.entry.pid = (unsigned int)getpid();
		info.entry.mapsz = mapsz;
		info.entry.map = map;
		meminfo(&info);
		if (info.entry.mapsz < 0) {
			free(map);
			return ESRCH;
		}
		if (info.entry.mapsz <= mapsz) {
			break;
		}
		mapsz = info.entry.mapsz + 16;
	}

	for (i = 0; i < info.entry.mapsz; i++) {
		uintptr_t lo = (uintptr_t)map[i].vaddr;
		if ((here >= lo) && (here - lo < map[i].size)) {
			pthread_attr_init(attr);
			err = pthread_attr_setstack(attr, map[i].vaddr, map[i].size);
			break;
		}
	}
	free(map);
	return err;
}


#endif /* PHX_COMPAT_PTHREAD_GETATTR_NP */


#if PHX_COMPAT_MADVISE
int madvise(void *addr, size_t len, int advice)
{
	(void)addr;
	(void)len;

	switch (advice) {
		case MADV_NORMAL:
		case MADV_RANDOM:
		case MADV_SEQUENTIAL:
		case MADV_WILLNEED:
		case MADV_DONTNEED:
			return 0;
		default:
			errno = EINVAL;
			return -1;
	}
}


#endif /* PHX_COMPAT_MADVISE */


#if PHX_COMPAT_MSYNC
int msync(void *addr, size_t len, int flags)
{
	(void)addr;
	(void)len;
	(void)flags;
	errno = ENOSYS;
	return -1;
}
#endif /* PHX_COMPAT_MSYNC */


#if PHX_COMPAT_SEM
int sem_init(sem_t *sem, int pshared, unsigned int value)
{
	if (pshared != 0) {
		errno = ENOSYS;
		return -1;
	}
	pthread_mutex_init(&sem->lock, NULL);
	pthread_cond_init(&sem->cond, NULL);
	sem->value = value;
	return 0;
}


int sem_destroy(sem_t *sem)
{
	pthread_cond_destroy(&sem->cond);
	pthread_mutex_destroy(&sem->lock);
	return 0;
}


int sem_wait(sem_t *sem)
{
	pthread_mutex_lock(&sem->lock);
	while (sem->value == 0) {
		pthread_cond_wait(&sem->cond, &sem->lock);
	}
	sem->value--;
	pthread_mutex_unlock(&sem->lock);
	return 0;
}


int sem_trywait(sem_t *sem)
{
	int ret = 0;

	pthread_mutex_lock(&sem->lock);
	if (sem->value == 0) {
		errno = EAGAIN;
		ret = -1;
	}
	else {
		sem->value--;
	}
	pthread_mutex_unlock(&sem->lock);
	return ret;
}


int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
	int ret = 0, err = 0;

	pthread_mutex_lock(&sem->lock);
	while ((sem->value == 0) && (err == 0)) {
		err = pthread_cond_timedwait(&sem->cond, &sem->lock, abstime);
	}
	if (sem->value > 0) {
		sem->value--;
	}
	else {
		errno = err;
		ret = -1;
	}
	pthread_mutex_unlock(&sem->lock);
	return ret;
}


int sem_post(sem_t *sem)
{
	pthread_mutex_lock(&sem->lock);
	sem->value++;
	pthread_cond_signal(&sem->cond);
	pthread_mutex_unlock(&sem->lock);
	return 0;
}


int sem_getvalue(sem_t *sem, int *value)
{
	pthread_mutex_lock(&sem->lock);
	*value = (int)sem->value;
	pthread_mutex_unlock(&sem->lock);
	return 0;
}

#endif /* PHX_COMPAT_SEM */