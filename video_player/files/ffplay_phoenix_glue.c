/*
 * Phoenix-RTOS
 *
 * ffplay glue: a larger default stack for threads started without one.
 *
 * libphoenix gives a thread created with a NULL (or default) attribute a 256 KiB stack
 * (PTHREAD_STACK_DEFAULT, include/arch/aarch64/limits.h); glibc gives 8 MiB. ffplay's
 * decoders run on threads that ask for no size -- SDL's (video_thread, audio_thread,
 * read_thread) and libavcodec's frame/slice workers -- and the H.264 decoder's call
 * chains are known to overflow a small stack on Phoenix (tools/ffmpeg-port/README.md:
 * "the H.264 decode must run on a large (8 MB) stack"). The link wraps pthread_create
 * (-Wl,--wrap=pthread_create) for this binary only and raises any request below
 * FFPLAY_THREAD_STACK (default 8 MiB; the environment variable of the same name, in
 * bytes, overrides it) that does not bring its own stack memory.
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <pthread.h>
#include <stdlib.h>

#define FFPLAY_THREAD_STACK_DEFAULT (8u * 1024u * 1024u)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);


static size_t ffplay_threadStack(void)
{
	static size_t size;
	const char *env;
	char *end;
	unsigned long v;

	if (size == 0) {
		size = FFPLAY_THREAD_STACK_DEFAULT;
		env = getenv("FFPLAY_THREAD_STACK");
		if (env != NULL) {
			v = strtoul(env, &end, 0);
			if ((*end == '\0') && (v >= 65536)) {
				size = v;
			}
		}
	}
	return size;
}


int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	pthread_attr_t local;
	size_t want = ffplay_threadStack(), have = 0;
	void *addr = NULL;
	int err;

	if (attr != NULL) {
		pthread_attr_getstack(attr, &addr, &have);
		if ((addr != NULL) || (have >= want)) {
			return __real_pthread_create(thread, attr, start, arg);
		}
		local = *attr;
	}
	else {
		pthread_attr_init(&local);
	}
	pthread_attr_setstacksize(&local, want);
	err = __real_pthread_create(thread, &local, start, arg);
	if (attr == NULL) {
		pthread_attr_destroy(&local);
	}
	return err;
}
