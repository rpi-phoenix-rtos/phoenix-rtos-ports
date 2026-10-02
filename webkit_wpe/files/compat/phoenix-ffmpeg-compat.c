/*
 * Phoenix-RTOS browser B8 (WPE WebKit <video>) -- FFmpeg's threads, not part of libphoenix.
 *
 * libphoenix gives a thread created without a stack size 256 KiB (PTHREAD_STACK_DEFAULT); glibc
 * gives 8 MiB. libavcodec's frame and slice workers ask for no size, and the H.264 decoder's
 * call chains overflow a small stack on Phoenix (the ffmpeg-port README: "the H.264 decode must
 * run on a large (8 MB) stack"; the video_player port's ffplay_phoenix_glue.c is the same fix
 * for its programs, through a global --wrap). In wpe-browser only FFmpeg's threads get it:
 * build-wpe.sh renames pthread_create to phx_ffmpeg_pthread_create in its copies of the FFmpeg
 * archives (objcopy --redefine-sym), so WebKit's and GLib's threads keep their own sizes.
 * WPE_PHOENIX_FFMPEG_THREAD_STACK (bytes, >= 64 KiB) overrides the 8 MiB.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pthread.h>
#include <stdlib.h>

#define PHX_FFMPEG_THREAD_STACK (8u * 1024u * 1024u)

int phx_ffmpeg_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);


static size_t phx_ffmpegThreadStack(void)
{
	static size_t size;
	const char *env;
	char *end;
	unsigned long v;

	if (size == 0) {
		size = PHX_FFMPEG_THREAD_STACK;
		env = getenv("WPE_PHOENIX_FFMPEG_THREAD_STACK");
		if (env != NULL) {
			v = strtoul(env, &end, 0);
			if ((*end == '\0') && (v >= 65536)) {
				size = v;
			}
		}
	}
	return size;
}


int phx_ffmpeg_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	pthread_attr_t local;
	size_t want = phx_ffmpegThreadStack(), have = 0;
	void *addr = NULL;
	int err;

	if (attr != NULL) {
		pthread_attr_getstack(attr, &addr, &have);
		if ((addr != NULL) || (have >= want)) {
			return pthread_create(thread, attr, start, arg);
		}
		local = *attr;
	}
	else {
		pthread_attr_init(&local);
	}
	pthread_attr_setstacksize(&local, want);
	err = pthread_create(thread, &local, start, arg);
	if (attr == NULL) {
		pthread_attr_destroy(&local);
	}
	return err;
}
