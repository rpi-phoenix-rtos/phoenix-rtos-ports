/* SPDX-License-Identifier: Zlib
 *
 * sdl_phoenix_glstubs.c — tiny libc gap-fillers for the ported Mesa V3D GL
 * stack on Phoenix-RTOS. These are NOT SDL code and are NOT compiled into
 * libSDL2.a; they are linked by any program that links libGL-phoenix.a /
 * libv3d-phoenix.a (the SDL2 GL test does so via build-sdl2-gltest.py). It is a
 * standalone, self-contained zlib copy so the SDL2 port carries its own stub and
 * does not depend on any other port's translation unit for it.
 *
 * pthread_getcpuclockid: Mesa's util/u_thread.c references it for per-thread CPU
 * timing. Phoenix's libphoenix does not implement it, so link fails without a
 * definition. A monotonic-clock stand-in is fine for Mesa's timing use.
 */
#include <pthread.h>
#include <time.h>

int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id)
{
    (void)thread;
    if (clock_id) {
        *clock_id = CLOCK_MONOTONIC;
    }
    return 0;
}
